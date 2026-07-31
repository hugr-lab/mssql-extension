//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/string_codec.cpp
//
// String family implementation. See codec/string_codec.hpp for behaviour
// parity notes and the FR-023 issue #91 length-validation contract.
//===----------------------------------------------------------------------===//

#include "codec/string_codec.hpp"

#include "codec/vector_format.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/interval.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include <simdutf.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace string {

namespace {

// Defer to the public EscapeSqlSingleQuotes API for in-module callers so
// both this file and external callers (FilterEncoder LIKE emitter) share
// one implementation.

// Append a raw UTF-16LE non-PLP nvarchar payload to `buf` with a 2-byte
// length prefix. Mirrors BCPRowEncoder::EncodeNVarchar bit-for-bit.
void AppendNVarcharNonPlp(duckdb::vector<uint8_t> &buf, const char *input, size_t input_len) {
	size_t start_pos = buf.size();
	buf.resize(start_pos + 2 + input_len * 2);
	size_t utf16_len = tds::encoding::Utf16LEEncodeDirect(input, input_len, buf.data() + start_pos + 2);
	buf[start_pos] = static_cast<uint8_t>(utf16_len & 0xFF);
	buf[start_pos + 1] = static_cast<uint8_t>((utf16_len >> 8) & 0xFF);
	buf.resize(start_pos + 2 + utf16_len);
}

// Single-chunk PLP frame primitives — the wire layout (8-byte UNKNOWN-length
// header, 4-byte LE chunk length, data, 4-byte zero terminator) is written in
// exactly one place so the legacy and W3 append paths cannot drift apart.
void WriteLe32(uint8_t *out, uint32_t v) {
	out[0] = static_cast<uint8_t>(v & 0xFF);
	out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
	out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
	out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void WritePlpHeader(uint8_t *out) {
	constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;
	for (int i = 0; i < 8; i++) {
		out[i] = static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF);
	}
}

// Append a PLP-framed nvarchar(max) payload to `buf`. Mirrors
// BCPRowEncoder::EncodeNVarcharPLP bit-for-bit.
void AppendNVarcharPlp(duckdb::vector<uint8_t> &buf, const char *input, size_t input_len) {
	if (input_len == 0) {
		// Empty value: header + zero terminator, no data chunk.
		const size_t start_pos = buf.size();
		buf.resize(start_pos + 8 + 4);
		WritePlpHeader(buf.data() + start_pos);
		WriteLe32(buf.data() + start_pos + 8, 0);
		return;
	}

	size_t start_pos = buf.size();
	size_t max_utf16_len = input_len * 2;
	buf.resize(start_pos + 8 + 4 + max_utf16_len + 4);

	uint8_t *out = buf.data() + start_pos;
	WritePlpHeader(out);
	uint8_t *chunk_len_ptr = out + 8;
	size_t utf16_len = tds::encoding::Utf16LEEncodeDirect(input, input_len, chunk_len_ptr + 4);
	WriteLe32(chunk_len_ptr, static_cast<uint32_t>(utf16_len));
	WriteLe32(chunk_len_ptr + 4 + utf16_len, 0);

	buf.resize(start_pos + 8 + 4 + utf16_len + 4);
}

// Shared encode body taking already-resolved UTF-8 string view. Picks the
// PLP / non-PLP path based on col, after running FR-023 validation.
//
// W3+W4 (spec 054): validate + length are computed ONCE per value
// (Utf16LEByteLengthView), the FR-023 check reuses that length, and the
// valid-input path appends at the exact final size (length prefix written
// up front, conversion via Utf16LEEncodeValidDirect with no re-validation
// and no oversize-resize-then-shrink). The pre-W3 flow validated twice and
// allocated a temporary std::string per value. Invalid UTF-8 (cold path)
// keeps the legacy append flow bit-for-bit.
void EncodeNVarcharFromUtf8(const char *utf8_data, size_t utf8_len, const mssql::BCPColumnMetadata &col,
							duckdb::vector<uint8_t> &buf) {
	bool valid_utf8 = false;
	const size_t utf16_byte_len = tds::encoding::Utf16LEByteLengthView(utf8_data, utf8_len, valid_utf8);

	// FR-023 (issue #91) — pre-encode length check for non-PLP nvarchar(N).
	// PLP columns are skipped (max_length sentinel 0xFFFF = nvarchar(max),
	// no client-side cap). Error wording is tested — do not change.
	if (!col.IsPLPType() && utf16_byte_len > col.max_length) {
		throw InvalidInputException(
			"MSSQL: NVARCHAR column '%s' overflow: value is %zu UCS-2 code units (%zu UTF-16LE bytes) "
			"but column max is %u code units (%u bytes)",
			col.name, utf16_byte_len / 2, utf16_byte_len, col.max_length / 2, col.max_length);
	}

	if (!valid_utf8) {
		// Cold path: legacy oversize-append flow, bit-identical to pre-W3.
		if (col.IsPLPType()) {
			AppendNVarcharPlp(buf, utf8_data, utf8_len);
		} else {
			AppendNVarcharNonPlp(buf, utf8_data, utf8_len);
		}
		return;
	}

	if (col.IsPLPType()) {
		if (utf16_byte_len == 0) {
			// Empty PLP value: header + zero terminator, no data chunk.
			AppendNVarcharPlp(buf, utf8_data, utf8_len);
			return;
		}
		const size_t start = buf.size();
		buf.resize(start + 8 + 4 + utf16_byte_len + 4);
		uint8_t *out = buf.data() + start;
		WritePlpHeader(out);
		WriteLe32(out + 8, static_cast<uint32_t>(utf16_byte_len));
		tds::encoding::Utf16LEEncodeValidDirect(utf8_data, utf8_len, out + 12);
		WriteLe32(out + 12 + utf16_byte_len, 0);
		return;
	}

	const size_t start = buf.size();
	buf.resize(start + 2 + utf16_byte_len);
	buf[start] = static_cast<uint8_t>(utf16_byte_len & 0xFF);
	buf[start + 1] = static_cast<uint8_t>((utf16_byte_len >> 8) & 0xFF);
	tds::encoding::Utf16LEEncodeValidDirect(utf8_data, utf8_len, buf.data() + start + 2);
}

//===--------------------------------------------------------------------===//
// Batch decode from staging (spec 055 D5/T10) — see the header for the scheme
// and its measurements.
//===--------------------------------------------------------------------===//

//! UTF-8 is at most 3 bytes per UTF-16 code unit: one BMP unit yields 1-3 bytes,
//! and a surrogate pair yields 4 bytes from TWO units, i.e. 2 per unit. So 3x
//! bounds every input — and 1.5x the wire bytes, since a unit is 2 bytes.
//! The upper bound is 3, NOT 2: U+0800-U+FFFF, which is all of CJK, is three
//! bytes per unit.
inline idx_t Utf8UpperBound(idx_t units) {
	return units * 3;
}

//! Average bytes one value's delimiter search covers before memchr's call
//! overhead pays for itself (D5).
//!
//! 256, not the 64 this shipped with: that number belonged to a walk that
//! skipped to each value's lower bound and so searched only the tail. The walk
//! now starts at the value itself (see WalkDelimited), which makes the run the
//! value's whole length — and at 80-112 bytes a call into memchr measured
//! WORSE than the word sweep (11.4 vs 8.2 ns/value). Past ~256 it wins clearly,
//! and by 1 KB values it is worth 28%.
static const size_t MEMCHR_THRESHOLD_BYTES = 256;

//! First 0x00 byte at or after `from`, or `limit` if there is none.
//!
//! Any zero byte in the output IS a delimiter: no UTF-8 encoding of a non-zero
//! code point contains a 0x00 byte, and U+FFFD (the invalid-input replacement)
//! is EF BF BD. So the search needs no state.
//!
//! Short runs are swept eight bytes at a time by the classic zero-byte word
//! test; long runs go to memchr, which is SIMD in libc but whose call overhead
//! only pays off once there is enough to scan. No count-trailing-zeros
//! intrinsic in the sweep: once a word is known to hold a zero its eight bytes
//! are checked directly, which keeps this portable to MSVC.
//!
//! Which one wins is a property of the COLUMN — how far its values sit from
//! their delimiter — so it is a template parameter resolved once by the caller,
//! not a test per value. Testing the distance to the end of the blob instead
//! answers a different question entirely: the blob is the whole column, tens of
//! kilobytes of it, so that test picks memchr for every value of every column.
template <bool MEMCHR>
inline size_t FindDelimiter(const char *data, size_t from, size_t limit) {
	if (MEMCHR) {
		const void *hit = std::memchr(data + from, 0, limit - from);
		return hit ? static_cast<size_t>(static_cast<const char *>(hit) - data) : limit;
	}
	size_t i = from;
	while (i + 8 <= limit) {
		uint64_t word;
		std::memcpy(&word, data + i, 8);
		if (((word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL) != 0) {
			for (size_t k = i; k < i + 8; k++) {
				if (data[k] == 0) {
					return k;
				}
			}
		}
		i += 8;
	}
	while (i < limit && data[i] != 0) {
		i++;
	}
	return i;
}

//! Convert a payload that failed validation, in bulk runs, with standard U+FFFD
//! substitution — the D2 resume loop, writing into `dst` instead of a string.
//!
//! Unpaired surrogates are legal in a UCS-2 collation, so this is real data
//! rather than corruption. Each maximal valid run is converted with one bulk
//! call; only the offending code unit is replaced and skipped. There is no
//! per-value step: the delimiters staged between values survive, because
//! U+FFFD contains no zero byte and a U+0000 unit is valid input that is never
//! the thing being replaced.
size_t ConvertReplacingInvalid(const char16_t *src, size_t units, char *dst, idx_t &replaced) {
	static const char REPLACEMENT[] = "\xEF\xBF\xBD";
	size_t in = 0;
	size_t out = 0;
	while (in < units) {
		const simdutf::result status = simdutf::validate_utf16le_with_errors(src + in, units - in);
		const bool ok = status.error == simdutf::error_code::SUCCESS;
		const size_t valid_units = ok ? units - in : status.count;
		if (valid_units > 0) {
			out += simdutf::convert_valid_utf16le_to_utf8(src + in, valid_units, dst + out);
		}
		if (ok) {
			return out;
		}
		std::memcpy(dst + out, REPLACEMENT, 3);
		out += 3;
		in += valid_units + 1;
		replaced++;
	}
	return out;
}

//! Length with trailing 0x20 bytes removed. Applying the fixed-length NCHAR trim
//! to the OUTPUT is exactly equivalent to trimming the UTF-16 input — a 0x20
//! byte is never a UTF-8 continuation byte — and unlike an input-side trim it
//! stays correct when the payload is invalid UTF-16 and gets U+FFFD
//! substitution, which is why the batch path can cover NCHAR at all.
inline uint32_t TrimTrailingUtf8Spaces(const char *data, uint32_t len) {
	while (len > 0 && data[len - 1] == ' ') {
		len--;
	}
	return len;
}

//! Store one value, trimmed or not. TRIM is a template parameter so the branch
//! resolves per column, not per value.
template <bool TRIM>
inline void StoreValue(string_t *result, idx_t row, const char *data, uint32_t len) {
	result[row] = string_t(data, TRIM ? TrimTrailingUtf8Spaces(data, len) : len);
}

//! Re-split a column in which some value contains a U+0000 of its own.
//!
//! Such a unit produces a zero byte that is indistinguishable from a separator,
//! so the plain boundary walk assigns every subsequent value wrongly. A longer
//! delimiter would not help — two consecutive NULs in the data defeat a two-unit
//! separator equally.
//!
//! Nothing is re-converted: the blob is already correct, only the boundary
//! assignment was wrong. The k-th zero BYTE in the output is produced by the
//! k-th zero UNIT in the input, in order and one for one, and the input's value
//! boundaries are known exactly from the staged offsets. So counting a value's
//! own zero units tells the walk how many zero bytes to step over before the one
//! that is really its delimiter.
//!
//! Both cursors only ever move forward, so this is two linear passes over the
//! column — one across the input units, one across the output bytes — not a
//! per-value scan.
//!
//! Always the sweep: a column that reaches here has already had every value's
//! delimiter located once, so the runs here are the same short ones.
template <bool TRIM>
void SplitWithEmbeddedNuls(const staging::ColumnStaging &st, idx_t count, const char16_t *src, const char *blob,
						   size_t written, string_t *result) {
	size_t in_unit = 0;
	size_t out_pos = 0;
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		// Every zero unit from here through this value's own delimiter.
		const size_t delimiter_unit = st.offsets[row] / 2 + st.lengths[row] / 2;
		size_t zeros = 0;
		while (in_unit <= delimiter_unit) {
			zeros += src[in_unit] == 0 ? 1 : 0;
			in_unit++;
		}
		size_t pos = out_pos;
		for (size_t k = 0; k < zeros; k++) {
			pos = FindDelimiter<false>(blob, pos, written);
			if (k + 1 < zeros) {
				pos++;
			}
		}
		StoreValue<TRIM>(result, row, blob + out_pos, static_cast<uint32_t>(pos - out_pos));
		out_pos = pos + 1;
	}
}

//! The boundary walk for a column that is not pure ASCII.
//!
//! Every byte is examined, in order, and each value takes the next zero byte as
//! its delimiter. That is what makes the caller's end-position check EXACT, and
//! it is the whole reason this does not skip.
//!
//! It used to skip to the value's own unit count first — a legal lower bound,
//! since every UTF-16 unit yields at least one UTF-8 byte, and worth 20-50% on
//! this kernel. It was also unsound. A value ending in U+0000 has its own zero
//! sitting at exactly that lower bound, so the search takes it for the
//! delimiter; the REAL delimiter one byte later then falls inside the next
//! value's skipped region and is jumped clean over. One zero per value is still
//! consumed, the walk still lands exactly on `written`, and two strings come out
//! silently wrong — which is what shipped, and what the spec-059 tests caught.
//!
//! Every unconsumed zero has to end up somewhere the walk can see, and without a
//! skip the only place left is past the end. Keeping the skip means proving no
//! value carries a U+0000, and every way of proving that costs more than the
//! skip saves: counting the output's zeros is +55-90%, and detecting them while
//! staging is +1.5-4.1 ns on every value of every string column, pure ASCII
//! included.
//!
//! Returns where the walk ended, which the caller checks against `written`.
template <bool TRIM, bool MEMCHR>
size_t WalkDelimited(const staging::ColumnStaging &st, idx_t count, const char *blob, size_t written,
					 string_t *result) {
	size_t offset = 0;
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		const size_t delimiter = FindDelimiter<MEMCHR>(blob, offset, written);
		StoreValue<TRIM>(result, row, blob + offset, static_cast<uint32_t>(delimiter - offset));
		offset = delimiter + 1;
	}
	return offset;
}

template <bool TRIM>
void DecodeChunkImpl(const staging::ColumnStaging &st, idx_t count, Vector &out) {
	string_t *result = FlatVector::GetData<string_t>(out);
	const idx_t units = st.PayloadSize() / 2;

	if (units == 0) {
		// All-NULL or all-empty. Measured as the one cell where the batch scheme
		// is not faster, so it exits before allocating anything.
		static const char EMPTY[1] = {0};
		for (idx_t row = 0; row < count; row++) {
			if (st.IsValid(row)) {
				result[row] = string_t(EMPTY, 0);
			}
		}
		return;
	}

	// One allocation for the whole column at the provable worst case. Untouched
	// tail pages are never faulted, so over-reserving costs address space rather
	// than time, and it buys a single-pass conversion. The waste is transient: it
	// lives in the chunk's string heap and is released with the chunk.
	//
	// When the bound is at most 12 bytes EmptyString inlines its storage into
	// this local. That stays correct: every value is then shorter than the inline
	// threshold too, so each string_t below copies rather than retains a pointer.
	string_t blob_slot = StringVector::EmptyString(out, Utf8UpperBound(units));
	char *const blob = blob_slot.GetDataWriteable();
	const char16_t *const src = reinterpret_cast<const char16_t *>(st.buffer.data());

	// The checked converter validates AND converts in the same pass and returns
	// the byte count. That is what removes the length pass rather than merely
	// speeding it up: measured on its own, utf8_length_from_utf16le costs about
	// twice the conversion, so "measure exactly, allocate exactly, convert"
	// spends two thirds of its time learning a number the converter returns
	// anyway. It returns 0 on invalid input, which is also how invalidity is
	// detected without a separate validation pass.
	size_t written = simdutf::convert_utf16le_to_utf8(src, units, blob);
	if (written == 0) {
		written = ConvertReplacingInvalid(src, units, blob, st.replaced_units);
	} else if (written == units) {
		// Every code unit produced exactly one byte, so the column is pure ASCII
		// and a value's output offset is its staged byte offset halved. The
		// verdict is free — a comparison of two numbers already in hand — and no
		// boundary search runs at all.
		for (idx_t row = 0; row < count; row++) {
			if (!st.IsValid(row)) {
				continue;
			}
			StoreValue<TRIM>(result, row, blob + st.offsets[row] / 2, st.lengths[row] / 2);
		}
		st.boundary = staging::BoundaryStrategy::AsciiOffsets;
		return;
	}

	// Multi-byte characters are present, so an output offset no longer follows
	// from its input offset. The conversion was still ONE call; the U+0000 unit
	// staged after each value separates the values in the output too.
	//
	// Each search covers one value, start to delimiter, so the average run is
	// simply the output divided by the values that produced it — one division to
	// pick the search for the whole column.
	//
	// It used to divide (written - units) instead, the length of the tail a
	// skipping search would have covered. With the skip gone that measured the
	// wrong thing, and badly: mostly-ASCII text expands by a fraction of a byte
	// per unit, so a column of 4 KB values scored ~0.2 and swept 4 KB a word at a
	// time instead of calling memchr once. Correcting the metric is worth 25-28%
	// on values of a kilobyte and up.
	//
	// `values` cannot be zero: reaching here means units > 0, which means some
	// row staged a payload, which means some row was not NULL.
	const idx_t values = count - st.null_count;
	const bool use_memchr = written / values >= MEMCHR_THRESHOLD_BYTES;
	st.boundary = use_memchr ? staging::BoundaryStrategy::Memchr : staging::BoundaryStrategy::Sweep;
	const size_t offset = use_memchr ? WalkDelimited<TRIM, true>(st, count, blob, written, result)
									 : WalkDelimited<TRIM, false>(st, count, blob, written, result);

	// The walk consumes the first `values` zero bytes of the output, in order and
	// without skipping any. So it ends on the last byte if and only if the output
	// holds exactly one zero per value — that is, if and only if no value carried
	// a U+0000 of its own. One comparison, and it is exact.
	//
	// It is exact only because the walk does not skip; see WalkDelimited for the
	// case that made the same comparison lie when it did.
	if (offset != written) {
		SplitWithEmbeddedNuls<TRIM>(st, count, src, blob, written, result);
		st.boundary = staging::BoundaryStrategy::EmbeddedNul;
	}
}

}  // namespace

size_t Utf16ByteLength(const std::string &utf8) {
	return tds::encoding::Utf16LEByteLength(utf8);
}

std::string EscapeSqlSingleQuotes(const std::string &str) {
	std::string result;
	result.reserve(str.size() + 4);
	for (char c : str) {
		result += c;
		if (c == '\'') {
			result += '\'';
		}
	}
	return result;
}

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row) {
	// R1 (spec 054): decode straight into the vector's string slot — no
	// intermediate std::string. For VALID UTF-16 the trailing-space trim for
	// fixed-length CHAR/NCHAR happens on the INPUT (a trailing U+0020 is a
	// trailing 0x0020 code unit / 0x20 byte, and a space can never be part of
	// a surrogate pair — so the trim neither changes validity nor differs
	// from the old trim-the-UTF-8-output behaviour). Invalid UTF-16 (e.g.
	// unpaired surrogates, legal in UCS-2 collations) must go through the
	// legacy decoder with the UNTRIMMED payload + output-side trim, exactly
	// as every pre-054 release did.
	const bool trim_trailing_spaces = col.type_id == tds::TDS_TYPE_BIGCHAR || col.type_id == tds::TDS_TYPE_NCHAR;

	// NTEXT belongs here too: its payload is UTF-16 exactly like NVARCHAR, only
	// its ROW framing differs (a text pointer, spec 055 D9). It was missing while
	// nothing called this entry point for it — the legacy path could not read
	// NTEXT at all before #197 — and the omission surfaced the moment the spec-056
	// constant path decoded one value through here and got the bytes read as
	// single-byte characters.
	if (col.type_id == tds::TDS_TYPE_NCHAR || col.type_id == tds::TDS_TYPE_NVARCHAR ||
		col.type_id == tds::TDS_TYPE_XML || col.type_id == tds::TDS_TYPE_NTEXT) {
		size_t byte_len = bytes.size();
		if (trim_trailing_spaces) {
			while (byte_len >= 2 && bytes[byte_len - 2] == 0x20 && bytes[byte_len - 1] == 0x00) {
				byte_len -= 2;
			}
		}
		const size_t utf8_len = tds::encoding::Utf8LengthFromUtf16LEView(bytes.data(), byte_len);
		if (utf8_len == SIZE_MAX) {
			// Invalid UTF-16 — legacy per-value fallback on the FULL payload,
			// then trim the decoded output (pre-054 semantics bit-for-bit).
			std::string str = tds::encoding::Utf16LEDecode(bytes.data(), bytes.size());
			if (trim_trailing_spaces) {
				while (!str.empty() && str.back() == ' ') {
					str.pop_back();
				}
			}
			FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, str);
			return;
		}
		string_t slot = StringVector::EmptyString(out, utf8_len);
		tds::encoding::Utf16LEDecodeValidInto(bytes.data(), byte_len, slot.GetDataWriteable());
		slot.Finalize();
		FlatVector::GetData<string_t>(out)[row] = slot;
		return;
	}

	// CHAR/VARCHAR are single-byte (collation-dependent in theory; in
	// practice the test fixtures and the pre-spec-045 path treated the
	// bytes as UTF-8 / CP1252 indistinguishably for ASCII).
	size_t len = bytes.size();
	if (trim_trailing_spaces) {
		while (len > 0 && bytes[len - 1] == 0x20) {
			len--;
		}
	}
	FlatVector::GetData<string_t>(out)[row] =
		StringVector::AddString(out, reinterpret_cast<const char *>(bytes.data()), len);
}

void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col,
							Vector &out) {
	// Fixed-length NCHAR is padded by SQL Server and the padding is stripped.
	// One instantiation per column, so the row loops carry no test for it.
	if (col.type_id == tds::TDS_TYPE_NCHAR) {
		DecodeChunkImpl<true>(st, count, out);
	} else {
		DecodeChunkImpl<false>(st, count, out);
	}
}

void EncodeToBcp(Vector &in, const UnifiedVectorFormat &fmt, idx_t row, const mssql::BCPColumnMetadata &col,
				 duckdb::vector<uint8_t> &buf) {
	(void)in;
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto str_val = FormatValue<string_t>(fmt, row);
		EncodeNVarcharFromUtf8(str_val.GetData(), str_val.GetSize(), col, buf);
		return;
	}
	case LogicalTypeId::INTERVAL: {
		// Render interval as canonical T-SQL-safe string before encoding.
		// New behaviour for INTERVAL columns (FR-026 — DDL routes to
		// NVARCHAR(50), encode routes to the canonical string form).
		auto iv = FormatValue<interval_t>(fmt, row);
		auto str = Interval::ToString(iv);
		EncodeNVarcharFromUtf8(str.c_str(), str.size(), col, buf);
		return;
	}
	default:
		throw NotImplementedException("codec::string::EncodeToBcp: unsupported type %s", col.duckdb_type.ToString());
	}
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	EncodeToBcpViaFormat(EncodeToBcp, in, row, col, buf);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto str_val = value.ToString();
		EncodeNVarcharFromUtf8(str_val.data(), str_val.size(), col, buf);
		return;
	}
	case LogicalTypeId::INTERVAL: {
		auto iv = value.GetValue<interval_t>();
		auto str = Interval::ToString(iv);
		EncodeNVarcharFromUtf8(str.c_str(), str.size(), col, buf);
		return;
	}
	default:
		throw NotImplementedException("codec::string::EncodeToBcp: unsupported type %s", col.duckdb_type.ToString());
	}
}

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx) {
	(void)ctx;	// VARCHAR / INTERVAL render identically in both contexts.
	if (v.IsNull()) {
		return "NULL";
	}
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return "N'" + EscapeSqlSingleQuotes(v.ToString()) + "'";
	case LogicalTypeId::INTERVAL: {
		auto iv = v.GetValue<interval_t>();
		return "N'" + EscapeSqlSingleQuotes(Interval::ToString(iv)) + "'";
	}
	default:
		throw NotImplementedException("codec::string::FormatSqlLiteral: unsupported type %s", type.ToString());
	}
}

std::string FormatDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, DdlContext ctx) {
	(void)ctx;	// String DDL is identical in both DdlContext values (FR-027 / FR-028).
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return cfg.text_type == mssql::CTASTextType::VARCHAR ? "VARCHAR(MAX)" : "NVARCHAR(MAX)";
	case LogicalTypeId::INTERVAL:
		// FR-026 — canonical DuckDB interval strings fit comfortably in 50
		// chars (e.g. "9999 years 11 months 30 days 23:59:59.999999" — 44
		// chars). Both CreateTable and CtasCreateTable agree on this shape;
		// pre-spec-045 the former returned NVARCHAR(100) and the latter
		// threw NotImplementedException.
		return "NVARCHAR(50)";
	default:
		throw NotImplementedException("codec::string::FormatDdlTypeName: unsupported type %s", type.ToString());
	}
}

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::INTERVAL:
		// Wrapper overhead only — `N''` (3 bytes) plus a small margin. The
		// caller (MSSQLValueSerializer::EstimateSerializedSize) adds the
		// value-aware `2 * GetString().size()` term to cover the worst-case
		// single-quote-doubling escape factor.
		return 4;
	default:
		throw NotImplementedException("codec::string::EstimateLiteralSize: unsupported type %s", type.ToString());
	}
}

}  // namespace string
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

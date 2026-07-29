//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/string_finalize.cpp — spec 055 D5.
//===----------------------------------------------------------------------===//

#include "codec/staging/string_finalize.hpp"

#include "mssql_compat.hpp"
#include "tds/encoding/utf16.hpp"

#include <simdutf.h>

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

namespace {

//! UTF-8 is at most 3 bytes per UTF-16 code unit: one BMP unit yields 1-3 bytes,
//! and a surrogate pair yields 4 bytes from TWO units, i.e. 2 per unit. So 3x
//! bounds every input — and 1.5x the wire bytes, since a unit is 2 bytes.
//! The upper bound is 3, NOT 2: U+0800-U+FFFF, which is all of CJK, is three
//! bytes per unit.
inline idx_t Utf8UpperBound(idx_t units) {
	return units * 3;
}

//! Run length beyond which memchr's call overhead pays for itself (D5).
static const size_t MEMCHR_THRESHOLD_BYTES = 64;

//! First 0x00 byte at or after `from`, or `limit` if there is none.
//!
//! Any zero byte in the output IS a delimiter: no UTF-8 encoding of a non-zero
//! code point contains a 0x00 byte, and U+FFFD (the invalid-input replacement)
//! is EF BF BD. So the search needs no state.
//!
//! Short runs are swept eight bytes at a time by the classic zero-byte word
//! test; long runs go to memchr, which is SIMD in libc but whose call overhead
//! only pays off once there is enough to scan. No count-trailing-zeros
//! intrinsic: once a word is known to hold a zero its eight bytes are checked
//! directly, which keeps this portable to MSVC.
inline size_t FindDelimiter(const char *data, size_t from, size_t limit) {
	if (limit - from >= MEMCHR_THRESHOLD_BYTES) {
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
size_t ConvertReplacingInvalid(const char16_t *src, size_t units, char *dst) {
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
	}
	return out;
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
void SplitWithEmbeddedNuls(const ColumnStaging &st, idx_t count, const char16_t *src, const char *blob, size_t written,
						   string_t *result) {
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
			pos = FindDelimiter(blob, pos, written);
			if (k + 1 < zeros) {
				pos++;
			}
		}
		result[row] = string_t(blob + out_pos, static_cast<uint32_t>(pos - out_pos));
		out_pos = pos + 1;
	}
}

}  // namespace

void FinalizeStringColumn(const ColumnStaging &st, idx_t count, Vector &out) {
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
		written = ConvertReplacingInvalid(src, units, blob);
	} else if (written == units) {
		// Every code unit produced exactly one byte, so the column is pure ASCII
		// and a value's output offset is its staged byte offset halved. The
		// verdict is free — a comparison of two numbers already in hand — and no
		// boundary search runs at all.
		for (idx_t row = 0; row < count; row++) {
			if (!st.IsValid(row)) {
				continue;
			}
			result[row] = string_t(blob + st.offsets[row] / 2, st.lengths[row] / 2);
		}
		return;
	}

	// Multi-byte characters are present, so an output offset no longer follows
	// from its input offset. The conversion was still ONE call; the U+0000 unit
	// staged after each value separates the values in the output too.
	//
	// The search starts at the value's own unit count, never before it: every
	// UTF-16 unit yields at least one UTF-8 byte, so a value's output cannot be
	// shorter than its input in units, and the first zero byte at or after that
	// point is provably this value's delimiter.
	//
	// Only the LOWER bound may be used to skip. Jumping to a guessed position
	// such as twice the unit count is a correctness bug, not a missed
	// optimization: a mixed-script value whose real length falls between u and 2u
	// would land on a LATER value's delimiter and silently merge two strings.
	// Every fixture here is single-script, so such a bug passes the whole matrix.
	size_t offset = 0;
	for (idx_t row = 0; row < count; row++) {
		if (!st.IsValid(row)) {
			continue;
		}
		const size_t delimiter = FindDelimiter(blob, offset + st.lengths[row] / 2, written);
		result[row] = string_t(blob + offset, static_cast<uint32_t>(delimiter - offset));
		offset = delimiter + 1;
	}

	// Exactly one delimiter per staged value means the walk lands exactly on the
	// end of the output. Anything else means a value contained a U+0000 of its
	// own, which shifts every boundary after it. This check is exact and costs
	// one comparison per chunk — as opposed to scanning each value for a zero
	// unit while staging it, which would put a pass over the payload on the hot
	// path to catch a case that then still has to be handled here.
	if (offset != written) {
		SplitWithEmbeddedNuls(st, count, src, blob, written, result);
	}
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

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

	if (col.type_id == tds::TDS_TYPE_NCHAR || col.type_id == tds::TDS_TYPE_NVARCHAR ||
		col.type_id == tds::TDS_TYPE_XML) {
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

//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/string_codec.cpp
//
// String family implementation. See codec/string_codec.hpp for behaviour
// parity notes and the FR-023 issue #91 length-validation contract.
//===----------------------------------------------------------------------===//

#include "codec/string_codec.hpp"

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

template <typename T>
T GetVectorValue(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	mssql_compat::ToUnifiedFormat(vec, 1, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

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

// Append a PLP-framed nvarchar(max) payload to `buf`. Mirrors
// BCPRowEncoder::EncodeNVarcharPLP bit-for-bit.
void AppendNVarcharPlp(duckdb::vector<uint8_t> &buf, const char *input, size_t input_len) {
	constexpr uint64_t UNKNOWN_PLP_LEN = 0xFFFFFFFFFFFFFFFEULL;

	if (input_len == 0) {
		for (int i = 0; i < 8; i++) {
			buf.push_back(static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF));
		}
		buf.push_back(0x00);
		buf.push_back(0x00);
		buf.push_back(0x00);
		buf.push_back(0x00);
		return;
	}

	size_t start_pos = buf.size();
	size_t max_utf16_len = input_len * 2;
	buf.resize(start_pos + 8 + 4 + max_utf16_len + 4);

	uint8_t *out = buf.data() + start_pos;
	for (int i = 0; i < 8; i++) {
		out[i] = static_cast<uint8_t>((UNKNOWN_PLP_LEN >> (i * 8)) & 0xFF);
	}
	out += 8;

	uint8_t *chunk_len_ptr = out;
	out += 4;
	size_t utf16_len = tds::encoding::Utf16LEEncodeDirect(input, input_len, out);
	out += utf16_len;

	uint32_t chunk_len = static_cast<uint32_t>(utf16_len);
	chunk_len_ptr[0] = static_cast<uint8_t>(chunk_len & 0xFF);
	chunk_len_ptr[1] = static_cast<uint8_t>((chunk_len >> 8) & 0xFF);
	chunk_len_ptr[2] = static_cast<uint8_t>((chunk_len >> 16) & 0xFF);
	chunk_len_ptr[3] = static_cast<uint8_t>((chunk_len >> 24) & 0xFF);

	out[0] = 0x00;
	out[1] = 0x00;
	out[2] = 0x00;
	out[3] = 0x00;

	buf.resize(start_pos + 8 + 4 + utf16_len + 4);
}

// FR-023 (issue #91) — pre-encode length check for non-PLP nvarchar(N).
// `utf16_byte_len = Utf16LEByteLength(utf8)`; throws a clear, column-named
// error when the value would not fit. PLP columns are skipped (the
// max_length sentinel 0xFFFF means nvarchar(max), no client-side cap).
void ValidateNVarcharLength(const char *utf8_data, size_t utf8_len, const mssql::BCPColumnMetadata &col) {
	if (col.IsPLPType()) {
		return;
	}
	// Compute the byte count without allocating an output buffer. simdutf-
	// backed via tds::encoding::Utf16LEByteLength (handles invalid input by
	// falling back to the legacy hand-rolled counter — same contract as
	// Utf16LEEncodeDirect).
	std::string tmp(utf8_data, utf8_len);
	size_t utf16_byte_len = tds::encoding::Utf16LEByteLength(tmp);
	if (utf16_byte_len > col.max_length) {
		throw InvalidInputException(
			"MSSQL: NVARCHAR column '%s' overflow: value is %zu UCS-2 code units (%zu UTF-16LE bytes) "
			"but column max is %u code units (%u bytes)",
			col.name, utf16_byte_len / 2, utf16_byte_len, col.max_length / 2, col.max_length);
	}
}

// Shared encode body taking already-resolved UTF-8 string view. Picks the
// PLP / non-PLP path based on col, after running FR-023 validation.
void EncodeNVarcharFromUtf8(const char *utf8_data, size_t utf8_len, const mssql::BCPColumnMetadata &col,
							duckdb::vector<uint8_t> &buf) {
	ValidateNVarcharLength(utf8_data, utf8_len, col);
	if (col.IsPLPType()) {
		AppendNVarcharPlp(buf, utf8_data, utf8_len);
	} else {
		AppendNVarcharNonPlp(buf, utf8_data, utf8_len);
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
	std::string str;

	if (col.type_id == tds::TDS_TYPE_NCHAR || col.type_id == tds::TDS_TYPE_NVARCHAR ||
		col.type_id == tds::TDS_TYPE_XML) {
		str = tds::encoding::Utf16LEDecode(bytes.data(), bytes.size());
	} else {
		// CHAR/VARCHAR are single-byte (collation-dependent in theory; in
		// practice the test fixtures and the pre-spec-045 path treated the
		// bytes as UTF-8 / CP1252 indistinguishably for ASCII).
		str = std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	}

	// Trim trailing spaces for fixed-length CHAR/NCHAR (matches
	// TypeConverter::ConvertString bit-for-bit).
	if (col.type_id == tds::TDS_TYPE_BIGCHAR || col.type_id == tds::TDS_TYPE_NCHAR) {
		size_t end = str.find_last_not_of(' ');
		if (end != std::string::npos) {
			str.erase(end + 1);
		} else {
			str.clear();
		}
	}

	mssql_compat::GetDataMutable<string_t>(out)[row] = StringVector::AddString(out, str);
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata &col, duckdb::vector<uint8_t> &buf) {
	switch (col.duckdb_type.id()) {
	case LogicalTypeId::VARCHAR: {
		auto str_val = GetVectorValue<string_t>(in, row);
		EncodeNVarcharFromUtf8(str_val.GetData(), str_val.GetSize(), col, buf);
		return;
	}
	case LogicalTypeId::INTERVAL: {
		// Render interval as canonical T-SQL-safe string before encoding.
		// New behaviour for INTERVAL columns (FR-026 — DDL routes to
		// NVARCHAR(50), encode routes to the canonical string form).
		auto iv = GetVectorValue<interval_t>(in, row);
		auto str = Interval::ToString(iv);
		EncodeNVarcharFromUtf8(str.c_str(), str.size(), col, buf);
		return;
	}
	default:
		throw NotImplementedException("codec::string::EncodeToBcp: unsupported type %s", col.duckdb_type.ToString());
	}
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

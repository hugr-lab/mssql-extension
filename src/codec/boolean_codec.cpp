//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/boolean_codec.cpp
//
// Boolean family implementation. See codec/boolean_codec.hpp.
//
// Behavior parity (vs pre-spec-045 baseline):
//   - DecodeFromTds mirrors TypeConverter::ConvertBoolean
//     (`!bytes.empty() && bytes[0] != 0`).
//   - EncodeToBcp mirrors BCPRowEncoder::EncodeBit (1-byte length prefix
//     0x01 + 1-byte value 0x00 or 0x01).
//   - FormatSqlLiteral produces "1" for true, "0" for false; identical in
//     LiteralContext::Filter and LiteralContext::InsertValues (FR-020 (b)).
//   - FormatDdlTypeName returns "BIT" — byte-identical in DdlContext::CreateTable
//     and DdlContext::CtasCreateTable (FR-027 / FR-028). cfg.text_type is
//     irrelevant for Boolean.
//===----------------------------------------------------------------------===//

#include "codec/boolean_codec.hpp"

#include "duckdb/common/exception.hpp"
#include "mssql_compat.hpp"

#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {
namespace boolean {

namespace {

bool GetVectorBool(Vector &vec, idx_t row_idx) {
	UnifiedVectorFormat format;
	mssql_compat::ToUnifiedFormat(vec, 1, format);
	auto data = UnifiedVectorFormat::GetData<bool>(format);
	auto idx = format.sel->get_index(row_idx);
	return data[idx];
}

}  // namespace

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata & /*col*/, Vector &out, idx_t row) {
	bool b = !bytes.empty() && bytes[0] != 0;
	mssql_compat::GetDataMutable<bool>(out)[row] = b;
}

void EncodeToBcp(Vector &in, idx_t row, const mssql::BCPColumnMetadata & /*col*/, duckdb::vector<uint8_t> &buf) {
	bool v = GetVectorBool(in, row);
	buf.push_back(1);
	buf.push_back(v ? 0x01 : 0x00);
}

void EncodeToBcp(const Value &value, const mssql::BCPColumnMetadata & /*col*/, duckdb::vector<uint8_t> &buf) {
	bool v = value.GetValue<bool>();
	buf.push_back(1);
	buf.push_back(v ? 0x01 : 0x00);
}

std::string FormatSqlLiteral(const Value &v, const LogicalType & /*type*/, LiteralContext /*ctx*/) {
	return v.GetValue<bool>() ? "1" : "0";
}

std::string FormatDdlTypeName(const LogicalType & /*type*/, const mssql::CTASConfig & /*cfg*/, DdlContext /*ctx*/) {
	return "BIT";
}

size_t EstimateLiteralSize(const LogicalType & /*type*/) {
	return 1;  // "0" or "1"
}

}  // namespace boolean
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

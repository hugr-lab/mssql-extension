//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/money_codec.cpp
//
// Money family implementation — **scan-decode only**. See codec/money_codec.hpp.
//
// Behaviour parity (vs pre-spec-045 baseline):
//   - DecodeFromTds mirrors TypeConverter::ConvertMoney —
//       bytes.size() == 8 → DecimalEncoding::ConvertMoney → DECIMAL(19,4)
//         (physical INT128 storage; the upstream caller allocates a hugeint vector)
//       bytes.size() == 4 → DecimalEncoding::ConvertSmallMoney → DECIMAL(10,4)
//         (physical INT64 storage; the lower 64 bits of the returned
//         hugeint_t hold the scaled int32, sign-extended)
//       anything else → InvalidInputException (matches legacy message format).
//
// EncodeToBcp / FormatSqlLiteral / FormatDdlTypeName are declared in the
// header for interface uniformity but **deliberately undefined**. SQL
// Server MONEY values become DuckDB DECIMAL(19,4) at decode time, so all
// non-decode operations are handled by the Decimal codec. Attempting to
// call any of the three undefined entry points produces a linker error
// (the "scan-decode-only" fence per data-model.md).
//===----------------------------------------------------------------------===//

#include "codec/money_codec.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "mssql_compat.hpp"
#include "tds/encoding/decimal_encoding.hpp"

#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {
namespace money {

void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata & /*col*/, Vector &out, idx_t row) {
	if (bytes.size() == 8) {
		// MONEY → DECIMAL(19,4), hugeint storage (precision > 18).
		hugeint_t int_value = tds::encoding::DecimalEncoding::ConvertMoney(bytes.data());
		mssql_compat::GetDataMutable<hugeint_t>(out)[row] = int_value;
	} else if (bytes.size() == 4) {
		// SMALLMONEY → DECIMAL(10,4), int64 storage. ConvertSmallMoney returns a
		// hugeint_t whose .lower carries the sign-extended int32 in its low 64 bits.
		hugeint_t int_value = tds::encoding::DecimalEncoding::ConvertSmallMoney(bytes.data());
		mssql_compat::GetDataMutable<int64_t>(out)[row] = static_cast<int64_t>(int_value.lower);
	} else {
		throw InvalidInputException("Invalid MONEY length: %d", bytes.size());
	}
}

}  // namespace money
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

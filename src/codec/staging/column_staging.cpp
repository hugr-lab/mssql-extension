//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/column_staging.cpp
//
// Out-of-line parts of the column staging structures (spec 055 D3).
// Everything on the per-value path lives in the header so it inlines; only the
// error path is here, deliberately, to keep it out of the append body.
//===----------------------------------------------------------------------===//

#include "codec/staging/column_staging.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

void ColumnStaging::ThrowPayloadOverflow(idx_t offset, uint32_t length) {
	throw InvalidInputException(
		"MSSQL: staged column payload would reach %llu bytes (offset %llu + length %u), past the %llu-byte cap. "
		"This indicates a corrupt length prefix on the wire rather than legitimate data.",
		static_cast<unsigned long long>(offset) + length, static_cast<unsigned long long>(offset), length,
		static_cast<unsigned long long>(MAX_STAGING_PAYLOAD_BYTES));
}

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

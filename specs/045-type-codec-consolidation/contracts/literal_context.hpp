//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/literal_context.hpp  (CONTRACT — to be placed at src/include/codec/)
//
// Distinguishes the two contexts in which T-SQL literal text is
// formatted:
//   - Filter:       T-SQL WHERE-clause literal (filter pushdown)
//   - InsertValues: T-SQL INSERT (...) VALUES (...) body literal
//
// Family modules dispatch on this enum ONLY where the two contexts
// produce genuinely different output. Documented divergence cases (see
// data-model.md "Divergence catalog"):
//
//   HUGEINT       (post-spec-045: both contexts unify to decimal)
//   DECIMAL       (post-spec-045: both contexts use PhysicalType-aware path)
//   TIMESTAMP_TZ  (both contexts pass offset 0 explicitly)
//   BLOB          (verify during Binary family migration)
//   UUID          (verify during Uuid family migration)
//
// For all other types, family modules SHOULD accept the parameter but
// mark it [[maybe_unused]] (or (void)ctx;) to make the no-divergence
// intent explicit.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {

enum class LiteralContext : uint8_t {
	Filter,
	InsertValues,
};

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

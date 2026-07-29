//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/uuid_finalize.hpp
//
// Batch materialization of a staged UNIQUEIDENTIFIER column (spec 055 D6).
//
// SQL Server sends a GUID mixed-endian — Data1/2/3 little-endian, Data4 as-is —
// and DuckDB stores a UUID as a sortable 128-bit integer. The conversion is
// pure arithmetic on a 16-byte value, with no branch and no allocation, so the
// batch form is a straight loop over the staged column.
//
// It runs over EVERY row, NULL ones included. The kernel is total over any byte
// pattern (it is four loads and some shifts), so a NULL row's stale slot yields
// a value the validity mask discards — and the loop stays branch-free.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

void FinalizeUuidColumn(const ColumnStaging &st, idx_t count, Vector &out);

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

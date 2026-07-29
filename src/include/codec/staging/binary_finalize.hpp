//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/staging/binary_finalize.hpp
//
// Batch materialization of a staged VARBINARY column (spec 055 D6).
//
// The simplest family there is: the wire bytes ARE the value, so there is no
// conversion at all — only the question of where the bytes live. The shipped
// path answers it per value, with StringVector::AddStringOrBlob allocating in
// the vector's string heap and copying into it once per row. A whole column
// needs exactly one allocation and one copy.
//
// No delimiter is involved, unlike the UTF-16 case: the staged offsets already
// describe the output exactly, because the output IS the input.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {
namespace mssql {
namespace codec {
namespace staging {

//! Publish a staged VARBINARY column into `out`.
//!
//! NULL rows are left untouched — their offset/length slots are undefined by
//! ColumnStaging's contract, and validity is published separately.
void FinalizeBinaryColumn(const ColumnStaging &st, idx_t count, Vector &out);

}  // namespace staging
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

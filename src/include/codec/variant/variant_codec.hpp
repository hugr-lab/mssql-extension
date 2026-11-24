//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/variant/variant_codec.hpp
//
// Spec 093 T1 (D3): a SQL_VARIANT column written into a DuckDB VARIANT vector. The envelope and the
// value's VARIANT payload are variant_envelope's (DuckDB-free); this is the vector side, through
// DuckDB's VariantBuilder (duckdb/common/types/variant/variant_builder.hpp): one BuildVariant call a
// column a chunk, a value's payload built in the builder's row blob.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/tds_column_metadata.hpp"

#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace variant {

//! What a column's values needed beyond their decode, for the stream's WARNING (once a column).
struct VariantColumnNotes {
	//! char / varchar values whose collation this build has no code page for, read as their bytes.
	idx_t unknown_collation = 0;
	//! char / varchar values under a UTF-8 collation whose bytes are not UTF-8, read as their bytes.
	idx_t invalid_utf8 = 0;
};

//! The staged envelopes of `count` rows (`st`: Var staging, NULL rows invalid) into `out`, a fresh flat
//! VARIANT vector. An envelope that is not a sql_variant is an InvalidInputException naming the column.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out,
							VariantColumnNotes *notes);

//! One envelope (`bytes`, non-empty) into `out` at `row` (the per-value path), counted into `notes` as the
//! chunk path counts.
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row,
				   VariantColumnNotes *notes);

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

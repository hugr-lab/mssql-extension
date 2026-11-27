//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/json/json_codec.hpp
//
// Spec 086 PR 6: a json column in its stored binary form (ColumnMetadata::json_disk_form: a backup's record;
// the server sends the text over the wire, J1) into the vector it is read as -- JSON, the server's own text
// (json_binary's RenderJsonText), or a VARIANT built in one pass the way DuckDB's JSON -> VARIANT builds it
// from that text (its number types, its sorted, unescaped keys), so the two paths give one VARIANT. The
// staged kernel (FinalizeKernel::Json, a chunk's column) and the per-value form, as every family's. A value
// that is no json document is NULL, counted for the caller's WARNING -- never an error. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/staging/column_staging.hpp"
#include "duckdb/common/types/vector.hpp"
#include "tds/tds_column_metadata.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace json {

//! A json column's values read as NULL because their stored bytes are no json document, and the first's
//! reason (for the WARNING, once a column).
struct JsonColumnNotes {
	idx_t malformed = 0;
	std::string first_malformed;
};

//! The staged values of `count` rows (`st`: Var staging, NULL rows invalid) into `out`, a fresh flat vector of
//! JSON (each value's server text) or VARIANT, as its type says.
void DecodeChunkFromStaging(const staging::ColumnStaging &st, idx_t count, const tds::ColumnMetadata &col, Vector &out,
							JsonColumnNotes *notes);

//! One value (`bytes`, its stored form) into `out` at `row` (the per-value path), counted into `notes` as the
//! chunk path counts.
void DecodeFromTds(const std::vector<uint8_t> &bytes, const tds::ColumnMetadata &col, Vector &out, idx_t row,
				   JsonColumnNotes *notes);

}  // namespace json
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

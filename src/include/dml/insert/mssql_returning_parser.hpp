#pragma once

#include <vector>
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/mssql_dml_execute.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// MSSQLReturningParser - Collects the RETURNING rows of an INSERT batch into a
// DuckDB DataChunk
//
// mssql::ExecuteDmlBatch reads the response and hands this the result sets
// (spec 080 acceptance 7: it used to read the socket itself, with a copy of
// the DML token loop of its own). Only the batch's LAST result set holds the
// RETURNING rows:
// - the read of the OUTPUT ... INTO table on SQL Server (issue #471), which
//   comes after the INSERT; the DROP after it sends no rows;
// - the bare OUTPUT elsewhere, the only set there, since an enabled trigger
//   refuses that form (334).
// A trigger may send result sets of its own before it ('disallow results from
// triggers' defaults to 0). So the rows are kept as they arrive and decoded
// only when the response is complete.
//
// Usage:
//   MSSQLReturningParser output(target, returning_column_ids);
//   mssql::DmlBatchOptions options;
//   output.Attach(options);
//   ... mssql::ExecuteDmlBatch(context, connection, sql, options) ...
//   auto chunk = output.TakeResult(context, warned);
//===----------------------------------------------------------------------===//

//! Which decode WARNINGs (spec 093 T1 / T5: a sql_variant value read as its bytes, a geometry /
//! geography value read as NULL) each RETURNING column has logged, by result index. Kept by the
//! statement (MSSQLInsertExecutor) across its batches, so each is said once a column a statement, as a
//! scan says it.
struct MSSQLReturningWarned {
	std::vector<uint8_t> spatial;  // mssql::SPATIAL_WARNED_* bits
	std::vector<bool> variant;
};

class MSSQLReturningParser {
public:
	// @param target Insert target with column metadata
	// @param returning_column_ids Column indices to return (maps to OUTPUT INSERTED columns)
	// @param client_transcode The catalog's answer (spec 092 D4); no default, as on MSSQLResultStream
	// @param geometry_as The catalog's answer (spec 093 T5); no default, likewise
	MSSQLReturningParser(const MSSQLInsertTarget &target, const vector<idx_t> &returning_column_ids,
						 bool client_transcode, tds::GeometryAs geometry_as);

	//! Route the result sets of the statement `options` runs into this parser.
	//! The parser must outlive that call.
	void Attach(mssql::DmlBatchOptions &options);

	//! Rows in the last result set received
	idx_t GetRowCount() const {
		return rows_.size();
	}

	//! The rows of the last result set, decoded, or nullptr when there were
	//! none. Throws when that set is not the RETURNING list's shape. A value read as NULL or as its
	//! bytes is said by the result stream's WARNING (mssql::LogSpatialNotes / LogVariantNotes) on
	//! `context`, unless `warned` says the statement has said it.
	unique_ptr<DataChunk> TakeResult(ClientContext &context, MSSQLReturningWarned &warned);

	// Get the DuckDB types for the result columns
	const vector<LogicalType> &GetResultTypes() const {
		return result_types_;
	}

private:
	// Target metadata
	const MSSQLInsertTarget &target_;

	// Column indices to return
	vector<idx_t> returning_column_ids_;

	bool client_transcode_;
	tds::GeometryAs geometry_as_;

	// Result types (derived from returning columns)
	vector<LogicalType> result_types_;

	// The last result set received: its columns and its rows, undecoded
	std::vector<tds::ColumnMetadata> columns_;
	std::vector<tds::RowData> rows_;
};

}  // namespace duckdb

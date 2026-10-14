#pragma once

#include <memory>
#include <vector>
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/mssql_dml_execute.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLReturningParser - Collects OUTPUT INSERTED rows into a DuckDB DataChunk
//
// The rows of an INSERT ... OUTPUT INSERTED batch, for the RETURNING clause.
// mssql::ExecuteDmlBatch reads the response; this receives its ROW tokens
// (spec 080 acceptance 7: it used to read the socket with a copy of the DML
// token loop of its own).
//
// Usage:
//   MSSQLReturningParser output(target, returning_column_ids);
//   mssql::DmlBatchOptions options;
//   output.Attach(options);
//   ... mssql::ExecuteDmlBatch(context, connection, sql, options) ...
//   auto chunk = output.TakeResult();
//===----------------------------------------------------------------------===//

class MSSQLReturningParser {
public:
	// @param target Insert target with column metadata
	// @param returning_column_ids Column indices to return (maps to OUTPUT INSERTED columns)
	MSSQLReturningParser(const MSSQLInsertTarget &target, const vector<idx_t> &returning_column_ids);

	//! Route the ROW tokens of the statement `options` runs into this parser.
	//! The parser must outlive that call.
	void Attach(mssql::DmlBatchOptions &options);

	//! Rows received so far
	idx_t GetRowCount() const {
		return row_count_;
	}

	//! The rows received, or nullptr when there were none.
	unique_ptr<DataChunk> TakeResult();

	// Get the DuckDB types for the result columns
	const vector<LogicalType> &GetResultTypes() const {
		return result_types_;
	}

private:
	// Convert one OUTPUT row into the chunk
	void ProcessRow(const tds::RowData &row, const std::vector<tds::ColumnMetadata> &columns);

	// Target metadata
	const MSSQLInsertTarget &target_;

	// Column indices to return
	vector<idx_t> returning_column_ids_;

	// Result types (derived from returning columns)
	vector<LogicalType> result_types_;

	unique_ptr<DataChunk> chunk_;
	idx_t row_count_ = 0;
};

}  // namespace duckdb

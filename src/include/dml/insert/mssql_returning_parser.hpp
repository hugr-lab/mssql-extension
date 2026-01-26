#pragma once

#include <memory>
#include <vector>
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLReturningParser - Parses OUTPUT INSERTED results into DuckDB DataChunk
//
// This class handles the TDS response from an INSERT ... OUTPUT INSERTED query
// and converts the result rows into a DuckDB DataChunk for the RETURNING clause.
//
// The parser:
// 1. Reads TDS packets from the connection
// 2. Parses COLMETADATA and ROW tokens
// 3. Converts values using TypeConverter
// 4. Fills a DataChunk with the results
//
// Usage:
//   MSSQLReturningParser parser(target, returning_column_ids);
//   auto chunk = parser.Parse(connection, socket);
//===----------------------------------------------------------------------===//

class MSSQLReturningParser {
public:
	// Constructor
	// @param target Insert target with column metadata
	// @param returning_column_ids Column indices to return (maps to OUTPUT INSERTED columns)
	MSSQLReturningParser(const MSSQLInsertTarget &target, const vector<idx_t> &returning_column_ids);

	//===----------------------------------------------------------------------===//
	// Parsing
	//===----------------------------------------------------------------------===//

	// Parse TDS response stream into DataChunk
	// @param connection TDS connection (for state management)
	// @param parser TokenParser with buffered data (may have partial data)
	// @param socket TDS socket for reading more data
	// @param timeout_ms Read timeout in milliseconds
	// @return DataChunk containing OUTPUT INSERTED results, or nullptr if empty
	unique_ptr<DataChunk> Parse(tds::TdsConnection &connection, tds::TokenParser &parser, tds::TdsSocket &socket,
								int timeout_ms = 30000);

	// Parse from a fresh connection (sends no query, just reads response)
	// Use this after ExecuteBatch() has been called
	// @param connection TDS connection with pending response
	// @param timeout_ms Read timeout in milliseconds
	// @return DataChunk containing OUTPUT INSERTED results, or nullptr if empty
	unique_ptr<DataChunk> ParseResponse(tds::TdsConnection &connection, int timeout_ms = 30000);

	//===----------------------------------------------------------------------===//
	// Result Information
	//===----------------------------------------------------------------------===//

	// Get number of rows parsed
	idx_t GetRowCount() const {
		return row_count_;
	}

	// Check if parsing encountered errors
	bool HasError() const {
		return !error_message_.empty();
	}

	// Get error message (if any)
	const string &GetErrorMessage() const {
		return error_message_;
	}

	// Get SQL Server error number (if any)
	uint32_t GetErrorNumber() const {
		return error_number_;
	}

	// Get the DuckDB types for the result columns
	const vector<LogicalType> &GetResultTypes() const {
		return result_types_;
	}

private:
	// Initialize result chunk with proper types
	unique_ptr<DataChunk> InitializeResultChunk();

	// Process a single row from parser into the chunk
	void ProcessRow(const tds::RowData &row, const std::vector<tds::ColumnMetadata> &columns, DataChunk &chunk,
					idx_t row_idx);

	// Target metadata
	const MSSQLInsertTarget &target_;

	// Column indices to return
	vector<idx_t> returning_column_ids_;

	// Result types (derived from returning columns)
	vector<LogicalType> result_types_;

	// Parsing state
	idx_t row_count_ = 0;
	string error_message_;
	uint32_t error_number_ = 0;
};

}  // namespace duckdb

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/string_util.hpp"
#include <string>
#include <vector>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertError - Error context when an INSERT batch fails
//
// Provides detailed error information including:
// - Which batch failed (statement index)
// - Which rows were in the failed batch (row range)
// - SQL Server error details (error number, message, SQLSTATE)
//===----------------------------------------------------------------------===//

struct MSSQLInsertError {
	// Batch identification
	idx_t statement_index;     // Batch number (0-based)
	idx_t row_offset_start;    // First row in failed batch (0-based)
	idx_t row_offset_end;      // Last row in failed batch (exclusive)

	// SQL Server error details
	int32_t sql_error_number;  // SQL Server error number (e.g., 2627 for PK violation)
	string sql_error_message;  // SQL Server error text
	string sql_state;          // SQLSTATE code if available

	// Default constructor
	MSSQLInsertError()
	    : statement_index(0), row_offset_start(0), row_offset_end(0),
	      sql_error_number(0) {}

	// Constructor with all fields
	MSSQLInsertError(idx_t stmt_idx, idx_t start, idx_t end,
	                 int32_t error_num, const string &error_msg, const string &state = "")
	    : statement_index(stmt_idx), row_offset_start(start), row_offset_end(end),
	      sql_error_number(error_num), sql_error_message(error_msg), sql_state(state) {}

	//===----------------------------------------------------------------------===//
	// Message Formatting
	//===----------------------------------------------------------------------===//

	// Format error message for display
	// Returns: "INSERT failed at statement N (rows X-Y): [error_num] message"
	string FormatMessage() const {
		return StringUtil::Format(
		    "INSERT failed at statement %d (rows %d-%d): [%d] %s",
		    statement_index,
		    row_offset_start,
		    row_offset_end > 0 ? row_offset_end - 1 : 0,
		    sql_error_number,
		    sql_error_message);
	}
};

//===----------------------------------------------------------------------===//
// MSSQLInsertResult - Result of a batch execution
//
// Contains success status, row count, and optional returned data.
//===----------------------------------------------------------------------===//

struct MSSQLInsertResult {
	// Execution status
	bool success;

	// Rows affected (from TDS DONE token)
	idx_t rows_affected;

	// Error details (valid when success == false)
	MSSQLInsertError error;

	// Default constructor (success case)
	MSSQLInsertResult()
	    : success(true), rows_affected(0) {}

	// Constructor for success case
	explicit MSSQLInsertResult(idx_t rows)
	    : success(true), rows_affected(rows) {}

	// Constructor for failure case
	explicit MSSQLInsertResult(const MSSQLInsertError &err)
	    : success(false), rows_affected(0), error(err) {}
};

//===----------------------------------------------------------------------===//
// MSSQLInsertStatistics - Execution metrics for INSERT operations
//
// Tracks timing and batch counts for observability and tuning.
//===----------------------------------------------------------------------===//

struct MSSQLInsertStatistics {
	// Row counts
	idx_t total_rows_inserted;
	idx_t total_batches_executed;

	// Timing (microseconds)
	uint64_t total_execution_time_us;
	uint64_t total_serialization_time_us;

	// Batch sizing
	idx_t min_batch_size;
	idx_t max_batch_size;
	idx_t avg_batch_size;

	// SQL sizing
	idx_t min_sql_bytes;
	idx_t max_sql_bytes;

	// Default constructor
	MSSQLInsertStatistics()
	    : total_rows_inserted(0), total_batches_executed(0),
	      total_execution_time_us(0), total_serialization_time_us(0),
	      min_batch_size(0), max_batch_size(0), avg_batch_size(0),
	      min_sql_bytes(0), max_sql_bytes(0) {}

	// Update statistics with batch info
	void RecordBatch(idx_t row_count, idx_t sql_bytes, uint64_t execution_time_us) {
		total_rows_inserted += row_count;
		total_batches_executed++;
		total_execution_time_us += execution_time_us;

		if (total_batches_executed == 1) {
			min_batch_size = max_batch_size = row_count;
			min_sql_bytes = max_sql_bytes = sql_bytes;
		} else {
			min_batch_size = std::min(min_batch_size, row_count);
			max_batch_size = std::max(max_batch_size, row_count);
			min_sql_bytes = std::min(min_sql_bytes, sql_bytes);
			max_sql_bytes = std::max(max_sql_bytes, sql_bytes);
		}

		// Rolling average
		avg_batch_size = total_rows_inserted / total_batches_executed;
	}

	// Get rows per second (0 if no time recorded)
	double GetRowsPerSecond() const {
		if (total_execution_time_us == 0) return 0.0;
		return static_cast<double>(total_rows_inserted) * 1000000.0 /
		       static_cast<double>(total_execution_time_us);
	}
};

}  // namespace duckdb

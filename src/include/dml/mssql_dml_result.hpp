#pragma once

#include <string>
#include "duckdb/common/types.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLDMLResult - Result from executing a DML batch
//
// Contains execution status, row count, and error information.
// Used by both UPDATE and DELETE operations.
//===----------------------------------------------------------------------===//

struct MSSQLDMLResult {
	//===----------------------------------------------------------------------===//
	// Execution Status
	//===----------------------------------------------------------------------===//

	// True if batch executed successfully
	bool success = false;

	// Number of rows affected (from @@ROWCOUNT)
	idx_t rows_affected = 0;

	//===----------------------------------------------------------------------===//
	// Error Information (only valid if !success)
	//===----------------------------------------------------------------------===//

	// SQL Server error message
	string error_message;

	// SQL Server error number
	int error_code = 0;

	// Which batch failed (1-based)
	idx_t batch_number = 0;

	//===----------------------------------------------------------------------===//
	// Factory Methods
	//===----------------------------------------------------------------------===//

	// Create success result
	static MSSQLDMLResult Success(idx_t rows_affected, idx_t batch_number = 0) {
		MSSQLDMLResult result;
		result.success = true;
		result.rows_affected = rows_affected;
		result.batch_number = batch_number;
		return result;
	}

	// Create failure result
	static MSSQLDMLResult Failure(const string &error_message, int error_code = 0, idx_t batch_number = 0) {
		MSSQLDMLResult result;
		result.success = false;
		result.error_message = error_message;
		result.error_code = error_code;
		result.batch_number = batch_number;
		return result;
	}

	//===----------------------------------------------------------------------===//
	// Error Message Formatting
	//===----------------------------------------------------------------------===//

	// Format error message for user display
	// Format: "MSSQL {operation} failed: batch {N}: {message}"
	string FormatError(const string &operation, idx_t total_batches = 0) const {
		string result = "MSSQL " + operation + " failed";
		if (batch_number > 0) {
			result += ": batch " + std::to_string(batch_number);
			if (total_batches > 0) {
				result += " of " + std::to_string(total_batches);
			}
		}
		if (!error_message.empty()) {
			result += ": " + error_message;
		}
		return result;
	}
};

}  // namespace duckdb

#pragma once

#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLDMLBatch - Represents a batch of rows ready for DML execution
//
// A batch contains:
// - Metadata for tracking and error reporting
// - Generated SQL statement with parameters
// - Parameter values and types for binding
//
// Used by both UPDATE and DELETE operations.
//===----------------------------------------------------------------------===//

struct MSSQLDMLBatch {
	//===----------------------------------------------------------------------===//
	// Batch Metadata
	//===----------------------------------------------------------------------===//

	// Sequential batch ID (1-based) for error reporting
	idx_t batch_number = 0;

	// Number of rows in this batch
	idx_t row_count = 0;

	//===----------------------------------------------------------------------===//
	// Generated SQL
	//===----------------------------------------------------------------------===//

	// Complete parameterized SQL statement
	// For UPDATE: UPDATE t SET ... FROM [schema].[table] t JOIN (VALUES...) v ON ...
	// For DELETE: DELETE t FROM [schema].[table] t JOIN (VALUES...) v ON ...
	string sql;

	//===----------------------------------------------------------------------===//
	// Parameters
	//===----------------------------------------------------------------------===//

	// Flattened parameter values for binding
	// Order depends on operation type:
	// - UPDATE: [pk1_r1, pk2_r1, v1_r1, v2_r1, pk1_r2, pk2_r2, v1_r2, v2_r2, ...]
	// - DELETE: [pk1_r1, pk2_r1, pk1_r2, pk2_r2, ...]
	vector<Value> parameters;

	// Parameter types (for TDS binding)
	// Matches 1:1 with parameters vector
	vector<LogicalType> parameter_types;

	//===----------------------------------------------------------------------===//
	// Validation
	//===----------------------------------------------------------------------===//

	// Check if batch is valid and ready for execution
	bool IsValid() const {
		return row_count > 0 && !sql.empty() && parameters.size() == parameter_types.size();
	}

	// Clear batch for reuse
	void Clear() {
		batch_number = 0;
		row_count = 0;
		sql.clear();
		parameters.clear();
		parameter_types.clear();
	}
};

}  // namespace duckdb

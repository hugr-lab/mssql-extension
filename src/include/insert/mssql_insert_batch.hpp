#pragma once

#include "duckdb/common/types.hpp"
#include <string>
#include <cstddef>

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertBatch - A batch of rows to be inserted as a single SQL statement
//
// Represents a complete INSERT statement ready for execution.
// Tracks row range for error reporting and state for observability.
//===----------------------------------------------------------------------===//

struct MSSQLInsertBatch {
	//===----------------------------------------------------------------------===//
	// Batch State
	//===----------------------------------------------------------------------===//

	enum class State {
		BUILDING,     // Accumulating rows
		READY,        // SQL generated, ready to execute
		EXECUTING,    // Sent to SQL Server
		COMPLETED,    // Successfully executed
		FAILED        // Execution failed
	};

	//===----------------------------------------------------------------------===//
	// Batch Data
	//===----------------------------------------------------------------------===//

	// Row range in the overall INSERT operation (for error reporting)
	idx_t row_offset_start;    // First row index (0-based)
	idx_t row_offset_end;      // Last row index (exclusive)

	// Row count in this batch
	idx_t row_count;

	// Generated SQL statement
	string sql_statement;

	// Size of SQL statement in bytes
	idx_t sql_bytes;

	// Current state
	State state;

	//===----------------------------------------------------------------------===//
	// Constructors
	//===----------------------------------------------------------------------===//

	// Default constructor
	MSSQLInsertBatch()
	    : row_offset_start(0), row_offset_end(0), row_count(0),
	      sql_bytes(0), state(State::BUILDING) {}

	// Constructor with row range
	MSSQLInsertBatch(idx_t start, idx_t end)
	    : row_offset_start(start), row_offset_end(end),
	      row_count(end - start), sql_bytes(0), state(State::BUILDING) {}

	//===----------------------------------------------------------------------===//
	// State Helpers
	//===----------------------------------------------------------------------===//

	bool IsBuilding() const { return state == State::BUILDING; }
	bool IsReady() const { return state == State::READY; }
	bool IsExecuting() const { return state == State::EXECUTING; }
	bool IsCompleted() const { return state == State::COMPLETED; }
	bool IsFailed() const { return state == State::FAILED; }

	// Get state as string (for logging/debugging)
	const char *GetStateString() const {
		switch (state) {
		case State::BUILDING: return "BUILDING";
		case State::READY: return "READY";
		case State::EXECUTING: return "EXECUTING";
		case State::COMPLETED: return "COMPLETED";
		case State::FAILED: return "FAILED";
		default: return "UNKNOWN";
		}
	}
};

}  // namespace duckdb

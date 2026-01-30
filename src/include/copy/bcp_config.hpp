#pragma once

#include <algorithm>
#include <cstddef>
#include "duckdb/common/types.hpp"

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// Default Values for COPY/BCP Settings
//===----------------------------------------------------------------------===//

// Default batch size (rows per local buffer batch)
constexpr idx_t MSSQL_DEFAULT_COPY_BATCH_ROWS = 10000;

// Default maximum batch size in bytes (32MB)
constexpr idx_t MSSQL_DEFAULT_COPY_MAX_BATCH_BYTES = 33554432;

// Minimum allowed max_batch_bytes (1MB)
constexpr idx_t MSSQL_MIN_COPY_BATCH_BYTES = 1048576;

// Default flush threshold (rows before sending to SQL Server and committing)
// This controls memory usage on both DuckDB and SQL Server sides
// 0 = no intermediate flushes (send all at end) - WARNING: high memory usage
// Default 100K rows keeps buffer around 10-50MB depending on row size
constexpr idx_t MSSQL_DEFAULT_COPY_FLUSH_ROWS = 100000;  // 100K rows

namespace mssql {

//===----------------------------------------------------------------------===//
// BCPCopyConfig - Configuration for COPY TO operations via BulkLoadBCP
//
// Loaded from DuckDB settings at runtime via LoadBCPCopyConfig().
// Controls batching behavior and table creation options.
//===----------------------------------------------------------------------===//

struct BCPCopyConfig {
	// Create table if it doesn't exist
	bool create_table = true;

	// Drop and recreate table if it exists
	bool overwrite = false;

	// Maximum rows per local buffer batch (before transferring to writer)
	idx_t batch_rows = MSSQL_DEFAULT_COPY_BATCH_ROWS;

	// Maximum bytes per local buffer batch
	idx_t max_batch_bytes = MSSQL_DEFAULT_COPY_MAX_BATCH_BYTES;

	// Rows before flushing to SQL Server (0 = flush only at end)
	// This controls memory usage on SQL Server - data is buffered until flush
	// Recommended: 100K-1M rows depending on row size
	idx_t flush_rows = MSSQL_DEFAULT_COPY_FLUSH_ROWS;

	// Use TABLOCK hint for INSERT BULK (enables table-level locking)
	// Improves performance by 15-30% for bulk loads by:
	// - Reducing lock overhead (table lock vs row locks)
	// - Enabling minimal logging in simple/bulk-logged recovery
	// - Allowing more parallel server-side processing
	// WARNING: Blocks other writers during COPY
	bool tablock = true;

	//===----------------------------------------------------------------------===//
	// Derived Values
	//===----------------------------------------------------------------------===//

	// Check if batch should be flushed based on row count
	bool ShouldFlushByRows(idx_t current_rows) const {
		return current_rows >= batch_rows;
	}

	// Check if batch should be flushed based on byte count
	bool ShouldFlushByBytes(idx_t current_bytes) const {
		return current_bytes >= max_batch_bytes;
	}

	// Check if data should be flushed to SQL Server
	// Returns true when accumulated rows reach flush_rows threshold
	bool ShouldFlushToServer(idx_t accumulated_rows) const {
		return flush_rows > 0 && accumulated_rows >= flush_rows;
	}

	//===----------------------------------------------------------------------===//
	// Validation
	//===----------------------------------------------------------------------===//

	// Validate configuration values
	// @throws InvalidInputException if values are out of range
	void Validate() const;
};

//===----------------------------------------------------------------------===//
// Configuration Loading
//===----------------------------------------------------------------------===//

// Load BCP COPY configuration from DuckDB settings
// @param context The client context to read settings from
// @return Validated BCPCopyConfig
BCPCopyConfig LoadBCPCopyConfig(ClientContext &context);

}  // namespace mssql
}  // namespace duckdb

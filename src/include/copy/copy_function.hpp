#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "copy/bcp_config.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/copy_function.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class ExtensionLoader;

namespace tds {
class TdsConnection;
}  // namespace tds

//===----------------------------------------------------------------------===//
// MSSQLCopyBindData - Bind-phase data for COPY TO MSSQL
//
// Captured during BCPCopyBind() and passed to all subsequent callbacks.
// Contains target resolution, configuration, and source schema info.
//===----------------------------------------------------------------------===//

struct MSSQLCopyBindData : public TableFunctionData {
	// Resolved target table
	mssql::BCPCopyTarget target;

	// COPY configuration (create_table, overwrite, batch sizes)
	mssql::BCPCopyConfig config;

	// Source query types (for column metadata generation)
	vector<LogicalType> source_types;

	// Source query column names
	vector<string> source_names;

	// Catalog name for connection provider lookup
	string catalog_name;

	// Target table column metadata (populated when copying to existing table)
	// When non-empty, these are used for COLMETADATA instead of source_types
	vector<mssql::BCPColumnMetadata> target_columns;

	// Flag indicating we're copying to an existing table (not creating new)
	bool use_target_types = false;

	// Column mapping: mapping[target_idx] = source_idx, or -1 if source doesn't have this column
	// Used when copying to existing table with name-based column matching
	vector<int32_t> column_mapping;

	// Flag indicating column mapping is needed (source columns differ from target)
	bool use_column_mapping = false;
};

//===----------------------------------------------------------------------===//
// MSSQLCopyGlobalState - Global state for COPY TO MSSQL
//
// Shared across all parallel Sink operations. Owns the connection
// and BCPWriter, tracks progress.
//===----------------------------------------------------------------------===//

struct MSSQLCopyGlobalState : public GlobalFunctionData {
	// Pinned TDS connection for BulkLoad operations
	std::shared_ptr<tds::TdsConnection> connection;

	// BCP packet writer (thread-safe)
	unique_ptr<mssql::BCPWriter> writer;

	// Column metadata for encoding
	vector<mssql::BCPColumnMetadata> columns;

	// Column mapping: mapping[target_idx] = source_idx, or -1 if source doesn't have this column
	// When non-empty, BCPWriter uses this to map source data to target columns
	vector<int32_t> column_mapping;

	// Progress tracking
	std::atomic<idx_t> rows_sent{0};		// Total rows sent to writer
	std::atomic<idx_t> bytes_sent{0};		// Total bytes sent
	std::atomic<idx_t> rows_confirmed{0};	// Total rows confirmed by SQL Server (across all batches)
	std::atomic<idx_t> batches_flushed{0};	// Number of batches flushed to server

	// Total rows expected (for progress reporting, 0 if unknown)
	idx_t total_rows_expected = 0;

	// INSERT BULK SQL (cached for re-execution on flush)
	string insert_bulk_sql;

	// Write synchronization
	std::mutex write_mutex;

	// Error state
	string error_message;
	bool has_error = false;
};

//===----------------------------------------------------------------------===//
// MSSQLCopyLocalState - Per-thread local state for COPY TO MSSQL
//
// Minimal state - we write directly to BCPWriter without local buffering
// to minimize memory usage.
//===----------------------------------------------------------------------===//

struct MSSQLCopyLocalState : public LocalFunctionData {
	// No local buffering needed - writes go directly to BCPWriter
};

//===----------------------------------------------------------------------===//
// MSSQL Copy Function Registration
//
// Registers the 'bcp' format CopyFunction with DuckDB.
//===----------------------------------------------------------------------===//

// Register MSSQL COPY functions with the database
// @param loader Extension loader for function registration
void RegisterMSSQLCopyFunctions(ExtensionLoader &loader);

//===----------------------------------------------------------------------===//
// CopyFunction Callbacks
//
// Implementation of DuckDB's CopyFunction interface for 'bcp' format.
//===----------------------------------------------------------------------===//

namespace mssql {

// Bind callback: Parse target URL/catalog, resolve options
// @param context Client context
// @param info Copy info with target path and options
// @param names Column names from source query
// @param sql_types Column types from source query
// @return Bind data for subsequent callbacks
unique_ptr<FunctionData> BCPCopyBind(ClientContext &context, CopyFunctionBindInput &input, const vector<string> &names,
									 const vector<LogicalType> &sql_types);

// InitGlobal callback: Acquire connection, send INSERT BULK, start BCP
// @param context Client context
// @param operator_state Operator state (unused)
// @param bind_data Bind data from BCPCopyBind
// @return Global state for Sink operations
unique_ptr<GlobalFunctionData> BCPCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
												 const string &file_path);

// InitLocal callback: Create per-thread buffer
// @param context Execution context
// @param bind_data Bind data from BCPCopyBind
// @param gstate Global state from BCPCopyInitGlobal
// @return Local state for this thread's Sink operations
unique_ptr<LocalFunctionData> BCPCopyInitLocal(ExecutionContext &context, FunctionData &bind_data);

// Sink callback: Accumulate rows and flush batches
// @param context Execution context
// @param bind_data Bind data from BCPCopyBind
// @param gstate Global state
// @param lstate Local state for this thread
// @param input DataChunk to process
void BCPCopySink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
				 LocalFunctionData &lstate, DataChunk &input);

// Combine callback: Flush remaining local buffer
// @param context Execution context
// @param bind_data Bind data from BCPCopyBind
// @param gstate Global state
// @param lstate Local state to flush
void BCPCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
					LocalFunctionData &lstate);

// Finalize callback: Send DONE token, read response
// @param context Client context
// @param bind_data Bind data from BCPCopyBind
// @param gstate Global state
void BCPCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate);

// GetProgress callback: Report copy progress
// @param context Client context
// @param bind_data Bind data from BCPCopyBind
// @param gstate Global state
// @return Progress percentage (0.0 to 1.0)
CopyFunctionExecutionMode BCPCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index);

}  // namespace mssql
}  // namespace duckdb

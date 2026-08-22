#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "copy/bcp_config.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/bulk_load_session.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/copy_function.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class ExtensionLoader;

namespace tds {
class TdsConnection;
class ConnectionPool;
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
	// Last-resort connection release (issue #191).
	//
	// BCPCopyInitGlobal and BCPCopyFinalize each release `connection` on the paths they own, and
	// both reset() it, so this destructor is a no-op whenever either ran. It exists for the path
	// neither covers: a throw from the sink (e.g. ValidateNVarcharLength rejecting an over-long
	// value). DuckDB does not call copy_to_finalize on an errored COPY, so nothing released the
	// connection — the pool kept it checked out in Executing state with the INSERT BULK
	// transaction open, holding locks on the target table until the pool was torn down at process
	// exit.
	//
	// Follows MSSQLResultStream's destructor contract (issue #178 / PR #179): touch NO
	// ClientContext here — it can run on a worker thread while the client thread commits the
	// query's transaction. The release targets below are captured on the client thread in
	// BCPCopyInitGlobal instead.
	~MSSQLCopyGlobalState();

	// Pinned TDS connection for BulkLoad operations
	std::shared_ptr<tds::TdsConnection> connection;

	// Release targets captured in BCPCopyInitGlobal (see destructor note above).
	//   pool_handle        — weak_ptr to the catalog's pool; a failed lock() (catalog torn down)
	//                        degrades to dropping the connection, never a dangling dereference.
	//   transaction_pinned — true when the connection is pinned to an explicit DuckDB
	//                        transaction; the destructor then only drops its reference, since the
	//                        MSSQLTransaction owns the pin.
	weak_ptr<tds::ConnectionPool> pool_handle;
	bool transaction_pinned = false;

	//! `mssql_reset_connection`, resolved in BCPCopyInitGlobal on the client
	//! thread — the destructor below has no ClientContext (issue #178).
	bool reset_on_release = tds::DEFAULT_RESET_CONNECTION;

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

	// Spec 057 step 0b: write-path phase counters, gated on MSSQL_COUNTERS.
	//
	// These replace a per-CHUNK `CopyDebugLog(1, "BCPCopySink: DONE ...")`, which
	// fflush'ed stderr ~245 times inside the timed path on a 500k-row load and
	// inflated the client CPU it was reporting roughly 4x (0.047 s -> 0.194 s).
	// Accumulate here, print once at finalize — the shape the read path already
	// uses. Atomic because the sink may run on several threads.
	std::atomic<idx_t> counter_sink_calls{0};	 // chunks handed to the sink
	std::atomic<uint64_t> counter_sink_ns{0};	 // wall inside BCPCopySink
	std::atomic<uint64_t> counter_encode_ns{0};	 // of which: BCPWriter::WriteRows
	// Snapshotted from the BCPWriter before it is reset — the writer is destroyed
	// in BCPCopyFinalize well before the summary prints, so reading it there gave
	// zeroes. Decomposes counter_flush_ns into the half that is ours and the half
	// that is the server's.
	uint64_t counter_build_send_ns = 0;
	uint64_t counter_server_wait_ns = 0;
	idx_t counter_send_calls = 0;

	std::atomic<uint64_t> counter_flush_ns{0};	// of which: a batch boundary, END TO END —
												// build + send + the server's confirmation.
												// NOT server time alone; see PrintWriteCounters.

	// INSERT BULK SQL (cached for re-execution on flush)
	string insert_bulk_sql;

	// Write synchronization
	std::mutex write_mutex;

	// Error state. Both fields became shared when the sink turned PARALLEL in
	// spec 057 step 7.
	//
	//! Guards `error_message` ONLY. Separate from write_mutex because the failing
	//! thread does not hold that one — a local writer never takes it, and a shared
	//! writer's unique_lock is released by unwinding before the catch runs — so
	//! the string was being assigned by several threads at once while others read
	//! it to build their own exception. That is a double free, not a torn read.
	std::mutex error_mutex;
	string error_message;

	//! Read at the top of every Sink call on every thread, so it stays a plain
	//! atomic load: the lock above is taken only on the error path.
	std::atomic<bool> has_error{false};

	//===------------------------------------------------------------------===//
	// Parallel writers (spec 057 step 7)
	//
	// The bound on this path is SQL Server's INGEST rate: send() blocks because
	// the receive window stays full while the server lays rows down, not because
	// the client or the wire is slow. Nothing done on one connection moves that
	// — the server parallelises across SESSIONS, so more of them is the lever.
	//
	// Each parallel writer is an independent INSERT BULK on its own pooled
	// connection. Concurrent bulk loads into a heap take mutually compatible BU
	// locks, which is exactly the case the TABLOCK `auto` policy turns the hint
	// ON for (spec 057 step 1).
	//===------------------------------------------------------------------===//

	//! Upper bound on writers, including the global one. 1 disables the feature.
	//! Resolved once in BCPCopyInitGlobal; zero inside a transaction, where the
	//! connection is pinned to the DuckDB transaction and must not be multiplied.
	idx_t parallel_writer_limit = 1;

	//! Writers handed out so far, the global one included.
	std::atomic<idx_t> parallel_writers_used{1};
};

//===----------------------------------------------------------------------===//
// MSSQLCopyLocalState - Per-thread local state for COPY TO MSSQL
//
// Minimal state - we write directly to BCPWriter without local buffering
// to minimize memory usage.
//===----------------------------------------------------------------------===//

struct MSSQLCopyLocalState : public LocalFunctionData {
	//! This thread's own bulk-load session, or unopened when it shares the global
	//! writer (in a transaction, at the limit, or acquisition failed). Owns the
	//! connection, the writer, the batch bookkeeping and the last-resort release
	//! that issue #191 is about — see copy/bulk_load_session.hpp; CTAS holds the
	//! same type.
	mssql::BulkLoadSession session;

	//! Catalog lookup (pool + handle) is done ONCE, on the first chunk. A failure
	//! there disables claiming for this thread — it falls back to the shared
	//! writer, which is the pre-parallel behaviour and always correct.
	bool init_attempted = false;
	//! Pool + handle captured at init, so the per-chunk claim retry needs no
	//! catalog lookup. ALWAYS set once `init_attempted` is true —
	//! `MSSQLCatalog::GetConnectionPool()` returns a reference, and a failed
	//! catalog lookup throws rather than yielding null. `may_claim` is the single
	//! switch for whether claiming is allowed (transaction-pinned and limit-of-one
	//! both arrive as `parallel_writer_limit`); do not guard on this pointer
	//! instead (PR #270 review).
	tds::ConnectionPool *pool = nullptr;
	weak_ptr<tds::ConnectionPool> pool_handle;
	//! Spec 070 W2: a thread that shares the global writer keeps RE-asking for
	//! its own on later chunks, because the volume gate in TryStart may not open
	//! until the load has produced enough rows. Cleared once it wins a writer or
	//! claiming is disabled, so the steady state costs nothing.
	bool may_claim = false;
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
unique_ptr<FunctionData> BCPCopyBind(ClientContext &context, CopyFunctionBindInput &input,
									 const vector<Identifier> &names, const vector<LogicalType> &sql_types);

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

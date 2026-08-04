#pragma once

#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_executor.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

#include <chrono>
#include <memory>
#include <vector>

namespace duckdb {

class MSSQLCatalog;

namespace mssql {

//===----------------------------------------------------------------------===//
// CTAS Execution Phase
//===----------------------------------------------------------------------===//

enum class CTASPhase {
	PENDING,		   // Not started
	DDL_EXECUTING,	   // CREATE TABLE in progress
	DDL_DONE,		   // CREATE TABLE completed
	INSERT_EXECUTING,  // INSERT batches in progress (legacy mode)
	BCP_EXECUTING,	   // BCP protocol in progress (Spec 027)
	COMPLETE,		   // Successfully completed
	SKIPPED,		   // IF NOT EXISTS and table already exists (Issue #44)
	FAILED			   // Error occurred
};

//===----------------------------------------------------------------------===//
// CTASExecutionState - Global sink state for CTAS physical operator
//===----------------------------------------------------------------------===//

struct CTASExecutionState {
	// Target info
	CTASTarget target;
	vector<CTASColumnDef> columns;
	CTASConfig config;

	// Execution state
	CTASPhase phase = CTASPhase::PENDING;

	// DDL state
	string ddl_sql;

	//! Spec 060: the clustered-index statement that follows CREATE TABLE, or
	//! empty. It cannot ride inside the CREATE — SQL Server needs its own DDL.
	string post_ddl_sql;
	idx_t ddl_bytes = 0;
	int64_t ddl_time_ms = 0;

	// INSERT state (wraps existing executor, used when config.use_bcp = false)
	MSSQLInsertTarget insert_target;  // Must remain valid for insert_executor's lifetime
	MSSQLInsertConfig insert_config;  // Must remain valid for insert_executor's lifetime
	unique_ptr<MSSQLInsertExecutor> insert_executor;
	idx_t rows_produced = 0;
	idx_t rows_inserted = 0;
	int64_t insert_time_ms = 0;

	//===----------------------------------------------------------------------===//
	// BCP State (Spec 027) - used when config.use_bcp = true
	//===----------------------------------------------------------------------===//
	unique_ptr<BCPWriter> bcp_writer;
	vector<BCPColumnMetadata> bcp_columns;
	BCPCopyTarget bcp_target;
	idx_t bcp_rows_in_batch = 0;  // Rows accumulated since last flush

	//! The INSERT BULK text, built once in ExecuteBCPInsert. Every batch boundary
	//! re-executes it, and each parallel writer opens its own session with it, so
	//! it is state rather than something rebuilt at each site — it was assembled
	//! from bcp_columns in two places before, which is two places to keep in step
	//! with the TABLOCK decision that is resolved just above it.
	string insert_bulk_sql;

	//! The first bulk-load connection, taken from the pool and returned to it.
	//!
	//! Never the connection an explicit DuckDB transaction has pinned — see
	//! ExecuteBCPInsert for why CTAS deliberately loads outside the transaction.
	std::shared_ptr<tds::TdsConnection> connection;

	// Weak handle to the catalog's pool so error/teardown paths can release the
	// connection without touching the catalog pointer (issue #191 pattern from
	// MSSQLCopyGlobalState; a failed lock() means the catalog is torn down).
	weak_ptr<tds::ConnectionPool> pool_handle;

	// Catalog reference for cache invalidation
	MSSQLCatalog *catalog = nullptr;

	// Error tracking
	string error_message;
	string cleanup_error;

	// Timing
	std::chrono::steady_clock::time_point start_time;

	CTASExecutionState() = default;

	// Last-resort release of a connection left mid-BCP-stream by a sink error
	// (mirrors MSSQLCopyGlobalState::~MSSQLCopyGlobalState, issue #191).
	~CTASExecutionState();

	// Close (if mid-stream) and return the BCP connection to the pool after an
	// error. A connection abandoned mid-bulk-load must not be reused: closing
	// the socket is what rolls back the INSERT BULK transaction server-side and
	// drops the target-table locks. Safe to call repeatedly / with no connection.
	void ReleaseBCPConnectionOnError() noexcept;

	// Initialize for execution
	void Initialize(MSSQLCatalog &catalog_ref, CTASTarget target_p, vector<CTASColumnDef> columns_p,
					CTASConfig config_p);

	// Execute CREATE TABLE DDL phase
	void ExecuteDDL(ClientContext &context);

	// Execute DROP TABLE for OR REPLACE
	void ExecuteDrop(ClientContext &context);

	// Check if table exists in SQL Server
	bool TableExists(ClientContext &context);

	// Check if schema exists in SQL Server
	bool SchemaExists(ClientContext &context);

	// Flush any remaining INSERT batches (or BCP batch if in BCP mode)
	void FlushInserts(ClientContext &context);

	//===----------------------------------------------------------------------===//
	// BCP Mode Methods (Spec 027)
	//===----------------------------------------------------------------------===//

	// Initialize BCP writer and column metadata
	// Called from ExecuteDDL when config.use_bcp = true
	void InitializeBCP(ClientContext &context);

	// Build the INSERT BULK text from bcp_target / bcp_columns / config.bcp_tablock
	string BuildInsertBulkSql() const;

	// Execute INSERT BULK command to start BCP session
	void ExecuteBCPInsert(ClientContext &context);

	// Add a chunk of data in BCP mode
	// @param context Client context
	// @param chunk DataChunk to add
	void AddChunkBCP(ClientContext &context, DataChunk &chunk);

	// Flush current BCP batch and start a new one
	void FlushBCP(ClientContext &context);

	// Attempt cleanup DROP TABLE on failure
	void AttemptCleanup(ClientContext &context);

	// Invalidate catalog cache after successful DDL
	void InvalidateCache();

	// Log observability metrics
	void LogMetrics() const;

	// Get phase name for error messages
	static string GetPhaseName(CTASPhase phase);
};

//===----------------------------------------------------------------------===//
// CTASObservability - Debug output structure
//===----------------------------------------------------------------------===//

struct CTASObservability {
	string target_table;
	bool or_replace = false;

	// DDL phase metrics
	idx_t ddl_bytes = 0;
	int64_t ddl_time_ms = 0;

	// INSERT phase metrics
	idx_t rows_produced = 0;
	idx_t rows_inserted = 0;
	idx_t batches_executed = 0;
	int64_t insert_time_ms = 0;

	// Outcome
	bool success = false;
	string failure_phase;
	string error_message;

	// Emit to debug log
	void Log(int level) const;
};

}  // namespace mssql
}  // namespace duckdb

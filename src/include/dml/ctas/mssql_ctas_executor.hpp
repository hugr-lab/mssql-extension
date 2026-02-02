#pragma once

#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_executor.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "tds/tds_connection.hpp"

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

	// Connection (pinned for duration)
	std::shared_ptr<tds::TdsConnection> connection;

	// Catalog reference for cache invalidation
	MSSQLCatalog *catalog = nullptr;

	// Error tracking
	string error_message;
	string cleanup_error;

	// Timing
	std::chrono::steady_clock::time_point start_time;

	CTASExecutionState() = default;

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

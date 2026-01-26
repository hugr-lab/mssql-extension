#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "dml/mssql_dml_config.hpp"
#include "dml/mssql_dml_result.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

// Forward declarations
namespace tds {
class ConnectionPool;
}

//===----------------------------------------------------------------------===//
// MSSQLUpdateExecutor - Orchestrates UPDATE batch execution
//
// Responsibilities:
// - Accumulate rows from Sink() calls
// - Build batches when threshold reached
// - Execute batches via TDS connection
// - Track total rows updated
//===----------------------------------------------------------------------===//

class MSSQLUpdateExecutor {
public:
	//===----------------------------------------------------------------------===//
	// Construction
	//===----------------------------------------------------------------------===//

	MSSQLUpdateExecutor(ClientContext &context, const MSSQLUpdateTarget &target, const MSSQLDMLConfig &config);

	~MSSQLUpdateExecutor();

	//===----------------------------------------------------------------------===//
	// Row Processing
	//===----------------------------------------------------------------------===//

	// Process a chunk of rows from the UPDATE operator
	// Accumulates rows and executes batches as needed
	// @param chunk DataChunk with columns: [rowid, update_col1, update_col2, ...]
	// @return Total rows updated so far
	idx_t Execute(DataChunk &chunk);

	// Finalize: flush any pending batch and return total
	// @return Final result with total rows updated
	MSSQLDMLResult Finalize();

	//===----------------------------------------------------------------------===//
	// Statistics
	//===----------------------------------------------------------------------===//

	// Get total rows updated (across all batches)
	idx_t GetTotalRowsUpdated() const {
		return total_rows_updated_;
	}

	// Get number of batches executed
	idx_t GetBatchCount() const {
		return batch_count_;
	}

private:
	// Context and configuration
	ClientContext &context_;
	const MSSQLUpdateTarget &target_;
	MSSQLDMLConfig config_;

	// Connection pool (lazy initialized)
	tds::ConnectionPool *connection_pool_ = nullptr;

	// Effective batch size (computed from config and params per row)
	idx_t effective_batch_size_;

	// Accumulated PK values for pending batch [row][pk_col]
	vector<vector<Value>> pending_pk_values_;

	// Accumulated update values for pending batch [row][update_col]
	vector<vector<Value>> pending_update_values_;

	// Statistics
	idx_t total_rows_updated_ = 0;
	idx_t batch_count_ = 0;

	// Has Finalize() been called?
	bool finalized_ = false;

	// Whether to defer batch execution until Finalize
	// This is needed when in a transaction where the scan and update share the pinned connection
	bool defer_execution_ = false;

	//===----------------------------------------------------------------------===//
	// Internal Methods
	//===----------------------------------------------------------------------===//

	// Get or initialize connection pool
	tds::ConnectionPool &GetConnectionPool();

	// Flush pending batch to SQL Server
	MSSQLDMLResult FlushBatch();

	// Execute SQL batch and return rows affected
	idx_t ExecuteBatch(const string &sql);

	// Extract row data from chunk and add to pending
	void AccumulateRow(DataChunk &chunk, idx_t row_idx);
};

}  // namespace duckdb

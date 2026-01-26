#pragma once

#include <memory>
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_error.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

// Forward declarations
namespace tds {
class ConnectionPool;
class TdsConnection;
}  // namespace tds

//===----------------------------------------------------------------------===//
// MSSQLInsertExecutor - Main orchestrator for INSERT operations
//
// This class manages the complete INSERT workflow:
// 1. Receives DataChunks from DuckDB execution
// 2. Batches rows using MSSQLBatchBuilder
// 3. Executes batches via TdsConnection
// 4. Handles errors and collects statistics
//
// Usage:
//   MSSQLInsertExecutor executor(context, target, config);
//   for each chunk:
//     rows_inserted += executor.Execute(chunk);
//   executor.Finalize();  // flush remaining batch
//===----------------------------------------------------------------------===//

class MSSQLInsertExecutor {
public:
	// Constructor
	// @param context DuckDB client context
	// @param target Insert target metadata
	// @param config Insert configuration
	MSSQLInsertExecutor(ClientContext &context, const MSSQLInsertTarget &target, const MSSQLInsertConfig &config);

	~MSSQLInsertExecutor();

	//===----------------------------------------------------------------------===//
	// Execution (Mode A: Bulk Insert without RETURNING)
	//===----------------------------------------------------------------------===//

	// Execute INSERT for a batch of rows
	// @param input_chunk DataChunk with rows to insert
	// @return Number of rows successfully inserted
	// @throws MSSQLInsertException on failure
	idx_t Execute(DataChunk &input_chunk);

	//===----------------------------------------------------------------------===//
	// Execution (Mode B: Insert with RETURNING)
	//===----------------------------------------------------------------------===//

	// Execute INSERT with RETURNING (uses OUTPUT INSERTED)
	// @param input_chunk DataChunk with rows to insert
	// @param returning_column_ids Column IDs to return
	// @return DataChunk containing OUTPUT INSERTED results
	// @throws MSSQLInsertException on failure
	unique_ptr<DataChunk> ExecuteWithReturning(DataChunk &input_chunk, const vector<idx_t> &returning_column_ids);

	//===----------------------------------------------------------------------===//
	// Finalization
	//===----------------------------------------------------------------------===//

	// Finalize operation (flush any pending batch)
	// Must be called after all chunks are processed
	void Finalize();

	// Finalize with RETURNING support
	// @return Final DataChunk from remaining batch (may be nullptr if no pending rows)
	unique_ptr<DataChunk> FinalizeWithReturning();

	//===----------------------------------------------------------------------===//
	// Statistics
	//===----------------------------------------------------------------------===//

	// Get execution statistics
	const MSSQLInsertStatistics &GetStatistics() const;

	// Get total rows inserted so far
	idx_t GetTotalRowsInserted() const;

private:
	ClientContext &context_;
	const MSSQLInsertTarget &target_;
	const MSSQLInsertConfig &config_;

	// State
	bool finalized_;
	MSSQLInsertStatistics statistics_;

	// Batch builder (created on first Execute call)
	unique_ptr<class MSSQLBatchBuilder> batch_builder_;

	// Connection pool reference
	tds::ConnectionPool *connection_pool_;

	// Returning column IDs (for RETURNING mode)
	vector<idx_t> returning_column_ids_;

	// Initialize batch builder if needed
	void EnsureBatchBuilder(bool with_output);

	// Execute a single batch and return rows affected
	idx_t ExecuteBatch(const string &sql);

	// Execute a batch and parse OUTPUT results
	unique_ptr<DataChunk> ExecuteBatchWithOutput(const string &sql, const vector<idx_t> &returning_column_ids);

	// Get connection pool from catalog
	tds::ConnectionPool &GetConnectionPool();
};

//===----------------------------------------------------------------------===//
// MSSQLInsertException - Exception for INSERT failures
//===----------------------------------------------------------------------===//

class MSSQLInsertException : public Exception {
public:
	explicit MSSQLInsertException(const MSSQLInsertError &error);

	const MSSQLInsertError &GetError() const {
		return error_;
	}

private:
	MSSQLInsertError error_;
};

}  // namespace duckdb

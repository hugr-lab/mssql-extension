#pragma once

#include <memory>
#include "dml/insert/mssql_insert_batch.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_error.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/mssql_statement_connection.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

// Forward declarations
class MSSQLCatalog;
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
	//! A batch failed: the transaction is rolled back and the connection gone,
	//! so nothing pending may be sent (the destructor used to flush it).
	bool failed_ = false;
	MSSQLInsertStatistics statistics_;

	//! The statement's one connection and its server transaction (spec 062
	//! W1c, issue #344): acquired on the first batch, committed in Finalize.
	MSSQLStatementConnection stmt_conn_;

	// Batch builder (created on first Execute call)
	unique_ptr<class MSSQLBatchBuilder> batch_builder_;

	// Connection pool reference
	tds::ConnectionPool *connection_pool_;

	// Returning column IDs (for RETURNING mode)
	vector<idx_t> returning_column_ids_;

	// Initialize batch builder if needed
	void EnsureBatchBuilder(bool with_output);

	// Execute a single batch and return rows affected. Takes the batch, not
	// its text: an error names the batch's own statement index and row range,
	// which the builder has already moved past by the time it runs.
	idx_t ExecuteBatch(const MSSQLInsertBatch &batch);

	// Execute a batch and parse OUTPUT results
	unique_ptr<DataChunk> ExecuteBatchWithOutput(const MSSQLInsertBatch &batch,
												 const vector<idx_t> &returning_column_ids);

	// Get connection pool from catalog
	tds::ConnectionPool &GetConnectionPool();

	// The target's catalog.
	MSSQLCatalog &GetMSSQLCatalog();

	// A batch failed: roll the statement's transaction back, return its
	// connection, and refuse further batches.
	void FailStatement(MSSQLCatalog &catalog);

	// The last batch is in: commit the statement's transaction and return its
	// connection.
	void CommitStatement();
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

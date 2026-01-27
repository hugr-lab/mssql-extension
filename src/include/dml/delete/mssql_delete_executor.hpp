//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mssql_delete_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "dml/delete/mssql_delete_statement.hpp"
#include "dml/delete/mssql_delete_target.hpp"
#include "dml/mssql_dml_config.hpp"
#include "dml/mssql_dml_result.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//! MSSQLDeleteExecutor handles batch accumulation and execution of DELETE operations
//! Coordinates between the physical operator and the TDS layer
class MSSQLDeleteExecutor {
public:
	//! Constructor
	//! @param context The client context for database access
	//! @param target The target table metadata
	//! @param config DML configuration (batch size, etc.)
	MSSQLDeleteExecutor(ClientContext &context, const MSSQLDeleteTarget &target, const MSSQLDMLConfig &config);

	//! Destructor
	~MSSQLDeleteExecutor();

	//! Execute DELETE for a chunk of rows
	//! Accumulates rows into batches and flushes when batch is full
	//! @param chunk DataChunk containing rowid column for rows to delete
	//! @return Number of rows processed in this call
	idx_t Execute(DataChunk &chunk);

	//! Finalize execution - flush any remaining batched rows
	//! @return Result indicating success/failure and affected row count
	MSSQLDMLResult Finalize();

	//! Get total rows deleted across all batches
	idx_t GetTotalRowsDeleted() const {
		return total_rows_deleted_;
	}

	//! Get number of batches executed
	idx_t GetBatchCount() const {
		return batch_count_;
	}

private:
	//! Client context reference
	ClientContext &context_;

	//! Target table metadata
	const MSSQLDeleteTarget &target_;

	//! DML configuration
	const MSSQLDMLConfig &config_;

	//! Statement generator
	unique_ptr<MSSQLDeleteStatement> statement_;

	//! Accumulated PK values for current batch
	vector<vector<Value>> pending_pk_values_;

	//! Total rows deleted
	idx_t total_rows_deleted_ = 0;

	//! Number of batches executed
	idx_t batch_count_ = 0;

	//! Effective batch size (accounting for parameter limits)
	idx_t effective_batch_size_;

	//! Whether execution has been finalized
	bool finalized_ = false;

	//! Whether to defer batch execution until Finalize
	//! This is needed when in a transaction where the scan and delete share the pinned connection
	bool defer_execution_ = false;

	//! Flush the current batch to the database
	//! @return Result of the batch execution
	MSSQLDMLResult FlushBatch();

	//! Execute a single batch
	//! @param batch The batch to execute
	//! @return Result of the execution
	MSSQLDMLResult ExecuteBatch(const MSSQLDMLBatch &batch);
};

}  // namespace duckdb

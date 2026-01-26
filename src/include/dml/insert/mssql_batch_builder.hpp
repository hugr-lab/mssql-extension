#pragma once

#include <string>
#include <vector>
#include "dml/insert/mssql_insert_batch.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLBatchBuilder - Accumulates rows and produces batched INSERT statements
//
// This class handles the batching logic for INSERT operations:
// - Tracks row count against max_rows_per_statement limit
// - Tracks SQL byte size against max_sql_bytes limit
// - Produces MSSQLInsertBatch objects ready for execution
//
// Usage:
//   MSSQLBatchBuilder builder(target, config, false);
//   for each row in chunk:
//     if (!builder.AddRow(chunk, row_idx)) {
//       batch = builder.FlushBatch();
//       execute(batch);
//     }
//   if (builder.HasPendingRows()) {
//     batch = builder.FlushBatch();
//     execute(batch);
//   }
//===----------------------------------------------------------------------===//

class MSSQLBatchBuilder {
public:
	// Constructor
	// @param target Insert target metadata
	// @param config Insert configuration (batch size, limits)
	// @param include_output_clause Whether to include OUTPUT clause
	MSSQLBatchBuilder(const MSSQLInsertTarget &target, const MSSQLInsertConfig &config, bool include_output_clause);

	//===----------------------------------------------------------------------===//
	// Row Operations
	//===----------------------------------------------------------------------===//

	// Add a row to the current batch
	// @param chunk DataChunk containing source data
	// @param row_index Row index within the chunk
	// @return true if row was added, false if batch is full (caller should flush)
	// @throws InvalidInputException if single row exceeds max_sql_bytes
	bool AddRow(DataChunk &chunk, idx_t row_index);

	// Check if there are pending rows in the current batch
	bool HasPendingRows() const;

	// Get number of pending rows
	idx_t GetPendingRowCount() const;

	// Generate SQL for current batch and reset
	// @return Complete MSSQLInsertBatch ready for execution
	MSSQLInsertBatch FlushBatch();

	//===----------------------------------------------------------------------===//
	// Progress Tracking
	//===----------------------------------------------------------------------===//

	// Get cumulative row offset (for error reporting)
	idx_t GetCurrentRowOffset() const;

	// Get number of batches flushed so far
	idx_t GetBatchCount() const;

private:
	const MSSQLInsertTarget &target_;
	const MSSQLInsertConfig &config_;
	bool include_output_clause_;

	// Current batch state
	vector<vector<string>> row_literals_;  // Serialized values per row
	idx_t current_sql_bytes_;			   // Estimated SQL size
	idx_t pending_row_count_;			   // Rows in current batch

	// Progress tracking
	idx_t current_row_offset_;	// Global row offset
	idx_t batch_count_;			// Number of batches flushed

	// Pre-calculated values for efficiency
	idx_t base_sql_size_;  // INSERT INTO ... VALUES prefix size

	// Serialize a row from DataChunk
	// @return vector of serialized literals for each column
	// @throws InvalidInputException on serialization errors
	vector<string> SerializeRow(DataChunk &chunk, idx_t row_index);

	// Estimate SQL size for a row
	idx_t EstimateRowSize(const vector<string> &literals) const;

	// Calculate base SQL size (everything except VALUES rows)
	void CalculateBaseSQLSize();
};

}  // namespace duckdb

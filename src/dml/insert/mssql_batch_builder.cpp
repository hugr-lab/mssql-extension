#include "dml/insert/mssql_batch_builder.hpp"
#include "dml/insert/mssql_insert_statement.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

MSSQLBatchBuilder::MSSQLBatchBuilder(const MSSQLInsertTarget &target, const MSSQLInsertConfig &config,
									 bool include_output_clause)
	: target_(target),
	  config_(config),
	  include_output_clause_(include_output_clause),
	  current_sql_bytes_(0),
	  pending_row_count_(0),
	  current_row_offset_(0),
	  batch_count_(0),
	  base_sql_size_(0) {
	// Pre-allocate for typical batch sizes
	row_literals_.reserve(config.EffectiveRowsPerStatement());

	// Calculate base SQL size
	CalculateBaseSQLSize();
	current_sql_bytes_ = base_sql_size_;
}

//===----------------------------------------------------------------------===//
// Base SQL Size Calculation
//===----------------------------------------------------------------------===//

void MSSQLBatchBuilder::CalculateBaseSQLSize() {
	// Build a dummy statement to measure the prefix size
	MSSQLInsertStatement stmt(target_, include_output_clause_);

	// "INSERT INTO [schema].[table] ([col1], [col2])"
	base_sql_size_ = 12;  // "INSERT INTO "
	base_sql_size_ += stmt.GetTableName().size();
	base_sql_size_ += 2;  // " ("
	base_sql_size_ += stmt.GetColumnList().size();
	base_sql_size_ += 1;  // ")"

	// OUTPUT clause
	auto output_clause = stmt.GetOutputClause();
	if (!output_clause.empty()) {
		base_sql_size_ += 1;  // "\n"
		base_sql_size_ += output_clause.size();
	}

	// "\nVALUES"
	base_sql_size_ += 7;

	// ";" at end
	base_sql_size_ += 1;
}

//===----------------------------------------------------------------------===//
// Row Serialization
//===----------------------------------------------------------------------===//

vector<string> MSSQLBatchBuilder::SerializeRow(DataChunk &chunk, idx_t row_index) {
	vector<string> literals;
	literals.reserve(target_.insert_column_indices.size());

	for (idx_t i = 0; i < target_.insert_column_indices.size(); i++) {
		auto col_idx = target_.insert_column_indices[i];
		const auto &col = target_.columns[col_idx];

		// Get vector from chunk (assumes chunk columns match insert_column_indices order)
		auto &vector = chunk.data[i];
		auto literal = MSSQLValueSerializer::SerializeFromVector(vector, row_index, col.duckdb_type);
		literals.push_back(std::move(literal));
	}

	return literals;
}

idx_t MSSQLBatchBuilder::EstimateRowSize(const vector<string> &literals) const {
	// "  (" + literals joined with ", " + ")"
	idx_t size = 4;	 // "  (" and ")"
	for (size_t i = 0; i < literals.size(); i++) {
		if (i > 0) {
			size += 2;	// ", "
		}
		size += literals[i].size();
	}
	// ",\n" for row separator (except last row, but we account for it)
	size += 2;
	return size;
}

//===----------------------------------------------------------------------===//
// Row Operations
//===----------------------------------------------------------------------===//

bool MSSQLBatchBuilder::AddRow(DataChunk &chunk, idx_t row_index) {
	// Serialize the row
	auto literals = SerializeRow(chunk, row_index);

	// Estimate size this row will add
	idx_t row_sql_size = EstimateRowSize(literals);

	// Check if single row exceeds limit
	if (row_sql_size > config_.max_sql_bytes) {
		throw InvalidInputException("Row at offset %llu exceeds maximum SQL size (%llu bytes)",
									static_cast<unsigned long long>(current_row_offset_),
									static_cast<unsigned long long>(config_.max_sql_bytes));
	}

	// Check if adding this row would exceed byte limit
	if (current_sql_bytes_ + row_sql_size > config_.max_sql_bytes && pending_row_count_ > 0) {
		return false;  // Batch full, caller should flush
	}

	// Check if we've hit the row count limit
	if (pending_row_count_ >= config_.EffectiveRowsPerStatement()) {
		return false;  // Batch full, caller should flush
	}

	// Add row to current batch
	row_literals_.push_back(std::move(literals));
	current_sql_bytes_ += row_sql_size;
	pending_row_count_++;
	current_row_offset_++;

	return true;
}

bool MSSQLBatchBuilder::HasPendingRows() const {
	return pending_row_count_ > 0;
}

idx_t MSSQLBatchBuilder::GetPendingRowCount() const {
	return pending_row_count_;
}

//===----------------------------------------------------------------------===//
// Batch Flushing
//===----------------------------------------------------------------------===//

MSSQLInsertBatch MSSQLBatchBuilder::FlushBatch() {
	MSSQLInsertBatch batch;

	// Set row range
	batch.row_offset_start = current_row_offset_ - pending_row_count_;
	batch.row_offset_end = current_row_offset_;
	batch.row_count = pending_row_count_;

	// Generate SQL
	MSSQLInsertStatement stmt(target_, include_output_clause_);
	batch.sql_statement = stmt.Build(row_literals_);
	batch.sql_bytes = batch.sql_statement.size();
	batch.state = MSSQLInsertBatch::State::READY;

	// Update tracking
	batch_count_++;

	// Reset for next batch
	row_literals_.clear();
	row_literals_.reserve(config_.EffectiveRowsPerStatement());
	current_sql_bytes_ = base_sql_size_;
	pending_row_count_ = 0;

	return batch;
}

//===----------------------------------------------------------------------===//
// Progress Tracking
//===----------------------------------------------------------------------===//

idx_t MSSQLBatchBuilder::GetCurrentRowOffset() const {
	return current_row_offset_;
}

idx_t MSSQLBatchBuilder::GetBatchCount() const {
	return batch_count_;
}

}  // namespace duckdb

#include "dml/insert/mssql_returning_parser.hpp"
#include "duckdb/common/exception.hpp"
#include "tds/encoding/type_converter.hpp"

namespace duckdb {

MSSQLReturningParser::MSSQLReturningParser(const MSSQLInsertTarget &target, const vector<idx_t> &returning_column_ids)
	: target_(target), returning_column_ids_(returning_column_ids) {
	// Build result types from returning column indices
	for (auto col_idx : returning_column_ids_) {
		if (col_idx < target_.columns.size()) {
			result_types_.push_back(target_.columns[col_idx].duckdb_type);
		}
	}
}

void MSSQLReturningParser::Attach(mssql::DmlBatchOptions &options) {
	options.on_row = [this](const tds::RowData &row, const std::vector<tds::ColumnMetadata> &columns) {
		ProcessRow(row, columns);
	};
}

void MSSQLReturningParser::ProcessRow(const tds::RowData &row, const std::vector<tds::ColumnMetadata> &columns) {
	if (!chunk_) {
		chunk_ = make_uniq<DataChunk>();
		chunk_->Initialize(Allocator::DefaultAllocator(), result_types_);
	}
	// One INSERT statement carries at most 1000 rows (the 1000-constant cap,
	// spec 062 W1b), so its OUTPUT fits one chunk. The old reader returned a
	// full chunk early and left the rest of the response unread.
	if (row_count_ >= STANDARD_VECTOR_SIZE) {
		throw IOException("INSERT ... RETURNING: one statement returned more than %llu rows",
						  (unsigned long long)STANDARD_VECTOR_SIZE);
	}
	// The OUTPUT INSERTED columns come in the order of returning_column_ids
	for (idx_t i = 0; i < columns.size() && i < result_types_.size(); i++) {
		bool is_null = (i < row.null_mask.size()) ? row.null_mask[i] : false;
		const auto &value = (i < row.values.size()) ? row.values[i] : std::vector<uint8_t>{};
		tds::encoding::TypeConverter::ConvertValue(value, is_null, columns[i], chunk_->data[i], row_count_);
	}
	row_count_++;
}

unique_ptr<DataChunk> MSSQLReturningParser::TakeResult() {
	if (row_count_ == 0) {
		return nullptr;
	}
	chunk_->SetChildCardinality(row_count_);
	return std::move(chunk_);
}

}  // namespace duckdb

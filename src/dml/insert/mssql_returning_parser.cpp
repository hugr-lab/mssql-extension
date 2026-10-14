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
	options.on_metadata = [this](const std::vector<tds::ColumnMetadata> &columns) {
		// A new result set: whatever came before it is not the RETURNING list.
		columns_ = columns;
		rows_.clear();
	};
	options.on_row = [this](const tds::RowData &row, const std::vector<tds::ColumnMetadata> &) {
		rows_.push_back(row);
	};
}

unique_ptr<DataChunk> MSSQLReturningParser::TakeResult() {
	if (rows_.empty()) {
		return nullptr;
	}
	if (columns_.size() != result_types_.size()) {
		throw IOException(
			"INSERT ... RETURNING on '%s.%s': the statement's last result set has %llu columns, "
			"the RETURNING list %llu",
			target_.schema_name, target_.table_name, (unsigned long long)columns_.size(),
			(unsigned long long)result_types_.size());
	}
	// One INSERT statement carries at most 1000 rows (the 1000-constant cap,
	// spec 062 W1b), so its RETURNING rows fit one chunk.
	if (rows_.size() > STANDARD_VECTOR_SIZE) {
		throw IOException("INSERT ... RETURNING: one statement returned more than %llu rows",
						  (unsigned long long)STANDARD_VECTOR_SIZE);
	}
	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), result_types_);
	for (idx_t r = 0; r < rows_.size(); r++) {
		const auto &row = rows_[r];
		for (idx_t i = 0; i < columns_.size(); i++) {
			bool is_null = (i < row.null_mask.size()) ? row.null_mask[i] : false;
			const auto &value = (i < row.values.size()) ? row.values[i] : std::vector<uint8_t>{};
			tds::encoding::TypeConverter::ConvertValue(value, is_null, columns_[i], chunk->data[i], r);
		}
	}
	chunk->SetChildCardinality(rows_.size());
	rows_.clear();
	return chunk;
}

}  // namespace duckdb

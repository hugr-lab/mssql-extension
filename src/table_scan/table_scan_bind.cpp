// Table Scan Bind Data Implementation
// Feature: 013-table-scan-filter-refactor

#include "table_scan/table_scan_bind.hpp"

namespace duckdb {
namespace mssql {

std::string TableScanBindData::GetFullTableName() const {
	// Returns [schema].[table] format
	std::string result = "[";
	// Escape ] as ]]
	for (char c : schema_name) {
		result += c;
		if (c == ']') {
			result += ']';
		}
	}
	result += "].[";
	for (char c : table_name) {
		result += c;
		if (c == ']') {
			result += ']';
		}
	}
	result += "]";
	return result;
}

bool TableScanBindData::IsValidColumnIndex(idx_t idx) const {
	return idx < all_column_names.size();
}

unique_ptr<FunctionData> TableScanBindData::Copy() const {
	auto result = make_uniq<TableScanBindData>();
	result->context_name = context_name;
	result->schema_name = schema_name;
	result->table_name = table_name;
	result->all_types = all_types;
	result->all_column_names = all_column_names;
	result->return_types = return_types;
	result->column_names = column_names;
	result->result_stream_id = result_stream_id;
	return std::move(result);
}

bool TableScanBindData::Equals(const FunctionData &other) const {
	auto &other_data = other.Cast<TableScanBindData>();
	return context_name == other_data.context_name &&
		   schema_name == other_data.schema_name &&
		   table_name == other_data.table_name;
}

} // namespace mssql
} // namespace duckdb

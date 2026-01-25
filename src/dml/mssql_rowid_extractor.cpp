#include "dml/mssql_rowid_extractor.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "insert/mssql_value_serializer.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// ExtractSingleRowPK - Extract PK values for a single row
//===----------------------------------------------------------------------===//

vector<Value> ExtractSingleRowPK(Vector &rowid_vector, idx_t row_idx,
                                 const mssql::PrimaryKeyInfo &pk_info) {
	vector<Value> pk_values;

	if (!pk_info.exists || pk_info.columns.empty()) {
		throw InternalException("ExtractSingleRowPK called on table without primary key");
	}

	// Get the rowid value at the specified row index
	auto rowid_value = rowid_vector.GetValue(row_idx);

	if (pk_info.IsScalar()) {
		// Scalar PK: rowid is the PK value directly
		pk_values.push_back(std::move(rowid_value));
	} else {
		// Composite PK: rowid is STRUCT with PK fields
		// Extract each field in PK column order
		if (rowid_value.type().id() != LogicalTypeId::STRUCT) {
			throw InternalException("Expected STRUCT rowid for composite PK, got %s",
			                        rowid_value.type().ToString());
		}

		auto &struct_children = StructValue::GetChildren(rowid_value);

		// PK columns are stored in key_ordinal order (1-based in sys.index_columns)
		// The STRUCT fields should match this order
		for (idx_t i = 0; i < pk_info.columns.size(); i++) {
			if (i >= struct_children.size()) {
				throw InternalException("STRUCT rowid has fewer fields than PK columns");
			}
			pk_values.push_back(struct_children[i]);
		}
	}

	return pk_values;
}

//===----------------------------------------------------------------------===//
// ExtractPKFromRowid - Extract PK values from rowid column (bulk)
//===----------------------------------------------------------------------===//

vector<vector<Value>> ExtractPKFromRowid(Vector &rowid_vector, idx_t count,
                                         const mssql::PrimaryKeyInfo &pk_info) {
	vector<vector<Value>> result;
	result.reserve(count);

	for (idx_t i = 0; i < count; i++) {
		result.push_back(ExtractSingleRowPK(rowid_vector, i, pk_info));
	}

	return result;
}

//===----------------------------------------------------------------------===//
// GetPKValueAsString - Convert PK value to T-SQL literal string
//===----------------------------------------------------------------------===//

string GetPKValueAsString(const Value &value, const LogicalType &duckdb_type) {
	return MSSQLValueSerializer::Serialize(value, duckdb_type);
}

}  // namespace duckdb

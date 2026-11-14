#include "dml/mssql_rowid_extractor.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// ExtractSingleRowPK - Extract PK values for a single row
//===----------------------------------------------------------------------===//

string NullKeyValueMessage(const string &column) {
	// The rowid's columns are NOT NULL (a primary key, or a unique index usable as the key: spec 077
	// W1), so a NULL here is a value the read could not hold -- today only a datetime2(7) outside
	// 1677-2262 read as TIMESTAMP_NS (issue #559). Keyed by NULL, the row would not be found.
	return StringUtil::Format(
		"mssql: key column \"%s\" of a row to update or delete was read as NULL, which a key column cannot "
		"hold -- a value its read type cannot hold, such as a datetime2(7) outside 1677-2262 read as "
		"TIMESTAMP_NS (issue #559) -- so the row cannot be found again by its key. Attach with datetime2_7_as "
		"'timestamp' to read such values (a datetime2(7) key is then not used: the table is keyed by another "
		"unique index, or the statement is refused by name), or narrow the statement to rows inside the range",
		column);
}

void RefuseNullKeyValue(const string &column) {
	throw InvalidInputException(NullKeyValueMessage(column));
}

vector<Value> ExtractSingleRowPK(Vector &rowid_vector, idx_t row_idx, const mssql::RowIdKeyInfo &pk_info) {
	vector<Value> pk_values;

	if (!pk_info.exists || pk_info.columns.empty()) {
		throw InternalException("ExtractSingleRowPK called on table without primary key");
	}

	// Get the rowid value at the specified row index
	auto rowid_value = rowid_vector.GetValue(row_idx);

	if (pk_info.IsScalar()) {
		// Scalar PK: rowid is the PK value directly
		if (rowid_value.IsNull()) {
			RefuseNullKeyValue(pk_info.columns[0].name);
		}
		pk_values.push_back(std::move(rowid_value));
	} else {
		// Composite PK: rowid is STRUCT with PK fields
		// Extract each field in PK column order
		if (rowid_value.type().id() != LogicalTypeId::STRUCT) {
			throw InternalException("Expected STRUCT rowid for composite PK, got %s", rowid_value.type().ToString());
		}

		auto &struct_children = StructValue::GetChildren(rowid_value);

		// PK columns are stored in key_ordinal order (1-based in sys.index_columns)
		// The STRUCT fields should match this order
		for (idx_t i = 0; i < pk_info.columns.size(); i++) {
			if (i >= struct_children.size()) {
				throw InternalException("STRUCT rowid has fewer fields than PK columns");
			}
			if (struct_children[i].IsNull()) {
				RefuseNullKeyValue(pk_info.columns[i].name);
			}
			pk_values.push_back(struct_children[i]);
		}
	}

	return pk_values;
}

//===----------------------------------------------------------------------===//
// ExtractPKFromRowid - Extract PK values from rowid column (bulk)
//===----------------------------------------------------------------------===//

vector<vector<Value>> ExtractPKFromRowid(Vector &rowid_vector, idx_t count, const mssql::RowIdKeyInfo &pk_info) {
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

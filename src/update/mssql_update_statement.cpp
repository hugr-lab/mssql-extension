#include "update/mssql_update_statement.hpp"
#include "duckdb/common/string_util.hpp"
#include "insert/mssql_value_serializer.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLUpdateStatement Implementation
//===----------------------------------------------------------------------===//

MSSQLUpdateStatement::MSSQLUpdateStatement(const MSSQLUpdateTarget &target) : target_(target) {}

MSSQLDMLBatch MSSQLUpdateStatement::Build(const vector<vector<Value>> &pk_values,
										  const vector<vector<Value>> &update_values, idx_t batch_number) {
	MSSQLDMLBatch batch;
	batch.batch_number = batch_number;
	batch.row_count = pk_values.size();

	if (batch.row_count == 0) {
		return batch;
	}

	// Build SQL statement using inline literals (not parameters)
	// UPDATE t
	// SET t.[col1] = v.[col1], t.[col2] = v.[col2]
	// FROM [schema].[table] AS t
	// JOIN (VALUES
	//   (1, 'value1', 100),
	//   (2, 'value2', 200)
	// ) AS v([pk1], [col1], [col2])
	// ON t.[pk1] = v.[pk1]

	string sql = "UPDATE t\n";
	sql += GenerateSetClause();
	sql += "\nFROM " + target_.GetFullyQualifiedName() + " AS t\n";
	sql += "JOIN (VALUES\n";

	for (idx_t row_idx = 0; row_idx < batch.row_count; row_idx++) {
		if (row_idx > 0) {
			sql += ",\n";
		}
		sql += "  (";

		// Add PK values first (serialized as literals)
		for (idx_t pk_idx = 0; pk_idx < pk_values[row_idx].size(); pk_idx++) {
			if (pk_idx > 0) {
				sql += ", ";
			}
			const auto &pk_type = target_.pk_info.columns[pk_idx].duckdb_type;
			sql += MSSQLValueSerializer::Serialize(pk_values[row_idx][pk_idx], pk_type);
		}

		// Add update values (serialized as literals)
		for (idx_t col_idx = 0; col_idx < update_values[row_idx].size(); col_idx++) {
			sql += ", ";
			const auto &col_type = target_.update_columns[col_idx].duckdb_type;
			sql += MSSQLValueSerializer::Serialize(update_values[row_idx][col_idx], col_type);
		}

		sql += ")";
	}

	sql += "\n) AS v(" + GenerateValuesColumnList() + ")\n";
	sql += GenerateOnClause();

	batch.sql = std::move(sql);
	return batch;
}

string MSSQLUpdateStatement::GenerateSetClause() const {
	string result = "SET ";
	for (idx_t i = 0; i < target_.update_columns.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		string col_name = EscapeIdentifier(target_.update_columns[i].name);
		result += "t." + col_name + " = v." + col_name;
	}
	return result;
}

string MSSQLUpdateStatement::GenerateValuesColumnList() const {
	string result;

	// PK columns first
	for (idx_t i = 0; i < target_.pk_info.columns.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += EscapeIdentifier(target_.pk_info.columns[i].name);
	}

	// Then update columns
	for (idx_t i = 0; i < target_.update_columns.size(); i++) {
		result += ", " + EscapeIdentifier(target_.update_columns[i].name);
	}

	return result;
}

string MSSQLUpdateStatement::GenerateOnClause() const {
	string result = "ON ";
	for (idx_t i = 0; i < target_.pk_info.columns.size(); i++) {
		if (i > 0) {
			result += " AND ";
		}
		string col_name = EscapeIdentifier(target_.pk_info.columns[i].name);
		result += "t." + col_name + " = v." + col_name;
	}
	return result;
}

string MSSQLUpdateStatement::EscapeIdentifier(const string &name) {
	return MSSQLValueSerializer::EscapeIdentifier(name);
}

}  // namespace duckdb

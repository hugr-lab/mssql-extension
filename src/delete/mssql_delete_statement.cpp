#include "delete/mssql_delete_statement.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "insert/mssql_value_serializer.hpp"

namespace duckdb {

MSSQLDeleteStatement::MSSQLDeleteStatement(const MSSQLDeleteTarget &target) : target_(target) {
	if (!target_.HasPrimaryKey()) {
		throw InvalidInputException("MSSQLDeleteStatement requires a table with primary key");
	}
}

string MSSQLDeleteStatement::EscapeIdentifier(const string &identifier) {
	// Escape square brackets within the identifier by doubling them
	string escaped = identifier;
	size_t pos = 0;
	while ((pos = escaped.find(']', pos)) != string::npos) {
		escaped.insert(pos, "]");
		pos += 2;
	}
	return "[" + escaped + "]";
}

string MSSQLDeleteStatement::GenerateDeleteClause() const {
	// DELETE t FROM [schema].[table] AS t
	string sql = "DELETE t FROM ";
	sql += EscapeIdentifier(target_.schema_name);
	sql += ".";
	sql += EscapeIdentifier(target_.table_name);
	sql += " AS t";
	return sql;
}

string MSSQLDeleteStatement::GenerateOnClause() const {
	// ON t.[pk1] = v.[pk1] AND t.[pk2] = v.[pk2] ...
	string sql = "ON ";
	auto &pk_columns = target_.pk_info.columns;
	for (idx_t i = 0; i < pk_columns.size(); i++) {
		if (i > 0) {
			sql += " AND ";
		}
		string col = EscapeIdentifier(pk_columns[i].name);
		sql += "t." + col + " = v." + col;
	}
	return sql;
}

MSSQLDMLBatch MSSQLDeleteStatement::Build(const vector<vector<Value>> &pk_values) const {
	MSSQLDMLBatch batch;

	if (pk_values.empty()) {
		return batch;
	}

	batch.row_count = pk_values.size();

	// Build the VALUES clause with inline literals (not parameters)
	// VALUES (1, 'a'), (2, 'b'), ...
	string values_clause = "VALUES ";
	auto &pk_columns = target_.pk_info.columns;
	idx_t pk_count = pk_columns.size();

	for (idx_t row = 0; row < pk_values.size(); row++) {
		if (row > 0) {
			values_clause += ", ";
		}
		values_clause += "(";

		const auto &row_pk = pk_values[row];
		if (row_pk.size() != pk_count) {
			throw InvalidInputException("PK value count mismatch: expected %d, got %d", pk_count, row_pk.size());
		}

		for (idx_t col = 0; col < pk_count; col++) {
			if (col > 0) {
				values_clause += ", ";
			}
			// Serialize the value as a SQL literal
			values_clause += MSSQLValueSerializer::Serialize(row_pk[col], pk_columns[col].duckdb_type);
		}
		values_clause += ")";
	}

	// Build column list for VALUES alias
	// AS v([pk1], [pk2], ...)
	string alias_columns = " AS v(";
	for (idx_t i = 0; i < pk_count; i++) {
		if (i > 0) {
			alias_columns += ", ";
		}
		alias_columns += EscapeIdentifier(pk_columns[i].name);
	}
	alias_columns += ")";

	// Assemble the full DELETE statement
	// DELETE t FROM [schema].[table] AS t
	// JOIN (VALUES (1), (2), ...) AS v([pk1])
	// ON t.[pk1] = v.[pk1]
	batch.sql = GenerateDeleteClause();
	batch.sql += " JOIN (";
	batch.sql += values_clause;
	batch.sql += ")";
	batch.sql += alias_columns;
	batch.sql += " ";
	batch.sql += GenerateOnClause();

	return batch;
}

MSSQLDMLBatch MSSQLDeleteStatement::BuildSingle(const vector<Value> &pk_value) const {
	vector<vector<Value>> pk_values;
	pk_values.push_back(pk_value);
	return Build(pk_values);
}

}  // namespace duckdb

#include "dml/insert/mssql_insert_statement.hpp"
#include "catalog/mssql_column_info.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

MSSQLInsertStatement::MSSQLInsertStatement(const MSSQLInsertTarget &target, bool include_output)
	: target_(target), include_output_(include_output) {}

//===----------------------------------------------------------------------===//
// Cache Initialization
//===----------------------------------------------------------------------===//

void MSSQLInsertStatement::InitializeCache() const {
	if (cache_initialized_) {
		return;
	}

	// Build table name: [schema].[table]
	cached_table_name_ = MSSQLValueSerializer::EscapeIdentifier(target_.schema_name) + "." +
						 MSSQLValueSerializer::EscapeIdentifier(target_.table_name);

	// Build column list: [col1], [col2], ...
	string columns;
	for (idx_t i = 0; i < target_.insert_column_indices.size(); i++) {
		if (i > 0) {
			columns += ", ";
		}
		const auto &col = target_.columns[target_.insert_column_indices[i]];
		columns += MSSQLValueSerializer::EscapeIdentifier(col.name);
	}
	cached_column_list_ = columns;

	// Build OUTPUT clause if needed
	if (include_output_ && !target_.returning_column_indices.empty()) {
		string output_cols;
		for (idx_t i = 0; i < target_.returning_column_indices.size(); i++) {
			if (i > 0) {
				output_cols += ", ";
			}
			const auto &col = target_.columns[target_.returning_column_indices[i]];
			const string escaped = MSSQLValueSerializer::EscapeIdentifier(col.name);
			// The OUTPUT list carries EVERY column of the table, not the ones the
			// user named: DuckDB's RETURNING projection sits above this operator
			// and expects the table's full width. So a single column the result
			// stream cannot decode used to fail the whole statement, even for an
			// `INSERT … RETURNING id` that never mentioned it — measured, a table
			// with a geometry column answered
			// `COLMETADATA parse error: Unsupported SQL Server type: UDT`.
			//
			// The read path already solved this for the same columns, and OUTPUT
			// accepts the same expressions: a method call for the spatial UDTs,
			// whose bytes then arrive as the OGC WKB a DuckDB GEOMETRY wants, and
			// a CAST to NVARCHAR(MAX) for the types with no decodable wire form
			// (sql_variant, hierarchyid, a CLR UDT). Both were tried against the
			// server before this was written.
			if (MSSQLColumnInfo::IsSpatialType(col.mssql_type)) {
				output_cols += "INSERTED." + escaped + ".STAsBinary() AS " + escaped;
			} else if (!MSSQLColumnInfo::IsKnownSQLServerType(col.mssql_type)) {
				output_cols += "CAST(INSERTED." + escaped + " AS NVARCHAR(MAX)) AS " + escaped;
			} else {
				output_cols += "INSERTED." + escaped;
			}
		}
		cached_output_clause_ = "OUTPUT " + output_cols;
	}

	cache_initialized_ = true;
}

//===----------------------------------------------------------------------===//
// Accessors
//===----------------------------------------------------------------------===//

string MSSQLInsertStatement::GetTableName() const {
	InitializeCache();
	return cached_table_name_;
}

string MSSQLInsertStatement::GetColumnList() const {
	InitializeCache();
	return cached_column_list_;
}

string MSSQLInsertStatement::GetOutputClause() const {
	InitializeCache();
	return cached_output_clause_;
}

//===----------------------------------------------------------------------===//
// SQL Generation
//===----------------------------------------------------------------------===//

string MSSQLInsertStatement::Build(const vector<vector<string>> &row_literals) const {
	InitializeCache();

	// Estimate total size for reservation
	size_t estimated_size = 50;	 // INSERT INTO + overhead
	estimated_size += cached_table_name_.size();
	estimated_size += cached_column_list_.size();
	estimated_size += cached_output_clause_.size();

	for (const auto &row : row_literals) {
		estimated_size += 4;  // "(", ")", ",\n"
		for (const auto &lit : row) {
			estimated_size += lit.size() + 2;  // ", "
		}
	}

	// Build statement
	string sql;
	sql.reserve(estimated_size);

	// INSERT INTO [schema].[table]
	sql += "INSERT INTO ";
	sql += cached_table_name_;

	// ([col1], [col2])
	sql += " (";
	sql += cached_column_list_;
	sql += ")";

	// OUTPUT clause (if enabled)
	if (!cached_output_clause_.empty()) {
		sql += "\n";
		sql += cached_output_clause_;
	}

	// VALUES
	sql += "\nVALUES";

	// Row literals
	for (size_t row_idx = 0; row_idx < row_literals.size(); row_idx++) {
		if (row_idx > 0) {
			sql += ",";
		}
		sql += "\n  (";

		const auto &row = row_literals[row_idx];
		for (size_t col_idx = 0; col_idx < row.size(); col_idx++) {
			if (col_idx > 0) {
				sql += ", ";
			}
			sql += row[col_idx];
		}

		sql += ")";
	}

	sql += ";";

	return sql;
}

}  // namespace duckdb

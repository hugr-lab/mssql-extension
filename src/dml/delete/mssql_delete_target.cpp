#include "dml/delete/mssql_delete_target.hpp"
#include "duckdb/common/string_util.hpp"
#include "query/mssql_sql_params.hpp"

namespace duckdb {

string MSSQLDeleteTarget::GetFullyQualifiedName() const {
	return mssql::QuoteIdentifier(schema_name) + "." + mssql::QuoteIdentifier(table_name);
}

idx_t MSSQLDeleteTarget::GetParamsPerRow() const {
	// For DELETE, only PK columns are needed
	return pk_info.columns.size();
}

}  // namespace duckdb

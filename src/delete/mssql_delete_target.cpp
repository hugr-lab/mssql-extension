#include "delete/mssql_delete_target.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

string MSSQLDeleteTarget::GetFullyQualifiedName() const {
	return "[" + schema_name + "].[" + table_name + "]";
}

idx_t MSSQLDeleteTarget::GetParamsPerRow() const {
	// For DELETE, only PK columns are needed
	return pk_info.columns.size();
}

}  // namespace duckdb

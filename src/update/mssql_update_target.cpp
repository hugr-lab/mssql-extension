#include "update/mssql_update_target.hpp"
#include "insert/mssql_value_serializer.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLUpdateTarget Implementation
//===----------------------------------------------------------------------===//

string MSSQLUpdateTarget::GetFullyQualifiedName() const {
	return MSSQLValueSerializer::EscapeIdentifier(schema_name) + "." +
	       MSSQLValueSerializer::EscapeIdentifier(table_name);
}

idx_t MSSQLUpdateTarget::GetParamsPerRow() const {
	// Parameters per row = PK columns + update columns
	return pk_info.columns.size() + update_columns.size();
}

}  // namespace duckdb

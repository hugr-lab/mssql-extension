#include "insert/mssql_insert_target.hpp"
#include "insert/mssql_value_serializer.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertTarget
//===----------------------------------------------------------------------===//

string MSSQLInsertTarget::GetFullyQualifiedName() const {
	// [catalog].[schema].[table]
	// For SQL Server, we typically use just [schema].[table]
	// as the catalog is specified at connection time
	return MSSQLValueSerializer::EscapeIdentifier(schema_name) + "." +
	       MSSQLValueSerializer::EscapeIdentifier(table_name);
}

}  // namespace duckdb

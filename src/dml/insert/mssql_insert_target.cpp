#include "dml/insert/mssql_insert_target.hpp"
#include "dml/insert/mssql_value_serializer.hpp"
#include "query/mssql_identifier.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertTarget
//===----------------------------------------------------------------------===//

string MSSQLInsertTarget::GetFullyQualifiedName() const {
	// [catalog].[schema].[table]
	// For SQL Server, we typically use just [schema].[table]
	// as the catalog is specified at connection time
	return mssql::QuoteIdentifier(schema_name) + "." + mssql::QuoteIdentifier(table_name);
}

}  // namespace duckdb

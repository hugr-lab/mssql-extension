#include "catalog/mssql_ddl_translator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// DDLOperation to String
//===----------------------------------------------------------------------===//

const char *DDLOperationToString(DDLOperation op) {
	switch (op) {
	case DDLOperation::CREATE_SCHEMA:
		return "CREATE_SCHEMA";
	case DDLOperation::DROP_SCHEMA:
		return "DROP_SCHEMA";
	case DDLOperation::CREATE_TABLE:
		return "CREATE_TABLE";
	case DDLOperation::DROP_TABLE:
		return "DROP_TABLE";
	case DDLOperation::RENAME_TABLE:
		return "RENAME_TABLE";
	case DDLOperation::ADD_COLUMN:
		return "ADD_COLUMN";
	case DDLOperation::DROP_COLUMN:
		return "DROP_COLUMN";
	case DDLOperation::RENAME_COLUMN:
		return "RENAME_COLUMN";
	case DDLOperation::ALTER_COLUMN_TYPE:
		return "ALTER_COLUMN_TYPE";
	case DDLOperation::ALTER_COLUMN_NULL:
		return "ALTER_COLUMN_NULL";
	default:
		return "UNKNOWN";
	}
}

//===----------------------------------------------------------------------===//
// Identifier Quoting
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::QuoteIdentifier(const string &identifier) {
	// SQL Server uses square brackets for quoting identifiers
	// The ] character is escaped by doubling it: ] -> ]]
	string result;
	result.reserve(identifier.size() + 2);
	result += '[';
	for (char c : identifier) {
		result += c;
		if (c == ']') {
			result += ']';	// Double the ] character
		}
	}
	result += ']';
	return result;
}

string MSSQLDDLTranslator::EscapeStringLiteral(const string &value) {
	// Escape single quotes by doubling them: ' -> ''
	string result;
	result.reserve(value.size() + 10);
	for (char c : value) {
		result += c;
		if (c == '\'') {
			result += '\'';
		}
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Type Mapping: DuckDB -> SQL Server
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::MapTypeToSQLServer(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BIT";

	case LogicalTypeId::TINYINT:
		return "TINYINT";

	case LogicalTypeId::SMALLINT:
		return "SMALLINT";

	case LogicalTypeId::INTEGER:
		return "INT";

	case LogicalTypeId::BIGINT:
		return "BIGINT";

	case LogicalTypeId::UTINYINT:
		// Unsigned types: SQL Server doesn't have unsigned, use next larger signed type
		return "TINYINT";  // Range 0-255 fits in SQL Server TINYINT

	case LogicalTypeId::USMALLINT:
		return "INT";  // Wider to fit full range

	case LogicalTypeId::UINTEGER:
		return "BIGINT";  // Wider to fit full range

	case LogicalTypeId::UBIGINT:
		return "DECIMAL(20,0)";	 // No native unsigned 64-bit

	case LogicalTypeId::FLOAT:
		return "REAL";	// 32-bit float

	case LogicalTypeId::DOUBLE:
		return "FLOAT";	 // 64-bit float in SQL Server

	case LogicalTypeId::DECIMAL: {
		// Get precision and scale, clamp to SQL Server limits
		uint8_t width, scale;
		type.GetDecimalProperties(width, scale);
		// SQL Server: precision 1-38, scale 0-precision
		uint8_t precision = width > 38 ? 38 : width;
		if (scale > precision) {
			scale = precision;
		}
		return StringUtil::Format("DECIMAL(%d,%d)", precision, scale);
	}

	case LogicalTypeId::VARCHAR: {
		// Check for collation info to determine length
		// Default to NVARCHAR for Unicode safety
		// DuckDB VARCHAR maps to NVARCHAR in SQL Server
		return "NVARCHAR(MAX)";	 // Default to MAX for unbounded strings
	}

	case LogicalTypeId::BLOB:
		return "VARBINARY(MAX)";

	case LogicalTypeId::DATE:
		return "DATE";

	case LogicalTypeId::TIME:
		return "TIME(7)";  // Maximum precision

	case LogicalTypeId::TIMESTAMP:
		return "DATETIME2(6)";	// Microsecond precision

	case LogicalTypeId::TIMESTAMP_TZ:
		return "DATETIMEOFFSET(7)";	 // With timezone

	case LogicalTypeId::UUID:
		return "UNIQUEIDENTIFIER";

	case LogicalTypeId::HUGEINT:
		// DuckDB HUGEINT is 128-bit, SQL Server max is DECIMAL(38,0)
		return "DECIMAL(38,0)";

	case LogicalTypeId::INTERVAL:
		// SQL Server doesn't have interval type, store as string
		return "NVARCHAR(100)";

	default:
		throw NotImplementedException("Cannot map DuckDB type '%s' to SQL Server type", type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// Column Definition Building
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::BuildColumnDefinition(const ColumnDefinition &column) {
	string result;

	// Column name
	result += QuoteIdentifier(column.GetName());
	result += " ";

	// Column type
	result += MapTypeToSQLServer(column.GetType());

	// Nullability (default is NULL in SQL Server if not specified)
	// DuckDB's ColumnDefinition doesn't expose nullability directly in the same way
	// We'll check for NOT NULL constraint
	// For now, default to NOT NULL to match common DDL expectations
	result += " NULL";

	return result;
}

//===----------------------------------------------------------------------===//
// Schema Operations
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::TranslateCreateSchema(const string &schema_name) {
	return "CREATE SCHEMA " + QuoteIdentifier(schema_name) + ";";
}

string MSSQLDDLTranslator::TranslateDropSchema(const string &schema_name) {
	return "DROP SCHEMA " + QuoteIdentifier(schema_name) + ";";
}

//===----------------------------------------------------------------------===//
// Table Operations
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::TranslateCreateTable(const string &schema_name, const string &table_name,
												const ColumnList &columns) {
	if (columns.empty()) {
		throw InvalidInputException("CREATE TABLE requires at least one column");
	}

	string result = "CREATE TABLE ";
	result += QuoteIdentifier(schema_name);
	result += ".";
	result += QuoteIdentifier(table_name);
	result += " (";

	bool first = true;
	for (auto &column : columns.Logical()) {
		if (!first) {
			result += ", ";
		}
		first = false;
		result += BuildColumnDefinition(column);
	}

	result += ");";
	return result;
}

string MSSQLDDLTranslator::TranslateDropTable(const string &schema_name, const string &table_name) {
	return "DROP TABLE " + QuoteIdentifier(schema_name) + "." + QuoteIdentifier(table_name) + ";";
}

string MSSQLDDLTranslator::TranslateRenameTable(const string &schema_name, const string &old_name,
												const string &new_name) {
	// SQL Server uses sp_rename for renaming tables
	// Syntax: EXEC sp_rename N'schema.old_name', N'new_name'
	// Note: new_name should not include schema
	string old_full_name = schema_name + "." + old_name;
	return "EXEC sp_rename N'" + EscapeStringLiteral(old_full_name) + "', N'" + EscapeStringLiteral(new_name) + "';";
}

//===----------------------------------------------------------------------===//
// Column Operations
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::TranslateAddColumn(const string &schema_name, const string &table_name,
											  const ColumnDefinition &column) {
	string result = "ALTER TABLE ";
	result += QuoteIdentifier(schema_name);
	result += ".";
	result += QuoteIdentifier(table_name);
	result += " ADD ";
	result += BuildColumnDefinition(column);
	result += ";";
	return result;
}

string MSSQLDDLTranslator::TranslateDropColumn(const string &schema_name, const string &table_name,
											   const string &column_name) {
	return "ALTER TABLE " + QuoteIdentifier(schema_name) + "." + QuoteIdentifier(table_name) + " DROP COLUMN " +
		   QuoteIdentifier(column_name) + ";";
}

string MSSQLDDLTranslator::TranslateRenameColumn(const string &schema_name, const string &table_name,
												 const string &old_name, const string &new_name) {
	// SQL Server uses sp_rename for renaming columns
	// Syntax: EXEC sp_rename N'schema.table.old_column', N'new_column', N'COLUMN'
	string old_full_name = schema_name + "." + table_name + "." + old_name;
	return "EXEC sp_rename N'" + EscapeStringLiteral(old_full_name) + "', N'" + EscapeStringLiteral(new_name) +
		   "', N'COLUMN';";
}

string MSSQLDDLTranslator::TranslateAlterColumnType(const string &schema_name, const string &table_name,
													const string &column_name, const LogicalType &new_type,
													bool is_nullable) {
	string result = "ALTER TABLE ";
	result += QuoteIdentifier(schema_name);
	result += ".";
	result += QuoteIdentifier(table_name);
	result += " ALTER COLUMN ";
	result += QuoteIdentifier(column_name);
	result += " ";
	result += MapTypeToSQLServer(new_type);
	result += is_nullable ? " NULL" : " NOT NULL";
	result += ";";
	return result;
}

string MSSQLDDLTranslator::TranslateAlterColumnNullability(const string &schema_name, const string &table_name,
														   const string &column_name, const LogicalType &current_type,
														   bool set_not_null) {
	// SQL Server requires specifying the full type when altering nullability
	string result = "ALTER TABLE ";
	result += QuoteIdentifier(schema_name);
	result += ".";
	result += QuoteIdentifier(table_name);
	result += " ALTER COLUMN ";
	result += QuoteIdentifier(column_name);
	result += " ";
	result += MapTypeToSQLServer(current_type);
	result += set_not_null ? " NOT NULL" : " NULL";
	result += ";";
	return result;
}

}  // namespace duckdb

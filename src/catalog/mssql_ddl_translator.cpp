#include "catalog/mssql_ddl_translator.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"

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

	// Don't explicitly specify NULL/NOT NULL here - let SQL Server use its defaults
	// or let constraints (PRIMARY KEY, NOT NULL) override this.
	// Explicitly specifying NULL prevents columns from being part of a PRIMARY KEY.

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
	// Delegate to the overload with empty constraints
	vector<unique_ptr<Constraint>> empty_constraints;
	return TranslateCreateTable(schema_name, table_name, columns, empty_constraints);
}

string MSSQLDDLTranslator::TranslateCreateTable(const string &schema_name, const string &table_name,
												const ColumnList &columns,
												const vector<unique_ptr<Constraint>> &constraints) {
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

	// Process constraints - look for PRIMARY KEY
	for (auto &constraint : constraints) {
		if (constraint->type == ConstraintType::UNIQUE) {
			auto &unique_constraint = constraint->Cast<UniqueConstraint>();
			if (unique_constraint.IsPrimaryKey()) {
				result += ", PRIMARY KEY (";

				// Get the column names for the PK
				const auto &pk_columns = unique_constraint.GetColumnNames();
				if (!pk_columns.empty()) {
					// Multi-column or named constraint
					for (idx_t i = 0; i < pk_columns.size(); i++) {
						if (i > 0) {
							result += ", ";
						}
						result += QuoteIdentifier(pk_columns[i]);
					}
				} else if (unique_constraint.HasIndex()) {
					// Single column constraint by index
					auto idx = unique_constraint.GetIndex();
					// Find the column name at this index
					idx_t col_idx = 0;
					for (auto &column : columns.Logical()) {
						if (col_idx == idx.index) {
							result += QuoteIdentifier(column.GetName());
							break;
						}
						col_idx++;
					}
				}

				result += ")";
			}
		}
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

//===----------------------------------------------------------------------===//
// CTAS-Specific Type Mapping
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::MapLogicalTypeToCTAS(const LogicalType &type, const mssql::CTASConfig &config) {
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
		// Get precision and scale, clamp to SQL Server limits (FR-017)
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
		// CTAS-specific: respect text_type setting (FR-013)
		if (config.text_type == mssql::CTASTextType::VARCHAR) {
			return "VARCHAR(MAX)";
		}
		return "NVARCHAR(MAX)";	 // Default to NVARCHAR for Unicode safety
	}

	case LogicalTypeId::BLOB:
		return "VARBINARY(MAX)";

	case LogicalTypeId::DATE:
		return "DATE";

	case LogicalTypeId::TIME:
		return "TIME(7)";  // Maximum precision

	case LogicalTypeId::TIMESTAMP:
		return "DATETIME2(7)";	// Maximum precision (100 nanoseconds)

	case LogicalTypeId::TIMESTAMP_TZ:
		return "DATETIMEOFFSET(7)";	 // With timezone

	case LogicalTypeId::UUID:
		return "UNIQUEIDENTIFIER";

	// Unsupported types - CTAS must fail with clear error (FR-012)
	case LogicalTypeId::HUGEINT:
		throw NotImplementedException(
			"CTAS does not support DuckDB type HUGEINT. "
			"Consider casting to DECIMAL(38,0) in your SELECT query.");

	case LogicalTypeId::UHUGEINT:
		throw NotImplementedException(
			"CTAS does not support DuckDB type UHUGEINT. "
			"Consider casting to DECIMAL(38,0) in your SELECT query.");

	case LogicalTypeId::INTERVAL:
		throw NotImplementedException(
			"CTAS does not support DuckDB type INTERVAL. "
			"SQL Server has no equivalent. Consider casting to VARCHAR.");

	case LogicalTypeId::LIST:
		throw NotImplementedException(
			"CTAS does not support DuckDB type LIST. "
			"SQL Server has no array type. Consider flattening or serializing to JSON.");

	case LogicalTypeId::STRUCT:
		throw NotImplementedException(
			"CTAS does not support DuckDB type STRUCT. "
			"SQL Server has no struct type. Consider flattening or serializing to JSON.");

	case LogicalTypeId::MAP:
		throw NotImplementedException(
			"CTAS does not support DuckDB type MAP. "
			"SQL Server has no map type. Consider serializing to JSON.");

	case LogicalTypeId::UNION:
		throw NotImplementedException(
			"CTAS does not support DuckDB type UNION. "
			"SQL Server has no union type. Consider normalizing the data.");

	case LogicalTypeId::ENUM:
		throw NotImplementedException(
			"CTAS does not support DuckDB type ENUM. "
			"Consider casting to VARCHAR or INTEGER.");

	case LogicalTypeId::BIT:
		throw NotImplementedException(
			"CTAS does not support DuckDB type BIT. "
			"Consider using BOOLEAN or BLOB.");

	case LogicalTypeId::ARRAY:
		throw NotImplementedException(
			"CTAS does not support DuckDB type ARRAY. "
			"SQL Server has no array type. Consider flattening or serializing to JSON.");

	default:
		throw NotImplementedException(
			"CTAS does not support DuckDB type '%s'. "
			"No SQL Server equivalent exists.",
			type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// CTAS CREATE TABLE Generation
//===----------------------------------------------------------------------===//

string MSSQLDDLTranslator::TranslateCreateTableFromSchema(const string &schema_name, const string &table_name,
														  const vector<mssql::CTASColumnDef> &columns) {
	if (columns.empty()) {
		throw InvalidInputException("CREATE TABLE requires at least one column");
	}

	string result = "CREATE TABLE ";
	result += QuoteIdentifier(schema_name);
	result += ".";
	result += QuoteIdentifier(table_name);
	result += " (";

	bool first = true;
	for (const auto &column : columns) {
		if (!first) {
			result += ", ";
		}
		first = false;

		// Column name (bracket-escaped per FR-010)
		result += QuoteIdentifier(column.name);
		result += " ";

		// Column type (already resolved to SQL Server type)
		result += column.mssql_type;

		// Nullability (FR-011)
		result += column.nullable ? " NULL" : " NOT NULL";
	}

	result += ");";
	return result;
}

}  // namespace duckdb

#include "catalog/mssql_ddl_translator.hpp"
#include "codec/binary_codec.hpp"
#include "codec/boolean_codec.hpp"
#include "codec/datetime_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/type_family.hpp"
#include "codec/uuid_codec.hpp"
#include "dml/ctas/mssql_ctas_config.hpp"
#include "dml/ctas/mssql_ctas_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"

namespace duckdb {

namespace {

// Single canonical DDL type-name dispatcher (spec 045 Phase 7 — FR-028).
// Both MapTypeToSQLServer (CreateTable) and MapLogicalTypeToCTAS (CtasCreateTable)
// share this body: family modules emit byte-identical strings for either ctx.
// FamilyFromLogicalType throws NotImplementedException for unsupported types
// (LIST/STRUCT/MAP/UNION/ENUM/BIT/ARRAY — caller may pre-filter for friendlier
// CTAS-specific error messages).
string DispatchDdlTypeName(const LogicalType &type, const mssql::CTASConfig &cfg, mssql::codec::DdlContext ctx) {
	switch (mssql::codec::FamilyFromLogicalType(type)) {
	case mssql::codec::TypeFamily::Boolean:
		return mssql::codec::boolean::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Integer:
		return mssql::codec::integer::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Float:
		return mssql::codec::float_family::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Decimal:
		return mssql::codec::decimal::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Money:
		// DuckDB has no MONEY LogicalType; FamilyFromLogicalType never returns
		// Money. Money is decode-only (from SQL Server MONEY/SMALLMONEY tokens,
		// surfaced as DuckDB DECIMAL). If we ever reach this arm, the type-family
		// mapping has a bug.
		throw InternalException(
			"DDL dispatcher reached Money arm for DuckDB type '%s' — "
			"Money family is decode-only and has no LogicalType mapping",
			type.ToString());
	case mssql::codec::TypeFamily::String:
		return mssql::codec::string::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Binary:
		return mssql::codec::binary::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::DateTime:
		return mssql::codec::datetime::FormatDdlTypeName(type, cfg, ctx);
	case mssql::codec::TypeFamily::Uuid:
		return mssql::codec::uuid::FormatDdlTypeName(type, cfg, ctx);
	}
	throw InternalException("Unreachable: unhandled TypeFamily in DDL dispatcher for '%s'", type.ToString());
}

}  // anonymous namespace

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
	try {
		return DispatchDdlTypeName(type, mssql::CTASConfig{}, mssql::codec::DdlContext::CreateTable);
	} catch (const NotImplementedException &) {
		// Preserve legacy non-CTAS error message for unsupported types.
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
	// Pre-filter known-unsupported nested types with CTAS-specific friendly errors.
	// All supported types route through the shared dispatcher (FR-028 byte-identity
	// with MapTypeToSQLServer for the same LogicalType).
	switch (type.id()) {
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
		break;
	}

	try {
		return DispatchDdlTypeName(type, config, mssql::codec::DdlContext::CtasCreateTable);
	} catch (const NotImplementedException &) {
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

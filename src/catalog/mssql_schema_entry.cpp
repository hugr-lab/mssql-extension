#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Helper: Create a CreateSchemaInfo from a name
//===----------------------------------------------------------------------===//

static CreateSchemaInfo MakeSchemaInfo(const string &name) {
	CreateSchemaInfo info;
	info.schema = name;
	info.internal = false;
	return info;
}

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

MSSQLSchemaEntry::MSSQLSchemaEntry(Catalog &catalog, const string &name)
	: SchemaCatalogEntry(catalog,
						 [&]() -> CreateSchemaInfo & {
							 static thread_local CreateSchemaInfo info;
							 info = MakeSchemaInfo(name);
							 return info;
						 }()),
	  tables_(*this) {}

MSSQLSchemaEntry::~MSSQLSchemaEntry() = default;

//===----------------------------------------------------------------------===//
// Entry Access
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLSchemaEntry::LookupEntry(CatalogTransaction transaction,
														 const EntryLookupInfo &lookup_info) {
	CatalogType type = lookup_info.GetCatalogType();
	const string &name = lookup_info.GetEntryName();

	if (type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}

	// Check if context exists
	if (!transaction.HasContext()) {
		return nullptr;
	}

	// Lookup table in our table set
	return tables_.GetEntry(transaction.GetContext(), name);
}

void MSSQLSchemaEntry::Scan(ClientContext &context, CatalogType type,
							const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}

	// Scan all tables
	tables_.Scan(context, callback);
}

void MSSQLSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// This variant without context may be called during catalog operations
	// We can't load tables without context, so this does nothing meaningful
	// The main Scan variant with ClientContext should be used instead
}

//===----------------------------------------------------------------------===//
// Write Operations - Check access mode and throw appropriate error
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	// Check if catalog is read-only
	auto &mssql_catalog = GetMSSQLCatalog();
	mssql_catalog.CheckWriteAccess("CREATE TABLE");

	// Get table name, columns, and constraints from bound info
	auto &base_info = info.Base();
	string table_name = base_info.table;

	// Extract columns from the bound info
	// BoundCreateTableInfo contains the resolved column definitions
	auto &columns = base_info.columns;

	// Extract constraints (includes PRIMARY KEY, UNIQUE, etc.)
	auto &constraints = base_info.constraints;

	// Generate T-SQL for CREATE TABLE (with constraints)
	string tsql = MSSQLDDLTranslator::TranslateCreateTable(name, table_name, columns, constraints);

	// Execute DDL on SQL Server
	if (transaction.HasContext()) {
		mssql_catalog.ExecuteDDL(transaction.GetContext(), tsql);
	} else {
		throw InternalException("Cannot execute CREATE TABLE without client context");
	}

	// Point invalidation: invalidate schema's table list and local table set
	mssql_catalog.InvalidateSchemaTableSet(name);

	// Look up the newly created table (triggers lazy load of table list)
	return tables_.GetEntry(transaction.GetContext(), table_name);
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE FUNCTION");
	throw NotImplementedException("MSSQL catalog: CREATE FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
														 TableCatalogEntry &table) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE INDEX");
	throw NotImplementedException(
		"MSSQL catalog: CREATE INDEX via DDL is not yet implemented. "
		"Use mssql_exec() to execute T-SQL directly.");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE VIEW");
	throw NotImplementedException(
		"MSSQL catalog: CREATE VIEW via DDL is not yet implemented. "
		"Use mssql_exec() to execute T-SQL directly.");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE SEQUENCE");
	throw NotImplementedException("MSSQL catalog: CREATE SEQUENCE is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
																 CreateTableFunctionInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE TABLE FUNCTION");
	throw NotImplementedException("MSSQL catalog: CREATE TABLE FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
																CreateCopyFunctionInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE COPY FUNCTION");
	throw NotImplementedException("MSSQL catalog: CREATE COPY FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
																  CreatePragmaFunctionInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE PRAGMA FUNCTION");
	throw NotImplementedException("MSSQL catalog: CREATE PRAGMA FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateCollation(CatalogTransaction transaction,
															 CreateCollationInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE COLLATION");
	throw NotImplementedException("MSSQL catalog: CREATE COLLATION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	GetMSSQLCatalog().CheckWriteAccess("CREATE TYPE");
	throw NotImplementedException("MSSQL catalog: CREATE TYPE is not supported");
}

void MSSQLSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	auto &mssql_catalog = GetMSSQLCatalog();
	mssql_catalog.CheckWriteAccess("ALTER");

	// Check if this is an ALTER TABLE operation
	if (info.type != AlterType::ALTER_TABLE) {
		throw NotImplementedException(
			"MSSQL catalog: ALTER %s via DDL is not yet implemented. "
			"Use mssql_exec() to execute T-SQL directly.",
			EnumUtil::ToString(info.type));
	}

	auto &alter_table_info = info.Cast<AlterTableInfo>();
	string tsql;

	// The table name is stored in the base AlterInfo class
	const string &table_name = alter_table_info.name;

	switch (alter_table_info.alter_table_type) {
	case AlterTableType::RENAME_TABLE: {
		auto &rename_info = alter_table_info.Cast<RenameTableInfo>();
		tsql = MSSQLDDLTranslator::TranslateRenameTable(name, table_name, rename_info.new_table_name);
		break;
	}

	case AlterTableType::ADD_COLUMN: {
		auto &add_info = alter_table_info.Cast<AddColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateAddColumn(name, table_name, add_info.new_column);
		break;
	}

	case AlterTableType::REMOVE_COLUMN: {
		auto &remove_info = alter_table_info.Cast<RemoveColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateDropColumn(name, table_name, remove_info.removed_column);
		break;
	}

	case AlterTableType::RENAME_COLUMN: {
		auto &rename_col_info = alter_table_info.Cast<RenameColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateRenameColumn(name, table_name, rename_col_info.old_name,
														 rename_col_info.new_name);
		break;
	}

	case AlterTableType::ALTER_COLUMN_TYPE: {
		auto &type_info = alter_table_info.Cast<ChangeColumnTypeInfo>();
		// SQL Server requires specifying nullability when altering type
		// We default to NULL since we don't have that info easily available
		tsql = MSSQLDDLTranslator::TranslateAlterColumnType(name, table_name, type_info.column_name,
															type_info.target_type, true);
		break;
	}

	case AlterTableType::SET_NOT_NULL: {
		auto &notnull_info = alter_table_info.Cast<SetNotNullInfo>();
		// For SET NOT NULL, we need the column's current type
		// Look up the table to get the column type
		if (!transaction.HasContext()) {
			throw InternalException("Cannot execute SET NOT NULL without client context");
		}
		auto entry = tables_.GetEntry(transaction.GetContext(), table_name);
		if (!entry) {
			throw CatalogException("Table '%s' not found", table_name);
		}
		auto &mssql_table = entry->Cast<MSSQLTableEntry>();
		LogicalType col_type;
		bool found = false;
		for (auto &col : mssql_table.GetMSSQLColumns()) {
			if (col.name == notnull_info.column_name) {
				col_type = col.duckdb_type;
				found = true;
				break;
			}
		}
		if (!found) {
			throw CatalogException("Column '%s' not found in table '%s'", notnull_info.column_name, table_name);
		}
		tsql = MSSQLDDLTranslator::TranslateAlterColumnNullability(name, table_name, notnull_info.column_name, col_type,
																   true);
		break;
	}

	case AlterTableType::DROP_NOT_NULL: {
		auto &dropnull_info = alter_table_info.Cast<DropNotNullInfo>();
		// For DROP NOT NULL, we need the column's current type
		if (!transaction.HasContext()) {
			throw InternalException("Cannot execute DROP NOT NULL without client context");
		}
		auto entry = tables_.GetEntry(transaction.GetContext(), table_name);
		if (!entry) {
			throw CatalogException("Table '%s' not found", table_name);
		}
		auto &mssql_table = entry->Cast<MSSQLTableEntry>();
		LogicalType col_type;
		bool found = false;
		for (auto &col : mssql_table.GetMSSQLColumns()) {
			if (col.name == dropnull_info.column_name) {
				col_type = col.duckdb_type;
				found = true;
				break;
			}
		}
		if (!found) {
			throw CatalogException("Column '%s' not found in table '%s'", dropnull_info.column_name, table_name);
		}
		tsql = MSSQLDDLTranslator::TranslateAlterColumnNullability(name, table_name, dropnull_info.column_name,
																   col_type, false);
		break;
	}

	default:
		throw NotImplementedException(
			"MSSQL catalog: ALTER TABLE %s via DDL is not yet implemented. "
			"Use mssql_exec() to execute T-SQL directly.",
			EnumUtil::ToString(alter_table_info.alter_table_type));
	}

	// Execute DDL on SQL Server
	if (!transaction.HasContext()) {
		throw InternalException("Cannot execute ALTER without client context");
	}
	mssql_catalog.ExecuteDDL(transaction.GetContext(), tsql);

	// Point invalidation: invalidate the altered table's column metadata
	mssql_catalog.GetMetadataCache().InvalidateTable(name, table_name);

	// Invalidate the local table set cache to pick up column changes
	mssql_catalog.InvalidateSchemaTableSet(name);
}

void MSSQLSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	auto &mssql_catalog = GetMSSQLCatalog();
	mssql_catalog.CheckWriteAccess("DROP");

	// Handle DROP TABLE
	if (info.type == CatalogType::TABLE_ENTRY) {
		// Generate T-SQL for DROP TABLE
		string tsql = MSSQLDDLTranslator::TranslateDropTable(name, info.name);

		// Execute DDL on SQL Server
		mssql_catalog.ExecuteDDL(context, tsql);

		// Point invalidation: invalidate schema's table list and local table set
		mssql_catalog.InvalidateSchemaTableSet(name);
		return;
	}

	// Other drop types not yet implemented
	throw NotImplementedException(
		"MSSQL catalog: DROP %s via DDL is not yet implemented. "
		"Use mssql_exec() to execute T-SQL directly.",
		CatalogTypeToString(info.type));
}

//===----------------------------------------------------------------------===//
// MSSQL-specific
//===----------------------------------------------------------------------===//

MSSQLCatalog &MSSQLSchemaEntry::GetMSSQLCatalog() {
	return catalog.Cast<MSSQLCatalog>();
}

MSSQLTableSet &MSSQLSchemaEntry::GetTableSet() {
	return tables_;
}

}  // namespace duckdb

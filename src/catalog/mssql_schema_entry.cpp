#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

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
    : SchemaCatalogEntry(catalog, [&]() -> CreateSchemaInfo& {
          static thread_local CreateSchemaInfo info;
          info = MakeSchemaInfo(name);
          return info;
      }()), tables_(*this) {
}

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
// Write Operations (all throw - read-only catalog)
//===----------------------------------------------------------------------===//

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                          BoundCreateTableInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE TABLE is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                             CreateFunctionInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateIndex(CatalogTransaction transaction,
                                                          CreateIndexInfo &info,
                                                          TableCatalogEntry &table) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE INDEX is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateView(CatalogTransaction transaction,
                                                         CreateViewInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE VIEW is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                             CreateSequenceInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE SEQUENCE is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                  CreateTableFunctionInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE TABLE FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                 CreateCopyFunctionInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE COPY FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                   CreatePragmaFunctionInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE PRAGMA FUNCTION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                              CreateCollationInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE COLLATION is not supported");
}

optional_ptr<CatalogEntry> MSSQLSchemaEntry::CreateType(CatalogTransaction transaction,
                                                         CreateTypeInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE TYPE is not supported");
}

void MSSQLSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: ALTER is not supported");
}

void MSSQLSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw NotImplementedException("MSSQL catalog is read-only: DROP is not supported");
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

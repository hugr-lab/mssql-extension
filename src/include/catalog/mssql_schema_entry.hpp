#pragma once

#include <memory>
#include "catalog/mssql_table_set.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Forward declarations
//===----------------------------------------------------------------------===//

class MSSQLCatalog;

//===----------------------------------------------------------------------===//
// MSSQLSchemaEntry - DuckDB schema entry for SQL Server schema
//===----------------------------------------------------------------------===//

class MSSQLSchemaEntry : public SchemaCatalogEntry {
public:
	// Constructor
	MSSQLSchemaEntry(Catalog &catalog, const string &name);

	~MSSQLSchemaEntry() override;

	//===----------------------------------------------------------------------===//
	// Required Overrides
	//===----------------------------------------------------------------------===//

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;

	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
										   TableCatalogEntry &table) override;

	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;

	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;

	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
												   CreateTableFunctionInfo &info) override;

	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
												  CreateCopyFunctionInfo &info) override;

	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
													CreatePragmaFunctionInfo &info) override;

	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;

	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;

	void Alter(CatalogTransaction transaction, AlterInfo &info) override;

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	void DropEntry(ClientContext &context, DropInfo &info) override;

	// LookupEntry is the new virtual method (replaces GetEntry)
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	//===----------------------------------------------------------------------===//
	// MSSQL-specific
	//===----------------------------------------------------------------------===//

	// Get parent MSSQL catalog
	MSSQLCatalog &GetMSSQLCatalog();

	// Get table set for lazy loading
	MSSQLTableSet &GetTableSet();

private:
	MSSQLTableSet tables_;	// Lazy-loaded tables and views
};

}  // namespace duckdb

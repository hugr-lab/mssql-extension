//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// catalog/backup/mssql_backup_catalog.hpp
//
// Spec 085 task 23 (PR 4b, D1): a SQL Server full backup attached as a read-only DuckDB catalog,
// `ATTACH 'x.bak' AS bk (TYPE mssql_backup)`. A second StorageExtension beside TYPE mssql, with its own
// trivial transaction manager and no pool, no remote pushdown, no MSSQLOptimizer rewrites (they ask
// for GetCatalogType() == "mssql"). One schema entry per schema of the model, one table entry per user
// table (views are not entries: T-SQL is not DuckDB SQL); a table entry's scan is mssql_backup_scan on
// the catalog's AttachedBackup. The file functions take the catalog's name in place of a path.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_opened.hpp"
#include "backup/backup_scan_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <map>
#include <memory>
#include <string>

namespace duckdb {

class MSSQLBackupSchemaEntry;

//! The catalog of an attached backup. Built whole at ATTACH (the model, its schemas and tables), never
//! changed after: no lock is needed to read it.
class MSSQLBackupCatalog : public Catalog {
public:
	static constexpr const char *TYPE = "mssql_backup";

	MSSQLBackupCatalog(AttachedDatabase &db, std::shared_ptr<const mssql::backup::AttachedBackup> backup);
	~MSSQLBackupCatalog() override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return TYPE;
	}
	optional<Identifier> GetDefaultSchema() const override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
												  OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void DropSchema(ClientContext &context, DropInfo &info) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
										PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
								 optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
								 PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
								 PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override;
	//! ATTACH OR REPLACE attaches anew: the file and the filters are read again, never the old catalog
	//! kept for the same path.
	bool HasConflictingAttachOptions(const string &path, const AttachOptions &options) override {
		return true;
	}

	const mssql::backup::AttachedBackup &Backup() const {
		return *backup_;
	}
	std::shared_ptr<const mssql::backup::AttachedBackup> SharedBackup() const {
		return backup_;
	}

	//! A refusal of anything but a read, naming the catalog (DuckDB's read-only check comes first; this is
	//! what is left for the paths it does not cover: INSERT, UPDATE, DELETE, MERGE, TRUNCATE are planned
	//! before it).
	[[noreturn]] void RefuseWrite() const;

private:
	std::shared_ptr<const mssql::backup::AttachedBackup> backup_;
	std::map<std::string, unique_ptr<MSSQLBackupSchemaEntry>> schemas_;
};

//! A schema of the backup: its user tables the ATTACH's filters keep.
class MSSQLBackupSchemaEntry : public SchemaCatalogEntry {
public:
	MSSQLBackupSchemaEntry(MSSQLBackupCatalog &catalog, CreateSchemaInfo &info);

	void AddTable(const mssql::backup::CatalogTable &table);

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

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
	void DropEntry(ClientContext &context, DropInfo &info) override;

private:
	MSSQLBackupCatalog &BackupCatalog() const;

	std::map<std::string, unique_ptr<CatalogEntry>> tables_;
};

//! A user table of the backup: its columns as mssql_backup_scan reads them, D8's virtual columns, and
//! the scan bound on the catalog's opened backup.
class MSSQLBackupTableEntry : public TableCatalogEntry {
public:
	MSSQLBackupTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
						  const mssql::backup::CatalogTable &table, mssql::backup::ScanColumns columns);

	const ColumnList &GetColumns() const override {
		return columns_;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	virtual_column_map_t GetVirtualColumns() const override;
	//! None: a backup has no row id DuckDB could write through.
	vector<column_t> GetRowIdColumns() const override {
		return {};
	}

private:
	ColumnList columns_;
	std::vector<mssql::backup::UnreadColumn> unread_;
	std::string schema_name_;  // as the backup spells them
	std::string table_name_;
	int64_t row_count_ = -1;  // the catalog's, -1 unknown
};

//! A backup is read, never written: a transaction holds nothing.
class MSSQLBackupTransactionManager : public TransactionManager {
public:
	explicit MSSQLBackupTransactionManager(AttachedDatabase &db) : TransactionManager(db) {}
	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override {}

private:
	std::mutex lock_;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions_;
};

void RegisterMSSQLBackupStorageExtension(ExtensionLoader &loader);

}  // namespace duckdb

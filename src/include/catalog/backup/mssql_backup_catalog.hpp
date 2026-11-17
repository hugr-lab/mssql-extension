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
// Task 24b (PR 4c-2, DQ-48): the catalog has a history -- the versions of a set of backups, `history
// true`, or of the one it attached (a full, and a full with its differential) -- and AT (VERSION / TIMESTAMP)
// picks one: each version its own schema and table entries over its own AttachedBackup, built at its
// first lookup (the default one at ATTACH).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_opened.hpp"
#include "backup/backup_scan_function.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {

class MSSQLBackupSchemaEntry;

//! The catalog of an attached backup. Each version's entries are built whole (the model, its schemas
//! and tables) at its first lookup and never changed after; the versions are kept for the catalog's life.
class MSSQLBackupCatalog : public Catalog {
public:
	static constexpr const char *TYPE = "mssql_backup";
	//! How a version's backup is opened for the catalog (the ATTACH's name and filters).
	using Opener = std::function<std::shared_ptr<const mssql::backup::AttachedBackup>(
		ClientContext &, const mssql::backup::HistoryVersion &)>;

	//! `default_version`: the version read without AT, `default_backup` its backup, opened by the ATTACH.
	MSSQLBackupCatalog(AttachedDatabase &db, std::string name,
					   std::shared_ptr<const mssql::backup::BackupHistory> history, uint64_t default_version,
					   std::shared_ptr<const mssql::backup::AttachedBackup> default_backup, Opener opener);
	~MSSQLBackupCatalog() override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return TYPE;
	}
	optional<Identifier> GetDefaultSchema() const override;
	bool SupportsTimeTravel() const override {
		return true;
	}

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

	const mssql::backup::BackupHistory &History() const {
		return *history_;
	}
	std::shared_ptr<const mssql::backup::BackupHistory> SharedHistory() const {
		return history_;
	}
	uint64_t DefaultVersion() const {
		return default_version_;
	}
	const std::string &Name() const {
		return name_;
	}
	//! The version an AT clause names -- `unit` VERSION (a number) or TIMESTAMP (the last version finished
	//! by it) --, the default one when `unit` is empty. A BinderException naming the catalog otherwise.
	uint64_t VersionAt(optional_ptr<ClientContext> context, const std::string &unit, const Value &value) const;
	//! The backup of `version`, opened now when it was not.
	std::shared_ptr<const mssql::backup::AttachedBackup> BackupOf(ClientContext &context, uint64_t version);

	//! A refusal of anything but a read, naming the catalog (DuckDB's read-only check comes first; this is
	//! what is left for the paths it does not cover: INSERT, UPDATE, DELETE, MERGE, TRUNCATE are planned
	//! before it).
	[[noreturn]] void RefuseWrite() const;

private:
	struct State {
		std::shared_ptr<const mssql::backup::AttachedBackup> backup;
		std::map<std::string, unique_ptr<MSSQLBackupSchemaEntry>> schemas;
	};
	//! The entries of `version`, built now when they are not: opened outside the lock (two lookups may
	//! open one version together; the first kept).
	State &StateOf(optional_ptr<ClientContext> context, uint64_t version);
	unique_ptr<State> MakeState(std::shared_ptr<const mssql::backup::AttachedBackup> backup);
	State &DefaultState() const {
		return *states_[default_version_ - 1];
	}

	std::string name_;
	std::shared_ptr<const mssql::backup::BackupHistory> history_;
	uint64_t default_version_;
	Opener opener_;
	std::mutex lock_;
	std::vector<unique_ptr<State>> states_;	 // by version - 1
};

//! A schema of one version of the backup: its user tables the ATTACH's filters keep, and `snapshots()`
//! (DuckLake's form: `FROM bk.snapshots()` is `mssql_backup_snapshots('bk')`).
class MSSQLBackupSchemaEntry : public SchemaCatalogEntry {
public:
	MSSQLBackupSchemaEntry(MSSQLBackupCatalog &catalog, CreateSchemaInfo &info,
						   std::shared_ptr<const mssql::backup::AttachedBackup> backup);

	//! The backup of the version this schema is of.
	const std::shared_ptr<const mssql::backup::AttachedBackup> &Backup() const {
		return backup_;
	}

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

	std::shared_ptr<const mssql::backup::AttachedBackup> backup_;
	std::map<std::string, unique_ptr<CatalogEntry>> tables_;
	unique_ptr<CatalogEntry> snapshots_;
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

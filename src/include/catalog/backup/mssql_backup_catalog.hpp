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
#include "duckdb/function/macro_function.hpp"
#include "duckdb/main/client_context_state.hpp"
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
//! and tables) at its first lookup and never changed after; a version's entries are kept for the
//! catalog's life, by its backups (a refresh renumbers the versions, never drops what was read).
class MSSQLBackupCatalog : public Catalog {
public:
	static constexpr const char *TYPE = "mssql_backup";
	//! How a version's backup is opened for the catalog (the ATTACH's name and filters).
	using Opener = std::function<std::shared_ptr<const mssql::backup::AttachedBackup>(
		ClientContext &, const mssql::backup::BackupHistory &, const mssql::backup::HistoryVersion &)>;
	//! How the history is read again (mssql_backup_refresh), from the one read last; none for a backup
	//! attached without `history`.
	using Reader = std::function<std::shared_ptr<const mssql::backup::BackupHistory>(
		ClientContext &, const mssql::backup::BackupHistory &previous)>;

	//! The history and the version read without AT, as of now: replaced whole by a refresh.
	struct Versions {
		std::shared_ptr<const mssql::backup::BackupHistory> history;
		uint64_t default_version = 0;
	};

	//! `default_at`: the version read without AT (none: the latest), `default_backup` its backup, opened
	//! by the ATTACH.
	MSSQLBackupCatalog(AttachedDatabase &db, std::string name,
					   std::shared_ptr<const mssql::backup::BackupHistory> history, mssql::backup::BackupAt default_at,
					   uint64_t default_version, std::shared_ptr<const mssql::backup::AttachedBackup> default_backup,
					   Opener opener, Reader reader);
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

	//! The versions now. A version named by number or time is resolved against them when a query is bound and
	//! pinned by its key (BackupAt::key) in the bind data, serialized with the plan (owner, 2026-11-19: no
	//! transaction keeps versions, and no connection state either): a refresh reaches the next bind.
	std::shared_ptr<const Versions> CurrentVersions() const;
	//! A query's bind: the version `at` names resolved, its state held by the query to its end, and its key
	//! (PinAttachedVersion), from one read of the versions.
	std::string Pin(ClientContext &context, const mssql::backup::BackupAt &at);
	//! The table macros every schema answers (DuckLake's `db.snapshots()` form), parsed once.
	const std::vector<std::pair<std::string, unique_ptr<MacroFunction>>> &Macros() const {
		return macros_;
	}
	const std::string &Name() const {
		return name_;
	}
	//! The backup of the version `at` names (by its key when pinned; else AT's VERSION a number, TIMESTAMP
	//! the last version finished by it; none: the default), opened now when it was not. A BinderException
	//! naming the catalog when there is no such version; an InvalidInputException when the pinned one is
	//! no longer among the files (a refresh dropped it: the query is to run again).
	std::shared_ptr<const mssql::backup::AttachedBackup> BackupAt(optional_ptr<ClientContext> context,
																  const mssql::backup::BackupAt &at);
	//! Task 24b (owner, 2026-11-17): the files the ATTACH named read again -- the headers of new or
	//! changed files, the others kept -- and the versions renumbered; the default version's entries built
	//! now. Returns the number of versions before and after. Refused by name for a backup attached
	//! without `history`.
	std::pair<uint64_t, uint64_t> Refresh(ClientContext &context);

	//! A version's backups and their files (the backup set GUIDs of its full and differential, each
	//! file's path, size and version tag): what a state is kept by, so a file changed or moved since is a
	//! new state, opened and checked anew.
	static std::string StateKey(const mssql::backup::BackupHistory &history,
								const mssql::backup::HistoryVersion &version);
	//! The same key of an opened version's backup.
	static std::string StateKey(const mssql::backup::AttachedBackup &backup);
	//! A version's backups alone (their backup set GUIDs): what snapshot_version / snapshot_time pin.
	static std::string Identity(const mssql::backup::BackupHistory &history,
								const mssql::backup::HistoryVersion &version);

	//! A refusal of anything but a read, naming the catalog (DuckDB's read-only check comes first; this is
	//! what is left for the paths it does not cover: INSERT, UPDATE, DELETE, MERGE, TRUNCATE are planned
	//! before it).
	[[noreturn]] void RefuseWrite() const;

	//! One version's entries: shared by the catalog (states_) and every transaction that read it.
	struct State {
		std::shared_ptr<const mssql::backup::AttachedBackup> backup;
		std::map<std::string, unique_ptr<MSSQLBackupSchemaEntry>> schemas;
	};

private:
	//! The version `at` names in `versions` (its default when none).
	uint64_t Resolve(const Versions &versions, optional_ptr<ClientContext> context,
					 const mssql::backup::BackupAt &at) const;
	//! The entries of `version` of `versions`: the ones `context`'s query read, else the catalog's, else
	//! built now -- opened outside the lock (two lookups may open one version together; the first kept). A
	//! query keeps what it reads to its end (MSSQLBackupQueryStates), so the entries its plan holds outlive
	//! a refresh that drops them from the catalog (review of #579).
	std::shared_ptr<State> StateOf(optional_ptr<ClientContext> context, const Versions &versions, uint64_t version);
	unique_ptr<State> MakeState(std::shared_ptr<const mssql::backup::AttachedBackup> backup);
	//! The default version's entries, held by the caller while it reads them: a refresh may drop them from
	//! the catalog meanwhile (review of #579).
	std::shared_ptr<State> DefaultState() const;

	std::string name_;
	//! The version snapshot_version / snapshot_time chose at ATTACH, by its backups; empty: the latest.
	std::string pinned_;
	Opener opener_;
	Reader reader_;
	std::vector<std::pair<std::string, unique_ptr<MacroFunction>>> macros_;
	//! One refresh at a time, across its read and its swap.
	std::mutex refresh_lock_;
	mutable std::mutex lock_;
	std::shared_ptr<const Versions> versions_;
	//! By StateKey. A refresh drops the states of versions the history no longer has; a query that read one
	//! keeps it (MSSQLBackupQueryStates) to its end (review of #579).
	std::map<std::string, std::shared_ptr<State>> states_;
};

//! A schema of one version of the backup: its user tables the ATTACH's filters keep, and the file
//! functions on the catalog's name as table macros, DuckLake's form: `FROM bk.snapshots()` is
//! `mssql_backup_snapshots('bk')`; `refresh()`, and `info()` / `schemas()` / `tables()` / `views()` /
//! `columns()` with `version :=` / `as_of :=`.
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
	std::map<std::string, unique_ptr<CatalogEntry>> functions_;	 // the table macros, by lower-case name
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

//! The version states one query read of a backup catalog, by StateKey (registered on its ClientContext,
//! cleared when a query begins and ends): the entries its plan holds live to its end though a refresh drops
//! them from the catalog -- the anchors TYPE mssql's catalog keeps for its entries (MSSQLBindAnchors). No
//! version is chosen here: the bind data pins it (BackupAt::key).
class MSSQLBackupQueryStates : public ClientContextState {
public:
	void QueryBegin(ClientContext &context) override;
	void QueryEnd(ClientContext &context) override;
	//! The catalog's in `context`.
	static MSSQLBackupQueryStates &For(ClientContext &context, const MSSQLBackupCatalog &catalog);

	std::mutex lock;
	std::map<std::string, std::shared_ptr<MSSQLBackupCatalog::State>> states;
};

//! A backup is read, never written: a transaction holds nothing (a query keeps what it reads,
//! MSSQLBackupQueryStates).
class MSSQLBackupTransaction : public Transaction {
public:
	MSSQLBackupTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {}
};

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

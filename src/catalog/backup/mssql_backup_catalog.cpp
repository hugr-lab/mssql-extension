//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// catalog/backup/mssql_backup_catalog.cpp
//
// Spec 085 task 23 (PR 4b, D1): the catalog of an attached backup, its schema and table entries, its
// transaction manager and TYPE mssql_backup's StorageExtension.
//===----------------------------------------------------------------------===//

#include "catalog/backup/mssql_backup_catalog.hpp"

#include "backup/backup_function_common.hpp"

#include "catalog/mssql_catalog_filter.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

using mssql::backup::AttachedBackup;
using mssql::backup::CatalogTable;

//===----------------------------------------------------------------------===//
// The catalog
//===----------------------------------------------------------------------===//

MSSQLBackupCatalog::MSSQLBackupCatalog(AttachedDatabase &db, std::shared_ptr<const AttachedBackup> backup)
	: Catalog(db), backup_(std::move(backup)) {}

MSSQLBackupCatalog::~MSSQLBackupCatalog() = default;

void MSSQLBackupCatalog::Initialize(bool) {
	const auto &model = *backup_->model;
	for (const auto &s : model.schemas) {
		if (!backup_->WantsSchema(s.name)) {
			continue;
		}
		CreateSchemaInfo info;
		info.SetSchema(Identifier(s.name));
		info.internal = false;
		schemas_[s.name] = make_uniq<MSSQLBackupSchemaEntry>(*this, info);
	}
	for (const auto &t : model.tables) {
		auto schema = schemas_.find(t.schema_name);
		if (schema != schemas_.end()) {
			schema->second->AddTable(t);
		}
	}
}

optional<Identifier> MSSQLBackupCatalog::GetDefaultSchema() const {
	// dbo, as SQL Server resolves a one-part name; the first schema the filter kept when it hid dbo.
	if (schemas_.empty() || schemas_.count("dbo")) {
		return Identifier("dbo");
	}
	return Identifier(schemas_.begin()->first);
}

//! The entry of `name` in `entries`: the one spelled so, else the one name that differs only in case
//! (SQL Server's names are case-insensitive under its default collations). None when two do.
template <class T>
static T *FindByName(const std::map<std::string, unique_ptr<T>> &entries, const std::string &name) {
	auto exact = entries.find(name);
	if (exact != entries.end()) {
		return exact->second.get();
	}
	T *found = nullptr;
	for (const auto &e : entries) {
		if (StringUtil::CIEquals(e.first, name)) {
			if (found) {
				return nullptr;
			}
			found = e.second.get();
		}
	}
	return found;
}

optional_ptr<SchemaCatalogEntry> MSSQLBackupCatalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &lookup,
																  OnEntryNotFound if_not_found) {
	auto found = FindByName(schemas_, lookup.GetEntryName());
	if (!found && if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema '%s' not found in the backup attached as %s", lookup.GetEntryName(),
							   backup_->name);
	}
	return found;
}

void MSSQLBackupCatalog::ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) {
	for (auto &s : schemas_) {
		callback(*s.second);
	}
}

void MSSQLBackupCatalog::RefuseWrite() const {
	throw PermissionException("%s is a backup attached read-only (TYPE mssql_backup): nothing is written to it",
							  backup_->name);
}

optional_ptr<CatalogEntry> MSSQLBackupCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	RefuseWrite();
}
void MSSQLBackupCatalog::DropSchema(ClientContext &, DropInfo &) {
	RefuseWrite();
}
PhysicalOperator &MSSQLBackupCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
														PhysicalOperator &) {
	RefuseWrite();
}
PhysicalOperator &MSSQLBackupCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
												 optional_ptr<PhysicalOperator>) {
	RefuseWrite();
}
PhysicalOperator &MSSQLBackupCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
												 PhysicalOperator &) {
	RefuseWrite();
}
PhysicalOperator &MSSQLBackupCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
												 PhysicalOperator &) {
	RefuseWrite();
}

DatabaseSize MSSQLBackupCatalog::GetDatabaseSize(ClientContext &) {
	DatabaseSize size;
	for (const auto &file : backup_->backup->Headers().files) {
		size.bytes += file.size;
	}
	return size;
}

string MSSQLBackupCatalog::GetDBPath() {
	return backup_->backup->Headers().files.front().path;
}

//===----------------------------------------------------------------------===//
// Schema entries
//===----------------------------------------------------------------------===//

MSSQLBackupSchemaEntry::MSSQLBackupSchemaEntry(MSSQLBackupCatalog &catalog, CreateSchemaInfo &info)
	: SchemaCatalogEntry(catalog, info) {}

MSSQLBackupCatalog &MSSQLBackupSchemaEntry::BackupCatalog() const {
	return catalog.Cast<MSSQLBackupCatalog>();
}

void MSSQLBackupSchemaEntry::AddTable(const CatalogTable &table) {
	auto columns = mssql::backup::ScanColumnsOf(table);
	CreateTableInfo info;
	info.SetTableName(Identifier(table.name));
	for (size_t i = 0; i < columns.names.size(); i++) {
		info.columns.AddColumn(ColumnDefinition(Identifier(columns.names[i]), columns.types[i]));
	}
	tables_[table.name] = make_uniq<MSSQLBackupTableEntry>(catalog, *this, info, table, std::move(columns));
}

optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::LookupEntry(CatalogTransaction, const EntryLookupInfo &lookup) {
	if (lookup.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return FindByName(tables_, lookup.GetEntryName());
}

void MSSQLBackupSchemaEntry::Scan(ClientContext &, CatalogType type,
								  const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, callback);
}

void MSSQLBackupSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &t : tables_) {
		callback(*t.second);
	}
}

optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
															   TableCatalogEntry &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreatePragmaFunction(CatalogTransaction,
																		CreatePragmaFunctionInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	BackupCatalog().RefuseWrite();
}
optional_ptr<CatalogEntry> MSSQLBackupSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	BackupCatalog().RefuseWrite();
}
void MSSQLBackupSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	BackupCatalog().RefuseWrite();
}
void MSSQLBackupSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	BackupCatalog().RefuseWrite();
}

//===----------------------------------------------------------------------===//
// Table entries
//===----------------------------------------------------------------------===//

MSSQLBackupTableEntry::MSSQLBackupTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
											 const CatalogTable &table, mssql::backup::ScanColumns columns)
	: TableCatalogEntry(catalog, schema, info),
	  columns_(info.columns.Copy()),
	  unread_(std::move(columns.unread)),
	  schema_name_(table.schema_name),
	  table_name_(table.name),
	  row_count_(table.row_count_known ? table.row_count : -1) {}

unique_ptr<BaseStatistics> MSSQLBackupTableEntry::GetStatistics(ClientContext &, column_t) {
	return nullptr;
}

TableFunction MSSQLBackupTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	return mssql::backup::AttachedScanFunction(context, *this, catalog.Cast<MSSQLBackupCatalog>().SharedBackup(),
											   schema_name_, table_name_, bind_data);
}

TableStorageInfo MSSQLBackupTableEntry::GetStorageInfo(ClientContext &) {
	TableStorageInfo info;
	if (row_count_ >= 0) {
		info.cardinality = static_cast<idx_t>(row_count_);
	}
	return info;
}

virtual_column_map_t MSSQLBackupTableEntry::GetVirtualColumns() const {
	return mssql::backup::ScanVirtualColumns(unread_);
}

//===----------------------------------------------------------------------===//
// Transactions
//===----------------------------------------------------------------------===//

// Keyed by the transaction, not its context: a context destroyed with a transaction open leaves no key
// behind.
Transaction &MSSQLBackupTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<Transaction>(*this, context);
	auto &result = *transaction;
	std::lock_guard<std::mutex> guard(lock_);
	transactions_[result] = std::move(transaction);
	return result;
}

ErrorData MSSQLBackupTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
	std::lock_guard<std::mutex> guard(lock_);
	transactions_.erase(transaction);
	return ErrorData();
}

void MSSQLBackupTransactionManager::RollbackTransaction(Transaction &transaction) {
	std::lock_guard<std::mutex> guard(lock_);
	transactions_.erase(transaction);
}

//===----------------------------------------------------------------------===//
// ATTACH … (TYPE mssql_backup)
//===----------------------------------------------------------------------===//

//! The attached backup catalogs of this DuckDB instance are found through its DatabaseManager, outside
//! any transaction (a file function's open may run in none); what is found is held by the caller's
//! shared_ptr, so a DETACH meanwhile leaves the statement its backup.
std::shared_ptr<const AttachedBackup> mssql::backup::FindAttachedBackup(ClientContext &context,
																		const std::string &name) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db) {
		return nullptr;
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		return nullptr;
	}
	return catalog.Cast<MSSQLBackupCatalog>().SharedBackup();
}

static unique_ptr<Catalog> MSSQLBackupAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context,
											 AttachedDatabase &db, const string &name, AttachInfo &info,
											 AttachOptions &options) {
	// schema_filter / table_filter keep what they match; schema_exclude / table_exclude drop what they
	// match. All regexes as TYPE mssql's: case-insensitive, a partial match.
	auto filter = std::make_shared<MSSQLCatalogFilter>();
	auto exclude = std::make_shared<MSSQLCatalogFilter>();
	std::vector<std::string> diff;
	for (auto it = options.options.begin(); it != options.options.end();) {
		const std::string key = StringUtil::Lower(it->first);
		if (key == "diff") {
			// Task 24 (D12): a differential laid over the attached full.
			if (!it->second.IsNull()) {
				for (auto &path : mssql::backup::PathsOf(it->second, "ATTACH (TYPE mssql_backup) diff")) {
					diff.push_back(path);
				}
			}
			it = options.options.erase(it);
			continue;
		}
		if (key != "schema_filter" && key != "table_filter" && key != "schema_exclude" && key != "table_exclude") {
			++it;
			continue;
		}
		const std::string pattern = it->second.IsNull() ? std::string() : it->second.ToString();
		const std::string invalid = MSSQLCatalogFilter::ValidatePattern(pattern);
		if (!invalid.empty()) {
			throw InvalidInputException("ATTACH %s (TYPE mssql_backup): %s: %s", name, key, invalid);
		}
		if (key == "schema_filter") {
			filter->SetSchemaFilter(pattern);
		} else if (key == "table_filter") {
			filter->SetTableFilter(pattern);
		} else if (key == "schema_exclude") {
			exclude->SetSchemaFilter(pattern);
		} else {
			exclude->SetTableFilter(pattern);
		}
		it = options.options.erase(it);
	}
	// DuckDB hands a storage extension the options it does not know itself: a misspelt one is refused,
	// not ignored.
	for (const auto &option : options.options) {
		throw InvalidInputException(
			"ATTACH %s (TYPE mssql_backup): '%s' is not an option of an attached backup "
			"(schema_filter, table_filter, schema_exclude, table_exclude, diff)",
			name, option.first);
	}
	// Always read-only (D1): DuckDB's own check refuses every write before the catalog sees it.
	db.SetReadOnlyDatabase();

	std::function<bool(const std::string &)> wants_schema;
	std::function<bool(const std::string &, const std::string &)> wants_object;
	if (filter->HasFilters() || exclude->HasFilters()) {
		// A name is kept when it matches the filter (or there is none) and not the exclusion; the table
		// filter and exclusion judge views too.
		auto keeps_schema = [filter, exclude](const std::string &schema) {
			return filter->MatchesSchema(schema) && !(exclude->HasSchemaFilter() && exclude->MatchesSchema(schema));
		};
		wants_schema = keeps_schema;
		wants_object = [filter, exclude, keeps_schema](const std::string &schema, const std::string &object) {
			return keeps_schema(schema) && filter->MatchesTable(object) &&
				   !(exclude->HasTableFilter() && exclude->MatchesTable(object));
		};
	}
	std::shared_ptr<AttachedBackup> attached;
	try {
		attached = mssql::backup::OpenAttachedBackup(context, name, {info.path}, std::move(wants_schema),
													 std::move(wants_object), diff);
	} catch (mssql::backup::FormatError &e) {
		throw InvalidInputException("ATTACH %s (TYPE mssql_backup): %s", name, e.what());
	}
	return make_uniq<MSSQLBackupCatalog>(db, std::move(attached));
}

static unique_ptr<TransactionManager> MSSQLBackupTransactionManagerFor(optional_ptr<StorageExtensionInfo>,
																	   AttachedDatabase &db, Catalog &) {
	return make_uniq<MSSQLBackupTransactionManager>(db);
}

void RegisterMSSQLBackupStorageExtension(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto storage = make_shared_ptr<StorageExtension>();
	storage->attach = MSSQLBackupAttach;
	storage->create_transaction_manager = MSSQLBackupTransactionManagerFor;
	StorageExtension::Register(config, MSSQLBackupCatalog::TYPE, std::move(storage));
}

}  // namespace duckdb

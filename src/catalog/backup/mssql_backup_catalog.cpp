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
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"

#include <set>

namespace duckdb {

using mssql::backup::AttachedBackup;
using mssql::backup::BackupHistory;
using mssql::backup::CatalogTable;

//===----------------------------------------------------------------------===//
// The catalog
//===----------------------------------------------------------------------===//

MSSQLBackupCatalog::MSSQLBackupCatalog(AttachedDatabase &db, std::string name,
									   std::shared_ptr<const BackupHistory> history, mssql::backup::BackupAt default_at,
									   uint64_t default_version, std::shared_ptr<const AttachedBackup> default_backup,
									   Opener opener, Reader reader)
	: Catalog(db), name_(std::move(name)), opener_(std::move(opener)), reader_(std::move(reader)) {
	// The file functions on the catalog's name as its own table functions (DuckLake's `db.snapshots()`),
	// parsed once for every schema of every version.
	std::string literal = "'";
	for (const char c : name_) {
		literal += c;
		if (c == '\'') {
			literal += c;
		}
	}
	literal += "'";
	// Each macro passes its keyword parameters, NULL by default, through to the function.
	struct Macro {
		const char *name;
		const char *function;
		std::vector<const char *> parameters;  // keyword, NULL by default
		std::vector<const char *> positional;  // before them, required
	};
	const std::vector<const char *> versioned{"version", "as_of"};
	const std::vector<Macro> MACROS = {
		{"snapshots", "mssql_backup_snapshots", {}},
		{"refresh", "mssql_backup_refresh", {}},
		{"info", "mssql_backup_info", versioned},
		{"schemas", "mssql_backup_schemas", versioned},
		{"tables", "mssql_backup_tables", versioned},
		{"views", "mssql_backup_views", versioned},
		{"columns", "mssql_backup_columns", versioned},
		{"transactions", "mssql_backup_transactions", versioned},
		{"transaction_records", "mssql_backup_transaction_records", versioned},
		{"schema_changes", "mssql_backup_schema_changes", {"start_version", "end_version"}},
		{"table_changes",
		 "mssql_backup_table_changes",
		 {"start_version", "end_version"},
		 {"schema_name", "table_name"}},
		{"table_insertions",
		 "mssql_backup_table_insertions",
		 {"start_version", "end_version"},
		 {"schema_name", "table_name"}},
		{"table_deletions",
		 "mssql_backup_table_deletions",
		 {"start_version", "end_version"},
		 {"schema_name", "table_name"}}};
	for (const auto &m : MACROS) {
		std::string sql = std::string("FROM ") + m.function + "(" + literal;
		for (const char *parameter : m.positional) {
			sql += std::string(", ") + parameter;
		}
		for (const char *parameter : m.parameters) {
			sql += std::string(", ") + parameter + " := " + parameter;
		}
		sql += ")";
		auto function = make_uniq<TableMacroFunction>(Parser::GetBuiltinParser().ParseSelectNode(sql));
		for (const char *parameter : m.positional) {
			function->parameters.push_back(make_uniq<ColumnRefExpression>(parameter));
		}
		for (const char *parameter : m.parameters) {
			function->parameters.push_back(make_uniq<ColumnRefExpression>(parameter));
			function->default_parameters.insert(Identifier(parameter), ConstantExpression::Null());
		}
		macros_.emplace_back(m.name, std::move(function));
	}
	auto versions = std::make_shared<Versions>();
	versions->history = std::move(history);
	versions->default_version = default_version;
	const auto &chosen = versions->history->versions[default_version - 1];
	if (default_at.IsSet()) {
		pinned_ = Identity(*versions->history, chosen);
	}
	states_[StateKey(*versions->history, chosen)] = std::shared_ptr<State>(MakeState(std::move(default_backup)));
	versions_ = std::move(versions);
}

MSSQLBackupCatalog::~MSSQLBackupCatalog() = default;

void MSSQLBackupCatalog::Initialize(bool) {}

static std::string BackupKey(const std::string &set_guid, const std::vector<std::string> &paths,
							 const std::vector<uint64_t> &sizes, const std::vector<std::string> &tags) {
	std::string key = set_guid;
	for (size_t i = 0; i < paths.size(); i++) {
		key += '\x1f' + paths[i] + '\x1e' + std::to_string(sizes[i]) + '\x1e' + tags[i];
	}
	return key;
}

std::string MSSQLBackupCatalog::StateKey(const BackupHistory &history, const mssql::backup::HistoryVersion &version) {
	auto of = [&](size_t i) {
		const auto &b = history.backups[i];
		return BackupKey(b.set.backup_set_guid.ToString(), b.paths, b.sizes, b.version_tags);
	};
	return of(version.full) + (version.IsDifferential() ? '\x1d' + of(version.diff) : std::string());
}

std::string MSSQLBackupCatalog::StateKey(const AttachedBackup &backup) {
	auto of = [](const mssql::backup::OpenedBackup &opened) {
		const auto &headers = opened.Headers();
		std::vector<std::string> paths, tags;
		std::vector<uint64_t> sizes;
		// The families' files in sequence order, as the history lists them.
		for (const auto &family : headers.families) {
			const auto &key = headers.files[family->file];
			paths.push_back(key.path);
			sizes.push_back(key.size);
			tags.push_back(key.version);
		}
		return BackupKey(headers.Set().backup_set_guid.ToString(), paths, sizes, tags);
	};
	const auto &opened = *backup.backup;
	return opened.Base() ? of(*opened.Base()) + '\x1d' + of(opened) : of(opened);
}

std::string MSSQLBackupCatalog::Identity(const BackupHistory &history, const mssql::backup::HistoryVersion &version) {
	std::string key = history.backups[version.full].set.backup_set_guid.ToString();
	if (version.IsDifferential()) {
		key += "/" + history.backups[version.diff].set.backup_set_guid.ToString();
	}
	return key;
}

std::shared_ptr<const MSSQLBackupCatalog::Versions> MSSQLBackupCatalog::CurrentVersions() const {
	std::lock_guard<std::mutex> guard(lock_);
	return versions_;
}

void MSSQLBackupQueryStates::QueryBegin(ClientContext &) {
	// A lookup outside a query (TableInfo, an appender, the relation API) leaves nothing for the next one.
	std::lock_guard<std::mutex> guard(lock);
	states.clear();
}

void MSSQLBackupQueryStates::QueryEnd(ClientContext &) {
	std::lock_guard<std::mutex> guard(lock);
	states.clear();
}

MSSQLBackupQueryStates &MSSQLBackupQueryStates::For(ClientContext &context) {
	return *context.registered_state->GetOrCreate<MSSQLBackupQueryStates>("mssql_backup_states");
}

std::string MSSQLBackupCatalog::Pin(ClientContext &context, const mssql::backup::BackupAt &at) {
	if (!at.key.empty()) {
		BackupAt(&context, at);
		return at.key;
	}
	// One read of the versions: the number resolved, its state held by the query (MSSQLBackupQueryStates)
	// and its key taken from the same versions -- a refresh between two reads could leave the key in none
	// of them (review of #580's merge).
	const auto versions = CurrentVersions();
	const auto number = Resolve(*versions, &context, at);
	StateOf(&context, *versions, number);
	return StateKey(*versions->history, versions->history->versions[number - 1]);
}

std::shared_ptr<MSSQLBackupCatalog::State> MSSQLBackupCatalog::DefaultState() const {
	std::lock_guard<std::mutex> guard(lock_);
	const auto &history = *versions_->history;
	return states_.at(StateKey(history, history.versions[versions_->default_version - 1]));
}

unique_ptr<MSSQLBackupCatalog::State> MSSQLBackupCatalog::MakeState(std::shared_ptr<const AttachedBackup> backup) {
	auto state = make_uniq<State>();
	state->backup = std::move(backup);
	const auto &model = *state->backup->model;
	for (const auto &s : model.schemas) {
		if (!state->backup->WantsSchema(s.name)) {
			continue;
		}
		CreateSchemaInfo info;
		info.SetSchema(Identifier(s.name));
		info.internal = false;
		state->schemas[s.name] = make_uniq<MSSQLBackupSchemaEntry>(*this, info, state->backup);
	}
	if (state->schemas.empty()) {
		// The filters kept no schema: an empty dbo, internal, so the catalog's own functions still answer
		// (`bk.tables()` lists nothing, `bk.snapshots()` the versions; owner, 2026-11-18).
		CreateSchemaInfo info;
		info.SetSchema(Identifier("dbo"));
		info.internal = true;
		state->schemas["dbo"] = make_uniq<MSSQLBackupSchemaEntry>(*this, info, state->backup);
	}
	for (const auto &t : model.tables) {
		auto schema = state->schemas.find(t.schema_name);
		if (schema != state->schemas.end()) {
			schema->second->AddTable(t);
		}
	}
	return state;
}

std::shared_ptr<MSSQLBackupCatalog::State> MSSQLBackupCatalog::StateOf(optional_ptr<ClientContext> context,
																	   const Versions &versions, uint64_t version) {
	const auto &history = *versions.history;
	const auto &entry = history.versions[version - 1];
	const std::string key = StateKey(history, entry);
	optional_ptr<MSSQLBackupQueryStates> query;
	if (context) {
		query = &MSSQLBackupQueryStates::For(*context);
		std::lock_guard<std::mutex> guard(query->lock);
		auto mine = query->states.find(this);
		if (mine != query->states.end()) {
			auto it = mine->second.find(key);
			if (it != mine->second.end()) {
				return it->second;
			}
		}
	}
	std::shared_ptr<State> found;
	{
		std::lock_guard<std::mutex> guard(lock_);
		auto it = states_.find(key);
		if (it != states_.end()) {
			found = it->second;
		}
	}
	if (!found) {
		if (!context) {
			throw InternalException("%s: version %llu of the backup is first read without a client context", name_,
									(unsigned long long)version);
		}
		std::shared_ptr<State> state(MakeState(opener_(*context, history, entry)));
		std::lock_guard<std::mutex> guard(lock_);
		auto &slot = states_[key];
		if (!slot) {
			slot = std::move(state);
		}
		found = slot;
	}
	// The query keeps what it read, once (by key), to its end: a refresh meanwhile drops the state from the
	// catalog only.
	if (query) {
		std::lock_guard<std::mutex> guard(query->lock);
		query->states[this].emplace(key, found);
	}
	return found;
}

uint64_t MSSQLBackupCatalog::Resolve(const Versions &versions, optional_ptr<ClientContext> context,
									 const mssql::backup::BackupAt &at) const {
	if (!at.IsSet()) {
		return versions.default_version;
	}
	return mssql::backup::ResolveVersion(*versions.history, context, at, "the backup attached as " + name_);
}

std::shared_ptr<const AttachedBackup> MSSQLBackupCatalog::BackupAt(optional_ptr<ClientContext> context,
																   const mssql::backup::BackupAt &at) {
	const auto versions = CurrentVersions();
	if (at.key.empty()) {
		return StateOf(context, *versions, Resolve(*versions, context, at))->backup;
	}
	// Pinned at bind: the version of that key, wherever it is now among the versions.
	for (const auto &v : versions->history->versions) {
		if (StateKey(*versions->history, v) == at.key) {
			return StateOf(context, *versions, v.number)->backup;
		}
	}
	// Dropped by a refresh since: still read while the query or the catalog holds its state.
	if (context) {
		auto &query = MSSQLBackupQueryStates::For(*context);
		std::lock_guard<std::mutex> guard(query.lock);
		auto mine = query.states.find(this);
		if (mine != query.states.end()) {
			auto it = mine->second.find(at.key);
			if (it != mine->second.end()) {
				return it->second->backup;
			}
		}
	}
	{
		std::lock_guard<std::mutex> guard(lock_);
		auto it = states_.find(at.key);
		if (it != states_.end()) {
			return it->second->backup;
		}
	}
	throw InvalidInputException(
		"%s: the version this query was bound to is no longer among the backups' files "
		"(a refresh dropped it): run the query again",
		name_);
}

std::pair<uint64_t, uint64_t> MSSQLBackupCatalog::Refresh(ClientContext &context) {
	if (!reader_) {
		throw InvalidInputException(
			"mssql_backup_refresh: %s was attached without history: ATTACH it with "
			"(TYPE mssql_backup, history true) to take new backups in",
			name_);
	}
	std::lock_guard<std::mutex> one_at_a_time(refresh_lock_);
	const auto before = CurrentVersions();
	auto refreshed = std::make_shared<Versions>();
	refreshed->history = reader_(context, *before->history);
	const auto &versions = refreshed->history->versions;
	refreshed->default_version = versions.back().number;
	if (!pinned_.empty()) {
		// The version snapshot_version / snapshot_time chose, by its backups, wherever it is now.
		bool found = false;
		for (const auto &v : versions) {
			if (Identity(*refreshed->history, v) == pinned_) {
				refreshed->default_version = v.number;
				found = true;
			}
		}
		if (!found) {
			DUCKDB_LOG_WARNING(context,
							   "mssql_backup_refresh: the version %s's snapshot_version / snapshot_time chose is no "
							   "longer among its files: the latest is read without AT",
							   name_);
		}
	}
	// The default version's entries now, so a lookup without AT never opens a backup under the swap -- put in
	// the catalog's states with the versions it publishes: StateOf may answer from the refreshing query's own
	// states without touching the catalog's (a version it read, dropped by another refresh, back since).
	const auto default_state = StateOf(&context, *refreshed, refreshed->default_version);
	const std::string default_key =
		StateKey(*refreshed->history, refreshed->history->versions[refreshed->default_version - 1]);
	{
		// The states of versions the history no longer has go (a rolling archive's oldest files gone); a query
		// that read one keeps it to its end.
		std::set<std::string> kept;
		for (const auto &v : versions) {
			kept.insert(StateKey(*refreshed->history, v));
		}
		std::lock_guard<std::mutex> guard(lock_);
		auto &slot = states_[default_key];
		if (!slot) {
			slot = default_state;
		}
		versions_ = refreshed;
		for (auto it = states_.begin(); it != states_.end();) {
			it = kept.count(it->first) ? std::next(it) : states_.erase(it);
		}
	}
	// Every query from the next one reads the new versions, in a transaction or not.
	return {before->history->versions.size(), versions.size()};
}

optional<Identifier> MSSQLBackupCatalog::GetDefaultSchema() const {
	// dbo, as SQL Server resolves a one-part name; the first schema the filter kept when it hid dbo.
	const auto state = DefaultState();
	const auto &schemas = state->schemas;
	if (schemas.empty() || schemas.count("dbo")) {
		return Identifier("dbo");
	}
	return Identifier(schemas.begin()->first);
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

optional_ptr<SchemaCatalogEntry> MSSQLBackupCatalog::LookupSchema(CatalogTransaction transaction,
																  const EntryLookupInfo &lookup,
																  OnEntryNotFound if_not_found) {
	// AT (task 24b) reaches the schema's lookup with the entry's: the version's schemas answer.
	const auto at = lookup.GetAtClause();
	// A version this catalog does not have is an error, never a miss, as DuckLake's: DuckDB asks a
	// catalog named explicitly and one of the search path alike, and the error names what versions exist
	// (a search path's catalog ahead of the one meant has the AT read against it too).
	const auto versions = CurrentVersions();
	mssql::backup::BackupAt chosen;
	if (at) {
		chosen.unit = at->Unit().GetIdentifierName();
		chosen.value = at->GetValue();
	}
	// The entry returned lives on in the query's states to its end.
	const auto state = StateOf(transaction.context, *versions, Resolve(*versions, transaction.context, chosen));
	auto found = FindByName(state->schemas, lookup.GetEntryName());
	if (!found && if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema '%s' not found in the backup attached as %s", lookup.GetEntryName(), name_);
	}
	return found;
}

void MSSQLBackupCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	const auto versions = CurrentVersions();
	const auto state = StateOf(&context, *versions, versions->default_version);
	for (auto &s : state->schemas) {
		callback(*s.second);
	}
}

void MSSQLBackupCatalog::RefuseWrite() const {
	throw PermissionException("%s is a backup attached read-only (TYPE mssql_backup): nothing is written to it", name_);
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
	// The default version's files: its full's, and its differential's.
	DatabaseSize size;
	const auto state = DefaultState();
	for (auto backup = state->backup->backup.get(); backup; backup = backup->Base().get()) {
		for (const auto &file : backup->Headers().files) {
			size.bytes += file.size;
		}
	}
	return size;
}

string MSSQLBackupCatalog::GetDBPath() {
	const auto state = DefaultState();
	return state->backup->backup->Headers().files.front().path;
}

//===----------------------------------------------------------------------===//
// Schema entries
//===----------------------------------------------------------------------===//

MSSQLBackupSchemaEntry::MSSQLBackupSchemaEntry(MSSQLBackupCatalog &catalog, CreateSchemaInfo &info,
											   std::shared_ptr<const AttachedBackup> backup)
	: SchemaCatalogEntry(catalog, info), backup_(std::move(backup)) {
	for (const auto &m : catalog.Macros()) {
		CreateMacroInfo macro(CatalogType::TABLE_MACRO_ENTRY);
		macro.SetQualifiedName(QualifiedName({Identifier(name)}, Identifier(m.first)));
		macro.temporary = true;
		macro.internal = true;
		macro.macros.push_back(m.second->Copy());
		functions_[m.first] = make_uniq_base<CatalogEntry, TableMacroCatalogEntry>(catalog, *this, macro);
	}
}

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
	if (lookup.GetCatalogType() == CatalogType::TABLE_FUNCTION_ENTRY) {
		auto found = functions_.find(StringUtil::Lower(lookup.GetEntryName()));
		return found == functions_.end() ? nullptr : found->second.get();
	}
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
	const auto &backup = schema.Cast<MSSQLBackupSchemaEntry>().Backup();
	// The version by its key: a plan run or deserialized after a refresh reads this one.
	return mssql::backup::AttachedScanFunction(context, *this, backup, MSSQLBackupCatalog::StateKey(*backup),
											   schema_name_, table_name_,
											   ParentCatalog().Cast<MSSQLBackupCatalog>().Fuzzy(), bind_data);
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
	auto transaction = make_uniq<MSSQLBackupTransaction>(*this, context);
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
std::shared_ptr<const AttachedBackup> mssql::backup::FindAttachedBackup(ClientContext &context, const std::string &name,
																		const BackupAt &at) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db) {
		return nullptr;
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		return nullptr;
	}
	return catalog.Cast<MSSQLBackupCatalog>().BackupAt(context, at);
}

std::string mssql::backup::AttachedFuzzy(ClientContext &context, const std::string &name) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db || db->GetCatalog().GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		return std::string();
	}
	return db->GetCatalog().Cast<MSSQLBackupCatalog>().Fuzzy();
}

void mssql::backup::PinAttachedVersion(ClientContext &context, const std::string &name, BackupAt &at) {
	auto db = DatabaseManager::Get(context).GetDatabase(Identifier(name));
	if (!db || db->GetCatalog().GetCatalogType() != MSSQLBackupCatalog::TYPE) {
		return;
	}
	at.key = db->GetCatalog().Cast<MSSQLBackupCatalog>().Pin(context, at);
}

std::string mssql::backup::AttachedStateKey(const BackupHistory &history, uint64_t number) {
	return MSSQLBackupCatalog::StateKey(history, history.versions[number - 1]);
}

static unique_ptr<Catalog> MSSQLBackupAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context,
											 AttachedDatabase &db, const string &name, AttachInfo &info,
											 AttachOptions &options) {
	// schema_filter / table_filter keep what they match; schema_exclude / table_exclude drop what they
	// match. All regexes as TYPE mssql's: case-insensitive, a partial match.
	auto filter = std::make_shared<MSSQLCatalogFilter>();
	auto exclude = std::make_shared<MSSQLCatalogFilter>();
	std::vector<std::string> diff;
	std::string fuzzy;	// D11 (task 27): "" = the setting
	bool history_mode = false;
	mssql::backup::BackupAt default_at;
	for (auto it = options.options.begin(); it != options.options.end();) {
		const std::string key = StringUtil::Lower(it->first);
		if (key == "history") {
			// Task 24b (DQ-48): every backup the path names, as one catalog's versions. A string too, as the
			// other boolean ATTACH options take one (DuckLake's METADATA_PARAMETERS, issue #325).
			history_mode = !it->second.IsNull() && it->second.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
			it = options.options.erase(it);
			continue;
		}
		if (key == "snapshot_version" || key == "snapshot_time") {
			// DuckLake's: the version read without AT.
			if (default_at.IsSet()) {
				throw InvalidInputException(
					"ATTACH %s (TYPE mssql_backup): snapshot_version and snapshot_time name "
					"the default version twice: give one",
					name);
			}
			if (!it->second.IsNull()) {
				default_at.unit = key == "snapshot_version" ? "version" : "timestamp";
				default_at.value = it->second;
			}
			it = options.options.erase(it);
			continue;
		}
		if (key == "fuzzy") {
			// D11 (task 27): what the tables' scans do with one that is not consistent.
			if (!it->second.IsNull()) {
				fuzzy = it->second.ToString();
				mssql::backup::FuzzyMode mode;
				if (!mssql::backup::ParseFuzzyMode(fuzzy, mode)) {
					throw InvalidInputException(
						"ATTACH %s (TYPE mssql_backup): fuzzy '%s' is not 'warn', 'error' or 'ignore'", name, fuzzy);
				}
			}
			it = options.options.erase(it);
			continue;
		}
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
			"(schema_filter, table_filter, schema_exclude, table_exclude, diff, history, snapshot_version, "
			"snapshot_time, fuzzy)",
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
	if (history_mode && !diff.empty()) {
		throw InvalidInputException(
			"ATTACH %s (TYPE mssql_backup): diff does not go beside history: the history "
			"lays each differential among its files over its own full",
			name);
	}
	const std::string catalog_name = name;
	try {
		std::shared_ptr<BackupHistory> history;
		std::shared_ptr<const AttachedBackup> attached;
		if (history_mode) {
			history = std::make_shared<BackupHistory>(mssql::backup::ReadBackupHistory(context, {info.path}));
			for (const auto &skipped : history->skipped) {
				DUCKDB_LOG_WARNING(context, "ATTACH %s (TYPE mssql_backup): \"%s\" is left out of the history: %s",
								   name, skipped.path, skipped.reason);
			}
		} else {
			// The backup attached (a full, or a full and its differential) is a history of its own: the full,
			// and the full with the differential.
			auto opened =
				mssql::backup::OpenAttachedBackup(context, name, {info.path}, wants_schema, wants_object, diff);
			attached = opened;
			std::vector<mssql::backup::HistoryFamily> families;
			for (auto backup = attached->backup.get(); backup; backup = backup->Base().get()) {
				const auto &headers = backup->Headers();
				for (const auto &family : headers.families) {
					const auto &key = headers.files[family->file];
					mssql::backup::HistoryFamily f;
					f.path = key.path;
					f.size = key.size;
					f.version_tag = key.version;
					f.set = family->set;
					families.push_back(std::move(f));
				}
			}
			history = std::make_shared<BackupHistory>(mssql::backup::BuildHistory(std::move(families)));
		}
		MSSQLBackupCatalog::Opener opener = [catalog_name, wants_schema, wants_object](
												ClientContext &c, const BackupHistory &versions,
												const mssql::backup::HistoryVersion &version) {
			const auto &full = versions.backups[version.full].paths;
			const std::vector<std::string> over =
				version.IsDifferential() ? versions.backups[version.diff].paths : std::vector<std::string>();
			try {
				auto opened =
					mssql::backup::OpenAttachedBackup(c, catalog_name, full, wants_schema, wants_object, over);
				// The files as the history read them (PR 4b's promise, from the ATTACH or the refresh, not
				// from the version's open).
				mssql::backup::CheckVersionFiles(*opened->backup, versions, version, catalog_name, true);
				return std::shared_ptr<const AttachedBackup>(std::move(opened));
			} catch (mssql::backup::FormatError &e) {
				throw InvalidInputException("%s (TYPE mssql_backup), version %llu: %s", catalog_name,
											(unsigned long long)version.number, e.what());
			}
		};
		const uint64_t default_version =
			mssql::backup::ResolveVersion(*history, context, default_at, "ATTACH " + name + " (TYPE mssql_backup)");
		// The backup opened is the default version's only when their files say so (review of #579: under a
		// clock set back, the latest by finish need not be the backup attached).
		if (!attached || MSSQLBackupCatalog::StateKey(*attached) !=
							 MSSQLBackupCatalog::StateKey(*history, history->versions[default_version - 1])) {
			attached = opener(context, *history, history->versions[default_version - 1]);
		}
		MSSQLBackupCatalog::Reader reader;
		if (history_mode) {
			const std::vector<std::string> paths{info.path};
			reader = [paths, catalog_name](ClientContext &c, const BackupHistory &previous) {
				try {
					auto read = std::make_shared<BackupHistory>(mssql::backup::ReadBackupHistory(c, paths, &previous));
					for (const auto &skipped : read->skipped) {
						DUCKDB_LOG_WARNING(c, "%s (TYPE mssql_backup): \"%s\" is left out of the history: %s",
										   catalog_name, skipped.path, skipped.reason);
					}
					return std::shared_ptr<const BackupHistory>(std::move(read));
				} catch (mssql::backup::FormatError &e) {
					throw InvalidInputException("mssql_backup_refresh: %s: %s", catalog_name, e.what());
				}
			};
		}
		auto catalog = make_uniq<MSSQLBackupCatalog>(db, catalog_name, std::move(history), default_at, default_version,
													 std::move(attached), std::move(opener), std::move(reader));
		catalog->SetFuzzy(fuzzy);
		return std::move(catalog);
	} catch (mssql::backup::FormatError &e) {
		throw InvalidInputException("ATTACH %s (TYPE mssql_backup): %s", name, e.what());
	}
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

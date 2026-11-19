#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_bind_anchors.hpp"
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_ddl_translator.hpp"
#include "catalog/mssql_entry_name_filter.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "catalog/mssql_table_options.hpp"
#include "catalog/mssql_transaction.hpp"
#include "catalog/mssql_transaction_metadata.hpp"
#include "codec/target_string_type.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_login_retry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_scan_filters.hpp"
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
	info.SetSchema(Identifier(name));
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

void MSSQLSchemaEntry::Scan(ClientContext &context, SchemaScanInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY || !info.filter) {
		SchemaCatalogEntry::Scan(context, info);
		return;
	}
	vector<string> names;
	// Budgeted across the scan: DuckDB asks each schema it kept in turn.
	const auto schemas_in_scan = MSSQLBindAnchors::For(context, GetMSSQLCatalog()).SchemasInScan();
	const idx_t schema_count = schemas_in_scan.IsValid() ? MaxValue<idx_t>(schemas_in_scan.GetIndex(), 1) : 1;
	if (mssql::EntryNameCandidates(info.filter->GetName(), LogicalType::VARCHAR, names) &&
		names.size() * schema_count <= mssql::MAX_LOOKED_UP_NAMES) {
		auto &stats_provider = GetMSSQLCatalog().GetStatisticsProvider();
		const string schema_name = name.GetIdentifierName();
		for (const auto &table_name : names) {
			auto entry = tables_.GetEntry(context, table_name);
			// Found by the name asked for, which the server compares under its collation: only
			// an entry named as the server spells it is what a listing would have shown.
			if (!entry || !entry->Cast<MSSQLTableEntry>().ShownInListing()) {
				continue;
			}
			// The count loaded with the entry, as a listing preloads it: else duckdb_tables()'s
			// storage info asks the server once per table (measured). Not in a transaction,
			// which reads the entry's own count and must not write the shared statistics.
			if (context.transaction.IsAutoCommit()) {
				auto &table = entry->Cast<MSSQLTableEntry>();
				stats_provider.PreloadRowCount(schema_name, table.name.GetIdentifierName(), table.GetApproxRowCount());
			}
			info.callback(*entry);
		}
		return;
	}
	// An oid is given to an entry when it is built and a rebuilt entry gets a new one, so an
	// entry not built yet cannot carry the oid asked for: the built ones are the whole answer.
	// Not in a transaction, whose entries come from its own metadata layer, nor under a cache TTL,
	// past which a listing reloads what the built entries may no longer match.
	if (!info.filter->GetName() && info.filter->GetOid() && context.transaction.IsAutoCommit() &&
		GetMSSQLCatalog().GetMetadataCache().GetTTLSeconds() == 0) {
		tables_.ScanBuilt(context, info.callback);
		return;
	}
	SchemaCatalogEntry::Scan(context, info);
}

SimilarCatalogEntry MSSQLSchemaEntry::GetSimilarEntry(CatalogTransaction transaction,
													  const EntryLookupInfo &lookup_info) {
	SimilarCatalogEntry result;
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY || !transaction.HasContext()) {
		// Scan lists tables only, and needs a context to list them.
		return result;
	}
	auto &context = transaction.GetContext();
	auto &catalog = GetMSSQLCatalog();
	const string schema_name = name.GetIdentifierName();
	catalog.EnsureCacheLoaded(context);
	vector<string> names;
	// Only a hint: DuckDB asks every attached schema whenever any name is
	// missing, a local table's included, so a server that cannot answer must
	// not turn "does not exist" into a connection error (review of #412).
	try {
		auto &shared = catalog.GetMetadataCache();
		// Only a transaction that holds a pinned connection has names of its
		// own (#380: the shared cache holds committed state only): one that
		// changed this schema on it (mssql_exec DDL), or one on a pool of ONE
		// that holds the pool's only connection. Otherwise the committed names,
		// through a pool connection -- a hint does not pin a connection or open
		// a server transaction. Catalog DDL inside a transaction marks the
		// schema changed too, but on a pool of many it ran and committed on a
		// pool connection and invalidated the shared names, so those are the
		// right answer; on a pool of one it ran on the pinned connection
		// (#419), which the transaction then holds.
		//
		// The catalog's transaction exists by now whether or not the statement
		// touched the catalog: DuckDB builds the CatalogTransaction it passes
		// here with Transaction::Get, which creates it. So "own" is asked of
		// what the transaction holds, not of whether it exists; asking
		// existence pinned a pool of one's only connection, and opened a server
		// transaction, to build a suggestion (review of #417).
		bool own = false;
		if (!context.transaction.IsAutoCommit()) {
			auto &transaction = MSSQLTransaction::Get(context, catalog);
			auto &metadata = transaction.Metadata(context);
			own = transaction.HasPinnedConnection() &&
				  (metadata.IsSchemaChanged(schema_name) ||
				   (catalog.GetConnectionLimit() <= 1 && !shared.TryGetTableNames(schema_name, names)));
			if (own) {
				auto &cache = metadata.Cache();
				if (!cache.TryGetTableNames(schema_name, names)) {
					auto connection = ConnectionProvider::GetConnection(context, catalog);
					try {
						cache.LoadAllTableNames(context, *connection);
					} catch (...) {
						ConnectionProvider::ReleaseConnection(context, catalog, std::move(connection));
						throw;
					}
					ConnectionProvider::ReleaseConnection(context, catalog, std::move(connection));
					cache.TryGetTableNames(schema_name, names);
				}
			}
		}
		if (!own && !shared.TryGetTableNames(schema_name, names)) {
			std::string why;
			mssql::LoginContextScope login_scope(transaction.context);	// issue #481
			auto connection = catalog.GetConnectionPool().Acquire(-1, &why);
			if (!connection) {
				throw IOException("Failed to acquire connection for table names: " + why);
			}
			try {
				shared.LoadAllTableNames(context, *connection);
			} catch (...) {
				catalog.GetConnectionPool().Release(std::move(connection));
				throw;
			}
			catalog.GetConnectionPool().Release(std::move(connection));
			shared.TryGetTableNames(schema_name, names);
		}
	} catch (const InterruptException &) {
		throw;
	} catch (const std::exception &) {
		return result;
	}
	const auto &wanted = lookup_info.GetEntryName();
	for (const auto &table_name : names) {
		auto score = StringUtil::SimilarityRating(table_name, wanted);
		if (score > result.score) {
			result.score = score;
			result.name = Identifier(table_name);
		}
	}
	return result;
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
	string table_name = base_info.GetTableName().GetIdentifierName();

	// Check if table already exists for IF NOT EXISTS / OR REPLACE handling (Issue #44)
	if (!transaction.HasContext()) {
		throw InternalException("Cannot execute CREATE TABLE without client context");
	}

	auto existing_entry = tables_.GetEntry(transaction.GetContext(), table_name);

	// Handle OnCreateConflict modes
	switch (base_info.on_conflict) {
	case OnCreateConflict::IGNORE_ON_CONFLICT:
		// IF NOT EXISTS: if table exists, return existing entry without error
		if (existing_entry) {
			return existing_entry;
		}
		break;

	case OnCreateConflict::REPLACE_ON_CONFLICT: {
		// OR REPLACE: drop existing table first
		if (existing_entry) {
			string drop_sql = MSSQLDDLTranslator::TranslateDropTable(name.GetIdentifierName(), table_name);
			mssql_catalog.ExecuteDDL(transaction.GetContext(), drop_sql);
		}
		break;
	}

	case OnCreateConflict::ERROR_ON_CONFLICT:
	default:
		// Default behavior: error if table exists (handled by SQL Server)
		break;
	}

	// Extract columns from the bound info
	// BoundCreateTableInfo contains the resolved column definitions
	auto &columns = base_info.columns;

	// Extract constraints (includes PRIMARY KEY, UNIQUE, etc.)
	auto &constraints = base_info.constraints;

	// Spec 060: an MSSQL_VARCHAR(n) column needs a UTF-8 collation or SQL Server
	// converts to the database code page on insert and drops everything outside
	// it, silently (issue #225). Same rule as CTAS and COPY, resolved in one place
	// on the catalog so the three cannot drift.
	vector<LogicalType> column_types;
	for (auto &column : columns.Logical()) {
		column_types.push_back(column.GetType());
	}
	mssql_catalog.ValidateStringTargets(column_types);

	bool wants_varchar = false;
	for (auto &column : columns.Logical()) {
		if (mssql::codec::NeedsVarcharCollation(column.GetType())) {
			wants_varchar = true;
			break;
		}
	}
	const string varchar_collation = mssql_catalog.ResolveVarcharCollation(transaction.GetContext(), wants_varchar);

	// Spec 060 D5: SQL Server table properties from CREATE TABLE ... WITH (...),
	// over the session defaults. DATA_COMPRESSION rides inside the CREATE; a
	// clustered index of either kind is its own statement and follows it.
	MSSQLTableOptions table_options = MSSQLTableOptions::FromSettings(transaction.GetContext());
	table_options.ApplyWithClause(base_info.options);
	mssql_catalog.ValidateTableOptions(table_options);

	// Generate T-SQL for CREATE TABLE (with constraints). The translator ends the
	// statement with ');', so a table-option suffix has to go before that
	// terminator rather than after it.
	string tsql = MSSQLDDLTranslator::TranslateCreateTable(name.GetIdentifierName(), table_name, columns, constraints,
														   varchar_collation);
	const string suffix = table_options.CreateTableSuffix();
	if (!suffix.empty()) {
		const auto terminator = tsql.find_last_of(';');
		if (terminator == string::npos) {
			tsql += suffix;
		} else {
			tsql.insert(terminator, suffix);
		}
	}

	// Execute DDL on SQL Server
	mssql_catalog.ExecuteDDL(transaction.GetContext(), tsql);

	const string post_create = table_options.PostCreateStatement(name.GetIdentifierName(), table_name);
	if (!post_create.empty()) {
		mssql_catalog.ExecuteDDL(transaction.GetContext(), post_create);
	}

	// Point invalidation: invalidate schema's table list and local table set
	mssql_catalog.InvalidateSchemaTableSet(name.GetIdentifierName());
	mssql_catalog.NoteTransactionChange(transaction.GetContext(), name.GetIdentifierName(), table_name);

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

// DuckDB 2.0 names the column of an ALTER as a path, for a field of a STRUCT
// column ("s.a"). SQL Server has no nested columns: one element, or a refusal.
static const Identifier &SingleColumnOf(const vector<Identifier> &column_path, const char *what) {
	if (column_path.size() != 1) {
		throw NotImplementedException(
			"MSSQL catalog: %s on a nested field is not supported; SQL Server has no nested "
			"columns",
			what);
	}
	return column_path[0];
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
	const string &table_name = alter_table_info.GetQualifiedName().Name().GetIdentifierName();

	// A column the statement names is a DuckDB name; T-SQL names the server's column (issue #572: in a
	// case-sensitive database `ID` beside `Id` reads as ID_1). Null when the table or column is not
	// known here: the name goes as written and the server answers for it.
	auto column_of = [&](const string &duckdb_name) -> const MSSQLColumnInfo * {
		if (!transaction.HasContext()) {
			return nullptr;
		}
		auto entry = tables_.GetEntry(transaction.GetContext(), table_name);
		if (!entry) {
			return nullptr;
		}
		for (auto &col : entry->Cast<MSSQLTableEntry>().GetMSSQLColumns()) {
			if (StringUtil::CIEquals(col.DuckDBName(), duckdb_name)) {
				return &col;
			}
		}
		return nullptr;
	};
	auto server_name = [&](const string &duckdb_name) -> string {
		auto col = column_of(duckdb_name);
		return col ? col->name : duckdb_name;
	};
	// ALTER COLUMN declares the column whole, so what the statement does not name is the column's own,
	// read from the server now (issue #577: the cache may be outdated by another client or mssql_exec, and
	// does not hold an alias type, a CLR type's schema, a typed xml's collection or SPARSE).
	auto server_column = [&](const Identifier &column_name, const char *what) {
		if (!transaction.HasContext()) {
			throw InternalException("Cannot execute %s without client context", what);
		}
		auto &context = transaction.GetContext();
		const string column = server_name(column_name.GetIdentifierName());
		auto found = mssql_catalog.ExecuteCatalogQuery(
			context, MSSQLDDLTranslator::ServerColumnTypeQuery(name.GetIdentifierName(), table_name, column));
		if (found.rows.empty()) {
			throw CatalogException("Column '%s' not found in table '%s'", column_name, table_name);
		}
		auto type = MSSQLDDLTranslator::ParseServerColumnType(found.rows[0]);
		if (type.xml_collection_id != 0) {
			auto collection = mssql_catalog.ExecuteCatalogQuery(
				context,
				"SELECT name, SCHEMA_NAME(schema_id) FROM sys.xml_schema_collections WHERE xml_collection_id = " +
					std::to_string(type.xml_collection_id));
			if (collection.rows.empty() || collection.rows[0].size() < 2) {
				throw CatalogException("Column '%s' of table '%s': its XML schema collection was not found",
									   column_name, table_name);
			}
			type.xml_collection = collection.rows[0][0];
			type.xml_collection_schema = collection.rows[0][1];
		}
		return std::make_pair(column, type);
	};

	switch (alter_table_info.alter_table_type) {
	case AlterTableType::RENAME_TABLE: {
		auto &rename_info = alter_table_info.Cast<RenameTableInfo>();
		tsql = MSSQLDDLTranslator::TranslateRenameTable(name.GetIdentifierName(), table_name,
														rename_info.new_table_name.GetIdentifierName());
		break;
	}

	case AlterTableType::ADD_COLUMN: {
		auto &add_info = alter_table_info.Cast<AddColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateAddColumn(name.GetIdentifierName(), table_name, add_info.new_column);
		break;
	}

	case AlterTableType::REMOVE_COLUMN: {
		auto &remove_info = alter_table_info.Cast<RemoveColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateDropColumn(name.GetIdentifierName(), table_name,
													   server_name(remove_info.removed_column.GetIdentifierName()));
		break;
	}

	case AlterTableType::RENAME_COLUMN: {
		auto &rename_col_info = alter_table_info.Cast<RenameColumnInfo>();
		tsql = MSSQLDDLTranslator::TranslateRenameColumn(name.GetIdentifierName(), table_name,
														 server_name(rename_col_info.old_name.GetIdentifierName()),
														 rename_col_info.new_name.GetIdentifierName());
		break;
	}

	case AlterTableType::ALTER_COLUMN_TYPE: {
		auto &type_info = alter_table_info.Cast<ChangeColumnTypeInfo>();
		// Its nullability kept (a literal NULL made a NOT NULL column nullable), and a character column
		// changed to a character type keeps its collation unless the new type names one: without COLLATE
		// the server gives the database's (a case-sensitive or UTF-8 column changed its comparisons, or
		// lost characters).
		const auto column =
			server_column(SingleColumnOf(type_info.column_path, "ALTER COLUMN TYPE"), "ALTER COLUMN TYPE");
		tsql = MSSQLDDLTranslator::TranslateAlterColumnType(
			name.GetIdentifierName(), table_name, column.first, type_info.target_type, column.second.nullable,
			MSSQLDDLTranslator::IsCharacterType(column.second.system_type) ? column.second.collation : string(),
			column.second.sparse);
		break;
	}

	case AlterTableType::SET_NOT_NULL:
	case AlterTableType::DROP_NOT_NULL: {
		const bool set_not_null = alter_table_info.alter_table_type == AlterTableType::SET_NOT_NULL;
		const char *what = set_not_null ? "SET NOT NULL" : "DROP NOT NULL";
		const auto &path = set_not_null ? alter_table_info.Cast<SetNotNullInfo>().column_path
										: alter_table_info.Cast<DropNotNullInfo>().column_path;
		// Re-declared as the server declares it, so only its nullability changes.
		const auto column = server_column(SingleColumnOf(path, what), what);
		tsql = MSSQLDDLTranslator::TranslateAlterColumnNullability(name.GetIdentifierName(), table_name, column.first,
																   MSSQLDDLTranslator::ColumnDeclaration(column.second),
																   set_not_null);
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
	mssql_catalog.GetMetadataCache().InvalidateTable(name.GetIdentifierName(), table_name);

	// Invalidate the local table set cache to pick up column changes
	mssql_catalog.InvalidateSchemaTableSet(name.GetIdentifierName());
	mssql_catalog.NoteTransactionChange(transaction.GetContext(), name.GetIdentifierName(), table_name);
}

void MSSQLSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	auto &mssql_catalog = GetMSSQLCatalog();
	mssql_catalog.CheckWriteAccess("DROP");

	// Handle DROP TABLE
	if (info.type == CatalogType::TABLE_ENTRY) {
		// Generate T-SQL for DROP TABLE
		string tsql = MSSQLDDLTranslator::TranslateDropTable(name.GetIdentifierName(),
															 info.GetQualifiedName().Name().GetIdentifierName());

		// Execute DDL on SQL Server
		mssql_catalog.ExecuteDDL(context, tsql);

		// Point invalidation: invalidate schema's table list and local table set
		mssql_catalog.InvalidateSchemaTableSet(name.GetIdentifierName());
		mssql_catalog.NoteTransactionChange(context, name.GetIdentifierName(),
											info.GetQualifiedName().Name().GetIdentifierName());
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

#include "catalog/mssql_table_entry.hpp"
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_primary_key.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/common.hpp"        // For COLUMN_IDENTIFIER_ROW_ID
#include "duckdb/common/exception.hpp"
#include "duckdb/common/table_column.hpp"  // For TableColumn, virtual_column_map_t
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "mssql_functions.hpp"
#include "table_scan/mssql_table_scan.hpp"

// Debug logging
static int GetTableEntryDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_TE_DEBUG(fmt, ...)                                    \
	do {                                                            \
		if (GetTableEntryDebugLevel() >= 1) {                       \
			fprintf(stderr, "[MSSQL TE] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {

//===----------------------------------------------------------------------===//
// Helper: Create a CreateTableInfo from MSSQL metadata
//===----------------------------------------------------------------------===//

static CreateTableInfo MakeTableInfo(const MSSQLTableMetadata &metadata) {
	CreateTableInfo info;
	info.table = metadata.name;

	// Build column definitions
	for (const auto &col : metadata.columns) {
		ColumnDefinition column_def(col.name, col.duckdb_type);
		info.columns.AddColumn(std::move(column_def));
	}

	return info;
}

//===----------------------------------------------------------------------===//
// Constructor / Destructor
//===----------------------------------------------------------------------===//

MSSQLTableEntry::MSSQLTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, const MSSQLTableMetadata &metadata)
	: TableCatalogEntry(catalog, schema,
						[&]() -> CreateTableInfo & {
							static thread_local CreateTableInfo info;
							info = MakeTableInfo(metadata);
							return info;
						}()),
	  mssql_columns_(metadata.columns),
	  object_type_(metadata.object_type),
	  approx_row_count_(metadata.approx_row_count) {}

MSSQLTableEntry::~MSSQLTableEntry() = default;

//===----------------------------------------------------------------------===//
// Required Overrides
//===----------------------------------------------------------------------===//

// Helper to escape SQL Server bracket identifier (] becomes ]])
static string EscapeBracketIdentifier(const string &name) {
	string result;
	result.reserve(name.size() + 2);
	for (char c : name) {
		result += c;
		if (c == ']') {
			result += ']';	// Double the ] character
		}
	}
	return result;
}

TableFunction MSSQLTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto &mssql_catalog = GetMSSQLCatalog();
	auto &mssql_schema = GetMSSQLSchema();

	// Create bind data with table info
	// Note: Don't generate the query here - it will be generated in InitGlobal
	// based on the column_ids from projection pushdown
	auto catalog_bind_data = make_uniq<MSSQLCatalogScanBindData>();
	catalog_bind_data->context_name = mssql_catalog.GetContextName();
	catalog_bind_data->schema_name = mssql_schema.name;
	catalog_bind_data->table_name = name;

	// Store pointer to this table entry for get_bind_info callback
	// This allows DuckDB to discover virtual columns like rowid
	catalog_bind_data->table_entry = this;

	// Store ALL column information - the query will use only projected columns
	for (const auto &col : mssql_columns_) {
		catalog_bind_data->all_column_names.push_back(col.name);
		catalog_bind_data->all_types.push_back(col.duckdb_type);
	}

	//===----------------------------------------------------------------------===//
	// Primary Key / RowId Support (Spec 001-pk-rowid-semantics)
	//===----------------------------------------------------------------------===//
	// Pre-populate PK info so InitGlobal can use it for rowid handling.
	// We load PK info here even if rowid is not requested because:
	// 1. We don't know if rowid will be requested until InitGlobal
	// 2. PK discovery is lazy-loaded and cached, so subsequent calls are fast
	// 3. This enables consistent error handling for views and no-PK tables

	if (object_type_ == MSSQLObjectType::VIEW) {
		// Views cannot have rowid - mark as not available
		catalog_bind_data->rowid_requested = false;
		MSSQL_TE_DEBUG("GetScanFunction: %s.%s is a VIEW (rowid not supported)",
		               mssql_schema.name.c_str(), name.c_str());
	} else {
		// Load PK info (lazy-loaded, cached)
		EnsurePKLoaded(context);

		if (pk_info_.exists) {
			// Table has a PK - populate rowid support fields
			catalog_bind_data->rowid_requested = true;  // Mark as available for InitGlobal
			catalog_bind_data->pk_is_composite = pk_info_.IsComposite();
			catalog_bind_data->rowid_type = pk_info_.rowid_type;

			// Store PK column names and types
			for (const auto &pk_col : pk_info_.columns) {
				catalog_bind_data->pk_column_names.push_back(pk_col.name);
				catalog_bind_data->pk_column_types.push_back(pk_col.duckdb_type);
			}

			MSSQL_TE_DEBUG("GetScanFunction: %s.%s has %zu PK column(s), composite=%s, rowid_type=%s",
			               mssql_schema.name.c_str(), name.c_str(),
			               pk_info_.columns.size(),
			               pk_info_.IsComposite() ? "true" : "false",
			               pk_info_.rowid_type.ToString().c_str());
		} else {
			// Table has no PK - rowid not supported
			catalog_bind_data->rowid_requested = false;
			MSSQL_TE_DEBUG("GetScanFunction: %s.%s has no PK (rowid not supported)",
			               mssql_schema.name.c_str(), name.c_str());
		}
	}

	MSSQL_TE_DEBUG("GetScanFunction: table=%s.%s with %zu columns (projection deferred to InitGlobal)",
				   mssql_schema.name.c_str(), name.c_str(), mssql_columns_.size());

	bind_data = std::move(catalog_bind_data);

	return mssql::GetCatalogScanFunction();
}

unique_ptr<BaseStatistics> MSSQLTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// We don't have detailed column-level statistics from SQL Server
	// Table-level cardinality is provided via GetStorageInfo
	return nullptr;
}

TableStorageInfo MSSQLTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo info;

	// Try to get fresh row count from statistics provider
	auto &mssql_catalog = GetMSSQLCatalog();
	auto &mssql_schema = GetMSSQLSchema();

	try {
		auto &pool = mssql_catalog.GetConnectionPool();
		auto connection = pool.Acquire();

		if (connection) {
			auto &stats_provider = mssql_catalog.GetStatisticsProvider();
			idx_t row_count = stats_provider.GetRowCount(*connection, mssql_schema.name, name);
			info.cardinality = row_count;
			pool.Release(std::move(connection));
			MSSQL_TE_DEBUG("GetStorageInfo: table=%s.%s cardinality=%llu (from DMV)", mssql_schema.name.c_str(),
						   name.c_str(), (unsigned long long)row_count);
		} else {
			// Fallback to cached row count if connection fails
			info.cardinality = approx_row_count_;
			MSSQL_TE_DEBUG("GetStorageInfo: table=%s.%s cardinality=%llu (cached, no connection)",
						   mssql_schema.name.c_str(), name.c_str(), (unsigned long long)approx_row_count_);
		}
	} catch (...) {
		// Fallback to cached row count on any error
		info.cardinality = approx_row_count_;
		MSSQL_TE_DEBUG("GetStorageInfo: table=%s.%s cardinality=%llu (cached, exception)", mssql_schema.name.c_str(),
					   name.c_str(), (unsigned long long)approx_row_count_);
	}

	return info;
}

void MSSQLTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
											LogicalUpdate &update, ClientContext &context) {
	// Updates are not supported - this shouldn't be called for read-only catalog
	throw NotImplementedException("MSSQL catalog is read-only: UPDATE binding is not supported");
}

//===----------------------------------------------------------------------===//
// MSSQL-specific Accessors
//===----------------------------------------------------------------------===//

const vector<MSSQLColumnInfo> &MSSQLTableEntry::GetMSSQLColumns() const {
	return mssql_columns_;
}

MSSQLObjectType MSSQLTableEntry::GetObjectType() const {
	return object_type_;
}

idx_t MSSQLTableEntry::GetApproxRowCount() const {
	return approx_row_count_;
}

MSSQLCatalog &MSSQLTableEntry::GetMSSQLCatalog() {
	return catalog.Cast<MSSQLCatalog>();
}

MSSQLSchemaEntry &MSSQLTableEntry::GetMSSQLSchema() {
	return schema.Cast<MSSQLSchemaEntry>();
}

//===----------------------------------------------------------------------===//
// Primary Key / RowId Support
//===----------------------------------------------------------------------===//

void MSSQLTableEntry::EnsurePKLoaded(ClientContext &context) const {
	if (pk_info_.loaded) {
		return;
	}

	MSSQL_TE_DEBUG("EnsurePKLoaded: loading PK for %s.%s", schema.name.c_str(), name.c_str());

	auto &mssql_catalog = const_cast<MSSQLTableEntry *>(this)->GetMSSQLCatalog();
	auto &mssql_schema = const_cast<MSSQLTableEntry *>(this)->GetMSSQLSchema();

	try {
		auto &pool = mssql_catalog.GetConnectionPool();
		auto connection = pool.Acquire();

		if (connection) {
			auto &cache = mssql_catalog.GetMetadataCache();
			pk_info_ = mssql::PrimaryKeyInfo::Discover(
				*connection,
				mssql_schema.name,
				name,
				cache.GetDatabaseCollation()
			);
			pool.Release(std::move(connection));
		} else {
			// No connection available - mark as loaded but no PK
			MSSQL_TE_DEBUG("EnsurePKLoaded: no connection available, assuming no PK");
			pk_info_.loaded = true;
			pk_info_.exists = false;
		}
	} catch (const std::exception &e) {
		MSSQL_TE_DEBUG("EnsurePKLoaded: error discovering PK: %s", e.what());
		pk_info_.loaded = true;
		pk_info_.exists = false;
	}
}

LogicalType MSSQLTableEntry::GetRowIdType(ClientContext &context) {
	// Views don't support rowid
	if (object_type_ == MSSQLObjectType::VIEW) {
		throw BinderException("MSSQL: rowid not supported for views");
	}

	// Ensure PK info is loaded
	EnsurePKLoaded(context);

	// Check if table has a PK
	if (!pk_info_.exists) {
		throw BinderException("MSSQL: rowid requires a primary key");
	}

	return pk_info_.rowid_type;
}

bool MSSQLTableEntry::HasPrimaryKey(ClientContext &context) {
	// Views don't have primary keys
	if (object_type_ == MSSQLObjectType::VIEW) {
		return false;
	}

	// Ensure PK info is loaded
	EnsurePKLoaded(context);

	return pk_info_.exists;
}

const mssql::PrimaryKeyInfo &MSSQLTableEntry::GetPrimaryKeyInfo(ClientContext &context) {
	EnsurePKLoaded(context);
	return pk_info_;
}

virtual_column_map_t MSSQLTableEntry::GetVirtualColumns() const {
	virtual_column_map_t result;

	MSSQL_TE_DEBUG("GetVirtualColumns: table=%s, pk_loaded=%s, pk_exists=%s",
	               name.c_str(),
	               pk_info_.loaded ? "true" : "false",
	               pk_info_.exists ? "true" : "false");

	// Views don't support rowid
	if (object_type_ == MSSQLObjectType::VIEW) {
		MSSQL_TE_DEBUG("GetVirtualColumns: %s is a VIEW, not exposing rowid", name.c_str());
		return result;
	}

	// Check if PK info is loaded and has a primary key
	// Note: PK info is lazy-loaded in GetScanFunction(), which is called before this
	// method during binding. If not loaded yet, we can't expose rowid.
	if (!pk_info_.loaded) {
		MSSQL_TE_DEBUG("GetVirtualColumns: PK info not loaded for %s, not exposing rowid", name.c_str());
		return result;
	}

	if (!pk_info_.exists) {
		MSSQL_TE_DEBUG("GetVirtualColumns: %s has no PK, not exposing rowid", name.c_str());
		return result;
	}

	// Expose rowid with the correct type based on PK structure
	result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", pk_info_.rowid_type)));
	MSSQL_TE_DEBUG("GetVirtualColumns: exposing rowid with type %s for %s",
	               pk_info_.rowid_type.ToString().c_str(), name.c_str());

	return result;
}

}  // namespace duckdb

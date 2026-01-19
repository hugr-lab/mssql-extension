#include "catalog/mssql_table_entry.hpp"
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_schema_entry.hpp"
#include "catalog/mssql_statistics.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "mssql_functions.hpp"

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

	// Store ALL column information - the query will use only projected columns
	for (const auto &col : mssql_columns_) {
		catalog_bind_data->all_column_names.push_back(col.name);
		catalog_bind_data->all_types.push_back(col.duckdb_type);
	}

	MSSQL_TE_DEBUG("GetScanFunction: table=%s.%s with %zu columns (projection deferred to InitGlobal)",
				   mssql_schema.name.c_str(), name.c_str(), mssql_columns_.size());

	bind_data = std::move(catalog_bind_data);

	return GetMSSQLCatalogScanFunction();
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

}  // namespace duckdb

// MSSQL Primary Key Discovery Implementation
// Feature: 001-pk-rowid-semantics

#include "catalog/mssql_primary_key.hpp"
#include <cstdlib>
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetPKDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_PK_DEBUG(fmt, ...)                                    \
	do {                                                            \
		if (GetPKDebugLevel() >= 1) {                               \
			fprintf(stderr, "[MSSQL PK] " fmt "\n", ##__VA_ARGS__); \
		}                                                           \
	} while (0)

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// SQL Query for Primary Key Discovery
//===----------------------------------------------------------------------===//

// Query to discover primary key columns for a table
// Uses sys.key_constraints, sys.indexes, sys.index_columns, sys.columns, sys.types
// Parameters: %s = [schema].[table] fully qualified name
static const char *PK_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    c.name AS column_name,
    c.column_id,
    ic.key_ordinal,
    t.name AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    ISNULL(c.collation_name, '') AS collation_name
FROM sys.key_constraints kc
JOIN sys.indexes i
    ON kc.parent_object_id = i.object_id
    AND kc.unique_index_id = i.index_id
JOIN sys.index_columns ic
    ON i.object_id = ic.object_id
    AND i.index_id = ic.index_id
JOIN sys.columns c
    ON ic.object_id = c.object_id
    AND ic.column_id = c.column_id
JOIN sys.types t
    ON c.user_type_id = t.user_type_id
WHERE kc.type = 'PK'
    AND kc.parent_object_id = OBJECT_ID('%s')
ORDER BY ic.key_ordinal
)";

//===----------------------------------------------------------------------===//
// Helper: Execute metadata query using MSSQLSimpleQuery
//===----------------------------------------------------------------------===//

using MetadataRowCallback = std::function<void(const vector<string> &values)>;

static void ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql, MetadataRowCallback callback) {
	auto result =
		MSSQLSimpleQuery::ExecuteWithCallback(connection, sql, [&callback](const std::vector<std::string> &row) {
			// Convert std::vector to duckdb::vector
			vector<string> duckdb_row;
			duckdb_row.reserve(row.size());
			for (const auto &val : row) {
				duckdb_row.push_back(val);
			}
			callback(duckdb_row);
			return true;  // continue processing
		});

	if (result.HasError()) {
		throw IOException("Primary key metadata query failed: %s", result.error_message);
	}
}

//===----------------------------------------------------------------------===//
// PKColumnInfo Implementation
//===----------------------------------------------------------------------===//

PKColumnInfo PKColumnInfo::FromMetadata(const string &name, int32_t column_id, int32_t key_ordinal,
										const string &type_name, int16_t max_length, uint8_t precision, uint8_t scale,
										const string &collation_name, const string &database_collation) {
	PKColumnInfo info;
	info.name = name;
	info.column_id = column_id;
	info.key_ordinal = key_ordinal;

	// Use database collation as fallback for text types
	if (collation_name.empty() && MSSQLColumnInfo::IsTextType(type_name)) {
		info.collation_name = database_collation;
	} else {
		info.collation_name = collation_name;
	}

	// Map SQL Server type to DuckDB type using MSSQLColumnInfo helper
	info.duckdb_type = MSSQLColumnInfo::MapSQLServerTypeToDuckDB(type_name, max_length, precision, scale);

	MSSQL_PK_DEBUG("  PK column: name=%s ordinal=%d type=%s -> %s", name.c_str(), key_ordinal, type_name.c_str(),
				   info.duckdb_type.ToString().c_str());

	return info;
}

//===----------------------------------------------------------------------===//
// PrimaryKeyInfo Implementation
//===----------------------------------------------------------------------===//

vector<string> PrimaryKeyInfo::GetColumnNames() const {
	vector<string> names;
	names.reserve(columns.size());
	for (const auto &col : columns) {
		names.push_back(col.name);
	}
	return names;
}

void PrimaryKeyInfo::ComputeRowIdType() {
	if (!exists || columns.empty()) {
		rowid_type = LogicalType::SQLNULL;
		return;
	}

	if (columns.size() == 1) {
		// Scalar PK: rowid type is the PK column type
		rowid_type = columns[0].duckdb_type;
		MSSQL_PK_DEBUG("rowid type: %s (scalar)", rowid_type.ToString().c_str());
	} else {
		// Composite PK: rowid type is STRUCT
		child_list_t<LogicalType> children;
		for (const auto &col : columns) {
			children.push_back({col.name, col.duckdb_type});
		}
		rowid_type = LogicalType::STRUCT(std::move(children));
		MSSQL_PK_DEBUG("rowid type: STRUCT with %zu fields (composite)", columns.size());
	}
}

PrimaryKeyInfo PrimaryKeyInfo::Discover(tds::TdsConnection &connection, const string &schema_name,
										const string &table_name, const string &database_collation) {
	PrimaryKeyInfo info;
	info.loaded = true;

	// Build fully qualified object name
	string full_name = "[" + schema_name + "].[" + table_name + "]";
	MSSQL_PK_DEBUG("Discovering primary key for %s", full_name.c_str());

	// Build query with object name
	string query = StringUtil::Format(PK_DISCOVERY_SQL_TEMPLATE, full_name);

	// Execute PK discovery query
	ExecuteMetadataQuery(connection, query, [&info, &database_collation](const vector<string> &values) {
		if (values.size() >= 8) {
			string col_name = values[0];
			int32_t col_id = 0;
			try {
				col_id = static_cast<int32_t>(std::stoi(values[1]));
			} catch (...) {
			}

			int32_t key_ordinal = 0;
			try {
				key_ordinal = static_cast<int32_t>(std::stoi(values[2]));
			} catch (...) {
			}

			string type_name = values[3];
			int16_t max_len = 0;
			try {
				max_len = static_cast<int16_t>(std::stoi(values[4]));
			} catch (...) {
			}

			uint8_t prec = 0;
			try {
				prec = static_cast<uint8_t>(std::stoi(values[5]));
			} catch (...) {
			}

			uint8_t scl = 0;
			try {
				scl = static_cast<uint8_t>(std::stoi(values[6]));
			} catch (...) {
			}

			string collation = values[7];

			auto pk_col = PKColumnInfo::FromMetadata(col_name, col_id, key_ordinal, type_name, max_len, prec, scl,
													 collation, database_collation);
			info.columns.push_back(std::move(pk_col));
		}
	});

	// Check if we found any PK columns
	if (info.columns.empty()) {
		MSSQL_PK_DEBUG("No primary key found for %s", full_name.c_str());
		info.exists = false;
	} else {
		MSSQL_PK_DEBUG("Found PK with %zu column(s) for %s", info.columns.size(), full_name.c_str());
		info.exists = true;
		info.ComputeRowIdType();
	}

	return info;
}

}  // namespace mssql
}  // namespace duckdb

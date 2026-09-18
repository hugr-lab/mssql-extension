// MSSQL Primary Key Discovery Implementation
// Feature: 001-pk-rowid-semantics

#include "catalog/mssql_primary_key.hpp"
#include <chrono>
#include <cstdlib>
#include <thread>
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "query/mssql_simple_query.hpp"
#include "query/mssql_sql_params.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetPKDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
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

// Spec 077 W1: every unique index on the table — the primary key is one of
// them, is_primary_key says which — with its key columns and the flags the
// choice needs. Only is_unique is filtered here: a non-unique index is never a
// candidate under any reading and a table can carry many. Everything else
// (filtered, disabled, hypothetical, a nullable or unmatchable key column) is
// decided client-side so that a rejected candidate can be reported WITH its
// reason; a row filtered out here never comes back.
//
// No join to sys.types: it drops every CLR UDT column (#353), which for a key
// on hierarchyid meant a primary key that came back with a column missing and
// a rowid built on the wrong shape. TYPE_NAME answers for both families.
//
// Parameters: @s schema, @t table (sp_executesql, one plan for every table).
static const char *PK_DISCOVERY_SQL_TEMPLATE = R"(
SELECT
    i.index_id,
    i.name AS index_name,
    i.is_primary_key,
    i.is_unique,
    i.has_filter,
    i.is_disabled,
    i.is_hypothetical,
    c.name AS column_name,
    c.column_id,
    ic.key_ordinal,
    ISNULL(TYPE_NAME(c.system_type_id), TYPE_NAME(c.user_type_id)) AS type_name,
    c.max_length,
    c.precision,
    c.scale,
    ISNULL(c.collation_name, '') AS collation_name,
    c.is_nullable,
    c.is_identity
FROM sys.indexes i
JOIN sys.index_columns ic
    ON i.object_id = ic.object_id
    AND i.index_id = ic.index_id
JOIN sys.columns c
    ON ic.object_id = c.object_id
    AND ic.column_id = c.column_id
WHERE i.object_id = OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t))
    AND i.is_unique = 1
    AND ic.is_included_column = 0
    AND ic.key_ordinal > 0
ORDER BY i.index_id, ic.key_ordinal
)";

//===----------------------------------------------------------------------===//
// Helper: Execute metadata query using MSSQLSimpleQuery
//===----------------------------------------------------------------------===//

using MetadataRowCallback = std::function<void(const vector<string> &values)>;

static void ExecuteMetadataQuery(tds::TdsConnection &connection, const string &sql, MetadataRowCallback callback,
								 const std::function<void()> &reset) {
	// Deadlock-victim retry, same contract as RunMetadataQuery in
	// mssql_metadata_cache.cpp: 1205 on a pure-read metadata query reruns
	// (bounded), and `reset` undoes whatever the aborted attempt accumulated so
	// the rerun is legal after rows have already been delivered. A PK query
	// returns one row per key column, so a composite key can and does die
	// mid-stream.
	constexpr int MAX_ATTEMPTS = 6;
	for (int attempt = 1;; attempt++) {
		idx_t rows_delivered = 0;
		auto result = MSSQLSimpleQuery::ExecuteWithCallback(
			connection, sql, [&callback, &rows_delivered](const std::vector<std::string> &row) {
				// Convert std::vector to duckdb::vector
				vector<string> duckdb_row;
				duckdb_row.reserve(row.size());
				for (const auto &val : row) {
					duckdb_row.push_back(val);
				}
				rows_delivered++;
				callback(duckdb_row);
				return true;  // continue processing
			});

		if (!result.HasError()) {
			return;
		}
		if (result.error_number == 1205 && attempt < MAX_ATTEMPTS && (rows_delivered == 0 || reset)) {
			if (rows_delivered > 0) {
				reset();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
			continue;
		}
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

	// Only SQL_ collations compare varchar and nvarchar under different rules;
	// a Windows or UTF-8 collation applies the same rules to both. The name is
	// spliced into the statement, so anything but a plain collation name is
	// left alone (and compared as before).
	string lower_type = StringUtil::Lower(type_name);
	const string &coll = info.collation_name;
	bool plain_name = !coll.empty();
	for (char c : coll) {
		if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) {
			plain_name = false;
		}
	}
	if ((lower_type == "varchar" || lower_type == "char") && plain_name &&
		StringUtil::StartsWith(StringUtil::Lower(coll), "sql_")) {
		info.key_compare_type = "varchar(" + (max_length < 0 ? string("max") : std::to_string(max_length)) + ")";
	}

	MSSQL_PK_DEBUG("  PK column: name=%s ordinal=%d type=%s -> %s", name.c_str(), key_ordinal, type_name.c_str(),
				   info.duckdb_type.ToString().c_str());

	return info;
}

//===----------------------------------------------------------------------===//
// RowIdKeyInfo Implementation
//===----------------------------------------------------------------------===//

vector<string> RowIdKeyInfo::GetColumnNames() const {
	vector<string> names;
	names.reserve(columns.size());
	for (const auto &col : columns) {
		names.push_back(col.name);
	}
	return names;
}

void RowIdKeyInfo::ComputeRowIdType() {
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
			children.push_back({Identifier(col.name), col.duckdb_type});
		}
		rowid_type = LogicalType::STRUCT(std::move(children));
		MSSQL_PK_DEBUG("rowid type: STRUCT with %zu fields (composite)", columns.size());
	}
}

const char *RowIdKeyInfo::DiscoverySqlTemplate() {
	return PK_DISCOVERY_SQL_TEMPLATE;
}

static int32_t ToInt(const string &v, int32_t fallback = 0) {
	try {
		return static_cast<int32_t>(std::stoi(v));
	} catch (...) {
		return fallback;
	}
}

static bool ToBool(const string &v) {
	return v == "1" || v == "true" || v == "True";
}

bool RowIdKeyInfo::AppendCandidateRow(RowIdKeyInfo &info, const vector<string> &values) {
	if (values.size() < 17) {
		return false;
	}
	const int32_t index_id = ToInt(values[0]);
	if (info.candidates_.empty() || info.candidates_.back().index_id != index_id) {
		RowIdKeyCandidate cand;
		cand.index_id = index_id;
		cand.index_name = values[1];
		cand.is_primary_key = ToBool(values[2]);
		cand.is_unique = ToBool(values[3]);
		cand.has_filter = ToBool(values[4]);
		cand.is_disabled = ToBool(values[5]);
		cand.is_hypothetical = ToBool(values[6]);
		info.candidates_.push_back(std::move(cand));
	}
	RowIdKeyColumn col;
	col.name = values[7];
	col.column_id = ToInt(values[8]);
	col.key_ordinal = ToInt(values[9]);
	col.type_name = values[10];
	col.max_length = static_cast<int16_t>(ToInt(values[11]));
	col.precision = static_cast<uint8_t>(ToInt(values[12]));
	col.scale = static_cast<uint8_t>(ToInt(values[13]));
	col.collation_name = values[14];
	col.is_nullable = ToBool(values[15]);
	col.is_identity = ToBool(values[16]);
	// The one client-side fact the SQL cannot supply: is this column read
	// through the lossy NVARCHAR(MAX) cast? Same predicate MSSQLColumnInfo uses.
	col.cast_required = !MSSQLColumnInfo::IsKnownSQLServerType(col.type_name);
	info.candidates_.back().columns.push_back(std::move(col));
	return true;
}

void RowIdKeyInfo::FinalizeChoice(const string &database_collation) {
	columns.clear();
	rejections.clear();
	index_name.clear();
	discovery_error.clear();
	source = RowIdKeySource::NONE;

	auto choice = ChooseRowIdKey(candidates_);
	candidates_.clear();

	rejections = std::move(choice.rejections);
	if (!choice.Found()) {
		exists = false;
		ComputeRowIdType();
		MSSQL_PK_DEBUG("no usable rowid key (%zu candidate(s) rejected)", rejections.size());
		return;
	}
	source = choice.source;
	index_name = choice.index_name;
	for (const auto &col : choice.columns) {
		columns.push_back(PKColumnInfo::FromMetadata(col.name, col.column_id, col.key_ordinal, col.type_name,
													 col.max_length, col.precision, col.scale, col.collation_name,
													 database_collation));
	}
	exists = true;
	ComputeRowIdType();
	MSSQL_PK_DEBUG("rowid key: %s '%s' with %zu column(s), %zu candidate(s) rejected",
				   source == RowIdKeySource::PRIMARY_KEY ? "primary key" : "unique index", index_name.c_str(),
				   columns.size(), rejections.size());
}

string PKColumnInfo::KeyComparand(const string &values_column) const {
	if (key_compare_type.empty()) {
		return values_column;
	}
	// COLLATE before the CAST: the conversion to varchar uses the code page of
	// its input's collation, and without it that is the database default's.
	return "CAST(" + values_column + " COLLATE " + collation_name + " AS " + key_compare_type + ")";
}

// DescribeRejections works on a RowIdKeyChoice; this struct keeps only the list.
static RowIdKeyChoice ChoiceWith(const vector<RowIdKeyRejection> &rejections) {
	RowIdKeyChoice c;
	c.rejections = rejections;
	return c;
}

string RowIdKeyInfo::RowIdRefusal(const string &schema_name, const string &table_name, const string &verb,
								  const string &catalog_name) const {
	// Two different mistakes get two different first sentences: a key that
	// exists but cannot address a row would send the user to add an index they
	// already have, so it is named as what it is.
	bool only_unmatchable = !rejections.empty();
	for (const auto &r : rejections) {
		if (!r.unmatchable) {
			only_unmatchable = false;
		}
	}
	string msg = "MSSQL: " + verb + " requires a table with a primary key or a usable unique index. ";
	// The answer is cached until invalidated, and an index added through
	// mssql_exec() does not invalidate it by default — so a user who follows
	// the advice below is told how to make the next statement see the index.
	const string invalidate_call = "mssql_invalidate_cache('" +
								   (catalog_name.empty() ? string("<catalog>") : catalog_name) + "', '" + schema_name +
								   "', '" + table_name + "')";
	const string invalidate_hint = " After adding one through mssql_exec(), run " + invalidate_call +
								   " so the next statement reads the indexes again.";
	if (!discovery_error.empty()) {
		msg += "The indexes of '" + schema_name + "." + table_name +
			   "' could not be read, so whether it has one is unknown: " + discovery_error +
			   ". The lookup is not retried on its own: " + invalidate_call +
			   " drops the cached answer so the next statement reads the indexes again.";
	} else if (rejections.empty()) {
		msg += "Table '" + schema_name + "." + table_name +
			   "' has neither. Add a PRIMARY KEY, or a UNIQUE index on NOT NULL columns without a filter." +
			   invalidate_hint;
	} else if (only_unmatchable) {
		msg += "Table '" + schema_name + "." + table_name +
			   "' has a key, but rowid cannot address a row through it: " + DescribeRejections(ChoiceWith(rejections)) +
			   ". Until the linked issue lands, add a UNIQUE index on NOT NULL columns of another type.";
	} else {
		msg += "Table '" + schema_name + "." + table_name +
			   "' has no usable one: " + DescribeRejections(ChoiceWith(rejections)) +
			   ". Add a PRIMARY KEY, or a UNIQUE index on NOT NULL columns without a filter." + invalidate_hint;
	}
	return msg;
}

RowIdKeyInfo RowIdKeyInfo::Discover(tds::TdsConnection &connection, const string &schema_name, const string &table_name,
									const string &database_collation) {
	RowIdKeyInfo info;

	// Build fully qualified object name
	string full_name = "[" + schema_name + "].[" + table_name + "]";
	MSSQL_PK_DEBUG("Discovering primary key for %s", full_name.c_str());

	// Spec 075 W4 (#334): names as sp_executesql parameters -- one plan for every table.
	string query = mssql::BuildExecuteSqlBatch(
		PK_DISCOVERY_SQL_TEMPLATE, "@s sysname, @t sysname",
		{{"s", mssql::NVarcharLiteral(schema_name)}, {"t", mssql::NVarcharLiteral(table_name)}});

	// Execute PK discovery query
	ExecuteMetadataQuery(
		connection, query, [&info](const vector<string> &values) { AppendCandidateRow(info, values); },
		[&info]() {
			// One row per key column: a candidate aborted after its first column
			// would otherwise come back with that column listed twice, and the
			// rowid STRUCT built from it would be wrong rather than merely missing.
			info.ClearCandidates();
		});

	info.FinalizeChoice(database_collation);
	return info;
}

}  // namespace mssql
}  // namespace duckdb

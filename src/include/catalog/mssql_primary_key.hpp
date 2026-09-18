#pragma once

#include <string>
#include <vector>
#include "catalog/mssql_rowid_key_choice.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// PKColumnInfo - Single PK column metadata
//===----------------------------------------------------------------------===//

struct PKColumnInfo {
	string name;			  // Column name
	int32_t column_id;		  // SQL Server column_id (1-based)
	int32_t key_ordinal;	  // Position in PK (1-based, from sys.index_columns)
	LogicalType duckdb_type;  // Mapped DuckDB type
	string collation_name;	  // For string columns (may affect DML predicates)

	// Default constructor
	PKColumnInfo() : column_id(0), key_ordinal(0), duckdb_type(LogicalType::INTEGER) {}

	// Construct from discovery query result
	static PKColumnInfo FromMetadata(const string &name, int32_t column_id, int32_t key_ordinal,
									 const string &type_name, int16_t max_length, uint8_t precision, uint8_t scale,
									 const string &collation_name, const string &database_collation);
};

//===----------------------------------------------------------------------===//
// RowIdKeyInfo - the key a table's rowid is built on
//
// Spec 077 W1: no longer only the primary key. The discovery statement returns
// every unique index on the table with its key columns and flags, and
// ChooseRowIdKey (catalog/mssql_rowid_key_choice.hpp) picks one: the primary
// key if it is usable, else a usable unique index by the documented order.
// `exists` therefore means "there is a key rowid can be built on", `source`
// says which kind, and `rejections` names every candidate that was not usable
// and why — the W5b refusal quotes them.
//===----------------------------------------------------------------------===//

struct RowIdKeyInfo {
	// Key existence (only meaningful once the owning MSSQLTableEntry has
	// published this struct via its pk_loaded_ atomic — see header).
	bool exists = false;  // Is there a usable key?

	// Which kind of key was chosen, and its name (empty when none).
	RowIdKeySource source = RowIdKeySource::NONE;
	string index_name;

	// Every candidate the choice turned down, with the reason.
	vector<RowIdKeyRejection> rejections;

	// Non-empty when the discovery query itself failed (no connection, a server
	// error). `exists` is false then too, but the refusal must not claim the
	// table has no key when the truth is that nobody could look.
	string discovery_error;

	// Key structure (only valid if exists == true)
	vector<PKColumnInfo> columns;  // Ordered by key_ordinal

	// Computed rowid type
	LogicalType rowid_type;	 // Scalar (single col) or STRUCT (composite)

	// Default constructor
	RowIdKeyInfo() : exists(false), rowid_type(LogicalType::SQLNULL) {}

	// Predicates
	bool IsScalar() const {
		return exists && columns.size() == 1;
	}
	bool IsComposite() const {
		return exists && columns.size() > 1;
	}

	// Get column names for SELECT clause
	vector<string> GetColumnNames() const;

	// Build rowid type from columns
	void ComputeRowIdType();

	// Factory method - discovers PK from SQL Server
	static RowIdKeyInfo Discover(tds::TdsConnection &connection, const string &schema_name, const string &table_name,
								 const string &database_collation);

	//! Spec 076 W2: the discovery statement, parameterised on @s / @t, so the
	//! catalog can send it in the same batch as the table's metadata and read
	//! its rows off the second result set instead of paying a round trip.
	static const char *DiscoverySqlTemplate();
	//! One row of that statement (17 columns: the index, then one of its key
	//! columns) accumulated as a candidate; false when the row does not have
	//! that shape. Rows arrive ordered by index_id, key_ordinal, so consecutive
	//! rows of one index form one candidate.
	static bool AppendCandidateRow(RowIdKeyInfo &info, const vector<string> &values);
	//! After the last row: run the choice over the accumulated candidates and
	//! publish the result into exists / source / index_name / columns /
	//! rejections / rowid_type. Idempotent on an empty candidate list.
	void FinalizeChoice(const string &database_collation);
	//! Discard accumulated candidates (a deadlock-victim rerun starts over).
	void ClearCandidates() {
		candidates_.clear();
	}

	//! The W5b refusal for a statement that needs rowid on this table: what
	//! was looked for, what was found, why each candidate was rejected, and
	//! what fixes it. `verb` is the statement kind, e.g. "UPDATE/DELETE".
	//! `catalog_name` is only used by the discovery-error shape, which names
	//! mssql_invalidate_cache(catalog, schema, table) as the way to retry: a
	//! failed lookup is cached like a result (the readers are lock-free).
	string RowIdRefusal(const string &schema_name, const string &table_name, const string &verb,
						const string &catalog_name = "") const;

private:
	vector<RowIdKeyCandidate> candidates_;
};

}  // namespace mssql
}  // namespace duckdb

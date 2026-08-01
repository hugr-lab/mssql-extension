//===----------------------------------------------------------------------===//
//                         DuckDB - MSSQL Extension
//
// catalog/mssql_table_options.hpp
//
// Spec 060 D5/D8/D9 — SQL Server table properties that CREATE TABLE cannot
// state on its own. A clustered columnstore index is where the storage win
// lives (and it is a separate DDL statement, not a column attribute), and
// DATA_COMPRESSION is a table option that must be set at creation to apply to a
// bulk load rather than to a later rebuild.
//
// Reached three ways, all landing here so they cannot disagree:
//   * CREATE TABLE ... WITH (table_kind = 'columnstore')
//   * COPY ... (FORMAT 'bcp', table_kind 'columnstore')
//   * the mssql_default_table_kind setting
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

class ClientContext;

//! What kind of table to create. HEAP is SQL Server's own default and what the
//! extension has always produced.
enum class MSSQLTableKind : uint8_t { HEAP, COLUMNSTORE, CLUSTERED };

struct MSSQLTableOptions {
	MSSQLTableKind kind = MSSQLTableKind::HEAP;

	//! Key columns for a CLUSTERED rowstore index. Required for that kind and
	//! meaningless for the others — a clustered COLUMNSTORE index has no key.
	vector<string> clustered_keys;

	//! PAGE or ROW, empty for none. Measured at −51% / −41% storage for
	//! +63% / +17% load time (spec 057). Applies during a bulk load only when
	//! the load takes a table lock; otherwise rows land uncompressed until
	//! someone rebuilds, so it is documented together with TABLOCK.
	string data_compression;

	//! Session defaults — mssql_default_table_kind.
	static MSSQLTableOptions FromSettings(ClientContext &context);

	//! Parse one name/value pair. Throws on an unknown name rather than
	//! ignoring it: a silently dropped WITH option is a table that is not what
	//! the statement asked for.
	void ApplyOption(const string &name, const string &value);

	//! Parse a CREATE TABLE ... WITH (...) clause.
	void ApplyWithClause(const case_insensitive_map_t<unique_ptr<ParsedExpression>> &options);

	//! Text appended inside CREATE TABLE — " WITH (DATA_COMPRESSION = PAGE)".
	string CreateTableSuffix() const;

	//! The statement to run after CREATE TABLE, or empty. A clustered index is
	//! its own DDL; it cannot ride along in the CREATE.
	string PostCreateStatement(const string &schema_name, const string &table_name) const;
};

}  // namespace duckdb

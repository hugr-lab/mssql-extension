//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_staged_dml.hpp
//
// Spec 080 D3: an UPDATE / DELETE delivered through a session `#stage`.
//
// The rows the plan selected go into `#stage_<uuid>` on the statement's own
// connection by INSERT BULK, and one statement then joins the target to the
// stage by the key: `UPDATE t SET … FROM … JOIN #stage` /
// `DELETE t FROM … JOIN #stage`. No rowid literal is rendered, so the key's
// values are compared as the server stores them.
//
// Rung 3 (a table with no usable key) always takes this path: the key is every
// column, the stage carries their old values (the match) and, for an UPDATE,
// the new values of the SET columns.
//
// The stage is created inside the statement's server transaction -- the
// LoadTransaction in autocommit, the DuckDB transaction's when pinned -- and a
// `#temp` created in a transaction is dropped by its ROLLBACK. So a failure
// needs no cleanup statement of its own: Fail() rolls back (autocommit) or the
// transaction's end does (pinned), and a connection left mid-response is
// closed, which ends the session. On success the stage is dropped before the
// commit. A fill that fails mid-stream leaves the connection mid-response, and
// it is closed even when it is the transaction's pinned one -- as a failed COPY
// or INSERT BULK in a transaction does: DuckDB aborts the transaction anyway.
//===----------------------------------------------------------------------===//

#pragma once

#include "catalog/mssql_column_info.hpp"
#include "copy/bulk_load_session.hpp"
#include "copy/target_resolver.hpp"
#include "dml/mssql_dml_config.hpp"
#include "dml/mssql_statement_connection.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"

#include <functional>

namespace duckdb {

class ClientContext;
class MSSQLCatalog;
class MSSQLTableEntry;

// Not UPDATE / DELETE: <windows.h> defines DELETE as a macro (review of #425).
enum class MSSQLStagedDmlKind : uint8_t { UPDATE_ROWS, DELETE_ROWS };

//! Where a chunk carries the key the stage matches by.
enum class MSSQLStagedKeySource : uint8_t {
	//! Rung 3: the last key_columns.size() columns (the hidden key columns,
	//! catalog/mssql_keyless_key.hpp).
	TRAILING_COLUMNS,
	//! Rungs 1-2: the rowid, the chunk's last column -- the key value itself
	//! for a one-column key, a STRUCT of the key columns for a composite one.
	ROWID
};

struct MSSQLStagedDmlTarget {
	MSSQLStagedDmlKind kind = MSSQLStagedDmlKind::UPDATE_ROWS;
	string catalog_name;
	string schema_name;
	string table_name;
	//! The columns the stage matches by: rung 3, every column in table order;
	//! rungs 1-2, the rowid key's columns in key order.
	vector<MSSQLColumnInfo> key_columns;
	MSSQLStagedKeySource key_source = MSSQLStagedKeySource::TRAILING_COLUMNS;
	//! Where the key sits in the chunk, when the plan says (a DELETE's
	//! row-id expressions; review of 2b): rung 3, one index per key column;
	//! rungs 1-2, the rowid's. Empty: the trailing columns / the last one.
	vector<idx_t> key_chunk_index;
	//! UPDATE: the SET columns and where each value sits in the chunk.
	vector<MSSQLColumnInfo> set_columns;
	vector<idx_t> set_chunk_index;
	//! `IS NOT DISTINCT FROM` for a nullable key column (D0), else the
	//! `EXISTS (… INTERSECT …)` form over the nullable ones.
	bool null_safe_operator = false;
	//! mssql_copy_flush_rows: the stage fill's batch boundary.
	idx_t flush_rows = 0;
	//! mssql_query_timeout, in seconds (0 = none), for the JOIN statement.
	int query_timeout_seconds = 0;
	//! The target's entry, for the planner's row estimate after a DELETE
	//! (spec 080 W3). Alive for the query: the bind anchors hold it.
	optional_ptr<MSSQLTableEntry> table_entry;
	//! The scan feeding the statement shares its connection and streams (the
	//! extension's optimizer did not run): hold every row until Finalize, when
	//! the scan has drained (review of #423).
	bool hold_until_finalize = false;

	//! Spec 080 PR 2b: UPDATE / DELETE ... RETURNING. The JOIN statement's
	//! OUTPUT goes INTO a session #out (OUTPUT INTO a table works where a bare
	//! OUTPUT is refused, beside an enabled trigger), read back after the count
	//! check into the rows DuckDB's RETURNING projection expects.
	bool returning = false;
	//! Every column of the table in table order, and the types DuckDB reads
	//! them as: the first part of the RETURNING chunk (UPDATE: the post-image,
	//! inserted.*; DELETE: the pre-image, deleted.*).
	vector<MSSQLColumnInfo> table_columns;
	vector<LogicalType> table_types;
	//! DELETE only: LogicalDelete appends the table's virtual columns after its
	//! columns, in GetVirtualColumns()' order. Each entry is the table column a
	//! virtual column repeats (rung 3's hidden key columns), or -1 for the rowid.
	vector<int64_t> virtual_sources;
	//! The rowid's type, and the table index of each key column it is built from.
	LogicalType rowid_type;
	vector<idx_t> key_table_index;
	//! The RETURNING chunk's types (the logical operator's).
	vector<LogicalType> returning_types;
	//! mssql_convert_varchar_max, for reading #out as the scan reads the table.
	bool convert_varchar_max = true;
};

class MSSQLStagedDml {
public:
	MSSQLStagedDml(ClientContext &context, MSSQLStagedDmlTarget target);
	~MSSQLStagedDml();

	//! Stage one chunk. The first one takes the statement's connection and
	//! creates the stage.
	void Execute(ClientContext &context, DataChunk &chunk);

	//! Run the JOIN statement, drop the stage, commit. Returns the rows the
	//! server reports for the statement.
	idx_t Finalize(ClientContext &context);

	//! The rows RETURNING produced (null without RETURNING); valid after
	//! Finalize.
	unique_ptr<ColumnDataCollection> TakeReturned() {
		return std::move(returned_);
	}

	//! The texts the operator sends, exposed for the unit test.
	static string CreateStageSql(const MSSQLStagedDmlTarget &target, const string &stage_name);
	//! `out_name` non-empty: the statement carries OUTPUT ... INTO it.
	static string JoinStatementSql(const MSSQLStagedDmlTarget &target, const string &stage_name,
								   const string &out_name = string());
	static string CreateOutSql(const MSSQLStagedDmlTarget &target, const string &out_name);

private:
	void Start(ClientContext &context);
	//! The staged columns of `chunk`, in stage order, as references.
	void BuildFillChunk(DataChunk &chunk);
	void FailAndThrow(ClientContext &context, const string &message);
	//! The JOIN found every staged row (rungs 1-2: every distinct key; rung 3:
	//! at least one row), or the statement fails before its commit.
	void CheckMatchedEverything(ClientContext &context, idx_t matched);
	//! Read #out into returned_, in the RETURNING chunk's layout.
	void ReadReturned(ClientContext &context);

	MSSQLStagedDmlTarget target_;
	MSSQLCatalog &catalog_;
	MSSQLStatementConnection stmt_conn_;
	std::shared_ptr<tds::TdsConnection> connection_;
	string stage_name_;

	// The stage fill: INSERT BULK into the stage, on the statement's connection.
	mssql::BCPCopyTarget bcp_target_;
	vector<mssql::BCPColumnMetadata> bcp_columns_;
	DataChunk fill_;
	string insert_bulk_sql_;
	mssql::BulkLoadSessionParams session_params_;
	mssql::BulkLoadSession session_;
	idx_t rows_staged_ = 0;
	string out_name_;
	unique_ptr<ColumnDataCollection> returned_;
	bool finalized_ = false;
};

//! Spec 080 D3, rungs 1-2: a keyed UPDATE / DELETE holds its rows until it
//! knows its path. Up to `threshold` rows (counted as they arrive) it goes as
//! today's VALUES-join statements; past it, every row -- the held ones first --
//! goes through #stage and one JOIN statement. Shared by MSSQL_UPDATE and
//! MSSQL_DELETE. Callers hold their operator's mutex.
class MSSQLStageSwitch {
public:
	MSSQLStageSwitch(const MSSQLStagedDmlTarget &target, idx_t threshold) : target_(target), threshold_(threshold) {}

	void Sink(ClientContext &context, DataChunk &chunk);

	bool IsStaged() const {
		return staged_ != nullptr;
	}

	//! The staged path's ending: the JOIN, the DROP, the commit; its count.
	idx_t FinalizeStaged(ClientContext &context);

	//! The statement path's: every held chunk, in arrival order.
	void Replay(const std::function<void(DataChunk &)> &send);

private:
	const MSSQLStagedDmlTarget &target_;
	idx_t threshold_;
	unique_ptr<ColumnDataCollection> held_;
	unique_ptr<MSSQLStagedDml> staged_;
};

}  // namespace duckdb

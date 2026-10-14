//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/merge/mssql_staged_merge.hpp
//
// Spec 080 D4 (PR 4b): a MERGE into a catalog table whose source is not on
// the same server. DuckDB evaluates the source and every expression over the
// source alone; the rows go into a session `#src_<uuid>` by INSERT BULK on the
// statement's own connection (MSSQLSessionStage, D3's stage), and ONE
// `MERGE [s].[t] AS [t] USING [#src] AS [s] ON … WHEN …;` does the match, the
// conditions and the writes on the server. The statement text is rendered at
// planning (mssql::RewriteMerge); this class runs it.
//===----------------------------------------------------------------------===//

#pragma once

#include "copy/target_resolver.hpp"
#include "dml/mssql_session_stage.hpp"
#include "tds/tds_request.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

struct MSSQLStagedMergeTarget {
	string catalog_name;
	string schema_name;
	string table_name;
	//! `#src_<uuid>`, the SELECT … INTO that creates it, and its columns in the
	//! order the operator's fill chunk lays them out.
	string src_name;
	string create_sql;
	vector<mssql::BCPColumnMetadata> src_columns;
	//! The MERGE as text (EXPLAIN, errors) and as the request sent: an
	//! sp_executesql RPC when it has parameters.
	string merge_sql;
	tds::Request merge_request;
	//! A WHEN NOT MATCHED BY SOURCE action: the MERGE runs on an empty source
	//! too (every target row is then unmatched). Without one, an empty source
	//! sends nothing.
	bool runs_on_empty_source = false;
	//! Every action is DO NOTHING: nothing is sent and the count is 0, as
	//! DuckDB's own MERGE ran such a statement (review of #478). The source is
	//! still evaluated.
	bool no_action = false;
	//! mssql_copy_flush_rows: the fill's batch boundary.
	idx_t flush_rows = 0;
	//! mssql_query_timeout in seconds (0 = none), for the CREATE and the DROP.
	//! The MERGE itself has no deadline, like a pushed DML: Ctrl-C cancels it.
	int query_timeout_seconds = 0;
};

class MSSQLStagedMerge {
public:
	MSSQLStagedMerge(ClientContext &context, const MSSQLStagedMergeTarget &target);

	//! Stage one chunk, its columns in #src order. The first one takes the
	//! statement's connection and creates #src.
	void Execute(ClientContext &context, DataChunk &fill);

	//! Run the MERGE, drop #src, commit. Returns the MERGE's count: the rows it
	//! inserted, updated and deleted.
	idx_t Finalize(ClientContext &context);

private:
	void Start(ClientContext &context);

	const MSSQLStagedMergeTarget &target_;
	MSSQLCatalog &catalog_;
	MSSQLSessionStage stage_;
	bool finalized_ = false;
};

}  // namespace duckdb

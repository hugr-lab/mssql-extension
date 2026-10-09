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
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

class ClientContext;
class MSSQLCatalog;

enum class MSSQLStagedDmlKind : uint8_t { UPDATE, DELETE };

struct MSSQLStagedDmlTarget {
	MSSQLStagedDmlKind kind = MSSQLStagedDmlKind::UPDATE;
	string catalog_name;
	string schema_name;
	string table_name;
	//! The columns the stage matches by: rung 3, every column in table order.
	//! They arrive as the LAST key_columns.size() columns of each chunk.
	vector<MSSQLColumnInfo> key_columns;
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

	//! The texts the operator sends, exposed for the unit test.
	static string CreateStageSql(const MSSQLStagedDmlTarget &target, const string &stage_name);
	static string JoinStatementSql(const MSSQLStagedDmlTarget &target, const string &stage_name);

private:
	void Start(ClientContext &context, DataChunk &first_chunk);
	void FailAndThrow(ClientContext &context, const string &message);

	MSSQLStagedDmlTarget target_;
	MSSQLCatalog &catalog_;
	MSSQLStatementConnection stmt_conn_;
	std::shared_ptr<tds::TdsConnection> connection_;
	string stage_name_;

	// The stage fill: INSERT BULK into the stage, on the statement's connection.
	mssql::BCPCopyTarget bcp_target_;
	vector<mssql::BCPColumnMetadata> bcp_columns_;
	vector<int32_t> column_mapping_;
	string insert_bulk_sql_;
	mssql::BulkLoadSessionParams session_params_;
	mssql::BulkLoadSession session_;
	idx_t rows_staged_ = 0;
	bool finalized_ = false;
};

}  // namespace duckdb

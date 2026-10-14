//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_dml_execute.hpp
//
// ExecuteDmlBatch sends one DML statement and reads its response (spec 080,
// acceptance 7).
//
// Its callers:
// - every INSERT, UPDATE and DELETE batch of the row-by-row paths;
// - an INSERT ... OUTPUT batch;
// - a pushed DML (`mssql_dml`).
// The staged path's #stage / JOIN statements, IDENTITY_INSERT, and a
// transaction's BEGIN / COMMIT still go through MSSQLSimpleQuery.
//
// The token loop used to be copied into five places. Each copy for the DML
// batches hard-coded a 30 s timeout, and issue #323's fix reached one copy
// and missed the others.
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_request.hpp"
#include "tds/tds_token_parser.hpp"

namespace duckdb {

class ClientContext;

namespace mssql {

//! What one DML statement's response said.
struct DmlBatchResponse {
	enum class Status : uint8_t {
		//! The final DONE arrived, with no ERROR token before it.
		OK,
		//! The server refused the statement with an ERROR token.
		SERVER_ERROR,
		//! The client lost the token stream after the request went out, so the
		//! server ran the statement (issue #344).
		PARSE_ERROR,
		//! The request did not go out.
		SEND_FAILED,
		//! The connection broke while the response was being read. The
		//! connection is Disconnected.
		RECEIVE_FAILED,
		//! mssql_query_timeout elapsed. ATTENTION has been sent.
		TIMEOUT,
		//! The query was interrupted (Ctrl-C). ATTENTION has been sent.
		INTERRUPTED
	};

	Status status = Status::OK;
	//! The count of the last DONE that carried one. An AFTER trigger's counts
	//! arrive before the statement's own count, so the last one is the
	//! statement's (measured, spec 080 PR 3a).
	idx_t rows_affected = 0;
	//! For a server error, the first ERROR token's number and text: the cause.
	//! The errors after it are its consequences. For any other failure, what
	//! went wrong, in a clause the caller puts after "<VERB> failed: ".
	uint32_t error_number = 0;
	std::string error_message;

	bool Succeeded() const {
		return status == Status::OK;
	}
	//! "SQL Server error N: text" for a server error, else the message as it is.
	std::string DescribeError() const;
};

struct DmlBatchOptions {
	//! The label the connection's debug log gives the request. A string
	//! literal: the connection keeps the pointer after the call returns.
	const char *label = "DML statement";
	//! No deadline, for a pushed DML. It answers nothing until its DONE, and a
	//! copy of millions of rows takes as long as it takes. Otherwise the
	//! deadline is mssql_query_timeout, with 0 meaning none. Either way the
	//! wait goes in 1 s slices that ask ClientContext::IsInterrupted, and a
	//! dead peer ends it through the socket's TCP keepalive.
	bool no_deadline = false;
	//! INSERT ... OUTPUT: called for each ROW token, with the columns of the
	//! COLMETADATA before it. Rows are skipped when this is unset.
	std::function<void(const tds::RowData &, const std::vector<tds::ColumnMetadata> &)> on_row;
};

//! Send `request` on an Idle connection and read its response.
//!
//! The response is read to the end of the message after a server error, a
//! desync, a timeout or an interrupt, so the socket is clean for the next
//! statement. A failed statement is reported in the status, never thrown.
//! The caller decides what the failure means for its statement and fails
//! that statement's connection. What does throw is a fault on the client
//! side: an OUTPUT value that does not convert, or a malformed packet
//! header. Either one leaves the connection Executing, and the caller's
//! Fail closes it.
//!
//! Also:
//! - leaves the connection Idle after a response read to its end, and
//!   Disconnected after an ATTENTION that was not acknowledged;
//! - logs INFO tokens (PRINT, or RAISERROR of severity 10 or less) to the
//!   context's log;
//! - applies mssql_test_fail_parse_after_tokens, the test lever.
DmlBatchResponse ExecuteDmlBatch(ClientContext &context, tds::TdsConnection &connection, const tds::Request &request,
								 const DmlBatchOptions &options);
//! The same for a batch's text, sent as it is. An INSERT batch can be up to
//! mssql_insert_max_sql_bytes long, and a Request would copy it.
DmlBatchResponse ExecuteDmlBatch(ClientContext &context, tds::TdsConnection &connection, const std::string &sql,
								 const DmlBatchOptions &options);

}  // namespace mssql
}  // namespace duckdb

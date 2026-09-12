#pragma once

#include <memory>
#include <string>
#include "duckdb.hpp"
#include "mssql_result_stream.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// MSSQLQueryExecutor - Orchestrates query execution with pool integration
//===----------------------------------------------------------------------===//

class MSSQLQueryExecutor {
public:
	explicit MSSQLQueryExecutor(const std::string &context_name);
	~MSSQLQueryExecutor() = default;

	// Execute a SQL query and return a streaming result
	// Acquires connection from pool, sends SQL_BATCH, returns result stream
	// Throws on connection failure or initial protocol errors
	unique_ptr<MSSQLResultStream> Execute(ClientContext &context, const std::string &sql);

	//! Spec 075: run `sql` on a connection the caller already holds -- the
	//! transaction's pinned one, or the session a prepared handle lives in. When
	//! `release_to_pool` is false the stream drops its reference at the end and
	//! the caller keeps ownership; when true the stream returns it to the pool as
	//! the pool-acquired path does.
	unique_ptr<MSSQLResultStream> ExecuteOn(ClientContext &context, std::shared_ptr<tds::TdsConnection> connection,
											const std::string &sql, bool transaction_pinned, bool release_to_pool);

	// Validate that the context exists
	void ValidateContext(ClientContext &context);

	// Get the context name
	const std::string &GetContextName() const {
		return context_name_;
	}

private:
	std::string context_name_;
	int acquire_timeout_ms_ = 30000;  // Pool acquire timeout
};

}  // namespace duckdb

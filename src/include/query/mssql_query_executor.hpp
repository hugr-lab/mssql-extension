#pragma once

#include "mssql_result_stream.hpp"
#include "tds/connection_pool.hpp"
#include "duckdb.hpp"
#include <memory>
#include <string>

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// MSSQLQueryExecutor - Orchestrates query execution with pool integration
//===----------------------------------------------------------------------===//

class MSSQLQueryExecutor {
public:
	explicit MSSQLQueryExecutor(const std::string& context_name);
	~MSSQLQueryExecutor() = default;

	// Execute a SQL query and return a streaming result
	// Acquires connection from pool, sends SQL_BATCH, returns result stream
	// Throws on connection failure or initial protocol errors
	unique_ptr<MSSQLResultStream> Execute(ClientContext& context, const std::string& sql);

	// Validate that the context exists
	void ValidateContext(ClientContext& context);

	// Get the context name
	const std::string& GetContextName() const { return context_name_; }

private:
	std::string context_name_;
	int acquire_timeout_ms_ = 30000;  // Pool acquire timeout
};

}  // namespace duckdb

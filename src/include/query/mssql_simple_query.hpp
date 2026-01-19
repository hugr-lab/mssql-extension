#pragma once

#include <functional>
#include <string>
#include <vector>
#include "tds/tds_connection.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLSimpleQuery - Simple query execution for metadata operations
//
// Provides a clean API for executing queries and getting string results.
// Used by catalog operations where full DataChunk streaming is not needed.
//
// Usage:
//   // Get all rows as strings
//   auto result = MSSQLSimpleQuery::Execute(connection, "SELECT name FROM sys.schemas");
//   for (const auto& row : result.rows) {
//       std::string schema_name = row[0];
//   }
//
//   // Get single value
//   std::string collation = MSSQLSimpleQuery::ExecuteScalar(connection, "SELECT ...");
//
//   // Process rows with callback (for large results)
//   MSSQLSimpleQuery::ExecuteWithCallback(connection, sql, [](const std::vector<std::string>& row) {
//       // process row
//       return true; // continue
//   });
//===----------------------------------------------------------------------===//

struct SimpleQueryResult {
	bool success = true;
	std::string error_message;
	uint32_t error_number = 0;

	std::vector<std::string> column_names;
	std::vector<std::vector<std::string>> rows;

	bool HasError() const {
		return !success;
	}
	bool HasRows() const {
		return !rows.empty();
	}
	size_t RowCount() const {
		return rows.size();
	}
};

class MSSQLSimpleQuery {
public:
	// Callback type for streaming row processing
	// Return false to stop processing
	using RowCallback = std::function<bool(const std::vector<std::string> &values)>;

	// Execute a query and return all results as strings
	static SimpleQueryResult Execute(tds::TdsConnection &connection, const std::string &sql, int timeout_ms = 30000);

	// Execute a query with row callback (for large results)
	static SimpleQueryResult ExecuteWithCallback(tds::TdsConnection &connection, const std::string &sql,
												 RowCallback callback, int timeout_ms = 30000);

	// Execute a query and return single scalar value
	static std::string ExecuteScalar(tds::TdsConnection &connection, const std::string &sql, int timeout_ms = 30000);
};

}  // namespace duckdb

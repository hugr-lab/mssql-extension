#pragma once

#include <functional>
#include <string>
#include <vector>
#include "tds/tds_connection.hpp"
#include "tds/tds_token_parser.hpp"

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

	// Affected row count from DONE token (for DML operations)
	int64_t rows_affected = 0;

	//! INFO tokens the server sent: PRINT output, RAISERROR below severity 11,
	//! procedure progress notices. Collected rather than dropped so the callers
	//! that have a ClientContext can log them; this class has none by design --
	//! every catalog metadata query goes through it too, and a normal query
	//! sends no INFO tokens at all, so an untouched empty vector costs nothing.
	std::vector<tds::TdsInfo> info_messages;

	bool HasError() const {
		return !success;
	}

	//! The failure for a human, naming the server only when it spoke:
	//! "SQL Server error 18456: Login failed for user 'sa'." -- or, when
	//! error_number is 0 (a query timeout, a socket failure, and since issue
	//! #323 a TDS parse error), just the message. Two callers used to format
	//! this independently, and when #323 taught one of them not to blame the
	//! server for the client's own parser, the other kept doing it (PR #332
	//! review).
	std::string DescribeError() const {
		if (error_number == 0) {
			return error_message;
		}
		return "SQL Server error " + std::to_string(error_number) + ": " + error_message;
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

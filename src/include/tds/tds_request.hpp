#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace tds {

//! What one request to the server carries (spec 083): T-SQL text sent as a
//! SQL_BATCH, or the body of an RPC request -- a procedure call such as
//! sp_executesql with its parameters bound, without ALL_HEADERS. Implicit from
//! text, so every caller that sends a batch is unchanged.
struct Request {
	//! The batch text; for an RPC, the call written out as T-SQL, for logs and
	//! error messages only (it is not sent).
	std::string sql;
	//! RPCReqBatch: proc id or name, option flags, parameters. Empty for a batch.
	std::vector<uint8_t> rpc_body;

	Request() = default;
	Request(std::string sql_p) : sql(std::move(sql_p)) {}  // NOLINT: implicit by design
	Request(const char *sql_p) : sql(sql_p) {}			   // NOLINT: implicit by design

	static Request Rpc(std::vector<uint8_t> body, std::string description) {
		Request request(std::move(description));
		request.rpc_body = std::move(body);
		return request;
	}

	bool IsRpc() const {
		return !rpc_body.empty();
	}

	bool operator==(const Request &other) const {
		return sql == other.sql && rpc_body == other.rpc_body;
	}
};

}  // namespace tds
}  // namespace duckdb

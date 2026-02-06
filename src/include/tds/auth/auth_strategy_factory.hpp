//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// auth_strategy_factory.hpp
//
// Factory for creating authentication strategies
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#pragma once

#include "tds/auth/auth_strategy.hpp"
#include "tds/auth/fedauth_strategy.hpp"
#include "tds/auth/sql_auth_strategy.hpp"

namespace duckdb {

// Forward declarations
class ClientContext;
struct MSSQLConnectionInfo;

namespace tds {

//===----------------------------------------------------------------------===//
// AuthStrategyFactory - Creates appropriate auth strategy based on connection
//===----------------------------------------------------------------------===//

class AuthStrategyFactory {
public:
	// Create strategy from connection info
	// For Azure auth, also requires ClientContext for secret/token access
	static AuthStrategyPtr Create(const MSSQLConnectionInfo &conn_info, ClientContext *context = nullptr);

	// Create SQL Server auth strategy directly
	static AuthStrategyPtr CreateSqlAuth(const std::string &username, const std::string &password,
	                                     const std::string &database, bool use_encrypt = true);

	// Create Azure FEDAUTH strategy directly
	// Requires ClientContext for token acquisition
	static AuthStrategyPtr CreateFedAuth(ClientContext &context, const std::string &secret_name,
	                                     const std::string &database, const std::string &host,
	                                     const std::string &tenant_override = "");

private:
	// Build token acquirer function that uses DuckDB context
	static TokenAcquirer BuildTokenAcquirer(ClientContext &context);
};

}  // namespace tds
}  // namespace duckdb

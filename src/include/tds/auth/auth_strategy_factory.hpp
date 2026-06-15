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
#include "tds/auth/manual_token_strategy.hpp"
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
	// `app_name` is the resolved LOGIN7 program_name (spec 047 FR-014; pass
	// `ResolveAppName(info)` from the caller, or "" for the default).
	static AuthStrategyPtr CreateSqlAuth(const std::string &username, const std::string &password,
										 const std::string &database, bool use_encrypt = true,
										 const std::string &app_name = "");

	// Create Azure FEDAUTH strategy directly
	// Requires ClientContext for token acquisition
	static AuthStrategyPtr CreateFedAuth(ClientContext &context, const std::string &secret_name,
										 const std::string &database, const std::string &host,
										 const std::string &tenant_override = "", const std::string &app_name = "");

	// Create manual token strategy from pre-provided JWT (Spec 032)
	// Token is validated for format and audience at creation time
	static AuthStrategyPtr CreateManualToken(const std::string &access_token, const std::string &database,
											 const std::string &app_name = "");

private:
	// Build token acquirer function that uses DuckDB context
	static TokenAcquirer BuildTokenAcquirer(ClientContext &context);
};

}  // namespace tds
}  // namespace duckdb

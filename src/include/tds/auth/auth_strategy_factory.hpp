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

#include <cstdint>
#include <string>

namespace duckdb {

// Forward declarations
class ClientContext;
struct MSSQLConnectionInfo;

namespace tds {

//===----------------------------------------------------------------------===//
// AuthStrategyFactory - Creates appropriate auth strategy based on connection
//===----------------------------------------------------------------------===//

// Reverse-resolve a host for SPN construction (issue #259).
//
// Active Directory registers SQL Server SPNs against the FQDN, so a connection
// made by IP -- Server=10.1.2.3,1433 -- yields "MSSQLSvc/10.1.2.3:1433", which
// can never match and fails with a KDC "server not found in Kerberos database"
// that says nothing about the address form being the problem.
//
// Returns the FQDN when `host` is an IP literal that reverse-resolves. Returns
// `host` unchanged when it is already a name (the overwhelmingly common case,
// and no lookup is performed). Returns an empty string when `host` is an IP
// literal that does NOT reverse-resolve, which the caller turns into an error
// naming the fix, because silently building an unmatched SPN is worse.
//
// Separately declared so it can be unit tested without a KDC.
std::string ResolveHostForSpn(const std::string &host);

// The canonical default SPN, "MSSQLSvc/<fqdn>:<port>", with `host` run through
// ResolveHostForSpn first. Throws when `host` is an IP literal with no PTR
// record, because a silently-unmatchable SPN is worse than a clear refusal.
//
// Shared deliberately: mssql_kerberos_auth_test / mssql_winsspi_auth_test are
// the tools users are pointed at when a Kerberos login fails, and each used to
// carry its own copy of this formatting. A diagnostic that reports a different
// SPN than the connection would actually request sends the reader in the wrong
// direction -- exactly the situation issue #259 is about.
std::string DeriveDefaultSpn(const std::string &host, uint16_t port);

// Non-throwing form for the diagnostic functions, whose contract is to RETURN a
// status string rather than abort the query -- an unresolvable IP is exactly
// what a user runs them to diagnose. Returns false and fills `error` with the
// explanation; the caller decides how to prefix it.
bool TryDeriveDefaultSpn(const std::string &host, uint16_t port, std::string &spn, std::string &error);

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

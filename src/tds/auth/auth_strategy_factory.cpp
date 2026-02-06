//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// auth_strategy_factory.cpp
//
// Factory for creating authentication strategies
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#include "tds/auth/auth_strategy_factory.hpp"
#include "azure/azure_token.hpp"
#include "mssql_storage.hpp"

namespace duckdb {
namespace tds {

AuthStrategyPtr AuthStrategyFactory::Create(const MSSQLConnectionInfo &conn_info, ClientContext *context) {
	// Priority: access_token > azure_secret > SQL auth (Spec 032)
	if (!conn_info.access_token.empty()) {
		// Manual token authentication (Spec 032)
		return CreateManualToken(conn_info.access_token, conn_info.database);
	} else if (conn_info.use_azure_auth) {
		// Azure secret-based authentication
		if (!context) {
			throw std::runtime_error("AuthStrategyFactory: ClientContext required for Azure authentication");
		}
		return CreateFedAuth(*context, conn_info.azure_secret_name, conn_info.database, conn_info.host);
	} else {
		// SQL Server authentication
		return CreateSqlAuth(conn_info.user, conn_info.password, conn_info.database, conn_info.use_encrypt);
	}
}

AuthStrategyPtr AuthStrategyFactory::CreateSqlAuth(const std::string &username, const std::string &password,
												   const std::string &database, bool use_encrypt) {
	return std::make_shared<SqlServerAuthStrategy>(username, password, database, use_encrypt);
}

AuthStrategyPtr AuthStrategyFactory::CreateFedAuth(ClientContext &context, const std::string &secret_name,
												   const std::string &database, const std::string &host,
												   const std::string &tenant_override) {
	auto strategy = std::make_shared<FedAuthStrategy>(secret_name, database, host, tenant_override);

	// Set up token acquirer that uses the DuckDB context
	strategy->SetTokenAcquirer(BuildTokenAcquirer(context));

	return strategy;
}

AuthStrategyPtr AuthStrategyFactory::CreateManualToken(const std::string &access_token, const std::string &database) {
	// Spec 032: Create ManualTokenAuthStrategy with pre-provided token
	// Token validation (JWT format, audience, expiration) is done in the constructor
	return std::make_shared<ManualTokenAuthStrategy>(access_token, database);
}

TokenAcquirer AuthStrategyFactory::BuildTokenAcquirer(ClientContext &context) {
	// Capture context by reference - caller must ensure context lifetime
	// In practice, context outlives the connection pool that uses this acquirer
	return [&context](const std::string &secret_name, const std::string &tenant_override) -> std::string {
		// Try to get cached token first
		std::string cached = mssql::azure::TokenCache::Instance().GetToken(secret_name);
		if (!cached.empty()) {
			return cached;
		}

		// Acquire fresh token using DuckDB Azure extension
		auto result = mssql::azure::AcquireToken(context, secret_name, tenant_override);
		if (!result.success) {
			throw std::runtime_error("Azure token acquisition failed: " + result.error_message);
		}
		return result.access_token;
	};
}

}  // namespace tds
}  // namespace duckdb

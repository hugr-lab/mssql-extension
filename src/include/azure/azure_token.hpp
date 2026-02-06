//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_token.hpp
//
// Azure AD token acquisition and caching
//===----------------------------------------------------------------------===//

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include "azure_secret_reader.hpp"
#include "duckdb.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

// Token endpoint base URL
constexpr const char *AZURE_AD_BASE_URL = "login.microsoftonline.com";

// Resource scope for Azure SQL Database
// Note: The trailing slash is required - audience must be "https://database.windows.net/"
// Using "https://database.windows.net//.default" produces correct audience with trailing slash
constexpr const char *AZURE_SQL_SCOPE = "https://database.windows.net//.default";

// Token refresh margin (seconds before expiration to trigger refresh)
constexpr int64_t TOKEN_REFRESH_MARGIN_SECONDS = 300;  // 5 minutes

// Default token lifetime if not specified (seconds)
constexpr int64_t DEFAULT_TOKEN_LIFETIME_SECONDS = 3600;  // 1 hour

// Device code flow constants
constexpr int64_t DEVICE_CODE_DEFAULT_TIMEOUT_SECONDS = 900;  // 15 minutes (Azure AD default)
constexpr int64_t DEVICE_CODE_DEFAULT_INTERVAL_SECONDS = 5;	  // Default polling interval

// Device code grant type
constexpr const char *DEVICE_CODE_GRANT_TYPE = "urn:ietf:params:oauth:grant-type:device_code";

// Default client ID for interactive auth (Azure CLI public client - works in all tenants)
constexpr const char *AZURE_INTERACTIVE_CLIENT_ID = "04b07795-8ddb-461a-bbee-02f9e1bf7b46";

// Default tenant for interactive auth
constexpr const char *AZURE_DEFAULT_TENANT = "common";

//===----------------------------------------------------------------------===//
// TokenResult - Result of a token acquisition attempt
//===----------------------------------------------------------------------===//
struct TokenResult {
	bool success;
	std::string access_token;
	std::string error_message;
	std::chrono::system_clock::time_point expires_at;

	// Factory methods
	static TokenResult Success(const std::string &token, std::chrono::system_clock::time_point expiry) {
		return {true, token, "", expiry};
	}

	static TokenResult Failure(const std::string &error) {
		return {false, "", error, std::chrono::system_clock::time_point{}};
	}
};

//===----------------------------------------------------------------------===//
// CachedToken - Cached token with expiration tracking
//===----------------------------------------------------------------------===//
struct CachedToken {
	std::string access_token;
	std::chrono::system_clock::time_point expires_at;

	// Check if token is still valid (with 5-minute margin)
	bool IsValid() const {
		auto margin = std::chrono::seconds(TOKEN_REFRESH_MARGIN_SECONDS);
		return std::chrono::system_clock::now() < (expires_at - margin);
	}
};

//===----------------------------------------------------------------------===//
// TokenCache - Thread-safe token cache
//===----------------------------------------------------------------------===//
class TokenCache {
public:
	// Get a cached token if available and valid (returns empty string if not found)
	std::string GetToken(const std::string &secret_name);

	// Check if a valid token exists in the cache
	bool HasValidToken(const std::string &secret_name);

	// Store a token in the cache
	void SetToken(const std::string &secret_name, const std::string &token,
				  std::chrono::system_clock::time_point expires_at);

	// Invalidate a specific token
	void Invalidate(const std::string &secret_name);

	// Clear all cached tokens
	void Clear();

	// Get singleton instance
	static TokenCache &Instance();

private:
	std::mutex mutex_;
	std::unordered_map<std::string, CachedToken> cache_;
};

//===----------------------------------------------------------------------===//
// AcquireToken - Main entry point for token acquisition
//
// Attempts to get a token from cache first, then acquires a new token if needed.
// Supports all Azure secret providers: service_principal, credential_chain, managed_identity
//
// Parameters:
//   context          - DuckDB client context
//   secret_name      - Name of the Azure secret to use for authentication
//   tenant_id_override - Optional tenant ID override for interactive auth
//
// Returns:
//   TokenResult with access token on success, error message on failure
//===----------------------------------------------------------------------===//
TokenResult AcquireToken(ClientContext &context, const std::string &secret_name,
						 const std::string &tenant_id_override = "");

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

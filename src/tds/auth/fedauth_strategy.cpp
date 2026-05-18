//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// fedauth_strategy.cpp
//
// Azure AD (FEDAUTH) authentication strategy implementation
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#include "tds/auth/fedauth_strategy.hpp"
#include "azure/azure_token.hpp"
#include "duckdb/main/database.hpp"
#include "tds/encoding/utf16.hpp"

#include <stdexcept>

namespace duckdb {
namespace tds {

FedAuthStrategy::FedAuthStrategy(DatabaseInstance &db, const std::string &secret_name, const std::string &database,
								 const std::string &host, const std::string &tenant_override)
	: db_(db), secret_name_(secret_name), database_(database), host_(host), tenant_override_(tenant_override) {}

PreloginOptions FedAuthStrategy::GetPreloginOptions() const {
	PreloginOptions options;
	options.use_encrypt = true;		 // Azure requires TLS
	options.request_fedauth = true;	 // Request FEDAUTH support
	options.sni_hostname = host_;	 // SNI for Azure routing
	return options;
}

Login7Options FedAuthStrategy::GetLogin7Options() const {
	Login7Options options;
	options.database = database_;
	options.username.clear();  // No username for FEDAUTH
	options.password.clear();  // No password for FEDAUTH
	options.app_name = "DuckDB";
	options.include_fedauth_ext = true;	 // Include FEDAUTH extension
	return options;
}

std::vector<uint8_t> FedAuthStrategy::GetFedAuthToken(const FedAuthInfo &info) {
	if (!token_acquirer_) {
		throw std::runtime_error("FedAuthStrategy: token acquirer not set");
	}

	// Acquire token using the configured acquirer function
	std::string access_token = token_acquirer_(secret_name_, tenant_override_);

	if (access_token.empty()) {
		throw std::runtime_error("FedAuthStrategy: failed to acquire Azure AD token");
	}

	// Encode token as UTF-16LE for FEDAUTH_TOKEN packet
	return encoding::Utf16LEEncode(access_token);
}

void FedAuthStrategy::InvalidateToken() {
	// Invalidate the cached token in TokenCache (spec 047 FR-012 namespace).
	// `tenant_override_` is included in the cache key by AcquireToken when non-empty,
	// so invalidation here matches the same key shape.
	std::string cache_key = secret_name_;
	if (!tenant_override_.empty()) {
		cache_key += ":" + tenant_override_;
	}
	mssql::azure::TokenCache::Instance().Invalidate(db_, cache_key);
}

bool FedAuthStrategy::IsTokenExpired() const {
	std::string cache_key = secret_name_;
	if (!tenant_override_.empty()) {
		cache_key += ":" + tenant_override_;
	}
	std::string cached = mssql::azure::TokenCache::Instance().GetToken(db_, cache_key);
	return cached.empty();	// Empty means expired or not cached
}

void FedAuthStrategy::SetTokenAcquirer(TokenAcquirer acquirer) {
	token_acquirer_ = std::move(acquirer);
}

}  // namespace tds
}  // namespace duckdb

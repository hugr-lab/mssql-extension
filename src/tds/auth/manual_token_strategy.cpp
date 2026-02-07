//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// manual_token_strategy.cpp
//
// Authentication strategy for pre-provided Azure AD access tokens
// Spec 032: FEDAUTH Token Provider Enhancements - US1
//===----------------------------------------------------------------------===//

#include "tds/auth/manual_token_strategy.hpp"
#include "duckdb/common/exception.hpp"
#include "tds/encoding/utf16.hpp"

namespace duckdb {
namespace tds {

ManualTokenAuthStrategy::ManualTokenAuthStrategy(const std::string &access_token, const std::string &database)
	: raw_token_(access_token), database_(database) {
	// Parse and validate the JWT token
	claims_ = mssql::azure::ParseJwtClaims(access_token);

	if (!claims_.valid) {
		throw InvalidInputException(
			"Invalid access token format: unable to parse JWT. %s. "
			"Ensure token is a valid Azure AD access token.",
			claims_.error);
	}

	// No audience validation - the server will reject tokens with wrong audience.
	// Different token providers use different audience formats (with/without trailing slash),
	// e.g. Fabric notebooks return "https://database.windows.net" without trailing slash.

	// Pre-encode token as UTF-16LE for efficiency
	token_utf16le_ = encoding::Utf16LEEncode(access_token);
}

PreloginOptions ManualTokenAuthStrategy::GetPreloginOptions() const {
	PreloginOptions options;
	options.use_encrypt = true;		 // Azure requires TLS
	options.request_fedauth = true;	 // Request FEDAUTH support
	options.sni_hostname.clear();	 // No SNI needed (token already acquired)
	return options;
}

Login7Options ManualTokenAuthStrategy::GetLogin7Options() const {
	Login7Options options;
	options.database = database_;
	options.username.clear();  // No username for FEDAUTH
	options.password.clear();  // No password for FEDAUTH
	options.app_name = "DuckDB";
	options.include_fedauth_ext = true;	 // Include FEDAUTH extension
	return options;
}

std::vector<uint8_t> ManualTokenAuthStrategy::GetFedAuthToken(const FedAuthInfo &info) {
	// Check if token is expired before returning
	if (IsTokenExpired()) {
		throw InvalidInputException("Access token expired at %s. Please provide a new token.",
									mssql::azure::FormatTimestamp(claims_.exp));
	}

	// Return pre-encoded UTF-16LE token
	return token_utf16le_;
}

bool ManualTokenAuthStrategy::IsTokenExpired() const {
	// Use 5-minute margin (300 seconds) to avoid mid-query failures
	return mssql::azure::IsTokenExpired(claims_.exp, 300);
}

}  // namespace tds
}  // namespace duckdb

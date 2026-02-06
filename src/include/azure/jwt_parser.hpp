//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// jwt_parser.hpp
//
// JWT parsing for Azure AD access tokens - claim extraction only
// Spec 032: FEDAUTH Token Provider Enhancements
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// JwtClaims - Parsed claims from Azure AD JWT access token
//===----------------------------------------------------------------------===//

struct JwtClaims {
	int64_t exp;		// Expiration timestamp (Unix seconds)
	std::string aud;	// Audience (resource URL)
	std::string oid;	// Object ID (user/service principal)
	std::string tid;	// Tenant ID
	bool valid;			// Parse success flag
	std::string error;	// Parse error message (if invalid)

	JwtClaims() : exp(0), valid(false) {}
};

//===----------------------------------------------------------------------===//
// JWT Parsing Functions
//===----------------------------------------------------------------------===//

// Parse JWT claims from an Azure AD access token
// Does NOT verify signature - token is already validated by Azure AD
// Returns JwtClaims with valid=true on success, valid=false with error message on failure
JwtClaims ParseJwtClaims(const std::string &token);

// Format Unix timestamp as human-readable UTC string
// Format: "YYYY-MM-DD HH:MM:SS UTC"
std::string FormatTimestamp(int64_t unix_timestamp);

// Check if token is expired (with optional margin in seconds, default 5 minutes)
bool IsTokenExpired(int64_t exp_timestamp, int64_t margin_seconds = 300);

// Expected audience for Azure SQL Database / Fabric
static const char *const AZURE_SQL_AUDIENCE = "https://database.windows.net/";

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

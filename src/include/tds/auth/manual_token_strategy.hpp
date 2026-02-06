//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// manual_token_strategy.hpp
//
// Authentication strategy for pre-provided Azure AD access tokens
// Spec 032: FEDAUTH Token Provider Enhancements - US1
//===----------------------------------------------------------------------===//

#pragma once

#include "azure/jwt_parser.hpp"
#include "tds/auth/auth_strategy.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// ManualTokenAuthStrategy - FEDAUTH with pre-provided token
//===----------------------------------------------------------------------===//
//
// This strategy is used when the user provides an Azure AD access token directly
// via the ACCESS_TOKEN option in ATTACH or MSSQL secret. Unlike FedAuthStrategy,
// this strategy cannot refresh tokens - it simply uses the provided token.
//
// Key differences from FedAuthStrategy:
// - No token acquirer function (token is pre-provided)
// - Cannot refresh tokens (can_refresh = false)
// - Validates token format and audience at construction time
// - Returns clear error message when token expires
//
class ManualTokenAuthStrategy : public AuthenticationStrategy {
public:
	// Construct from a raw JWT access token
	// Validates token format and parses claims
	// Throws InvalidInputException if token is malformed or has wrong audience
	explicit ManualTokenAuthStrategy(const std::string &access_token, const std::string &database);

	//===----------------------------------------------------------------------===//
	// AuthenticationStrategy interface
	//===----------------------------------------------------------------------===//

	bool RequiresFedAuth() const override {
		return true;
	}

	std::string GetName() const override {
		return "ManualToken";
	}

	PreloginOptions GetPreloginOptions() const override;
	Login7Options GetLogin7Options() const override;

	// Returns the pre-provided token as UTF-16LE encoded bytes
	// Throws InvalidInputException if token is expired
	std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo &info) override;

	// Manual tokens cannot be refreshed - this is a no-op
	void InvalidateToken() override {}

	// Check if the token is expired (with 5-minute margin)
	bool IsTokenExpired() const override;

	//===----------------------------------------------------------------------===//
	// Token Information (for logging/debugging)
	//===----------------------------------------------------------------------===//

	// Get parsed claims from the token
	const mssql::azure::JwtClaims &GetClaims() const {
		return claims_;
	}

private:
	std::string raw_token_;				  // Original UTF-8 token
	std::vector<uint8_t> token_utf16le_;  // Pre-encoded UTF-16LE for FEDAUTH
	mssql::azure::JwtClaims claims_;	  // Parsed JWT claims
	std::string database_;				  // Target database name
};

}  // namespace tds
}  // namespace duckdb

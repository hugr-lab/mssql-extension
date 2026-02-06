//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// auth_strategy.hpp
//
// Authentication strategy interface for TDS connections
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// Forward Declarations
//===----------------------------------------------------------------------===//

class TdsConnection;

//===----------------------------------------------------------------------===//
// PreloginOptions - Options for PRELOGIN packet
//===----------------------------------------------------------------------===//

struct PreloginOptions {
	bool use_encrypt = true;	   // Request TLS encryption
	bool request_fedauth = false;  // Include FEDAUTHREQUIRED option
	std::string sni_hostname;	   // SNI hostname for TLS (Azure routing)
};

//===----------------------------------------------------------------------===//
// Login7Options - Options for LOGIN7 packet
//===----------------------------------------------------------------------===//

struct Login7Options {
	std::string database;			   // Target database name
	std::string username;			   // SQL auth username (empty for FEDAUTH)
	std::string password;			   // SQL auth password (empty for FEDAUTH)
	std::string app_name = "DuckDB";   // Application name
	bool include_fedauth_ext = false;  // Include FEDAUTH feature extension
};

//===----------------------------------------------------------------------===//
// FedAuthInfo - Information from FEDAUTHINFO token
//===----------------------------------------------------------------------===//

struct FedAuthInfo {
	std::string sts_url;  // Security Token Service URL
	std::string spn;	  // Service Principal Name (resource)
};

//===----------------------------------------------------------------------===//
// AuthenticationStrategy - Abstract interface for authentication methods
//===----------------------------------------------------------------------===//

class AuthenticationStrategy {
public:
	virtual ~AuthenticationStrategy() = default;

	//===----------------------------------------------------------------------===//
	// Strategy Information
	//===----------------------------------------------------------------------===//

	// Does this strategy use FEDAUTH (Azure AD) authentication?
	virtual bool RequiresFedAuth() const = 0;

	// Human-readable name for logging/debugging
	virtual std::string GetName() const = 0;

	//===----------------------------------------------------------------------===//
	// Authentication Flow Options
	//===----------------------------------------------------------------------===//

	// Get options for PRELOGIN packet
	virtual PreloginOptions GetPreloginOptions() const = 0;

	// Get options for LOGIN7 packet
	virtual Login7Options GetLogin7Options() const = 0;

	//===----------------------------------------------------------------------===//
	// Token Acquisition (FEDAUTH only)
	//===----------------------------------------------------------------------===//

	// Get FEDAUTH token as UTF-16LE encoded bytes
	// Called after receiving FEDAUTHINFO token from server
	// Only called if RequiresFedAuth() returns true
	virtual std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo &info) = 0;

	//===----------------------------------------------------------------------===//
	// Token Refresh (FEDAUTH only)
	//===----------------------------------------------------------------------===//

	// Invalidate cached token (forces re-acquisition on next GetFedAuthToken)
	// Called when auth fails and retry is needed
	virtual void InvalidateToken() {}

	// Check if token is expired and needs refresh
	virtual bool IsTokenExpired() const {
		return false;
	}

protected:
	AuthenticationStrategy() = default;
};

//===----------------------------------------------------------------------===//
// Type alias for shared ownership
//===----------------------------------------------------------------------===//

using AuthStrategyPtr = std::shared_ptr<AuthenticationStrategy>;

}  // namespace tds
}  // namespace duckdb

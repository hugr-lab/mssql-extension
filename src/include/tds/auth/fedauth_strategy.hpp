//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// fedauth_strategy.hpp
//
// Azure AD (FEDAUTH) authentication strategy
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#pragma once

#include "tds/auth/auth_strategy.hpp"

#include <functional>

namespace duckdb {

// Forward declaration for DuckDB context
class ClientContext;

namespace tds {

//===----------------------------------------------------------------------===//
// TokenAcquirer - Function type for acquiring Azure AD tokens
//===----------------------------------------------------------------------===//

// Takes secret name and optional tenant override, returns access token
using TokenAcquirer = std::function<std::string(const std::string &secret_name, const std::string &tenant_override)>;

//===----------------------------------------------------------------------===//
// FedAuthStrategy - Azure AD (FEDAUTH) authentication
//===----------------------------------------------------------------------===//

class FedAuthStrategy : public AuthenticationStrategy {
public:
	// Constructor with secret name and optional tenant override
	FedAuthStrategy(const std::string &secret_name, const std::string &database, const std::string &host,
					const std::string &tenant_override = "");

	~FedAuthStrategy() override = default;

	//===----------------------------------------------------------------------===//
	// AuthenticationStrategy Interface
	//===----------------------------------------------------------------------===//

	bool RequiresFedAuth() const override {
		return true;
	}

	std::string GetName() const override {
		return "AzureFedAuth";
	}

	PreloginOptions GetPreloginOptions() const override;

	Login7Options GetLogin7Options() const override;

	std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo &info) override;

	//===----------------------------------------------------------------------===//
	// Token Management
	//===----------------------------------------------------------------------===//

	void InvalidateToken() override;

	bool IsTokenExpired() const override;

	// Set the token acquirer function (called by factory with DuckDB context)
	void SetTokenAcquirer(TokenAcquirer acquirer);

	// Get the secret name (for logging/debugging)
	const std::string &GetSecretName() const {
		return secret_name_;
	}

private:
	std::string secret_name_;
	std::string database_;
	std::string host_;
	std::string tenant_override_;
	TokenAcquirer token_acquirer_;
};

}  // namespace tds
}  // namespace duckdb

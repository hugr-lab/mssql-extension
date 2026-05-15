//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// integrated_auth_strategy.hpp
//
// Adapter that wraps a multi-round IAuthenticator (Krb5Authenticator on
// POSIX, WinSspiAuthenticator on Windows) in the existing single-shot
// AuthenticationStrategy interface so the connection-pool / ATTACH paths
// can dispatch through one common type.
//
// Spec 042 Phase 3.
//===----------------------------------------------------------------------===//

#pragma once

#include "tds/auth/auth_strategy.hpp"
#include "tds/auth/iauthenticator.hpp"

#include <memory>
#include <string>

namespace duckdb {
namespace tds {

class IntegratedAuthStrategy : public AuthenticationStrategy {
public:
	IntegratedAuthStrategy(std::shared_ptr<IAuthenticator> authenticator, std::string database,
						   std::string display_name, bool use_encrypt = true)
		: authenticator_(std::move(authenticator)),
		  database_(std::move(database)),
		  display_name_(std::move(display_name)),
		  use_encrypt_(use_encrypt) {}

	bool RequiresFedAuth() const override {
		return false;
	}

	std::string GetName() const override {
		return display_name_;
	}

	PreloginOptions GetPreloginOptions() const override {
		PreloginOptions opts;
		opts.use_encrypt = use_encrypt_;
		opts.request_fedauth = false;
		return opts;
	}

	Login7Options GetLogin7Options() const override {
		Login7Options opts;
		opts.database = database_;
		opts.app_name = "DuckDB MSSQL Extension";
		opts.username = "";	 // not used in integrated auth
		opts.password = "";	 // not used in integrated auth
		opts.include_fedauth_ext = false;
		return opts;
	}

	std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo & /*info*/) override {
		// Not a FEDAUTH strategy. Throwing keeps the error path obvious if a
		// caller misroutes.
		throw std::runtime_error(
			"IntegratedAuthStrategy::GetFedAuthToken should not be called; integrated auth does not use FEDAUTH.");
	}

	AuthenticatorPtr GetAuthenticator() override {
		return authenticator_;
	}

private:
	std::shared_ptr<IAuthenticator> authenticator_;
	std::string database_;
	std::string display_name_;
	bool use_encrypt_;
};

}  // namespace tds
}  // namespace duckdb

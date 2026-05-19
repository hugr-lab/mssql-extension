//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// sql_auth_strategy.hpp
//
// SQL Server authentication strategy (username/password)
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#pragma once

#include "tds/auth/auth_strategy.hpp"

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// SqlServerAuthStrategy - Traditional SQL Server authentication
//===----------------------------------------------------------------------===//

class SqlServerAuthStrategy : public AuthenticationStrategy {
public:
	// `app_name` is the LOGIN7 program_name (spec 047 FR-014). Empty = use
	// the extension default. Pre-resolved via `ResolveAppName()` at the
	// factory layer so each strategy stores the literal string to send.
	SqlServerAuthStrategy(const std::string &username, const std::string &password, const std::string &database,
						  bool use_encrypt = true, const std::string &app_name = "");

	~SqlServerAuthStrategy() override = default;

	//===----------------------------------------------------------------------===//
	// AuthenticationStrategy Interface
	//===----------------------------------------------------------------------===//

	bool RequiresFedAuth() const override {
		return false;
	}

	std::string GetName() const override {
		return "SqlServerAuth";
	}

	PreloginOptions GetPreloginOptions() const override;

	Login7Options GetLogin7Options() const override;

	// Not used for SQL auth - returns empty vector
	std::vector<uint8_t> GetFedAuthToken(const FedAuthInfo &info) override;

private:
	std::string username_;
	std::string password_;
	std::string database_;
	bool use_encrypt_;
	std::string app_name_;
};

}  // namespace tds
}  // namespace duckdb

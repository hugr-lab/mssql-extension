//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// sql_auth_strategy.cpp
//
// SQL Server authentication strategy implementation
// Spec 031: Connection & FEDAUTH Refactoring - US7
//===----------------------------------------------------------------------===//

#include "tds/auth/sql_auth_strategy.hpp"

namespace duckdb {
namespace tds {

SqlServerAuthStrategy::SqlServerAuthStrategy(const std::string &username, const std::string &password,
											 const std::string &database, bool use_encrypt)
	: username_(username), password_(password), database_(database), use_encrypt_(use_encrypt) {}

PreloginOptions SqlServerAuthStrategy::GetPreloginOptions() const {
	PreloginOptions options;
	options.use_encrypt = use_encrypt_;
	options.request_fedauth = false;  // SQL auth doesn't use FEDAUTH
	options.sni_hostname.clear();	  // No SNI needed for SQL auth
	return options;
}

Login7Options SqlServerAuthStrategy::GetLogin7Options() const {
	Login7Options options;
	options.database = database_;
	options.username = username_;
	options.password = password_;
	options.app_name = "DuckDB";
	options.include_fedauth_ext = false;  // SQL auth doesn't use FEDAUTH
	return options;
}

std::vector<uint8_t> SqlServerAuthStrategy::GetFedAuthToken(const FedAuthInfo &info) {
	// SQL auth doesn't use FEDAUTH tokens
	return {};
}

}  // namespace tds
}  // namespace duckdb

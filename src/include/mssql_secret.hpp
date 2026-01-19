//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// mssql_secret.hpp
//
// Secret type registration for MSSQL credentials
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

// Secret field names (constants)
constexpr const char *MSSQL_SECRET_HOST = "host";
constexpr const char *MSSQL_SECRET_PORT = "port";
constexpr const char *MSSQL_SECRET_DATABASE = "database";
constexpr const char *MSSQL_SECRET_USER = "user";
constexpr const char *MSSQL_SECRET_PASSWORD = "password";
constexpr const char *MSSQL_SECRET_USE_ENCRYPT = "use_encrypt";	 // Optional, defaults to false

// Register MSSQL secret type and creation function
void RegisterMSSQLSecretType(ExtensionLoader &loader);

// Create secret from user-provided parameters
// Throws: InvalidInputException on validation failure
unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(ClientContext &context, CreateSecretInput &input);

// Validate secret fields
// Returns: empty string if valid, error message if invalid
string ValidateMSSQLSecretFields(const CreateSecretInput &input);

}  // namespace duckdb

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
constexpr const char *MSSQL_SECRET_USE_ENCRYPT = "use_encrypt";        // Optional, defaults to true
constexpr const char *MSSQL_SECRET_CATALOG = "catalog";                // Optional, defaults to true
constexpr const char *MSSQL_SECRET_AZURE_SECRET = "azure_secret";      // Optional, for Azure AD auth
constexpr const char *MSSQL_SECRET_AZURE_TENANT_ID = "azure_tenant_id"; // Optional, tenant for interactive auth

// Register MSSQL secret type and creation function
void RegisterMSSQLSecretType(ExtensionLoader &loader);

// Create secret from user-provided parameters
// Throws: InvalidInputException on validation failure
unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(ClientContext &context, CreateSecretInput &input);

// Validate secret fields (with context for Azure secret validation)
// Returns: empty string if valid, error message if invalid
string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input);

}  // namespace duckdb

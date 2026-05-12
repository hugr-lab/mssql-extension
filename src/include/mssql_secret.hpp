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
constexpr const char *MSSQL_SECRET_USE_ENCRYPT = "use_encrypt";			 // Optional, defaults to true
constexpr const char *MSSQL_SECRET_CATALOG = "catalog";					 // Optional, defaults to true
constexpr const char *MSSQL_SECRET_AZURE_SECRET = "azure_secret";		 // Optional, for Azure AD auth
constexpr const char *MSSQL_SECRET_AZURE_TENANT_ID = "azure_tenant_id";	 // Optional, tenant for interactive auth
constexpr const char *MSSQL_SECRET_ACCESS_TOKEN = "access_token";	 // Optional, direct Azure AD JWT token (Spec 032)
constexpr const char *MSSQL_SECRET_SCHEMA_FILTER = "schema_filter";	 // Optional, regex schema visibility filter
constexpr const char *MSSQL_SECRET_TABLE_FILTER = "table_filter";	 // Optional, regex table visibility filter

// Spec 042: Integrated Authentication (Kerberos / SSPI). All optional.
constexpr const char *MSSQL_SECRET_AUTHENTICATOR = "authenticator";			   // krb5 / winsspi
constexpr const char *MSSQL_SECRET_KRB5_CONFIGFILE = "krb5_configfile";		   // /etc/krb5.conf override
constexpr const char *MSSQL_SECRET_KRB5_KEYTABFILE = "krb5_keytabfile";		   // Selects keytab credential mode
constexpr const char *MSSQL_SECRET_KRB5_CREDCACHEFILE = "krb5_credcachefile";  // ccache path override
constexpr const char *MSSQL_SECRET_KRB5_REALM = "krb5_realm";				   // AD realm (uppercased)
constexpr const char *MSSQL_SECRET_KRB5_DNSLOOKUPKDC = "krb5_dnslookupkdc";	   // bool, krb5.conf override
constexpr const char *MSSQL_SECRET_SPN = "service_principal_name";			   // Override default SPN derivation

// Register MSSQL secret type and creation function
void RegisterMSSQLSecretType(ExtensionLoader &loader);

// Create secret from user-provided parameters
// Throws: InvalidInputException on validation failure
unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(ClientContext &context, CreateSecretInput &input);

// Validate secret fields (with context for Azure secret validation)
// Returns: empty string if valid, error message if invalid
string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input);

}  // namespace duckdb

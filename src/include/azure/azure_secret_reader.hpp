//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_secret_reader.hpp
//
// Read Azure secrets from DuckDB's SecretManager
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include "duckdb.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// AzureSecretInfo - Parsed information from a DuckDB Azure secret
//===----------------------------------------------------------------------===//
struct AzureSecretInfo {
	std::string secret_name;
	std::string provider;		// service_principal, credential_chain, managed_identity
	std::string tenant_id;		// Required for service_principal, optional for others
	std::string client_id;		// Required for service_principal, optional for managed_identity
	std::string client_secret;	// Required for service_principal only
	std::string chain;			// For credential_chain: cli, env, managed_identity, interactive
};

//===----------------------------------------------------------------------===//
// ReadAzureSecret - Read and parse an Azure secret from SecretManager
//
// Parameters:
//   context     - DuckDB client context
//   secret_name - Name of the Azure secret to read
//
// Returns:
//   AzureSecretInfo with parsed secret fields
//
// Throws:
//   InvalidInputException if secret not found, wrong type, or Azure extension not loaded
//===----------------------------------------------------------------------===//
AzureSecretInfo ReadAzureSecret(ClientContext &context, const std::string &secret_name);

//===----------------------------------------------------------------------===//
// ValidateAzureSecretExists - Check if an Azure secret exists (lightweight)
//
// Parameters:
//   context     - DuckDB client context
//   secret_name - Name of the Azure secret to check
//
// Returns:
//   true if secret exists and is TYPE azure, false otherwise
//===----------------------------------------------------------------------===//
bool ValidateAzureSecretExists(ClientContext &context, const std::string &secret_name);

//===----------------------------------------------------------------------===//
// GetAzureSecretType - Get the type of a secret (for error messages)
//
// Parameters:
//   context     - DuckDB client context
//   secret_name - Name of the secret to check
//
// Returns:
//   Secret type as string, or empty string if not found
//===----------------------------------------------------------------------===//
std::string GetAzureSecretType(ClientContext &context, const std::string &secret_name);

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

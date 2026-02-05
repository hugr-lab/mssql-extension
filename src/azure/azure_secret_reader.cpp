//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_secret_reader.cpp
//
// Implementation of Azure secret reading from DuckDB's SecretManager
//===----------------------------------------------------------------------===//

#include "azure/azure_secret_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {
namespace mssql {
namespace azure {

AzureSecretInfo ReadAzureSecret(ClientContext &context, const std::string &secret_name) {
	if (secret_name.empty()) {
		throw InvalidInputException("Error: Secret name required");
	}

	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	// Try to lookup the secret by name
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
	if (!secret_entry) {
		throw InvalidInputException("Error: Azure secret '%s' not found", secret_name);
	}

	auto &secret = secret_entry->secret;

	// Check if it's an Azure secret
	if (secret->GetType() != "azure") {
		throw InvalidInputException("Error: Secret '%s' is not an Azure secret (type: %s)",
		                            secret_name, secret->GetType());
	}

	// Use static_cast - we've already verified it's an Azure secret which is always KeyValueSecret
	// This avoids dynamic_cast RTTI warnings when crossing extension boundaries on macOS
	auto &kv_secret = static_cast<const KeyValueSecret &>(*secret);

	AzureSecretInfo info;
	info.secret_name = secret_name;

	// Read provider from secret metadata (built-in field)
	info.provider = kv_secret.GetProvider();
	if (info.provider.empty()) {
		throw InvalidInputException("Azure secret '%s' missing 'provider' field", secret_name);
	}

	// Read optional fields based on provider
	auto tenant_val = kv_secret.TryGetValue("tenant_id");
	if (!tenant_val.IsNull()) {
		info.tenant_id = tenant_val.ToString();
	}

	auto client_id_val = kv_secret.TryGetValue("client_id");
	if (!client_id_val.IsNull()) {
		info.client_id = client_id_val.ToString();
	}

	auto client_secret_val = kv_secret.TryGetValue("client_secret");
	if (!client_secret_val.IsNull()) {
		info.client_secret = client_secret_val.ToString();
	}

	auto chain_val = kv_secret.TryGetValue("chain");
	if (!chain_val.IsNull()) {
		info.chain = chain_val.ToString();
	}

	// Validate required fields per provider
	if (info.provider == "service_principal") {
		if (info.tenant_id.empty() || info.client_id.empty() || info.client_secret.empty()) {
			throw InvalidInputException(
			    "Service principal requires tenant_id, client_id, client_secret");
		}
	}

	return info;
}

bool ValidateAzureSecretExists(ClientContext &context, const std::string &secret_name) {
	if (secret_name.empty()) {
		return false;
	}

	try {
		auto &secret_manager = SecretManager::Get(context);
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
		return secret_entry && secret_entry->secret->GetType() == "azure";
	} catch (...) {
		return false;
	}
}

std::string GetAzureSecretType(ClientContext &context, const std::string &secret_name) {
	if (secret_name.empty()) {
		return "";
	}

	try {
		auto &secret_manager = SecretManager::Get(context);
		auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
		auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
		if (secret_entry) {
			return secret_entry->secret->GetType();
		}
	} catch (...) {
		// Ignore errors
	}
	return "";
}

} // namespace azure
} // namespace mssql
} // namespace duckdb

#include "mssql_secret.hpp"
#include "azure/azure_secret_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Validation
//===----------------------------------------------------------------------===//

string ValidateMSSQLSecretFields(ClientContext &context, const CreateSecretInput &input) {
	// Check for access_token parameter first (Spec 032: takes precedence over azure_secret)
	auto token_it = input.options.find(MSSQL_SECRET_ACCESS_TOKEN);
	bool has_access_token = token_it != input.options.end() && !token_it->second.ToString().empty();

	// Check for azure_secret parameter
	auto azure_it = input.options.find(MSSQL_SECRET_AZURE_SECRET);
	bool has_azure_secret = azure_it != input.options.end() && !azure_it->second.ToString().empty();

	if (has_access_token) {
		// Manual token auth mode (Spec 032) - only require host, port, database
		// Token validation (JWT format, audience) is done at connection time
		const char *required_fields[] = {MSSQL_SECRET_HOST, MSSQL_SECRET_DATABASE};
		for (auto field : required_fields) {
			auto it = input.options.find(field);
			if (it == input.options.end()) {
				return StringUtil::Format("Missing required field '%s'. Provide %s parameter when creating secret.",
										  field, field);
			}
			auto str_val = it->second.ToString();
			if (str_val.empty()) {
				return StringUtil::Format("Field '%s' cannot be empty.", field);
			}
		}
	} else if (has_azure_secret) {
		// Azure auth mode - validate Azure secret exists and is correct type
		std::string azure_name = azure_it->second.ToString();

		// Check if secret exists and is TYPE azure
		if (!mssql::azure::ValidateAzureSecretExists(context, azure_name)) {
			// Check if it exists but is wrong type
			std::string actual_type = mssql::azure::GetAzureSecretType(context, azure_name);
			if (!actual_type.empty()) {
				return StringUtil::Format("Error: Secret '%s' is not an Azure secret (type: %s)", azure_name,
										  actual_type);
			}
			return StringUtil::Format("Error: Azure secret '%s' not found", azure_name);
		}

		// user/password are optional with Azure auth
		// Only require host, port, database
		const char *required_fields[] = {MSSQL_SECRET_HOST, MSSQL_SECRET_DATABASE};
		for (auto field : required_fields) {
			auto it = input.options.find(field);
			if (it == input.options.end()) {
				return StringUtil::Format("Missing required field '%s'. Provide %s parameter when creating secret.",
										  field, field);
			}
			auto str_val = it->second.ToString();
			if (str_val.empty()) {
				return StringUtil::Format("Field '%s' cannot be empty.", field);
			}
		}
	} else {
		// SQL auth mode - require user and password
		auto user_it = input.options.find(MSSQL_SECRET_USER);
		auto pass_it = input.options.find(MSSQL_SECRET_PASSWORD);

		bool has_user = user_it != input.options.end() && !user_it->second.ToString().empty();
		bool has_password = pass_it != input.options.end() && !pass_it->second.ToString().empty();

		if (!has_user && !has_password) {
			return "Either user/password or azure_secret required";
		}

		// Check required string fields for SQL auth
		const char *required_string_fields[] = {MSSQL_SECRET_HOST, MSSQL_SECRET_DATABASE, MSSQL_SECRET_USER,
												MSSQL_SECRET_PASSWORD};

		for (auto field : required_string_fields) {
			auto it = input.options.find(field);
			if (it == input.options.end()) {
				return StringUtil::Format("Missing required field '%s'. Provide %s parameter when creating secret.",
										  field, field);
			}
			auto str_val = it->second.ToString();
			if (str_val.empty()) {
				return StringUtil::Format("Field '%s' cannot be empty.", field);
			}
		}
	}

	// Check port field (optional, defaults to 1433)
	auto port_it = input.options.find(MSSQL_SECRET_PORT);
	if (port_it != input.options.end()) {
		// Validate port range if provided
		int64_t port_value;
		try {
			port_value = port_it->second.GetValue<int64_t>();
		} catch (...) {
			return StringUtil::Format("Port must be a valid integer. Got: %s", port_it->second.ToString());
		}

		if (port_value < 1 || port_value > 65535) {
			return StringUtil::Format("Port must be between 1 and 65535. Got: %lld", port_value);
		}
	}

	return "";	// Valid
}

//===----------------------------------------------------------------------===//
// Secret creation
//===----------------------------------------------------------------------===//

unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	// Validate all fields (with context for Azure secret validation)
	string error = ValidateMSSQLSecretFields(context, input);
	if (!error.empty()) {
		throw InvalidInputException("MSSQL Error: %s", error);
	}

	// Create KeyValueSecret with all fields
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	// Copy all values to the secret (TrySetValue looks up the key from input.options)
	result->TrySetValue(MSSQL_SECRET_HOST, input);
	result->TrySetValue(MSSQL_SECRET_PORT, input);
	result->TrySetValue(MSSQL_SECRET_DATABASE, input);

	// user/password are optional if access_token or azure_secret is provided
	auto token_it = input.options.find(MSSQL_SECRET_ACCESS_TOKEN);
	bool has_access_token = token_it != input.options.end() && !token_it->second.ToString().empty();

	auto azure_it = input.options.find(MSSQL_SECRET_AZURE_SECRET);
	bool has_azure_secret = azure_it != input.options.end() && !azure_it->second.ToString().empty();

	if (has_access_token) {
		// Spec 032: Manual token authentication - store access_token
		result->TrySetValue(MSSQL_SECRET_ACCESS_TOKEN, input);
		// Mark access_token as redacted (hidden in duckdb_secrets() output)
		result->redact_keys.insert(MSSQL_SECRET_ACCESS_TOKEN);
	} else if (has_azure_secret) {
		result->TrySetValue(MSSQL_SECRET_AZURE_SECRET, input);
		// Also store azure_tenant_id if provided (required for interactive auth)
		result->TrySetValue(MSSQL_SECRET_AZURE_TENANT_ID, input);
	}

	// Only set user/password if provided
	auto user_it = input.options.find(MSSQL_SECRET_USER);
	if (user_it != input.options.end() && !user_it->second.ToString().empty()) {
		result->TrySetValue(MSSQL_SECRET_USER, input);
	}
	auto pass_it = input.options.find(MSSQL_SECRET_PASSWORD);
	if (pass_it != input.options.end() && !pass_it->second.ToString().empty()) {
		result->TrySetValue(MSSQL_SECRET_PASSWORD, input);
	}

	// Handle optional use_encrypt parameter (defaults to true for security)
	auto use_ssl_it = input.options.find(MSSQL_SECRET_USE_ENCRYPT);
	if (use_ssl_it != input.options.end()) {
		result->TrySetValue(MSSQL_SECRET_USE_ENCRYPT, input);
	} else {
		// Set default value of true (TLS enabled by default for security)
		result->secret_map[MSSQL_SECRET_USE_ENCRYPT] = Value::BOOLEAN(true);
	}

	// Handle optional catalog parameter (defaults to true if not provided)
	// When false, catalog integration is disabled (raw query mode only)
	auto catalog_it = input.options.find(MSSQL_SECRET_CATALOG);
	if (catalog_it != input.options.end()) {
		result->TrySetValue(MSSQL_SECRET_CATALOG, input);
	} else {
		// Set default value of true (enable catalog integration)
		result->secret_map[MSSQL_SECRET_CATALOG] = Value::BOOLEAN(true);
	}

	// Handle optional catalog visibility filters (Spec 033)
	result->TrySetValue(MSSQL_SECRET_SCHEMA_FILTER, input);
	result->TrySetValue(MSSQL_SECRET_TABLE_FILTER, input);

	// Mark password as redacted (hidden in duckdb_secrets() output)
	result->redact_keys.insert(MSSQL_SECRET_PASSWORD);

	return std::move(result);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterMSSQLSecretType(ExtensionLoader &loader) {
	// Register secret type
	SecretType mssql_type;
	mssql_type.name = "mssql";
	mssql_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	mssql_type.default_provider = "config";

	loader.RegisterSecretType(mssql_type);

	// Register creation function with named parameters
	CreateSecretFunction create_func;
	create_func.secret_type = "mssql";
	create_func.provider = "config";
	create_func.function = CreateMSSQLSecretFromConfig;
	create_func.named_parameters[MSSQL_SECRET_HOST] = LogicalType::VARCHAR;
	create_func.named_parameters[MSSQL_SECRET_PORT] = LogicalType::INTEGER;
	create_func.named_parameters[MSSQL_SECRET_DATABASE] = LogicalType::VARCHAR;
	create_func.named_parameters[MSSQL_SECRET_USER] = LogicalType::VARCHAR;
	create_func.named_parameters[MSSQL_SECRET_PASSWORD] = LogicalType::VARCHAR;
	create_func.named_parameters[MSSQL_SECRET_USE_ENCRYPT] = LogicalType::BOOLEAN;	 // Optional
	create_func.named_parameters[MSSQL_SECRET_CATALOG] = LogicalType::BOOLEAN;		 // Optional, defaults to true
	create_func.named_parameters[MSSQL_SECRET_AZURE_SECRET] = LogicalType::VARCHAR;	 // Optional, for Azure AD auth
	create_func.named_parameters[MSSQL_SECRET_AZURE_TENANT_ID] =
		LogicalType::VARCHAR;  // Optional, tenant for interactive auth
	create_func.named_parameters[MSSQL_SECRET_ACCESS_TOKEN] =
		LogicalType::VARCHAR;  // Optional, direct Azure AD JWT token (Spec 032)
	create_func.named_parameters[MSSQL_SECRET_SCHEMA_FILTER] =
		LogicalType::VARCHAR;  // Optional, regex schema visibility filter (Spec 033)
	create_func.named_parameters[MSSQL_SECRET_TABLE_FILTER] =
		LogicalType::VARCHAR;  // Optional, regex table visibility filter (Spec 033)

	loader.RegisterFunction(std::move(create_func));
}

}  // namespace duckdb

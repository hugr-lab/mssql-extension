#include "mssql_secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Validation
//===----------------------------------------------------------------------===//

string ValidateMSSQLSecretFields(const CreateSecretInput &input) {
	// Check required string fields
	const char *required_string_fields[] = {MSSQL_SECRET_HOST, MSSQL_SECRET_DATABASE, MSSQL_SECRET_USER,
	                                        MSSQL_SECRET_PASSWORD};

	for (auto field : required_string_fields) {
		auto it = input.options.find(field);
		if (it == input.options.end()) {
			return StringUtil::Format("Missing required field '%s'. Provide %s parameter when creating secret.", field,
			                          field);
		}
		auto str_val = it->second.ToString();
		if (str_val.empty()) {
			return StringUtil::Format("Field '%s' cannot be empty.", field);
		}
	}

	// Check port field
	auto port_it = input.options.find(MSSQL_SECRET_PORT);
	if (port_it == input.options.end()) {
		return "Missing required field 'port'. Provide port parameter when creating secret.";
	}

	// Validate port range
	int64_t port_value;
	try {
		port_value = port_it->second.GetValue<int64_t>();
	} catch (...) {
		return StringUtil::Format("Port must be a valid integer. Got: %s", port_it->second.ToString());
	}

	if (port_value < 1 || port_value > 65535) {
		return StringUtil::Format("Port must be between 1 and 65535. Got: %lld", port_value);
	}

	return "";  // Valid
}

//===----------------------------------------------------------------------===//
// Secret creation
//===----------------------------------------------------------------------===//

unique_ptr<BaseSecret> CreateMSSQLSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
	// Validate all fields
	string error = ValidateMSSQLSecretFields(input);
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
	result->TrySetValue(MSSQL_SECRET_USER, input);
	result->TrySetValue(MSSQL_SECRET_PASSWORD, input);

	// Handle optional use_ssl parameter (defaults to false if not provided)
	auto use_ssl_it = input.options.find(MSSQL_SECRET_USE_SSL);
	if (use_ssl_it != input.options.end()) {
		result->TrySetValue(MSSQL_SECRET_USE_SSL, input);
	} else {
		// Set default value of false
		result->secret_map[MSSQL_SECRET_USE_SSL] = Value::BOOLEAN(false);
	}

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
	create_func.named_parameters[MSSQL_SECRET_USE_SSL] = LogicalType::BOOLEAN;  // Optional

	loader.RegisterFunction(std::move(create_func));
}

}  // namespace duckdb

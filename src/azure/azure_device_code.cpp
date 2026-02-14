//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_device_code.cpp
//
// OAuth2 Device Authorization Grant (RFC 8628) implementation
//===----------------------------------------------------------------------===//

#include "azure/azure_device_code.hpp"
#include "azure/azure_http.hpp"
#include "duckdb/common/exception.hpp"

#include <chrono>
#include <iostream>
#include <map>
#include <thread>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// JSON Helper Functions
//===----------------------------------------------------------------------===//

// Parse JSON string value (simple parser for known Azure AD responses)
static std::string ParseJsonString(const std::string &json, const std::string &key) {
	std::string search_key = "\"" + key + "\"";
	auto key_pos = json.find(search_key);
	if (key_pos == std::string::npos) {
		return "";
	}

	auto colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return "";
	}

	auto value_start = json.find('"', colon_pos);
	if (value_start == std::string::npos) {
		return "";
	}

	auto value_end = json.find('"', value_start + 1);
	if (value_end == std::string::npos) {
		return "";
	}

	return json.substr(value_start + 1, value_end - value_start - 1);
}

// Parse JSON integer value
static int ParseJsonInt(const std::string &json, const std::string &key) {
	std::string search_key = "\"" + key + "\"";
	auto key_pos = json.find(search_key);
	if (key_pos == std::string::npos) {
		return 0;
	}

	auto colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return 0;
	}

	auto value_start = colon_pos + 1;
	while (value_start < json.length() && (json[value_start] == ' ' || json[value_start] == '\t')) {
		value_start++;
	}

	int value = 0;
	while (value_start < json.length() && isdigit(json[value_start])) {
		value = value * 10 + (json[value_start] - '0');
		value_start++;
	}

	return value;
}

//===----------------------------------------------------------------------===//
// RequestDeviceCode
//===----------------------------------------------------------------------===//

DeviceCodeResponse RequestDeviceCode(const std::string &tenant_id, const std::string &client_id) {
	std::string effective_tenant = tenant_id.empty() ? AZURE_DEFAULT_TENANT : tenant_id;
	std::string effective_client = client_id.empty() ? AZURE_INTERACTIVE_CLIENT_ID : client_id;

	std::string path = "/" + effective_tenant + "/oauth2/v2.0/devicecode";

	std::map<std::string, std::string> params;
	params["client_id"] = effective_client;
	params["scope"] = AZURE_SQL_SCOPE;

	auto response = HttpPost(AZURE_AD_BASE_URL, path, params);

	if (!response.error.empty()) {
		throw InvalidInputException("HTTP request failed: %s", response.error);
	}

	if (response.status != 200) {
		std::string error = ParseJsonString(response.body, "error");
		std::string error_desc = ParseJsonString(response.body, "error_description");
		if (!error.empty()) {
			throw InvalidInputException("Device code request failed: %s - %s", error, error_desc);
		}
		throw InvalidInputException("HTTP error %d from Azure AD", response.status);
	}

	// Parse response
	DeviceCodeResponse result;
	result.device_code = ParseJsonString(response.body, "device_code");
	result.user_code = ParseJsonString(response.body, "user_code");
	result.verification_uri = ParseJsonString(response.body, "verification_uri");
	result.message = ParseJsonString(response.body, "message");
	result.expires_in = ParseJsonInt(response.body, "expires_in");
	result.interval = ParseJsonInt(response.body, "interval");

	if (result.device_code.empty()) {
		throw InvalidInputException("Invalid device code response from Azure AD");
	}

	// Set defaults if not provided
	if (result.expires_in == 0) {
		result.expires_in = static_cast<int>(DEVICE_CODE_DEFAULT_TIMEOUT_SECONDS);
	}
	if (result.interval == 0) {
		result.interval = static_cast<int>(DEVICE_CODE_DEFAULT_INTERVAL_SECONDS);
	}

	return result;
}

//===----------------------------------------------------------------------===//
// PollForToken
//===----------------------------------------------------------------------===//

TokenResult PollForToken(const std::string &tenant_id, const std::string &client_id, const std::string &device_code,
						 int interval, int timeout) {
	std::string effective_tenant = tenant_id.empty() ? AZURE_DEFAULT_TENANT : tenant_id;
	std::string effective_client = client_id.empty() ? AZURE_INTERACTIVE_CLIENT_ID : client_id;

	std::string path = "/" + effective_tenant + "/oauth2/v2.0/token";

	std::map<std::string, std::string> params;
	params["grant_type"] = DEVICE_CODE_GRANT_TYPE;
	params["client_id"] = effective_client;
	params["device_code"] = device_code;

	auto start_time = std::chrono::steady_clock::now();
	auto timeout_duration = std::chrono::seconds(timeout);

	while (true) {
		// Check timeout
		auto elapsed = std::chrono::steady_clock::now() - start_time;
		if (elapsed >= timeout_duration) {
			return TokenResult::Failure("Error: Device code expired. Please try again.");
		}

		auto response = HttpPost(AZURE_AD_BASE_URL, path, params);

		if (!response.error.empty()) {
			// Network error - retry with backoff
			std::this_thread::sleep_for(std::chrono::seconds(interval * 2));
			continue;
		}

		// For device code flow, 400 errors with specific error codes are expected during polling
		std::string error = ParseJsonString(response.body, "error");
		if (!error.empty()) {
			if (error == "authorization_pending") {
				// User hasn't completed login yet, continue polling
				std::this_thread::sleep_for(std::chrono::seconds(interval));
				continue;
			} else if (error == "authorization_declined") {
				return TokenResult::Failure("Error: Authorization was declined by user");
			} else if (error == "expired_token") {
				return TokenResult::Failure("Error: Device code expired. Please try again.");
			} else if (error == "bad_verification_code") {
				return TokenResult::Failure("Error: Invalid device code. Please try again.");
			} else {
				std::string error_desc = ParseJsonString(response.body, "error_description");
				return TokenResult::Failure("Error during authentication: " + error_desc);
			}
		}

		// Parse successful response
		std::string access_token = ParseJsonString(response.body, "access_token");
		if (access_token.empty()) {
			return TokenResult::Failure("Error: No access token in response");
		}

		int expires_in = ParseJsonInt(response.body, "expires_in");
		if (expires_in == 0) {
			expires_in = static_cast<int>(DEFAULT_TOKEN_LIFETIME_SECONDS);
		}

		auto expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
		return TokenResult::Success(access_token, expires_at);
	}
}

//===----------------------------------------------------------------------===//
// DisplayDeviceCodeMessage
//===----------------------------------------------------------------------===//

void DisplayDeviceCodeMessage(const DeviceCodeResponse &response) {
	if (!response.message.empty()) {
		std::cerr << response.message << std::endl;
	} else {
		std::cerr << "To sign in, visit " << response.verification_uri << " and enter code " << response.user_code
				  << std::endl;
	}
}

//===----------------------------------------------------------------------===//
// AcquireInteractiveToken
//===----------------------------------------------------------------------===//

TokenResult AcquireInteractiveToken(const AzureSecretInfo &info) {
	// Note: Exceptions propagate to AcquireToken() for proper error message extraction

	// Request device code
	auto device_code_response = RequestDeviceCode(info.tenant_id, info.client_id);

	// Display message to user
	DisplayDeviceCodeMessage(device_code_response);

	// Poll for token
	return PollForToken(info.tenant_id, info.client_id, device_code_response.device_code, device_code_response.interval,
						device_code_response.expires_in);
}

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

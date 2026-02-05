//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_device_code.cpp
//
// OAuth2 Device Authorization Grant (RFC 8628) implementation
//===----------------------------------------------------------------------===//

#include "azure/azure_device_code.hpp"
#include "duckdb/common/exception.hpp"

#include <curl/curl.h>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// CURL Helper Functions
//===----------------------------------------------------------------------===//

// Callback function for CURL to write response data
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *response) {
	size_t total_size = size * nmemb;
	response->append(static_cast<char *>(contents), total_size);
	return total_size;
}

// URL encode a string
static std::string UrlEncode(CURL *curl, const std::string &value) {
	char *encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
	if (!encoded) {
		return value;
	}
	std::string result(encoded);
	curl_free(encoded);
	return result;
}

// Parse JSON string value (simple parser for known Azure AD responses)
static std::string ParseJsonString(const std::string &json, const std::string &key) {
	std::string search_key = "\"" + key + "\"";
	auto key_pos = json.find(search_key);
	if (key_pos == std::string::npos) {
		return "";
	}

	// Find the colon after the key
	auto colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return "";
	}

	// Find the opening quote of the value
	auto value_start = json.find('"', colon_pos);
	if (value_start == std::string::npos) {
		return "";
	}

	// Find the closing quote
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

	// Find the colon after the key
	auto colon_pos = json.find(':', key_pos + search_key.length());
	if (colon_pos == std::string::npos) {
		return 0;
	}

	// Skip whitespace
	auto value_start = colon_pos + 1;
	while (value_start < json.length() && (json[value_start] == ' ' || json[value_start] == '\t')) {
		value_start++;
	}

	// Parse integer
	int value = 0;
	while (value_start < json.length() && isdigit(json[value_start])) {
		value = value * 10 + (json[value_start] - '0');
		value_start++;
	}

	return value;
}

// Make HTTP POST request using libcurl
static std::string HttpPost(const std::string &url, const std::string &body) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		throw InvalidInputException("Failed to initialize CURL");
	}

	std::string response;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	// Set headers
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		throw InvalidInputException("HTTP request failed: %s", curl_easy_strerror(res));
	}

	// For device code flow, 400 errors with specific error codes are expected during polling
	if (http_code != 200 && http_code != 400) {
		throw InvalidInputException("HTTP error %ld: %s", http_code, response);
	}

	return response;
}

//===----------------------------------------------------------------------===//
// RequestDeviceCode
//===----------------------------------------------------------------------===//

DeviceCodeResponse RequestDeviceCode(const std::string &tenant_id, const std::string &client_id) {
	std::string effective_tenant = tenant_id.empty() ? AZURE_DEFAULT_TENANT : tenant_id;
	std::string effective_client = client_id.empty() ? AZURE_INTERACTIVE_CLIENT_ID : client_id;

	std::string url = "https://" + std::string(AZURE_AD_BASE_URL) + "/" + effective_tenant + "/oauth2/v2.0/devicecode";

	// URL encode parameters using a temporary CURL handle
	CURL *curl = curl_easy_init();
	if (!curl) {
		throw InvalidInputException("Failed to initialize CURL for URL encoding");
	}

	std::string body = "client_id=" + UrlEncode(curl, effective_client) + "&scope=" + UrlEncode(curl, AZURE_SQL_SCOPE);
	curl_easy_cleanup(curl);

	std::string response = HttpPost(url, body);

	// Parse response
	DeviceCodeResponse result;
	result.device_code = ParseJsonString(response, "device_code");
	result.user_code = ParseJsonString(response, "user_code");
	result.verification_uri = ParseJsonString(response, "verification_uri");
	result.message = ParseJsonString(response, "message");
	result.expires_in = ParseJsonInt(response, "expires_in");
	result.interval = ParseJsonInt(response, "interval");

	if (result.device_code.empty()) {
		std::string error = ParseJsonString(response, "error");
		std::string error_desc = ParseJsonString(response, "error_description");
		if (!error.empty()) {
			throw InvalidInputException("Device code request failed: %s - %s", error, error_desc);
		}
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

	std::string url = "https://" + std::string(AZURE_AD_BASE_URL) + "/" + effective_tenant + "/oauth2/v2.0/token";

	// URL encode parameters using a temporary CURL handle
	CURL *curl = curl_easy_init();
	if (!curl) {
		return TokenResult::Failure("Failed to initialize CURL for URL encoding");
	}

	std::string body = "grant_type=" + UrlEncode(curl, DEVICE_CODE_GRANT_TYPE) +
					   "&client_id=" + UrlEncode(curl, effective_client) +
					   "&device_code=" + UrlEncode(curl, device_code);
	curl_easy_cleanup(curl);

	auto start_time = std::chrono::steady_clock::now();
	auto timeout_duration = std::chrono::seconds(timeout);

	while (true) {
		// Check timeout
		auto elapsed = std::chrono::steady_clock::now() - start_time;
		if (elapsed >= timeout_duration) {
			return TokenResult::Failure("Error: Device code expired. Please try again.");
		}

		std::string response;
		try {
			response = HttpPost(url, body);
		} catch (const InvalidInputException &e) {
			// Network error - retry with backoff
			std::this_thread::sleep_for(std::chrono::seconds(interval * 2));
			continue;
		}

		// Check for error
		std::string error = ParseJsonString(response, "error");
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
				std::string error_desc = ParseJsonString(response, "error_description");
				return TokenResult::Failure("Error during authentication: " + error_desc);
			}
		}

		// Parse successful response
		std::string access_token = ParseJsonString(response, "access_token");
		if (access_token.empty()) {
			return TokenResult::Failure("Error: No access token in response");
		}

		int expires_in = ParseJsonInt(response, "expires_in");
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

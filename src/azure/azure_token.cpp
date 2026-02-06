//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_token.cpp
//
// Azure AD token acquisition and caching implementation
// Uses libcurl for HTTP requests (no Azure SDK dependency)
//===----------------------------------------------------------------------===//

#include "azure/azure_token.hpp"
#include "azure/azure_device_code.hpp"
#include "azure/azure_secret_reader.hpp"
#include "duckdb/common/exception.hpp"

#include <curl/curl.h>
#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>

// Windows compatibility for popen/pclose
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// TokenCache implementation
//===----------------------------------------------------------------------===//

TokenCache &TokenCache::Instance() {
	static TokenCache instance;
	return instance;
}

std::string TokenCache::GetToken(const std::string &secret_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = cache_.find(secret_name);
	if (it != cache_.end() && it->second.IsValid()) {
		return it->second.access_token;
	}
	return "";
}

bool TokenCache::HasValidToken(const std::string &secret_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = cache_.find(secret_name);
	return it != cache_.end() && it->second.IsValid();
}

void TokenCache::SetToken(const std::string &secret_name, const std::string &token,
						  std::chrono::system_clock::time_point expires_at) {
	std::lock_guard<std::mutex> lock(mutex_);
	cache_[secret_name] = CachedToken{token, expires_at};
}

void TokenCache::Invalidate(const std::string &secret_name) {
	std::lock_guard<std::mutex> lock(mutex_);
	cache_.erase(secret_name);
}

void TokenCache::Clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	cache_.clear();
}

//===----------------------------------------------------------------------===//
// CURL Helper Functions
//===----------------------------------------------------------------------===//

// Callback function for CURL to write response data
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *response) {
	size_t total_size = size * nmemb;
	response->append(static_cast<char *>(contents), total_size);
	return total_size;
}

// URL encode a string using CURL
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

//===----------------------------------------------------------------------===//
// Helper functions
//===----------------------------------------------------------------------===//

// Parse chain string into components (e.g., "cli;env;managed_identity" -> ["cli", "env", "managed_identity"])
static std::vector<std::string> ParseChain(const std::string &chain) {
	std::vector<std::string> items;
	if (chain.empty()) {
		// Default chain order
		items.push_back("cli");
		items.push_back("env");
		items.push_back("managed_identity");
		return items;
	}

	std::istringstream ss(chain);
	std::string item;
	while (std::getline(ss, item, ';')) {
		// Trim whitespace
		size_t start = item.find_first_not_of(" \t");
		size_t end = item.find_last_not_of(" \t");
		if (start != std::string::npos) {
			items.push_back(item.substr(start, end - start + 1));
		}
	}
	return items;
}

// Check if chain contains interactive
static bool ChainContainsInteractive(const std::string &chain) {
	auto items = ParseChain(chain);
	for (const auto &item : items) {
		if (item == "interactive") {
			return true;
		}
	}
	return false;
}

// Check if chain contains cli
static bool ChainContainsCLI(const std::string &chain) {
	auto items = ParseChain(chain);
	for (const auto &item : items) {
		if (item == "cli") {
			return true;
		}
	}
	return false;
}

// Check if chain contains env (Spec 032 - User Story 2)
static bool ChainContainsEnv(const std::string &chain) {
	auto items = ParseChain(chain);
	for (const auto &item : items) {
		if (item == "env") {
			return true;
		}
	}
	return false;
}

// Forward declaration for AcquireTokenFromEnv
static TokenResult AcquireTokenForServicePrincipal(const AzureSecretInfo &info);

//===----------------------------------------------------------------------===//
// AcquireTokenFromEnv - Environment-based service principal (Spec 032)
//===----------------------------------------------------------------------===//

// Acquire token using Azure SDK environment variables:
// - AZURE_TENANT_ID
// - AZURE_CLIENT_ID
// - AZURE_CLIENT_SECRET
static TokenResult AcquireTokenFromEnv() {
	// Read environment variables
	const char *tenant_id = std::getenv("AZURE_TENANT_ID");
	const char *client_id = std::getenv("AZURE_CLIENT_ID");
	const char *client_secret = std::getenv("AZURE_CLIENT_SECRET");

	// Build a helpful error message listing which vars are set and which are missing
	std::vector<std::string> set_vars;
	std::vector<std::string> missing_vars;

	if (tenant_id && strlen(tenant_id) > 0) {
		set_vars.push_back("AZURE_TENANT_ID");
	} else {
		missing_vars.push_back("AZURE_TENANT_ID");
	}

	if (client_id && strlen(client_id) > 0) {
		set_vars.push_back("AZURE_CLIENT_ID");
	} else {
		missing_vars.push_back("AZURE_CLIENT_ID");
	}

	if (client_secret && strlen(client_secret) > 0) {
		set_vars.push_back("AZURE_CLIENT_SECRET");
	} else {
		missing_vars.push_back("AZURE_CLIENT_SECRET");
	}

	// Check for missing variables
	if (!missing_vars.empty()) {
		std::string error_msg;
		if (set_vars.empty()) {
			// None are set
			error_msg = "Environment variables AZURE_TENANT_ID, AZURE_CLIENT_ID, and AZURE_CLIENT_SECRET "
			            "are not set. Required for credential_chain with 'env' provider.";
		} else {
			// Some are set, some are missing
			std::string set_str, missing_str;
			for (size_t i = 0; i < set_vars.size(); i++) {
				if (i > 0)
					set_str += " and ";
				set_str += set_vars[i];
			}
			for (size_t i = 0; i < missing_vars.size(); i++) {
				if (i > 0)
					missing_str += " and ";
				missing_str += missing_vars[i];
			}
			error_msg = "Environment variable" + std::string(missing_vars.size() > 1 ? "s " : " ") + missing_str +
			            " not set. " + set_str + " " + (set_vars.size() > 1 ? "are" : "is") +
			            " set but all three are required for credential_chain with 'env' provider.";
		}
		return TokenResult::Failure(error_msg);
	}

	// Build AzureSecretInfo from environment variables
	AzureSecretInfo info;
	info.provider = "service_principal";
	info.tenant_id = tenant_id;
	info.client_id = client_id;
	info.client_secret = client_secret;

	// Use the existing service principal flow
	return AcquireTokenForServicePrincipal(info);
}

//===----------------------------------------------------------------------===//
// AcquireTokenForServicePrincipal - Client credentials flow
//===----------------------------------------------------------------------===//

TokenResult AcquireTokenForServicePrincipal(const AzureSecretInfo &info) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		return TokenResult::Failure("Failed to initialize CURL");
	}

	std::string url = "https://" + std::string(AZURE_AD_BASE_URL) + "/" + info.tenant_id + "/oauth2/v2.0/token";

	std::string body =
		"grant_type=client_credentials"
		"&client_id=" +
		UrlEncode(curl, info.client_id) + "&client_secret=" + UrlEncode(curl, info.client_secret) +
		"&scope=" + UrlEncode(curl, AZURE_SQL_SCOPE);

	std::string response;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode res = curl_easy_perform(curl);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		return TokenResult::Failure("HTTP request failed: " + std::string(curl_easy_strerror(res)));
	}

	if (http_code != 200) {
		std::string error = ParseJsonString(response, "error");
		std::string error_desc = ParseJsonString(response, "error_description");
		if (!error_desc.empty()) {
			return TokenResult::Failure("Azure AD error: " + error_desc);
		}
		return TokenResult::Failure("HTTP error " + std::to_string(http_code));
	}

	std::string access_token = ParseJsonString(response, "access_token");
	if (access_token.empty()) {
		return TokenResult::Failure("No access token in response");
	}

	int expires_in = ParseJsonInt(response, "expires_in");
	if (expires_in == 0) {
		expires_in = static_cast<int>(DEFAULT_TOKEN_LIFETIME_SECONDS);
	}

	auto expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
	return TokenResult::Success(access_token, expires_at);
}

//===----------------------------------------------------------------------===//
// AcquireTokenWithAzureCLI - Use az account get-access-token
//===----------------------------------------------------------------------===//

TokenResult AcquireTokenWithAzureCLI(const AzureSecretInfo &info) {
	// Execute az account get-access-token command
	// Note: az CLI uses --resource (not --scope) and doesn't want the /.default suffix
	std::string command =
		"az account get-access-token --resource https://database.windows.net "
		"--query accessToken -o tsv 2>&1";

	std::array<char, 4096> buffer;
	std::string result;

	FILE *pipe = popen(command.c_str(), "r");
	if (!pipe) {
		return TokenResult::Failure("Failed to execute az command");
	}

	while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
		result += buffer.data();
	}

	int exit_code = pclose(pipe);

	// Trim whitespace
	size_t start = result.find_first_not_of(" \t\n\r");
	size_t end = result.find_last_not_of(" \t\n\r");
	if (start != std::string::npos) {
		result = result.substr(start, end - start + 1);
	}

	if (exit_code != 0) {
		if (result.find("az login") != std::string::npos || result.find("Please run 'az login'") != std::string::npos) {
			return TokenResult::Failure("Azure CLI credentials expired. Run 'az login' to refresh.");
		}
		return TokenResult::Failure("Azure CLI error: " + result);
	}

	if (result.empty() || result.find("ERROR") != std::string::npos) {
		return TokenResult::Failure("Failed to get token from Azure CLI: " + result);
	}

	// Token from CLI - assume 1 hour expiry
	auto expires_at = std::chrono::system_clock::now() + std::chrono::seconds(DEFAULT_TOKEN_LIFETIME_SECONDS);
	return TokenResult::Success(result, expires_at);
}

//===----------------------------------------------------------------------===//
// ExtractErrorMessage - Extract plain message from DuckDB JSON exception
//===----------------------------------------------------------------------===//
static std::string ExtractErrorMessage(const std::exception &e) {
	std::string error_str = e.what();
	// DuckDB exceptions format as JSON via what() - extract the plain message
	auto msg_pos = error_str.find("\"exception_message\":\"");
	if (msg_pos != std::string::npos) {
		auto start = msg_pos + 21;	// length of "\"exception_message\":\""
		auto end = error_str.find("\"", start);
		if (end != std::string::npos) {
			return error_str.substr(start, end - start);
		}
	}
	return error_str;
}

//===----------------------------------------------------------------------===//
// AcquireToken - Main entry point
//===----------------------------------------------------------------------===//

TokenResult AcquireToken(ClientContext &context, const std::string &secret_name,
						 const std::string &tenant_id_override) {
	// Check cache first (include tenant in cache key for interactive auth)
	std::string cache_key = secret_name;
	if (!tenant_id_override.empty()) {
		cache_key += ":" + tenant_id_override;
	}

	std::string cached = TokenCache::Instance().GetToken(cache_key);
	if (!cached.empty()) {
		return TokenResult::Success(cached,
									std::chrono::system_clock::now() + std::chrono::hours(1));	// Approximate expiry
	}

	try {
		// Read Azure secret
		AzureSecretInfo info = ReadAzureSecret(context, secret_name);

		// Apply tenant_id override for interactive auth
		if (!tenant_id_override.empty()) {
			info.tenant_id = tenant_id_override;
		}

		TokenResult result = TokenResult::Failure("Unknown provider: " + info.provider);

		// Choose authentication method based on provider
		if (info.provider == "service_principal") {
			result = AcquireTokenForServicePrincipal(info);
		} else if (info.provider == "credential_chain") {
			// Check chains in priority order: env > cli > interactive
			// This matches Azure SDK DefaultAzureCredential behavior
			if (ChainContainsEnv(info.chain)) {
				result = AcquireTokenFromEnv();
			} else if (ChainContainsCLI(info.chain)) {
				result = AcquireTokenWithAzureCLI(info);
			} else if (ChainContainsInteractive(info.chain)) {
				result = AcquireInteractiveToken(info);
			} else {
				result = TokenResult::Failure("Unsupported credential chain: " + info.chain +
											  ". Supported: env, cli, interactive");
			}
		} else if (info.provider == "managed_identity") {
			// Managed identity uses IMDS endpoint - simplified for now
			result = TokenResult::Failure(
				"Managed identity not yet implemented. Use service_principal or credential_chain with "
				"cli/interactive.");
		}

		// Cache successful result
		if (result.success) {
			TokenCache::Instance().SetToken(cache_key, result.access_token, result.expires_at);
		}

		return result;
	} catch (const std::exception &e) {
		return TokenResult::Failure(ExtractErrorMessage(e));
	}
}

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

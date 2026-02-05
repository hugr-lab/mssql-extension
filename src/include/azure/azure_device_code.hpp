//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_device_code.hpp
//
// OAuth2 Device Authorization Grant (RFC 8628) for interactive authentication
//===----------------------------------------------------------------------===//

#pragma once

#include "azure_secret_reader.hpp"
#include "azure_token.hpp"
#include <string>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// DeviceCodeResponse - Response from Azure AD device code endpoint
//===----------------------------------------------------------------------===//
struct DeviceCodeResponse {
	std::string device_code;      // Long code for token polling
	std::string user_code;        // Short code displayed to user (e.g., "ABC123")
	std::string verification_uri; // URL user visits (https://microsoft.com/devicelogin)
	std::string message;          // Human-readable instructions for user
	int expires_in;               // Seconds until device code expires (default: 900)
	int interval;                 // Seconds to wait between polling requests
};

//===----------------------------------------------------------------------===//
// DeviceCodePollingState - State of device code polling
//===----------------------------------------------------------------------===//
struct DeviceCodePollingState {
	bool pending;             // User hasn't completed login yet
	bool success;             // Token was successfully acquired
	std::string error;        // Error code if failed
	std::string error_description; // Error description if failed
};

//===----------------------------------------------------------------------===//
// RequestDeviceCode - Request a device code from Azure AD
//
// Parameters:
//   tenant_id - Azure AD tenant ID (or "common" for multi-tenant)
//   client_id - Application/client ID
//
// Returns:
//   DeviceCodeResponse with device_code, user_code, verification_uri
//
// Throws:
//   InvalidInputException on HTTP errors or invalid response
//===----------------------------------------------------------------------===//
DeviceCodeResponse RequestDeviceCode(const std::string &tenant_id, const std::string &client_id);

//===----------------------------------------------------------------------===//
// PollForToken - Poll token endpoint for authentication completion
//
// Parameters:
//   tenant_id   - Azure AD tenant ID
//   client_id   - Application/client ID
//   device_code - Device code from RequestDeviceCode()
//   interval    - Polling interval in seconds
//   timeout     - Maximum time to wait in seconds
//
// Returns:
//   TokenResult with access token on success, error message on failure/timeout
//===----------------------------------------------------------------------===//
TokenResult PollForToken(const std::string &tenant_id, const std::string &client_id,
                         const std::string &device_code, int interval, int timeout);

//===----------------------------------------------------------------------===//
// AcquireInteractiveToken - Main entry point for Device Code Flow
//
// Performs the full device code authentication flow:
// 1. Request device code from Azure AD
// 2. Display verification URL and user code to user
// 3. Poll for token until user completes authentication
//
// Parameters:
//   info - Parsed Azure secret information (must have chain='interactive')
//
// Returns:
//   TokenResult with access token on success, error message on failure
//===----------------------------------------------------------------------===//
TokenResult AcquireInteractiveToken(const AzureSecretInfo &info);

//===----------------------------------------------------------------------===//
// DisplayDeviceCodeMessage - Display device code to user
//
// Outputs the verification URL and user code to console for user to complete auth
//
// Parameters:
//   response - DeviceCodeResponse from RequestDeviceCode()
//===----------------------------------------------------------------------===//
void DisplayDeviceCodeMessage(const DeviceCodeResponse &response);

} // namespace azure
} // namespace mssql
} // namespace duckdb

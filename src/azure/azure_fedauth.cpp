//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// azure_fedauth.cpp
//
// FEDAUTH token encoding and endpoint detection for TDS protocol
//===----------------------------------------------------------------------===//

#include "azure/azure_fedauth.hpp"
#include "azure/azure_token.hpp"
#include "mssql_platform.hpp"
#include "tds/encoding/utf16.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>

namespace duckdb {
namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// FEDAUTH Token Encoding
//===----------------------------------------------------------------------===//

std::vector<uint8_t> EncodeFedAuthToken(const std::string &token_utf8) {
	// Use existing UTF-16LE encoding from TDS encoding utilities
	return tds::encoding::Utf16LEEncode(token_utf8);
}

FedAuthData BuildFedAuthExtension(ClientContext &context, const std::string &azure_secret_name) {
	// Acquire token using Phase 1 infrastructure
	auto token_result = AcquireToken(context, azure_secret_name);

	if (!token_result.success) {
		throw ConnectionException("Azure AD authentication failed: %s", token_result.error_message);
	}

	// Build the FEDAUTH extension data
	FedAuthData data;
	data.library = FedAuthLibrary::MSAL;
	data.token_utf16le = EncodeFedAuthToken(token_result.access_token);

	return data;
}

}  // namespace azure

//===----------------------------------------------------------------------===//
// Endpoint Detection Functions
//===----------------------------------------------------------------------===//

EndpointType GetEndpointType(const std::string &host) {
	// Check most specific patterns first
	if (IsFabricEndpoint(host)) {
		return EndpointType::Fabric;
	}
	if (IsSynapseEndpoint(host)) {
		return EndpointType::Synapse;
	}
	if (IsAzureEndpoint(host)) {
		return EndpointType::AzureSQL;
	}
	return EndpointType::OnPremises;
}

bool RequiresHostnameVerification(EndpointType type) {
	// All Azure endpoints require hostname verification
	// On-premises can use self-signed certificates
	switch (type) {
	case EndpointType::AzureSQL:
	case EndpointType::Fabric:
	case EndpointType::Synapse:
		return true;
	case EndpointType::OnPremises:
	default:
		return false;
	}
}

// Case-insensitive contains check
static bool ContainsIgnoreCase(const std::string &haystack, const std::string &needle) {
	if (needle.empty() || haystack.size() < needle.size()) {
		return false;
	}

	auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
	                      [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); });
	return it != haystack.end();
}

bool IsAzureEndpoint(const std::string &host) {
	// Azure SQL Database: *.database.windows.net
	if (ContainsIgnoreCase(host, ".database.windows.net")) {
		return true;
	}
	// Microsoft Fabric and Azure Synapse are also Azure endpoints
	if (IsFabricEndpoint(host)) {
		return true;
	}
	if (IsSynapseEndpoint(host)) {
		return true;
	}
	return false;
}

bool IsFabricEndpoint(const std::string &host) {
	// Microsoft Fabric Warehouse: *.datawarehouse.fabric.microsoft.com
	if (ContainsIgnoreCase(host, ".datawarehouse.fabric.microsoft.com")) {
		return true;
	}
	// Power BI Dedicated (Fabric): *.pbidedicated.windows.net
	if (ContainsIgnoreCase(host, ".pbidedicated.windows.net")) {
		return true;
	}
	return false;
}

bool IsSynapseEndpoint(const std::string &host) {
	// Azure Synapse Analytics: *-ondemand.sql.azuresynapse.net
	if (ContainsIgnoreCase(host, ".sql.azuresynapse.net")) {
		return true;
	}
	return false;
}

}  // namespace mssql
}  // namespace duckdb

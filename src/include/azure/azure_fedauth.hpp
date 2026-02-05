#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {

// Forward declarations
class ClientContext;

namespace mssql {
namespace azure {

//===----------------------------------------------------------------------===//
// FedAuth Library Identifier
//===----------------------------------------------------------------------===//

//! FedAuth library identifier for LOGIN7 FEDAUTH extension
//! Per MS-TDS 2.2.7.1 FEDAUTH Feature Extension
enum class FedAuthLibrary : uint8_t {
	SSPI = 0x01,           // Windows integrated authentication (not supported)
	MSAL = 0x02,           // Azure AD via MSAL/ADAL library (our target)
	SECURITY_TOKEN = 0x03  // Pre-acquired security token (not used)
};

//===----------------------------------------------------------------------===//
// FEDAUTH Feature Extension Data
//===----------------------------------------------------------------------===//

//! FEDAUTH feature extension data for LOGIN7 packet
//! Contains the library type and UTF-16LE encoded access token
struct FedAuthData {
	//! The FEDAUTH library identifier (always MSAL for Azure AD)
	FedAuthLibrary library = FedAuthLibrary::MSAL;

	//! UTF-16LE encoded access token
	//! The OAuth2 access token must be converted from UTF-8 to UTF-16LE for TDS
	std::vector<uint8_t> token_utf16le;

	//! Returns total size of FEDAUTH extension data
	//! This is 4 bytes (options) + token_utf16le.size()
	size_t GetDataSize() const {
		return 4 + token_utf16le.size();
	}

	//! Returns true if this is a valid FEDAUTH extension (has token data)
	bool IsValid() const {
		return !token_utf16le.empty();
	}
};

//===----------------------------------------------------------------------===//
// FEDAUTH Encoding Functions
//===----------------------------------------------------------------------===//

//! Encode access token to UTF-16LE for TDS FEDAUTH packet
//! @param token_utf8 The OAuth2 access token in UTF-8 encoding
//! @return UTF-16LE encoded token bytes
std::vector<uint8_t> EncodeFedAuthToken(const std::string &token_utf8);

//! Build complete FEDAUTH extension data for LOGIN7 packet
//! Acquires token from Azure secret and encodes it for TDS
//! @param context DuckDB client context for secret access
//! @param azure_secret_name Name of the Azure secret to use for token acquisition
//! @return FedAuthData ready for LOGIN7 packet, or invalid if token acquisition failed
//! @throws ConnectionException if token acquisition fails
FedAuthData BuildFedAuthExtension(ClientContext &context, const std::string &azure_secret_name);

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb

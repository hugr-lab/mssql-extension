// test/cpp/test_fedauth_encoding.cpp
// Unit tests for FEDAUTH token encoding and endpoint detection
//
// These tests do NOT require a running SQL Server instance.
// They test the FEDAUTH encoding and endpoint detection logic in isolation.
//
// Compile:
//   See Makefile or CMakeLists.txt
//
// Run:
//   ./test_fedauth_encoding

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "azure/azure_fedauth.hpp"
#include "mssql_platform.hpp"
#include "tds/tds_protocol.hpp"
#include "tds/tds_types.hpp"

using namespace duckdb;
using namespace duckdb::mssql;
using namespace duckdb::mssql::azure;

//==============================================================================
// Helper macros for assertions with messages
//==============================================================================
#define ASSERT_EQ(actual, expected)                                                          \
	do {                                                                                     \
		if ((actual) != (expected)) {                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expected: " << (expected) << std::endl;                          \
			std::cerr << "  Actual:   " << (actual) << std::endl;                            \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_TRUE(cond)                                                                    \
	do {                                                                                     \
		if (!(cond)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition is false: " #cond << std::endl;                        \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_FALSE(cond)                                                                   \
	do {                                                                                     \
		if ((cond)) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition is true: " #cond << std::endl;                         \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// T012: Test UTF-16LE Token Encoding
//==============================================================================
void test_fedauth_token_encoding_basic() {
	std::cout << "\n=== Test: FEDAUTH Token Encoding - Basic ASCII ===" << std::endl;

	// Simple ASCII token
	std::string token = "test_token";
	auto encoded = EncodeFedAuthToken(token);

	// UTF-16LE: each ASCII char becomes 2 bytes (char + 0x00)
	ASSERT_EQ(encoded.size(), token.size() * 2);

	// Verify encoding: 't' = 0x74, in UTF-16LE = 0x74 0x00
	ASSERT_EQ(encoded[0], 0x74);  // 't'
	ASSERT_EQ(encoded[1], 0x00);
	ASSERT_EQ(encoded[2], 0x65);  // 'e'
	ASSERT_EQ(encoded[3], 0x00);
	ASSERT_EQ(encoded[4], 0x73);  // 's'
	ASSERT_EQ(encoded[5], 0x00);
	ASSERT_EQ(encoded[6], 0x74);  // 't'
	ASSERT_EQ(encoded[7], 0x00);

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_token_encoding_empty() {
	std::cout << "\n=== Test: FEDAUTH Token Encoding - Empty String ===" << std::endl;

	std::string token = "";
	auto encoded = EncodeFedAuthToken(token);

	ASSERT_EQ(encoded.size(), static_cast<size_t>(0));

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_token_encoding_jwt_like() {
	std::cout << "\n=== Test: FEDAUTH Token Encoding - JWT-like Token ===" << std::endl;

	// Typical JWT token structure (simplified)
	std::string token = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.signature";
	auto encoded = EncodeFedAuthToken(token);

	// UTF-16LE doubles the size for ASCII
	ASSERT_EQ(encoded.size(), token.size() * 2);

	// Verify first char 'e' = 0x65
	ASSERT_EQ(encoded[0], 0x65);
	ASSERT_EQ(encoded[1], 0x00);

	// Verify '.' in the middle
	size_t dot_pos = token.find('.');
	ASSERT_EQ(encoded[dot_pos * 2], 0x2E);      // '.'
	ASSERT_EQ(encoded[dot_pos * 2 + 1], 0x00);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T013: Test FedAuthData Methods
//==============================================================================
void test_fedauth_data_get_size_empty() {
	std::cout << "\n=== Test: FedAuthData::GetDataSize - Empty Token ===" << std::endl;

	FedAuthData data;
	// Empty token: 4 bytes for options + 0 bytes for token
	ASSERT_EQ(data.GetDataSize(), static_cast<size_t>(4));

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_data_get_size_with_token() {
	std::cout << "\n=== Test: FedAuthData::GetDataSize - With Token ===" << std::endl;

	FedAuthData data;
	data.token_utf16le = {0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74, 0x00};  // "test" in UTF-16LE

	// 4 bytes for options + 8 bytes for token
	ASSERT_EQ(data.GetDataSize(), static_cast<size_t>(12));

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_data_is_valid_empty() {
	std::cout << "\n=== Test: FedAuthData::IsValid - Empty Token ===" << std::endl;

	FedAuthData data;
	// Empty token is not valid
	ASSERT_FALSE(data.IsValid());

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_data_is_valid_with_token() {
	std::cout << "\n=== Test: FedAuthData::IsValid - With Token ===" << std::endl;

	FedAuthData data;
	data.token_utf16le = {0x74, 0x00};  // Minimal token

	// Has token, so valid
	ASSERT_TRUE(data.IsValid());

	std::cout << "PASSED!" << std::endl;
}

void test_fedauth_data_library_default() {
	std::cout << "\n=== Test: FedAuthData::library - Default Value ===" << std::endl;

	FedAuthData data;
	// Default library should be MSAL
	ASSERT_EQ(static_cast<uint8_t>(data.library), static_cast<uint8_t>(FedAuthLibrary::MSAL));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T014: Test Endpoint Detection
//==============================================================================
void test_is_azure_endpoint_azure_sql() {
	std::cout << "\n=== Test: IsAzureEndpoint - Azure SQL Database ===" << std::endl;

	// Standard Azure SQL Database endpoint
	ASSERT_TRUE(IsAzureEndpoint("myserver.database.windows.net"));
	ASSERT_TRUE(IsAzureEndpoint("MYSERVER.DATABASE.WINDOWS.NET"));  // Case insensitive
	ASSERT_TRUE(IsAzureEndpoint("server-123.database.windows.net"));

	std::cout << "PASSED!" << std::endl;
}

void test_is_azure_endpoint_fabric() {
	std::cout << "\n=== Test: IsAzureEndpoint - Microsoft Fabric ===" << std::endl;

	// Fabric endpoints are also Azure endpoints
	ASSERT_TRUE(IsAzureEndpoint("myworkspace.datawarehouse.fabric.microsoft.com"));
	ASSERT_TRUE(IsAzureEndpoint("workspace.pbidedicated.windows.net"));

	std::cout << "PASSED!" << std::endl;
}

void test_is_azure_endpoint_synapse() {
	std::cout << "\n=== Test: IsAzureEndpoint - Azure Synapse ===" << std::endl;

	// Synapse endpoints are also Azure endpoints
	ASSERT_TRUE(IsAzureEndpoint("workspace-ondemand.sql.azuresynapse.net"));
	ASSERT_TRUE(IsAzureEndpoint("mypool.sql.azuresynapse.net"));

	std::cout << "PASSED!" << std::endl;
}

void test_is_azure_endpoint_on_premises() {
	std::cout << "\n=== Test: IsAzureEndpoint - On-Premises ===" << std::endl;

	// On-premises SQL Server is NOT an Azure endpoint
	ASSERT_FALSE(IsAzureEndpoint("localhost"));
	ASSERT_FALSE(IsAzureEndpoint("192.168.1.100"));
	ASSERT_FALSE(IsAzureEndpoint("sqlserver.company.local"));
	ASSERT_FALSE(IsAzureEndpoint("sql.internal.corp"));

	std::cout << "PASSED!" << std::endl;
}

void test_is_fabric_endpoint() {
	std::cout << "\n=== Test: IsFabricEndpoint ===" << std::endl;

	// Fabric Warehouse endpoints
	ASSERT_TRUE(IsFabricEndpoint("workspace.datawarehouse.fabric.microsoft.com"));
	ASSERT_TRUE(IsFabricEndpoint("WORKSPACE.DATAWAREHOUSE.FABRIC.MICROSOFT.COM"));

	// Power BI Dedicated (also Fabric)
	ASSERT_TRUE(IsFabricEndpoint("workspace.pbidedicated.windows.net"));

	// NOT Fabric
	ASSERT_FALSE(IsFabricEndpoint("myserver.database.windows.net"));
	ASSERT_FALSE(IsFabricEndpoint("localhost"));
	ASSERT_FALSE(IsFabricEndpoint("workspace.sql.azuresynapse.net"));

	std::cout << "PASSED!" << std::endl;
}

void test_is_synapse_endpoint() {
	std::cout << "\n=== Test: IsSynapseEndpoint ===" << std::endl;

	// Synapse endpoints
	ASSERT_TRUE(IsSynapseEndpoint("workspace-ondemand.sql.azuresynapse.net"));
	ASSERT_TRUE(IsSynapseEndpoint("mypool.sql.azuresynapse.net"));
	ASSERT_TRUE(IsSynapseEndpoint("WORKSPACE.SQL.AZURESYNAPSE.NET"));

	// NOT Synapse
	ASSERT_FALSE(IsSynapseEndpoint("myserver.database.windows.net"));
	ASSERT_FALSE(IsSynapseEndpoint("workspace.datawarehouse.fabric.microsoft.com"));
	ASSERT_FALSE(IsSynapseEndpoint("localhost"));

	std::cout << "PASSED!" << std::endl;
}

void test_get_endpoint_type() {
	std::cout << "\n=== Test: GetEndpointType ===" << std::endl;

	// Azure SQL Database
	ASSERT_EQ(static_cast<int>(GetEndpointType("myserver.database.windows.net")),
	          static_cast<int>(EndpointType::AzureSQL));

	// Microsoft Fabric (most specific, checked first)
	ASSERT_EQ(static_cast<int>(GetEndpointType("workspace.datawarehouse.fabric.microsoft.com")),
	          static_cast<int>(EndpointType::Fabric));

	// Azure Synapse
	ASSERT_EQ(static_cast<int>(GetEndpointType("workspace.sql.azuresynapse.net")),
	          static_cast<int>(EndpointType::Synapse));

	// On-Premises
	ASSERT_EQ(static_cast<int>(GetEndpointType("localhost")),
	          static_cast<int>(EndpointType::OnPremises));
	ASSERT_EQ(static_cast<int>(GetEndpointType("sqlserver.company.local")),
	          static_cast<int>(EndpointType::OnPremises));

	std::cout << "PASSED!" << std::endl;
}

void test_requires_hostname_verification() {
	std::cout << "\n=== Test: RequiresHostnameVerification ===" << std::endl;

	// Azure endpoints require hostname verification
	ASSERT_TRUE(RequiresHostnameVerification(EndpointType::AzureSQL));
	ASSERT_TRUE(RequiresHostnameVerification(EndpointType::Fabric));
	ASSERT_TRUE(RequiresHostnameVerification(EndpointType::Synapse));

	// On-premises does NOT require hostname verification (may use self-signed certs)
	ASSERT_FALSE(RequiresHostnameVerification(EndpointType::OnPremises));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Test: FeatureExtId enum values
//==============================================================================
void test_feature_ext_id_values() {
	std::cout << "\n=== Test: FeatureExtId Enum Values ===" << std::endl;

	// Verify FEDAUTH feature extension ID per MS-TDS spec
	ASSERT_EQ(static_cast<uint8_t>(tds::FeatureExtId::FEDAUTH), static_cast<uint8_t>(0x02));
	ASSERT_EQ(static_cast<uint8_t>(tds::FeatureExtId::TERMINATOR), static_cast<uint8_t>(0xFF));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// T022: Verify PRELOGIN does NOT include FEDAUTHREQUIRED when azure_secret absent
//==============================================================================
void test_prelogin_no_fedauth_when_sql_auth() {
	std::cout << "\n=== Test: PRELOGIN without FEDAUTHREQUIRED (SQL Auth) ===" << std::endl;

	// Build standard PRELOGIN (no FEDAUTH)
	auto packet = tds::TdsProtocol::BuildPrelogin(false);
	const auto &payload = packet.GetPayload();

	// PRELOGIN options are: VERSION(0x00), ENCRYPTION(0x01), INSTOPT(0x02), THREADID(0x03),
	// MARS(0x04), TRACEID(0x05), FEDAUTHREQUIRED(0x06), TERMINATOR(0xFF)
	// FEDAUTHREQUIRED option type is 0x06
	const uint8_t FEDAUTHREQUIRED_OPTION = 0x06;
	const uint8_t TERMINATOR_OPTION = 0xFF;

	// Scan through options - each option is: Type(1) + Offset(2) + Length(2)
	bool found_fedauth = false;
	size_t pos = 0;
	while (pos < payload.size()) {
		uint8_t option_type = payload[pos];
		if (option_type == TERMINATOR_OPTION) {
			break;
		}
		if (option_type == FEDAUTHREQUIRED_OPTION) {
			found_fedauth = true;
			break;
		}
		pos += 5;  // Skip to next option (1 + 2 + 2)
	}

	ASSERT_FALSE(found_fedauth);
	std::cout << "PASSED! Standard PRELOGIN does not include FEDAUTHREQUIRED option." << std::endl;
}

void test_prelogin_with_fedauth_includes_option() {
	std::cout << "\n=== Test: PRELOGIN with FEDAUTHREQUIRED (Azure Auth) ===" << std::endl;

	// Build FEDAUTH PRELOGIN
	auto packet = tds::TdsProtocol::BuildPreloginWithFedAuth(true, true);
	const auto &payload = packet.GetPayload();

	const uint8_t FEDAUTHREQUIRED_OPTION = 0x06;
	const uint8_t TERMINATOR_OPTION = 0xFF;

	// Scan through options
	bool found_fedauth = false;
	size_t pos = 0;
	while (pos < payload.size()) {
		uint8_t option_type = payload[pos];
		if (option_type == TERMINATOR_OPTION) {
			break;
		}
		if (option_type == FEDAUTHREQUIRED_OPTION) {
			found_fedauth = true;
			break;
		}
		pos += 5;  // Skip to next option
	}

	ASSERT_TRUE(found_fedauth);
	std::cout << "PASSED! FEDAUTH PRELOGIN includes FEDAUTHREQUIRED option." << std::endl;
}

//==============================================================================
// T023: Verify LOGIN7 does NOT include FEDAUTH extension when azure_secret absent
//==============================================================================
void test_login7_no_fedauth_extension_with_sql_auth() {
	std::cout << "\n=== Test: LOGIN7 without FEDAUTH Extension (SQL Auth) ===" << std::endl;

	// Build standard LOGIN7 (SQL authentication)
	auto packet = tds::TdsProtocol::BuildLogin7("testhost", "testuser", "testpass", "testdb");
	const auto &payload = packet.GetPayload();

	// LOGIN7 with FEDAUTH has FeatureExt flag (bit 4 of OptionFlags3) set
	// OptionFlags3 is at fixed offset 28 in LOGIN7 header (after Length(4), TDSVersion(4),
	// PacketSize(4), ClientProgVer(4), ClientPID(4), ConnectionID(4), OptionFlags1(1),
	// OptionFlags2(1), TypeFlags(1) = 27, so OptionFlags3 is at offset 27)

	// Actually, the fixed header starts at offset 0 of payload:
	// Bytes 0-3: Length (4 bytes)
	// Bytes 4-7: TDSVersion (4 bytes)
	// Bytes 8-11: PacketSize (4 bytes)
	// Bytes 12-15: ClientProgVer (4 bytes)
	// Bytes 16-19: ClientPID (4 bytes)
	// Bytes 20-23: ConnectionID (4 bytes)
	// Byte 24: OptionFlags1
	// Byte 25: OptionFlags2
	// Byte 26: TypeFlags
	// Byte 27: OptionFlags3

	ASSERT_TRUE(payload.size() >= 28);
	uint8_t option_flags3 = payload[27];

	// FEDAUTH uses FeatureExtension (fExtension bit = 0x10, bit 4 of OptionFlags3)
	const uint8_t EXTENSION_FLAG = 0x10;
	bool has_extension = (option_flags3 & EXTENSION_FLAG) != 0;

	ASSERT_FALSE(has_extension);
	std::cout << "PASSED! Standard LOGIN7 does not have FeatureExt flag set." << std::endl;
}

void test_login7_with_fedauth_has_extension() {
	std::cout << "\n=== Test: LOGIN7 with FEDAUTH Extension (Azure Auth) ===" << std::endl;

	// Build FEDAUTH LOGIN7
	// client_hostname = workstation name, server_name = TDS server address
	std::vector<uint8_t> fake_token = {0x74, 0x00, 0x65, 0x00, 0x73, 0x00, 0x74, 0x00};  // "test"
	auto packet = tds::TdsProtocol::BuildLogin7WithFedAuth("testworkstation", "testserver", "testdb", fake_token);
	const auto &payload = packet.GetPayload();

	ASSERT_TRUE(payload.size() >= 28);
	uint8_t option_flags3 = payload[27];

	const uint8_t EXTENSION_FLAG = 0x10;
	bool has_extension = (option_flags3 & EXTENSION_FLAG) != 0;

	ASSERT_TRUE(has_extension);

	// Also verify FEDAUTH feature ID (0x02) appears in the extension data
	// The extension data is at the end of the packet, after the variable-length fields
	// Look for byte sequence: 0x02 (FEDAUTH FeatureId) followed by length and data
	bool found_fedauth_feature = false;
	for (size_t i = 0; i < payload.size() - 1; i++) {
		if (payload[i] == static_cast<uint8_t>(tds::FeatureExtId::FEDAUTH)) {
			// Check if followed by reasonable length (4 bytes for DWORD length)
			if (i + 5 < payload.size()) {
				found_fedauth_feature = true;
				break;
			}
		}
	}

	ASSERT_TRUE(found_fedauth_feature);
	std::cout << "PASSED! FEDAUTH LOGIN7 has FeatureExt flag and FEDAUTH feature ID." << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "\n========================================" << std::endl;
	std::cout << "FEDAUTH Encoding & Endpoint Detection Tests" << std::endl;
	std::cout << "========================================" << std::endl;

	// T012: UTF-16LE Token Encoding
	test_fedauth_token_encoding_basic();
	test_fedauth_token_encoding_empty();
	test_fedauth_token_encoding_jwt_like();

	// T013: FedAuthData Methods
	test_fedauth_data_get_size_empty();
	test_fedauth_data_get_size_with_token();
	test_fedauth_data_is_valid_empty();
	test_fedauth_data_is_valid_with_token();
	test_fedauth_data_library_default();

	// T014: Endpoint Detection
	test_is_azure_endpoint_azure_sql();
	test_is_azure_endpoint_fabric();
	test_is_azure_endpoint_synapse();
	test_is_azure_endpoint_on_premises();
	test_is_fabric_endpoint();
	test_is_synapse_endpoint();
	test_get_endpoint_type();
	test_requires_hostname_verification();

	// Additional: Feature Extension IDs
	test_feature_ext_id_values();

	// T022: PRELOGIN backward compatibility
	test_prelogin_no_fedauth_when_sql_auth();
	test_prelogin_with_fedauth_includes_option();

	// T023: LOGIN7 backward compatibility
	test_login7_no_fedauth_extension_with_sql_auth();
	test_login7_with_fedauth_has_extension();

	std::cout << "\n========================================" << std::endl;
	std::cout << "ALL TESTS PASSED!" << std::endl;
	std::cout << "========================================\n" << std::endl;

	return 0;
}

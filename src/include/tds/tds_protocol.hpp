#pragma once

#include <string>
#include <vector>
#include "tds_packet.hpp"
#include "tds_types.hpp"

namespace duckdb {
namespace tds {

// PRELOGIN response data
struct PreloginResponse {
	uint8_t version_major;
	uint8_t version_minor;
	uint16_t version_build;
	EncryptionOption encryption;
	bool fedauth_echo;	// True if server's FEDAUTHREQUIRED was non-zero (must echo in LOGIN7)
	bool success;
	std::string error_message;
};

// LOGIN7 response data
struct LoginResponse {
	bool success;
	uint16_t spid;	// Server Process ID
	std::string server_name;
	std::string database;
	uint32_t tds_version;
	std::string error_message;
	uint32_t error_number;
	uint32_t negotiated_packet_size;  // Server-negotiated packet size from ENVCHANGE

	// Routing info from ENVCHANGE type 20 (Azure SQL/Fabric gateway redirection)
	bool has_routing = false;	// True if server requested routing
	std::string routed_server;	// New server hostname to connect to
	uint16_t routed_port = 0;	// New port to connect to

	// FEDAUTHINFO token data (for ADAL workflow)
	bool has_fedauth_info = false;	// True if FEDAUTHINFO token was received
	std::string sts_url;			// Security Token Service URL from server
	std::string server_spn;			// Server Principal Name from server
};

// TDS Protocol message builders and parsers
// Implements PRELOGIN, LOGIN7, and basic response handling
class TdsProtocol {
public:
	// Build PRELOGIN packet
	// Negotiates TDS version and encryption
	// Parameters:
	//   use_encrypt - if true, requests ENCRYPT_ON from server
	//                 if false, sends ENCRYPT_NOT_SUP (no encryption)
	static TdsPacket BuildPrelogin(bool use_encrypt = false);

	// Build PRELOGIN packet with FEDAUTHREQUIRED option for Azure AD authentication
	// Parameters:
	//   use_encrypt - if true, requests ENCRYPT_ON from server
	//   fedauth_required - if true, includes FEDAUTHREQUIRED option (0x06)
	static TdsPacket BuildPreloginWithFedAuth(bool use_encrypt, bool fedauth_required);

	// Parse PRELOGIN response
	static PreloginResponse ParsePreloginResponse(const std::vector<uint8_t> &data);

	// Build LOGIN7 packet for SQL Server authentication
	// Parameters:
	//   host - client hostname (for logging on server side)
	//   username - SQL Server login name
	//   password - SQL Server password (will be encoded)
	//   database - initial database to connect to
	//   app_name - application name (optional, for server logging)
	//   packet_size - requested packet size (default 4096)
	static TdsPacket BuildLogin7(const std::string &host, const std::string &username, const std::string &password,
								 const std::string &database, const std::string &app_name = "DuckDB MSSQL Extension",
								 uint32_t packet_size = TDS_DEFAULT_PACKET_SIZE);

	// Parse LOGIN7 response (LOGINACK token and potential errors)
	static LoginResponse ParseLoginResponse(const std::vector<uint8_t> &data);

	// Build LOGIN7 packet with FEDAUTH feature extension for Azure AD authentication (DEPRECATED)
	// This uses SecurityToken flow which embeds token directly in LOGIN7.
	// For Microsoft Fabric, use BuildLogin7WithADAL instead.
	// Parameters:
	//   client_hostname - client workstation name (for server logging, e.g., "MyWorkstation")
	//   server_name - TDS server name (may include instance name, e.g., "host" or "host\instance")
	//   database - initial database to connect to
	//   fedauth_token - UTF-16LE encoded access token from Azure AD
	//   fedauth_echo - if true, set echo bit in FEDAUTH options (server's FEDAUTHREQUIRED was non-zero)
	//   app_name - application name (optional, for server logging)
	//   packet_size - requested packet size (default 4096)
	// Note: username/password not used with FEDAUTH - token replaces them
	static TdsPacket BuildLogin7WithFedAuth(const std::string &client_hostname, const std::string &server_name,
											const std::string &database, const std::vector<uint8_t> &fedauth_token,
											bool fedauth_echo = false,
											const std::string &app_name = "DuckDB MSSQL Extension",
											uint32_t packet_size = TDS_DEFAULT_PACKET_SIZE);

	// Build LOGIN7 packet with ADAL FEDAUTH workflow for Azure AD authentication
	// This uses ADAL flow: LOGIN7 contains small FEDAUTH extension, server responds with
	// FEDAUTHINFO token containing STS URL, then client sends token in separate FEDAUTH_TOKEN packet.
	// This is the flow required by Microsoft Fabric.
	// Parameters:
	//   client_hostname - client workstation name (for server logging, e.g., "MyWorkstation")
	//   server_name - TDS server name (may include instance name, e.g., "host" or "host\instance")
	//   database - initial database to connect to
	//   fedauth_echo - if true, set echo bit in FEDAUTH options (server's FEDAUTHREQUIRED was non-zero)
	//   app_name - application name (optional, for server logging)
	//   packet_size - requested packet size (default 4096)
	// Note: Token is NOT included - will be sent in separate FEDAUTH_TOKEN packet after receiving FEDAUTHINFO
	static TdsPacket BuildLogin7WithADAL(const std::string &client_hostname, const std::string &server_name,
										 const std::string &database, bool fedauth_echo = false,
										 const std::string &app_name = "DuckDB MSSQL Extension",
										 uint32_t packet_size = TDS_DEFAULT_PACKET_SIZE);

	// Build FEDAUTH_TOKEN packet to send access token after receiving FEDAUTHINFO
	// Used in ADAL workflow: server sends FEDAUTHINFO with STS URL, client fetches token,
	// then sends it via this packet.
	// Parameters:
	//   token_utf16le - UTF-16LE encoded access token from Azure AD
	//   nonce - optional 32-byte nonce (can be empty)
	static TdsPacket BuildFedAuthToken(const std::vector<uint8_t> &token_utf16le,
									   const std::vector<uint8_t> &nonce = {});

	// Build empty SQL_BATCH packet for ping
	// This sends an empty batch which triggers a DONE response
	static TdsPacket BuildPing();

	// Build SQL_BATCH packet with SQL query
	// SQL text is UTF-16LE encoded
	// Parameters:
	//   sql - SQL statement to execute
	//   transaction_descriptor - 8-byte transaction descriptor (nullptr = no active transaction)
	static TdsPacket BuildSqlBatch(const std::string &sql, const uint8_t *transaction_descriptor = nullptr);

	// Build multiple SQL_BATCH packets for large queries
	// Returns vector of packets with proper continuation flags
	// Parameters:
	//   sql - SQL statement to execute
	//   max_packet_size - maximum TDS packet size
	//   transaction_descriptor - 8-byte transaction descriptor (nullptr = no active transaction)
	static std::vector<TdsPacket> BuildSqlBatchMultiPacket(const std::string &sql,
														   size_t max_packet_size = TDS_DEFAULT_PACKET_SIZE,
														   const uint8_t *transaction_descriptor = nullptr);

	// Build ATTENTION packet for cancellation
	static TdsPacket BuildAttention();

	// Build multiple BULK_LOAD packets for large data
	// Returns vector of packets with proper continuation flags (EOM on last packet only)
	// Parameters:
	//   payload - raw BCP data (COLMETADATA + ROW tokens + DONE token)
	//   max_packet_size - maximum TDS packet size (from server negotiation)
	static std::vector<TdsPacket> BuildBulkLoadMultiPacket(const std::vector<uint8_t> &payload,
														   size_t max_packet_size = TDS_DEFAULT_PACKET_SIZE);

	// Parse DONE token to check for ATTENTION_ACK
	static bool ParseDoneForAttentionAck(const std::vector<uint8_t> &data);

	// Parse general response to check for success/error
	// Returns true if response indicates success (DONE without error)
	static bool IsSuccessResponse(const std::vector<uint8_t> &data);

	// Extract error message from response if present
	static std::string ExtractErrorMessage(const std::vector<uint8_t> &data);

private:
	// Password encoding for LOGIN7
	// XOR each byte with 0xA5, then rotate left 4 bits
	static std::vector<uint8_t> EncodePassword(const std::string &password);

	// Helper to read UTF-16LE string from buffer
	static std::string ReadUTF16LE(const uint8_t *data, size_t char_count);

	// Helper to find token in response
	static const uint8_t *FindToken(const uint8_t *data, size_t length, TokenType token);
};

}  // namespace tds
}  // namespace duckdb

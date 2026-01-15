#pragma once

#include "tds_types.hpp"
#include "tds_packet.hpp"
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

// PRELOGIN response data
struct PreloginResponse {
	uint8_t version_major;
	uint8_t version_minor;
	uint16_t version_build;
	EncryptionOption encryption;
	bool success;
	std::string error_message;
};

// LOGIN7 response data
struct LoginResponse {
	bool success;
	uint16_t spid;           // Server Process ID
	std::string server_name;
	std::string database;
	uint32_t tds_version;
	std::string error_message;
	uint32_t error_number;
};

// TDS Protocol message builders and parsers
// Implements PRELOGIN, LOGIN7, and basic response handling
class TdsProtocol {
public:
	// Build PRELOGIN packet
	// Negotiates TDS version and encryption (always ENCRYPT_OFF for this phase)
	static TdsPacket BuildPrelogin();

	// Parse PRELOGIN response
	static PreloginResponse ParsePreloginResponse(const std::vector<uint8_t>& data);

	// Build LOGIN7 packet for SQL Server authentication
	// Parameters:
	//   host - client hostname (for logging on server side)
	//   username - SQL Server login name
	//   password - SQL Server password (will be encoded)
	//   database - initial database to connect to
	//   app_name - application name (optional, for server logging)
	//   packet_size - requested packet size (default 4096)
	static TdsPacket BuildLogin7(const std::string& host,
	                             const std::string& username,
	                             const std::string& password,
	                             const std::string& database,
	                             const std::string& app_name = "DuckDB MSSQL Extension",
	                             uint32_t packet_size = TDS_DEFAULT_PACKET_SIZE);

	// Parse LOGIN7 response (LOGINACK token and potential errors)
	static LoginResponse ParseLoginResponse(const std::vector<uint8_t>& data);

	// Build empty SQL_BATCH packet for ping
	// This sends an empty batch which triggers a DONE response
	static TdsPacket BuildPing();

	// Build ATTENTION packet for cancellation
	static TdsPacket BuildAttention();

	// Parse DONE token to check for ATTENTION_ACK
	static bool ParseDoneForAttentionAck(const std::vector<uint8_t>& data);

	// Parse general response to check for success/error
	// Returns true if response indicates success (DONE without error)
	static bool IsSuccessResponse(const std::vector<uint8_t>& data);

	// Extract error message from response if present
	static std::string ExtractErrorMessage(const std::vector<uint8_t>& data);

private:
	// Password encoding for LOGIN7
	// XOR each byte with 0xA5, then rotate left 4 bits
	static std::vector<uint8_t> EncodePassword(const std::string& password);

	// Helper to read UTF-16LE string from buffer
	static std::string ReadUTF16LE(const uint8_t* data, size_t char_count);

	// Helper to find token in response
	static const uint8_t* FindToken(const uint8_t* data, size_t length, TokenType token);
};

}  // namespace tds
}  // namespace duckdb

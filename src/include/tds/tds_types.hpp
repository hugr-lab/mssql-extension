#pragma once

#include <cstdint>
#include <string>

namespace duckdb {
namespace tds {

// TDS Protocol Version (TDS 7.4 for SQL Server 2019+)
constexpr uint32_t TDS_VERSION_7_4 = 0x74000004;

// TDS Packet Types
enum class PacketType : uint8_t {
	SQL_BATCH = 1,       // SQL batch request
	RPC = 3,             // Remote procedure call
	TABULAR_RESULT = 4,  // Server response
	ATTENTION = 6,       // Cancel signal
	BULK_LOAD = 7,       // Bulk data
	TRANSACTION = 14,    // Transaction management
	LOGIN7 = 16,         // Login request
	SSPI = 17,           // Windows authentication
	PRELOGIN = 18        // Pre-login handshake
};

// TDS Packet Status Flags
enum class PacketStatus : uint8_t {
	NORMAL = 0x00,               // Normal packet
	END_OF_MESSAGE = 0x01,       // Last packet of message (EOM)
	IGNORE_EVENT = 0x02,         // Ignore this event
	RESET_CONNECTION = 0x08,     // Reset connection
	RESET_SKIP_TRAN = 0x10       // Reset and skip transaction
};

// Connection State Machine (FR-009)
enum class ConnectionState : uint8_t {
	Disconnected = 0,    // No TCP connection
	Authenticating = 1,  // PRELOGIN/LOGIN7 in progress
	Idle = 2,            // Connected, ready for queries
	Executing = 3,       // Query in progress
	Cancelling = 4       // ATTENTION sent, awaiting ACK
};

// PRELOGIN Option Types
enum class PreloginOption : uint8_t {
	VERSION = 0,
	ENCRYPTION = 1,
	INSTOPT = 2,
	THREADID = 3,
	MARS = 4,
	TRACEID = 5,
	FEDAUTHREQUIRED = 6,
	NONCEOPT = 7,
	TERMINATOR = 0xFF
};

// Encryption Options
enum class EncryptionOption : uint8_t {
	ENCRYPT_OFF = 0x00,
	ENCRYPT_ON = 0x01,
	ENCRYPT_NOT_SUP = 0x02,
	ENCRYPT_REQ = 0x03
};

// TDS Token Types (response parsing)
enum class TokenType : uint8_t {
	DONE = 0xFD,
	DONEPROC = 0xFE,
	DONEINPROC = 0xFF,
	ERROR_TOKEN = 0xAA,
	INFO = 0xAB,
	LOGINACK = 0xAD,
	ENVCHANGE = 0xE3,
	COLMETADATA = 0x81,
	ROW = 0xD1,
	RETURNSTATUS = 0x79,
	ORDER = 0xA9,
	RETURNVALUE = 0xAC
};

// DONE Token Status Flags
enum class DoneStatus : uint16_t {
	DONE_FINAL = 0x0000,
	DONE_MORE = 0x0001,
	DONE_ERROR = 0x0002,
	DONE_INXACT = 0x0004,
	DONE_COUNT = 0x0010,
	DONE_ATTN = 0x0020,      // ATTENTION acknowledgment
	DONE_SRVERROR = 0x0100
};

// TDS Packet Header Size
constexpr size_t TDS_HEADER_SIZE = 8;

// Default and maximum packet sizes
constexpr size_t TDS_MIN_PACKET_SIZE = 512;
constexpr size_t TDS_DEFAULT_PACKET_SIZE = 4096;
constexpr size_t TDS_MAX_PACKET_SIZE = 32767;

// Timeout defaults (in seconds)
constexpr int DEFAULT_CONNECTION_TIMEOUT = 30;
constexpr int DEFAULT_IDLE_TIMEOUT = 300;
constexpr int DEFAULT_ACQUIRE_TIMEOUT = 30;
constexpr int CANCELLATION_TIMEOUT = 5;

// Default pool settings
constexpr size_t DEFAULT_CONNECTION_LIMIT = 64;
constexpr size_t DEFAULT_MIN_CONNECTIONS = 0;
constexpr bool DEFAULT_CONNECTION_CACHE = true;

// Long-idle threshold for tiered validation (seconds)
constexpr int LONG_IDLE_THRESHOLD = 60;

// Convert ConnectionState to string for debugging
const char* ConnectionStateToString(ConnectionState state);

}  // namespace tds
}  // namespace duckdb

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
	TABNAME = 0x04,       // Table name for browsable results
	COLINFO = 0xA5,       // Column info for browsable results
	DONE = 0xFD,
	DONEPROC = 0xFE,
	DONEINPROC = 0xFF,
	ERROR_TOKEN = 0xAA,
	INFO = 0xAB,
	LOGINACK = 0xAD,
	ENVCHANGE = 0xE3,
	COLMETADATA = 0x81,
	ROW = 0xD1,
	NBCROW = 0xD2,        // Null Bitmap Compressed Row
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

//===----------------------------------------------------------------------===//
// SQL Server Data Type IDs (TDS wire format)
//===----------------------------------------------------------------------===//

// Fixed-length types (no length prefix in wire format)
constexpr uint8_t TDS_TYPE_NULL = 0x1F;
constexpr uint8_t TDS_TYPE_TINYINT = 0x30;
constexpr uint8_t TDS_TYPE_BIT = 0x32;
constexpr uint8_t TDS_TYPE_SMALLINT = 0x34;
constexpr uint8_t TDS_TYPE_INT = 0x38;
constexpr uint8_t TDS_TYPE_SMALLDATETIME = 0x3A;
constexpr uint8_t TDS_TYPE_REAL = 0x3B;
constexpr uint8_t TDS_TYPE_MONEY = 0x3C;
constexpr uint8_t TDS_TYPE_DATETIME = 0x3D;
constexpr uint8_t TDS_TYPE_FLOAT = 0x3E;
constexpr uint8_t TDS_TYPE_SMALLMONEY = 0x7A;
constexpr uint8_t TDS_TYPE_BIGINT = 0x7F;

// Nullable fixed-length types (length prefix)
constexpr uint8_t TDS_TYPE_INTN = 0x26;
constexpr uint8_t TDS_TYPE_BITN = 0x68;
constexpr uint8_t TDS_TYPE_FLOATN = 0x6D;
constexpr uint8_t TDS_TYPE_MONEYN = 0x6E;
constexpr uint8_t TDS_TYPE_DATETIMEN = 0x6F;

// Decimal/Numeric types
constexpr uint8_t TDS_TYPE_DECIMAL = 0x6A;
constexpr uint8_t TDS_TYPE_NUMERIC = 0x6C;

// GUID type
constexpr uint8_t TDS_TYPE_UNIQUEIDENTIFIER = 0x24;

// String types (collation info in metadata)
constexpr uint8_t TDS_TYPE_BIGCHAR = 0xAF;     // CHAR
constexpr uint8_t TDS_TYPE_BIGVARCHAR = 0xA7; // VARCHAR
constexpr uint8_t TDS_TYPE_NCHAR = 0xEF;
constexpr uint8_t TDS_TYPE_NVARCHAR = 0xE7;

// Binary types
constexpr uint8_t TDS_TYPE_BIGBINARY = 0xAD;     // BINARY
constexpr uint8_t TDS_TYPE_BIGVARBINARY = 0xA5; // VARBINARY

// Date/Time types (SQL Server 2008+)
constexpr uint8_t TDS_TYPE_DATE = 0x28;
constexpr uint8_t TDS_TYPE_TIME = 0x29;
constexpr uint8_t TDS_TYPE_DATETIME2 = 0x2A;
constexpr uint8_t TDS_TYPE_DATETIMEOFFSET = 0x2B;

// Unsupported types (will fail with clear error)
constexpr uint8_t TDS_TYPE_XML = 0xF1;
constexpr uint8_t TDS_TYPE_UDT = 0xF0;         // Also GEOGRAPHY, GEOMETRY, HIERARCHYID
constexpr uint8_t TDS_TYPE_SQL_VARIANT = 0x62;
constexpr uint8_t TDS_TYPE_IMAGE = 0x22;       // Deprecated
constexpr uint8_t TDS_TYPE_TEXT = 0x23;        // Deprecated
constexpr uint8_t TDS_TYPE_NTEXT = 0x63;       // Deprecated

// Column flags bitmask (from COLMETADATA)
constexpr uint16_t COL_FLAG_NULLABLE = 0x0001;
constexpr uint16_t COL_FLAG_CASE_SENSITIVE = 0x0002;
constexpr uint16_t COL_FLAG_IDENTITY = 0x0010;
constexpr uint16_t COL_FLAG_COMPUTED = 0x0020;

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
constexpr int DEFAULT_QUERY_TIMEOUT = 30;
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

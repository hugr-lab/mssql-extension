#include "tds/tds_protocol.hpp"
#include <cstring>
#include <stdexcept>
#include "tds/encoding/utf16.hpp"
#ifdef _WIN32
#include <process.h>
#define GET_PID() _getpid()
#else
#include <unistd.h>
#define GET_PID() getpid()
#endif

namespace duckdb {
namespace tds {

// PRELOGIN option offsets in the packet
struct PreloginOptionHeader {
	uint8_t option;
	uint16_t offset;
	uint16_t length;
};

TdsPacket TdsProtocol::BuildPrelogin(bool use_encrypt) {
	TdsPacket packet(PacketType::PRELOGIN);

	// Build option headers first
	// VERSION: 6 bytes
	// ENCRYPTION: 1 byte
	// TERMINATOR: 0 bytes

	// Calculate offsets (headers come first, then data)
	// Each header is 5 bytes: option(1) + offset(2) + length(2)
	// 3 options = 15 bytes of headers, but terminator is just 1 byte
	// So: VERSION header (5) + ENCRYPTION header (5) + TERMINATOR (1) = 11 bytes for headers
	uint16_t data_offset = 11;

	// VERSION option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::VERSION));
	packet.AppendUInt16BE(data_offset);	 // offset
	packet.AppendUInt16BE(6);			 // length

	// ENCRYPTION option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::ENCRYPTION));
	packet.AppendUInt16BE(data_offset + 6);	 // offset (after VERSION data)
	packet.AppendUInt16BE(1);				 // length

	// TERMINATOR
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::TERMINATOR));

	// VERSION data: UL_VERSION (4 bytes) + US_SUBBUILD (2 bytes)
	// We report as a generic TDS 7.4 client
	packet.AppendByte(15);	   // Major version (SQL Server 2019 = 15.x)
	packet.AppendByte(0);	   // Minor version
	packet.AppendUInt16BE(0);  // Build number
	packet.AppendUInt16BE(0);  // Sub-build number

	// ENCRYPTION data: request encryption based on use_encrypt parameter
	// ENCRYPT_ON: Client requests encryption, expects server to agree
	// ENCRYPT_NOT_SUP: Client does not support encryption (plaintext only)
	if (use_encrypt) {
		packet.AppendByte(static_cast<uint8_t>(EncryptionOption::ENCRYPT_ON));
	} else {
		packet.AppendByte(static_cast<uint8_t>(EncryptionOption::ENCRYPT_NOT_SUP));
	}

	return packet;
}

PreloginResponse TdsProtocol::ParsePreloginResponse(const std::vector<uint8_t> &data) {
	PreloginResponse response = {};
	response.success = false;

	if (data.empty()) {
		response.error_message = "Empty PRELOGIN response";
		return response;
	}

	// Parse option headers
	size_t pos = 0;
	while (pos < data.size()) {
		uint8_t option = data[pos];

		if (option == static_cast<uint8_t>(PreloginOption::TERMINATOR)) {
			response.success = true;
			break;
		}

		if (pos + 5 > data.size()) {
			response.error_message = "Truncated PRELOGIN response";
			return response;
		}

		uint16_t offset = (static_cast<uint16_t>(data[pos + 1]) << 8) | data[pos + 2];
		uint16_t length = (static_cast<uint16_t>(data[pos + 3]) << 8) | data[pos + 4];

		if (option == static_cast<uint8_t>(PreloginOption::VERSION)) {
			if (offset + length <= data.size() && length >= 6) {
				response.version_major = data[offset];
				response.version_minor = data[offset + 1];
				response.version_build = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
			}
		} else if (option == static_cast<uint8_t>(PreloginOption::ENCRYPTION)) {
			if (offset < data.size()) {
				response.encryption = static_cast<EncryptionOption>(data[offset]);
			}
		}

		pos += 5;
	}

	return response;
}

std::vector<uint8_t> TdsProtocol::EncodePassword(const std::string &password) {
	// Convert password to UTF-16LE
	std::vector<uint8_t> encoded = encoding::Utf16LEEncode(password);

	// Apply TDS password obfuscation to each byte:
	// Per MS-TDS spec: swap nibbles first, then XOR with 0xA5
	for (auto &byte : encoded) {
		byte = ((byte << 4) & 0xF0) | ((byte >> 4) & 0x0F);
		byte ^= 0xA5;
	}

	return encoded;
}

TdsPacket TdsProtocol::BuildLogin7(const std::string &host, const std::string &username, const std::string &password,
								   const std::string &database, const std::string &app_name, uint32_t packet_size) {
	TdsPacket packet(PacketType::LOGIN7);

	// LOGIN7 fixed header is 94 bytes
	// After that come variable-length strings

	// Calculate string offsets and lengths
	// Strings are stored as UTF-16LE, so length = chars, not bytes
	// Offset points to position in packet (after the 94-byte header relative to start of LOGIN7 data)

	uint16_t hostname_len = static_cast<uint16_t>(host.size());
	uint16_t username_len = static_cast<uint16_t>(username.size());
	uint16_t password_len = static_cast<uint16_t>(password.size());
	uint16_t appname_len = static_cast<uint16_t>(app_name.size());
	uint16_t servername_len = static_cast<uint16_t>(host.size());  // Server name same as host
	uint16_t database_len = static_cast<uint16_t>(database.size());

	// Variable data starts at offset 94 (end of fixed header)
	uint16_t var_offset = 94;

	uint16_t hostname_offset = var_offset;
	uint16_t username_offset = hostname_offset + hostname_len * 2;
	uint16_t password_offset = username_offset + username_len * 2;
	uint16_t appname_offset = password_offset + password_len * 2;
	uint16_t servername_offset = appname_offset + appname_len * 2;
	uint16_t unused_offset = servername_offset + servername_len * 2;  // CltIntName (unused)
	uint16_t language_offset = unused_offset;						  // Language (unused)
	uint16_t database_offset = language_offset;						  // Database follows unused fields
	// Actually, database follows in the sequence

	// Recalculate properly
	// Fields in order: HostName, UserName, Password, AppName, ServerName, Unused, CltIntName, Language, Database, SSPI,
	// AtchDBFile, ChangePassword We only use HostName, UserName, Password, AppName, ServerName, Database Others have
	// length 0

	var_offset = 94;
	hostname_offset = var_offset;
	var_offset += hostname_len * 2;

	username_offset = var_offset;
	var_offset += username_len * 2;

	password_offset = var_offset;
	var_offset += password_len * 2;

	appname_offset = var_offset;
	var_offset += appname_len * 2;

	servername_offset = var_offset;
	var_offset += servername_len * 2;

	// Unused field (ExtensionOffset in newer versions)
	uint16_t unused1_offset = var_offset;
	uint16_t unused1_len = 0;

	// CltIntName (client interface name - unused)
	uint16_t cltintname_offset = var_offset;
	uint16_t cltintname_len = 0;

	// Language (unused)
	uint16_t language_off = var_offset;
	uint16_t language_len = 0;

	// Database
	database_offset = var_offset;
	var_offset += database_len * 2;

	// SSPI (unused)
	uint16_t sspi_offset = var_offset;
	uint16_t sspi_len = 0;

	// AtchDBFile (unused)
	uint16_t atchdb_offset = var_offset;
	uint16_t atchdb_len = 0;

	// ChangePassword (unused)
	uint16_t changepass_offset = var_offset;
	uint16_t changepass_len = 0;

	// Total length
	uint32_t total_length = var_offset;

	// Build fixed header (94 bytes)

	// Offset 0: Length (4 bytes, LE)
	packet.AppendUInt32LE(total_length);

	// Offset 4: TDSVersion (4 bytes, LE)
	packet.AppendUInt32LE(TDS_VERSION_7_4);

	// Offset 8: PacketSize (4 bytes, LE)
	packet.AppendUInt32LE(packet_size);

	// Offset 12: ClientProgVer (4 bytes, LE) - our version
	packet.AppendUInt32LE(0x00000001);

	// Offset 16: ClientPID (4 bytes, LE)
	packet.AppendUInt32LE(static_cast<uint32_t>(GET_PID()));

	// Offset 20: ConnectionID (4 bytes, LE) - 0 for new connection
	packet.AppendUInt32LE(0);

	// Offset 24: OptionFlags1 (1 byte)
	// Bit 5: USE_DB (switch to database on login)
	// Bit 6: SET_LANG
	uint8_t flags1 = 0x20;	// USE_DB
	packet.AppendByte(flags1);

	// Offset 25: OptionFlags2 (1 byte)
	// Bit 1 (0x02): fODBC - enables ODBC compatibility mode, which automatically
	//                       sets ANSI session options (CONCAT_NULL_YIELDS_NULL,
	//                       ANSI_WARNINGS, ANSI_NULLS, ANSI_PADDING, QUOTED_IDENTIFIER)
	// Bit 7: INTEGRATED_SECURITY (not using)
	uint8_t flags2 = 0x02;  // fODBC flag for ANSI compatibility
	packet.AppendByte(flags2);

	// Offset 26: TypeFlags (1 byte)
	// Bit 4-7: SQLTYPE (0 = DFLT)
	// Bit 0: READONLY (0 = read/write)
	uint8_t type_flags = 0x00;
	packet.AppendByte(type_flags);

	// Offset 27: OptionFlags3 (1 byte)
	// Various TDS 7.2+ options
	uint8_t flags3 = 0x00;
	packet.AppendByte(flags3);

	// Offset 28: ClientTimeZone (4 bytes, LE) - minutes from UTC
	packet.AppendUInt32LE(0);

	// Offset 32: ClientLCID (4 bytes, LE) - locale ID
	packet.AppendUInt32LE(0x0409);	// en-US

	// Offset 36-93: Offset/Length pairs for variable fields (58 bytes = 29 uint16_t values)
	// Each field: offset (2 bytes) + length (2 bytes) = 4 bytes
	// 12 fields * 4 = 48 bytes... let me check spec

	// Actually the layout at offset 36 is:
	// ibHostName (2) + cchHostName (2)
	// ibUserName (2) + cchUserName (2)
	// ibPassword (2) + cchPassword (2)
	// ibAppName (2) + cchAppName (2)
	// ibServerName (2) + cchServerName (2)
	// ibUnused (2) + cbUnused (2)  -- or extension offset in newer
	// ibCltIntName (2) + cchCltIntName (2)
	// ibLanguage (2) + cchLanguage (2)
	// ibDatabase (2) + cchDatabase (2)
	// ClientID (6 bytes) -- MAC address
	// ibSSPI (2) + cbSSPI (2)
	// ibAtchDBFile (2) + cchAtchDBFile (2)
	// ibChangePassword (2) + cchChangePassword (2)
	// cbSSPILong (4)

	// HostName
	packet.AppendUInt16LE(hostname_offset);
	packet.AppendUInt16LE(hostname_len);

	// UserName
	packet.AppendUInt16LE(username_offset);
	packet.AppendUInt16LE(username_len);

	// Password
	packet.AppendUInt16LE(password_offset);
	packet.AppendUInt16LE(password_len);

	// AppName
	packet.AppendUInt16LE(appname_offset);
	packet.AppendUInt16LE(appname_len);

	// ServerName
	packet.AppendUInt16LE(servername_offset);
	packet.AppendUInt16LE(servername_len);

	// Unused/Extension (offset, length = 0)
	packet.AppendUInt16LE(unused1_offset);
	packet.AppendUInt16LE(unused1_len);

	// CltIntName
	packet.AppendUInt16LE(cltintname_offset);
	packet.AppendUInt16LE(cltintname_len);

	// Language
	packet.AppendUInt16LE(language_off);
	packet.AppendUInt16LE(language_len);

	// Database
	packet.AppendUInt16LE(database_offset);
	packet.AppendUInt16LE(database_len);

	// ClientID (6 bytes) - MAC address, we use zeros
	for (int i = 0; i < 6; i++) {
		packet.AppendByte(0);
	}

	// SSPI
	packet.AppendUInt16LE(sspi_offset);
	packet.AppendUInt16LE(sspi_len);

	// AtchDBFile
	packet.AppendUInt16LE(atchdb_offset);
	packet.AppendUInt16LE(atchdb_len);

	// ChangePassword
	packet.AppendUInt16LE(changepass_offset);
	packet.AppendUInt16LE(changepass_len);

	// cbSSPILong (4 bytes) - long SSPI length, 0 for us
	packet.AppendUInt32LE(0);

	// Now append variable data

	// HostName (UTF-16LE)
	packet.AppendUTF16LE(host);

	// UserName (UTF-16LE)
	packet.AppendUTF16LE(username);

	// Password (encoded, then as UTF-16LE-ish)
	std::vector<uint8_t> encoded_password = EncodePassword(password);
	packet.AppendPayload(encoded_password);

	// AppName (UTF-16LE)
	packet.AppendUTF16LE(app_name);

	// ServerName (UTF-16LE)
	packet.AppendUTF16LE(host);

	// Database (UTF-16LE)
	packet.AppendUTF16LE(database);

	return packet;
}

std::string TdsProtocol::ReadUTF16LE(const uint8_t *data, size_t char_count) {
	std::string result;
	result.reserve(char_count);
	for (size_t i = 0; i < char_count; i++) {
		// Simple conversion - assumes ASCII range
		result.push_back(static_cast<char>(data[i * 2]));
	}
	return result;
}

const uint8_t *TdsProtocol::FindToken(const uint8_t *data, size_t length, TokenType token) {
	uint8_t target = static_cast<uint8_t>(token);
	for (size_t i = 0; i < length; i++) {
		if (data[i] == target) {
			return data + i;
		}
	}
	return nullptr;
}

LoginResponse TdsProtocol::ParseLoginResponse(const std::vector<uint8_t> &data) {
	LoginResponse response = {};
	response.success = false;
	response.negotiated_packet_size = TDS_DEFAULT_PACKET_SIZE;	// Default until server tells us

	if (data.empty()) {
		response.error_message = "Empty LOGIN response";
		return response;
	}

	const uint8_t *ptr = data.data();
	const uint8_t *end = ptr + data.size();

	// Look for LOGINACK token (0xAD)
	while (ptr < end) {
		uint8_t token_type = *ptr++;

		if (token_type == static_cast<uint8_t>(TokenType::LOGINACK)) {
			if (ptr + 2 > end)
				break;

			// Length (2 bytes, LE)
			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + len > end)
				break;

			// Interface (1 byte)
			// TDSVersion (4 bytes)
			// ProgName length (1 byte)
			// ProgName (variable)
			// ProgVersion (4 bytes)

			if (len >= 10) {
				// Skip interface
				ptr++;
				// Read TDS version (big-endian in LOGINACK)
				response.tds_version = (static_cast<uint32_t>(ptr[0]) << 24) | (static_cast<uint32_t>(ptr[1]) << 16) |
									   (static_cast<uint32_t>(ptr[2]) << 8) | static_cast<uint32_t>(ptr[3]);
				ptr += 4;

				// Server name length
				uint8_t name_len = *ptr++;
				if (ptr + name_len * 2 <= end) {
					response.server_name = ReadUTF16LE(ptr, name_len);
				}
			}

			response.success = true;
			return response;

		} else if (token_type == static_cast<uint8_t>(TokenType::ERROR_TOKEN)) {
			if (ptr + 2 > end)
				break;

			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + len > end || len < 14)
				break;

			// Error number (4 bytes, LE)
			response.error_number = ptr[0] | (static_cast<uint32_t>(ptr[1]) << 8) |
									(static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
			ptr += 4;

			// State (1 byte)
			ptr++;
			// Class (1 byte)
			ptr++;

			// Message length (2 bytes, LE) - in characters
			uint16_t msg_len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + msg_len * 2 <= end) {
				response.error_message = ReadUTF16LE(ptr, msg_len);
			}

			response.success = false;
			return response;

		} else if (token_type == static_cast<uint8_t>(TokenType::DONE) ||
				   token_type == static_cast<uint8_t>(TokenType::DONEPROC) ||
				   token_type == static_cast<uint8_t>(TokenType::DONEINPROC)) {
			// DONE token: skip 8 bytes (status + curcmd + rowcount)
			if (ptr + 8 <= end) {
				ptr += 8;
			} else {
				break;
			}
		} else if (token_type == static_cast<uint8_t>(TokenType::ENVCHANGE)) {
			if (ptr + 2 > end)
				break;
			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;
			if (ptr + len <= end) {
				const uint8_t *env_data = ptr;
				// ENVCHANGE type is first byte
				uint8_t env_type = env_data[0];
				// Type 4 = PACKETSIZE
				if (env_type == 4 && len >= 3) {
					// NewValueLen (1 byte)
					uint8_t new_val_len = env_data[1];
					// NewValue (string of digits in UTF-16LE)
					if (new_val_len > 0 && 2 + new_val_len * 2 <= len) {
						std::string packet_size_str = ReadUTF16LE(env_data + 2, new_val_len);
						try {
							response.negotiated_packet_size = std::stoul(packet_size_str);
						} catch (...) {
							// Ignore parse errors
						}
					}
				}
				ptr += len;
			} else {
				break;
			}
		} else if (token_type == static_cast<uint8_t>(TokenType::INFO)) {
			if (ptr + 2 > end)
				break;
			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;
			if (ptr + len <= end) {
				ptr += len;
			} else {
				break;
			}
		} else {
			// Unknown token - try to skip by reading length if available
			// Most tokens have 2-byte length after token type
			if (ptr + 2 <= end) {
				uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
				ptr += 2;
				if (len > 0 && ptr + len <= end) {
					ptr += len;
				}
			} else {
				break;
			}
		}
	}

	// If we get here without finding LOGINACK, check if we got an error
	if (!response.success && response.error_message.empty()) {
		response.error_message = "No LOGINACK token in response";
	}

	return response;
}

TdsPacket TdsProtocol::BuildPing() {
	// Send a simple SELECT 1 query as a ping
	// This is more reliable than an empty batch and includes proper ALL_HEADERS
	return BuildSqlBatch("SELECT 1");
}

TdsPacket TdsProtocol::BuildSqlBatch(const std::string &sql, const uint8_t *transaction_descriptor) {
	TdsPacket packet(PacketType::SQL_BATCH);

	// SQL_BATCH for TDS 7.1+ requires ALL_HEADERS prefix (MS-TDS 2.2.6.6)
	// When MARS is enabled (which modern SQL Server requires), we must include
	// the Transaction Descriptor header.
	//
	// ALL_HEADERS structure:
	//   TotalLength (4 bytes, DWORD) - includes the TotalLength field itself
	//   *Header - one or more headers
	//
	// Transaction Descriptor Header (MS-TDS 2.2.6.5.1):
	//   HeaderLength (4 bytes, DWORD) = 18 (4 + 2 + 8 + 4)
	//   HeaderType (2 bytes, USHORT) = 0x0002
	//   TransactionDescriptor (8 bytes) = from parameter or 0 for no transaction
	//   OutstandingRequestCount (4 bytes, DWORD) = 1

	// Transaction Descriptor Header: 18 bytes
	// Total headers = 4 (length field) + 18 (trans desc) = 22 bytes
	uint32_t total_headers_length = 22;
	packet.AppendByte(total_headers_length & 0xFF);
	packet.AppendByte((total_headers_length >> 8) & 0xFF);
	packet.AppendByte((total_headers_length >> 16) & 0xFF);
	packet.AppendByte((total_headers_length >> 24) & 0xFF);

	// Transaction Descriptor Header
	uint32_t header_length = 18;  // 4 + 2 + 8 + 4
	packet.AppendByte(header_length & 0xFF);
	packet.AppendByte((header_length >> 8) & 0xFF);
	packet.AppendByte((header_length >> 16) & 0xFF);
	packet.AppendByte((header_length >> 24) & 0xFF);

	// Header Type = 0x0002 (Transaction Descriptor)
	packet.AppendByte(0x02);
	packet.AppendByte(0x00);

	// Transaction Descriptor (8 bytes) - use provided descriptor or zeros
	if (transaction_descriptor) {
		for (int i = 0; i < 8; i++) {
			packet.AppendByte(transaction_descriptor[i]);
		}
	} else {
		for (int i = 0; i < 8; i++) {
			packet.AppendByte(0x00);
		}
	}

	// Outstanding Request Count = 1
	packet.AppendByte(0x01);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);

	// SQL text encoded as UTF-16LE
	std::vector<uint8_t> encoded = encoding::Utf16LEEncode(sql);
	packet.AppendPayload(encoded);

	return packet;
}

std::vector<TdsPacket> TdsProtocol::BuildSqlBatchMultiPacket(const std::string &sql, size_t max_packet_size,
															 const uint8_t *transaction_descriptor) {
	std::vector<TdsPacket> packets;

	// Encode SQL to UTF-16LE
	std::vector<uint8_t> sql_encoded = encoding::Utf16LEEncode(sql);

	if (sql_encoded.empty()) {
		// Empty query - send ping (SELECT 1) to get a valid response
		packets.push_back(BuildPing());
		return packets;
	}

	// Build ALL_HEADERS section (required for SQL_BATCH in TDS 7.2+)
	// This is 22 bytes: TotalLength (4) + Transaction Descriptor Header (18)
	std::vector<uint8_t> all_headers;
	all_headers.reserve(22);

	// TotalLength = 22 (little-endian)
	uint32_t total_headers_length = 22;
	all_headers.push_back(total_headers_length & 0xFF);
	all_headers.push_back((total_headers_length >> 8) & 0xFF);
	all_headers.push_back((total_headers_length >> 16) & 0xFF);
	all_headers.push_back((total_headers_length >> 24) & 0xFF);

	// Transaction Descriptor Header: HeaderLength = 18
	uint32_t header_length = 18;
	all_headers.push_back(header_length & 0xFF);
	all_headers.push_back((header_length >> 8) & 0xFF);
	all_headers.push_back((header_length >> 16) & 0xFF);
	all_headers.push_back((header_length >> 24) & 0xFF);

	// HeaderType = 0x0002 (Transaction Descriptor)
	all_headers.push_back(0x02);
	all_headers.push_back(0x00);

	// TransactionDescriptor (8 bytes) - use provided descriptor or zeros
	if (transaction_descriptor) {
		for (int i = 0; i < 8; i++) {
			all_headers.push_back(transaction_descriptor[i]);
		}
	} else {
		for (int i = 0; i < 8; i++) {
			all_headers.push_back(0x00);
		}
	}

	// OutstandingRequestCount = 1 (4 bytes, little-endian)
	all_headers.push_back(0x01);
	all_headers.push_back(0x00);
	all_headers.push_back(0x00);
	all_headers.push_back(0x00);

	// Combine ALL_HEADERS + SQL for fragmentation
	std::vector<uint8_t> payload;
	payload.reserve(all_headers.size() + sql_encoded.size());
	payload.insert(payload.end(), all_headers.begin(), all_headers.end());
	payload.insert(payload.end(), sql_encoded.begin(), sql_encoded.end());

	// TDS packet header is 8 bytes, so payload can be up to (max_packet_size - 8) bytes
	size_t max_payload = max_packet_size - TDS_HEADER_SIZE;

	// If it fits in a single packet, use single packet (this should match BuildSqlBatch behavior)
	if (payload.size() <= max_payload) {
		TdsPacket packet(PacketType::SQL_BATCH);
		packet.AppendPayload(payload);
		packets.push_back(std::move(packet));
		return packets;
	}

	// Split into multiple packets
	size_t offset = 0;
	while (offset < payload.size()) {
		size_t chunk_size = std::min(max_payload, payload.size() - offset);
		bool is_last = (offset + chunk_size >= payload.size());

		TdsPacket packet(PacketType::SQL_BATCH);

		// Set EOM flag only on last packet
		if (!is_last) {
			packet.SetEndOfMessage(false);
		}

		// Append this chunk of the payload
		packet.AppendPayload(payload.data() + offset, chunk_size);

		packets.push_back(std::move(packet));
		offset += chunk_size;
	}

	return packets;
}

std::vector<TdsPacket> TdsProtocol::BuildBulkLoadMultiPacket(const std::vector<uint8_t> &payload,
															 size_t max_packet_size) {
	std::vector<TdsPacket> packets;

	if (payload.empty()) {
		// Empty payload - return empty packets
		return packets;
	}

	// TDS packet header is 8 bytes, so payload can be up to (max_packet_size - 8) bytes
	size_t max_payload = max_packet_size - TDS_HEADER_SIZE;

	// If it fits in a single packet, use single packet
	if (payload.size() <= max_payload) {
		TdsPacket packet(PacketType::BULK_LOAD);
		packet.AppendPayload(payload);
		packets.push_back(std::move(packet));
		return packets;
	}

	// Split into multiple packets
	size_t offset = 0;
	while (offset < payload.size()) {
		size_t chunk_size = std::min(max_payload, payload.size() - offset);
		bool is_last = (offset + chunk_size >= payload.size());

		TdsPacket packet(PacketType::BULK_LOAD);

		// Set EOM flag only on last packet
		if (!is_last) {
			packet.SetEndOfMessage(false);
		}

		// Append this chunk of the payload
		packet.AppendPayload(payload.data() + offset, chunk_size);

		packets.push_back(std::move(packet));
		offset += chunk_size;
	}

	return packets;
}

TdsPacket TdsProtocol::BuildAttention() {
	TdsPacket packet(PacketType::ATTENTION);
	// Single byte payload with 0xFF marker
	packet.AppendByte(0xFF);
	return packet;
}

bool TdsProtocol::ParseDoneForAttentionAck(const std::vector<uint8_t> &data) {
	// Look for DONE token with DONE_ATTN flag
	const uint8_t *ptr = data.data();
	const uint8_t *end = ptr + data.size();

	while (ptr < end) {
		uint8_t token = *ptr++;

		if (token == static_cast<uint8_t>(TokenType::DONE) || token == static_cast<uint8_t>(TokenType::DONEPROC) ||
			token == static_cast<uint8_t>(TokenType::DONEINPROC)) {
			if (ptr + 8 <= end) {
				// Status (2 bytes, LE)
				uint16_t status = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
				if (status & static_cast<uint16_t>(DoneStatus::DONE_ATTN)) {
					return true;
				}
				ptr += 8;
			} else {
				break;
			}
		} else {
			// Skip other tokens
			if (ptr + 2 <= end) {
				uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
				ptr += 2;
				if (ptr + len <= end) {
					ptr += len;
				} else {
					break;
				}
			} else {
				break;
			}
		}
	}

	return false;
}

bool TdsProtocol::IsSuccessResponse(const std::vector<uint8_t> &data) {
	// Check for ERROR token
	for (size_t i = 0; i < data.size(); i++) {
		if (data[i] == static_cast<uint8_t>(TokenType::ERROR_TOKEN)) {
			return false;
		}
	}

	// Check for DONE with error flag
	const uint8_t *ptr = data.data();
	const uint8_t *end = ptr + data.size();

	while (ptr < end) {
		uint8_t token = *ptr++;

		if (token == static_cast<uint8_t>(TokenType::DONE) || token == static_cast<uint8_t>(TokenType::DONEPROC) ||
			token == static_cast<uint8_t>(TokenType::DONEINPROC)) {
			if (ptr + 2 <= end) {
				uint16_t status = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
				if (status & static_cast<uint16_t>(DoneStatus::DONE_ERROR)) {
					return false;
				}
			}
			return true;
		}

		// Skip other tokens
		if (ptr + 2 <= end) {
			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;
			if (ptr + len <= end) {
				ptr += len;
			} else {
				break;
			}
		} else {
			break;
		}
	}

	return true;
}

std::string TdsProtocol::ExtractErrorMessage(const std::vector<uint8_t> &data) {
	const uint8_t *ptr = data.data();
	const uint8_t *end = ptr + data.size();

	while (ptr < end) {
		uint8_t token = *ptr++;

		if (token == static_cast<uint8_t>(TokenType::ERROR_TOKEN)) {
			if (ptr + 2 > end)
				break;

			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + len > end || len < 10)
				break;

			// Skip error number (4), state (1), class (1)
			ptr += 6;

			// Message length (2 bytes, LE)
			uint16_t msg_len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + msg_len * 2 <= end) {
				return ReadUTF16LE(ptr, msg_len);
			}
			break;
		}

		// Skip other tokens
		if (ptr + 2 <= end) {
			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;
			if (ptr + len <= end) {
				ptr += len;
			} else {
				break;
			}
		} else {
			break;
		}
	}

	return "";
}

}  // namespace tds
}  // namespace duckdb

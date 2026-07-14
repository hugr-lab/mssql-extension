#include "tds/tds_protocol.hpp"
#include <cstdio>
#include <cstdlib>
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

// Debug logging
static int GetMssqlDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_PROTO_DEBUG_LOG(lvl, fmt, ...)                           \
	do {                                                               \
		if (GetMssqlDebugLevel() >= lvl)                               \
			fprintf(stderr, "[MSSQL PROTO] " fmt "\n", ##__VA_ARGS__); \
	} while (0)

namespace duckdb {
namespace tds {

// MS-TDS §2.2.6.4 caps each variable LOGIN7 string field at 128 UTF-16
// code units. Spec 043 FR-008 enforces this in the helper below.
static constexpr size_t LOGIN7_MAX_FIELD_CODE_UNITS = 128;

// LOGIN7 variable-field encoding result. Holds the UTF-16LE bytes
// (already obfuscated when this is the password field), the UTF-16
// code-unit count for the `cch*` slot in the fixed header, and the
// `ib*` offset assigned to the field in the variable-data region.
struct Login7VarField {
	std::vector<uint8_t> utf16le_bytes;
	uint16_t cch;
	uint16_t ib;
};

// Encode one variable LOGIN7 string field from UTF-8.
//
// Spec 043 FR-001..FR-008. The helper owns:
//   * UTF-8 -> UTF-16LE conversion via the simdutf wrapper (FR-033).
//   * The 128-UTF-16-code-unit cap check (FR-008). On overflow throws
//     `std::runtime_error` with the exact wording mandated by the spec.
//     The TDS layer is DuckDB-free; the exception propagates upward
//     and surfaces to the user at ATTACH/connect time.
//   * The TDS password obfuscation (FR-003) when `obfuscate_password`
//     is true — applied to the encoded UTF-16LE bytes after conversion.
//   * `cumulative_ib_offset` bookkeeping: returns the offset assigned
//     to this field and advances the running offset by the encoded
//     byte length.
static Login7VarField EncodeLogin7VarField(const char *field_name, const std::string &utf8_text,
										   uint16_t &cumulative_ib_offset, bool obfuscate_password = false) {
	Login7VarField result;
	result.ib = cumulative_ib_offset;

	if (!utf8_text.empty()) {
		result.utf16le_bytes = encoding::Utf16LEEncode(utf8_text);
	}

	const size_t code_units = result.utf16le_bytes.size() / 2;
	if (code_units > LOGIN7_MAX_FIELD_CODE_UNITS) {
		throw std::runtime_error(std::string("LOGIN7 field ") + field_name +
								 " exceeds the TDS limit of 128 UTF-16 code units (got " + std::to_string(code_units) +
								 ")");
	}
	result.cch = static_cast<uint16_t>(code_units);

	if (obfuscate_password) {
		for (auto &byte : result.utf16le_bytes) {
			byte = static_cast<uint8_t>(((byte << 4) & 0xF0) | ((byte >> 4) & 0x0F));
			byte ^= 0xA5;
		}
	}

	cumulative_ib_offset = static_cast<uint16_t>(cumulative_ib_offset + result.utf16le_bytes.size());
	return result;
}

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
	response.fedauth_echo = false;

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
		} else if (option == static_cast<uint8_t>(PreloginOption::FEDAUTHREQUIRED)) {
			// Per MS-TDS and go-mssqldb: if server's FEDAUTHREQUIRED value is non-zero,
			// client must echo it back in LOGIN7's FEDAUTH options byte (bit 0)
			if (offset < data.size() && length >= 1) {
				response.fedauth_echo = (data[offset] != 0);
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

	// LOGIN7 fixed header is 94 bytes; variable-length strings follow.
	// Per MS-TDS §2.2.6.4, every variable string field is recorded by a
	// (ib*, cch*) pair where ib* is the byte offset into the packet and
	// cch* is the UTF-16 code-unit count (NOT the UTF-8 byte count).
	// Spec 043 FR-001..FR-002 fixes the v0.1.18 confusion of these two
	// numbers via the EncodeLogin7VarField helper.

	uint16_t var_offset = 94;
	Login7VarField field_hostname = EncodeLogin7VarField("HostName", host, var_offset);
	Login7VarField field_username = EncodeLogin7VarField("UserName", username, var_offset);
	Login7VarField field_password = EncodeLogin7VarField("Password", password, var_offset, /*obfuscate_password=*/true);
	Login7VarField field_appname = EncodeLogin7VarField("AppName", app_name, var_offset);
	Login7VarField field_servername = EncodeLogin7VarField("ServerName", host, var_offset);	 // ServerName mirrors host

	// Unused / extension / CltIntName / Language all have length 0 and share
	// the current cumulative offset (per MS-TDS §2.2.6.4 zero-length fields
	// still carry an ib* pointing to the next-available byte position).
	uint16_t unused1_offset = var_offset;
	uint16_t unused1_len = 0;
	uint16_t cltintname_offset = var_offset;
	uint16_t cltintname_len = 0;
	uint16_t language_off = var_offset;
	uint16_t language_len = 0;

	Login7VarField field_database = EncodeLogin7VarField("Database", database, var_offset);

	uint16_t sspi_offset = var_offset;
	uint16_t sspi_len = 0;
	uint16_t atchdb_offset = var_offset;
	uint16_t atchdb_len = 0;
	uint16_t changepass_offset = var_offset;
	uint16_t changepass_len = 0;

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
	// Bit 5 (0x20): USE_DB - switch to database on login
	// Bit 7 (0x80): SET_LANG - use language from login
	// Per go-mssqldb: both flags are set (0xA0)
	uint8_t flags1 = 0x20 | 0x80;  // USE_DB | SET_LANG
	packet.AppendByte(flags1);

	// Offset 25: OptionFlags2 (1 byte)
	// Bit 1 (0x02): fODBC - enables ODBC compatibility mode, which automatically
	//                       sets ANSI session options (CONCAT_NULL_YIELDS_NULL,
	//                       ANSI_WARNINGS, ANSI_NULLS, ANSI_PADDING, QUOTED_IDENTIFIER)
	// Bit 7: INTEGRATED_SECURITY (not using)
	uint8_t flags2 = 0x02;	// fODBC flag for ANSI compatibility
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
	packet.AppendUInt16LE(field_hostname.ib);
	packet.AppendUInt16LE(field_hostname.cch);

	// UserName
	packet.AppendUInt16LE(field_username.ib);
	packet.AppendUInt16LE(field_username.cch);

	// Password
	packet.AppendUInt16LE(field_password.ib);
	packet.AppendUInt16LE(field_password.cch);

	// AppName
	packet.AppendUInt16LE(field_appname.ib);
	packet.AppendUInt16LE(field_appname.cch);

	// ServerName
	packet.AppendUInt16LE(field_servername.ib);
	packet.AppendUInt16LE(field_servername.cch);

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
	packet.AppendUInt16LE(field_database.ib);
	packet.AppendUInt16LE(field_database.cch);

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

	// Variable data: each field's UTF-16LE bytes were produced by
	// EncodeLogin7VarField above. Password bytes are already obfuscated.
	packet.AppendPayload(field_hostname.utf16le_bytes);
	packet.AppendPayload(field_username.utf16le_bytes);
	packet.AppendPayload(field_password.utf16le_bytes);
	packet.AppendPayload(field_appname.utf16le_bytes);
	packet.AppendPayload(field_servername.utf16le_bytes);
	packet.AppendPayload(field_database.utf16le_bytes);

	return packet;
}

//===----------------------------------------------------------------------===//
// BuildLogin7WithSSPI - Integrated Authentication (Kerberos / SSPI) -- Spec 042
//
// Layout matches BuildLogin7 exactly except:
//   * OptionFlags2 has bit 7 (fIntSecurity / 0x80) set, in addition to fODBC.
//   * UserName / Password fields have length 0 and contribute no bytes.
//   * The SSPI field at offset 36+offset_pair_index*4 carries the initial SPNEGO
//     blob from IAuthenticator::InitialBytes(). For blobs > 65 535 bytes, cbSSPI
//     is set to 0xFFFF and the real length is written into cbSSPILong at offset 86.
//
// See [MS-TDS] 2.2.6.4 and spec 042 research.md R1.
//===----------------------------------------------------------------------===//
TdsPacket TdsProtocol::BuildLogin7WithSSPI(const std::string &client_hostname, const std::string &server_name,
										   const std::string &database, const std::vector<uint8_t> &sspi_initial_blob,
										   const std::string &app_name, uint32_t packet_size) {
	TdsPacket packet(PacketType::LOGIN7);

	uint16_t hostname_len = static_cast<uint16_t>(client_hostname.size());
	uint16_t username_len = 0;	// integrated auth: not used
	uint16_t password_len = 0;	// integrated auth: not used
	uint16_t appname_len = static_cast<uint16_t>(app_name.size());
	uint16_t servername_len = static_cast<uint16_t>(server_name.size());
	uint16_t database_len = static_cast<uint16_t>(database.size());

	// SSPI blob length handling: cbSSPI is 16-bit; for blobs > 65535 bytes,
	// cbSSPI is sentinel 0xFFFF and cbSSPILong (32-bit at offset 86) holds the
	// real length. ibSSPI still points to the start of the blob in either case.
	uint32_t sspi_blob_total = static_cast<uint32_t>(sspi_initial_blob.size());
	uint16_t sspi_short_len = (sspi_blob_total > 0xFFFF) ? 0xFFFF : static_cast<uint16_t>(sspi_blob_total);
	uint32_t sspi_long_len = (sspi_blob_total > 0xFFFF) ? sspi_blob_total : 0;

	// Variable data starts at offset 94 (end of fixed header). var_offset is
	// 32-bit because the SPNEGO+Kerberos blob alone can exceed 64 KB for users
	// in many AD groups (Microsoft documents PAC sizes up to 1 MB) -- a
	// uint16_t accumulator wraps mod 65 536 and produces a corrupted LOGIN7
	// Length header. spec 042 ultrareview bug_032.
	//
	// Individual offsets that precede the SSPI region fit in 16 bits (all the
	// UTF-16 fields combined are tiny). The total length and the SSPI blob
	// itself are the 32-bit values.
	uint32_t var_offset = 94;
	uint16_t hostname_offset = static_cast<uint16_t>(var_offset);
	var_offset += hostname_len * 2;

	uint16_t username_offset = static_cast<uint16_t>(var_offset);  // length 0
	uint16_t password_offset = static_cast<uint16_t>(var_offset);
	uint16_t appname_offset = static_cast<uint16_t>(var_offset);
	var_offset += appname_len * 2;

	uint16_t servername_offset = static_cast<uint16_t>(var_offset);
	var_offset += servername_len * 2;

	uint16_t unused1_offset = static_cast<uint16_t>(var_offset);
	uint16_t cltintname_offset = static_cast<uint16_t>(var_offset);
	uint16_t language_offset = static_cast<uint16_t>(var_offset);

	uint16_t database_offset = static_cast<uint16_t>(var_offset);
	var_offset += database_len * 2;

	// SSPI blob position. The OFFSET (ibSSPI) is 16-bit per the TDS spec --
	// blobs cannot start past 64 KB into the LOGIN7 record. That's fine: the
	// only thing big about LOGIN7 is the SSPI blob itself, and it comes after
	// all small UTF-16 fields. We assert defensively in case future field
	// additions push the offset past the limit.
	if (var_offset > 0xFFFF) {
		throw std::runtime_error("LOGIN7 SSPI offset exceeds 65535 bytes -- pre-SSPI fields too large");
	}
	uint16_t sspi_offset = static_cast<uint16_t>(var_offset);
	var_offset += sspi_blob_total;

	// AtchDBFile / ChangePassword have length 0 so their offset isn't read
	// by the server; clamp to keep the wire format clean.
	uint16_t atchdb_offset = static_cast<uint16_t>(var_offset > 0xFFFF ? 0xFFFF : var_offset);
	uint16_t changepass_offset = atchdb_offset;

	uint32_t total_length = var_offset;

	// Fixed header (94 bytes)

	// Length (4 bytes)
	packet.AppendUInt32LE(total_length);
	// TDSVersion
	packet.AppendUInt32LE(TDS_VERSION_7_4);
	// PacketSize
	packet.AppendUInt32LE(packet_size);
	// ClientProgVer
	packet.AppendUInt32LE(0x00000001);
	// ClientPID
	packet.AppendUInt32LE(static_cast<uint32_t>(GET_PID()));
	// ConnectionID
	packet.AppendUInt32LE(0);

	// OptionFlags1: USE_DB | SET_LANG
	uint8_t flags1 = 0x20 | 0x80;
	packet.AppendByte(flags1);

	// OptionFlags2: fODBC | fIntSecurity (Spec 042)
	// Bit 1 (0x02): fODBC - ANSI compatibility
	// Bit 7 (0x80): fIntSecurity - Integrated Auth, server reads SSPI field instead of user/pwd
	uint8_t flags2 = 0x02 | 0x80;
	packet.AppendByte(flags2);

	// TypeFlags
	packet.AppendByte(0x00);
	// OptionFlags3
	packet.AppendByte(0x00);
	// ClientTimeZone
	packet.AppendUInt32LE(0);
	// ClientLCID
	packet.AppendUInt32LE(0x0409);	// en-US

	// Variable-length field offset/length pairs

	packet.AppendUInt16LE(hostname_offset);
	packet.AppendUInt16LE(hostname_len);

	packet.AppendUInt16LE(username_offset);
	packet.AppendUInt16LE(username_len);

	packet.AppendUInt16LE(password_offset);
	packet.AppendUInt16LE(password_len);

	packet.AppendUInt16LE(appname_offset);
	packet.AppendUInt16LE(appname_len);

	packet.AppendUInt16LE(servername_offset);
	packet.AppendUInt16LE(servername_len);

	// Unused/Extension
	packet.AppendUInt16LE(unused1_offset);
	packet.AppendUInt16LE(0);

	// CltIntName
	packet.AppendUInt16LE(cltintname_offset);
	packet.AppendUInt16LE(0);

	// Language
	packet.AppendUInt16LE(language_offset);
	packet.AppendUInt16LE(0);

	// Database
	packet.AppendUInt16LE(database_offset);
	packet.AppendUInt16LE(database_len);

	// ClientID (6 bytes)
	for (int i = 0; i < 6; i++) {
		packet.AppendByte(0);
	}

	// SSPI offset / cbSSPI
	packet.AppendUInt16LE(sspi_offset);
	packet.AppendUInt16LE(sspi_short_len);

	// AtchDBFile
	packet.AppendUInt16LE(atchdb_offset);
	packet.AppendUInt16LE(0);

	// ChangePassword
	packet.AppendUInt16LE(changepass_offset);
	packet.AppendUInt16LE(0);

	// cbSSPILong (4 bytes) - real length when sspi_short_len == 0xFFFF
	packet.AppendUInt32LE(sspi_long_len);

	// Variable data section
	packet.AppendUTF16LE(client_hostname);
	// username / password fields contribute zero bytes
	packet.AppendUTF16LE(app_name);
	packet.AppendUTF16LE(server_name);
	packet.AppendUTF16LE(database);

	// SSPI initial blob (raw bytes, NOT UTF-16)
	if (!sspi_initial_blob.empty()) {
		packet.AppendPayload(sspi_initial_blob);
	}

	return packet;
}

//===----------------------------------------------------------------------===//
// BuildSSPIMessage - SSPI continuation packet (Spec 042)
//
// [MS-TDS] 2.2.6.16: a single packet of type 0x11 whose payload is the
// raw GSSAPI/SSPI blob from IAuthenticator::NextBytes().
//===----------------------------------------------------------------------===//
TdsPacket TdsProtocol::BuildSSPIMessage(const std::vector<uint8_t> &sspi_blob) {
	TdsPacket packet(PacketType::SSPI);
	if (!sspi_blob.empty()) {
		packet.AppendPayload(sspi_blob);
	}
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

			const uint8_t *loginack_end = ptr + len;
			if (len >= 10) {
				// Skip interface
				ptr++;
				// Read TDS version (big-endian in LOGINACK)
				response.tds_version = (static_cast<uint32_t>(ptr[0]) << 24) | (static_cast<uint32_t>(ptr[1]) << 16) |
									   (static_cast<uint32_t>(ptr[2]) << 8) | static_cast<uint32_t>(ptr[3]);
				ptr += 4;

				// Server name length. Clamp to the token's own extent, not the
				// whole buffer, so a crafted name_len can't read into the tokens
				// that follow the LOGINACK (issue #183; same class as the ERROR
				// msg_len clamp below).
				uint8_t name_len = *ptr++;
				if (ptr + static_cast<size_t>(name_len) * 2 <= loginack_end) {
					response.server_name = ReadUTF16LE(ptr, name_len);
				}
			}

			response.success = true;
			// Don't return yet - continue parsing for ENVCHANGE (ROUTING, PACKETSIZE, etc.)
			ptr = loginack_end;

		} else if (token_type == static_cast<uint8_t>(TokenType::ERROR_TOKEN)) {
			if (ptr + 2 > end)
				break;

			uint16_t len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + len > end || len < 14)
				break;

			// Bound every field read below to the token's own declared extent, not
			// the whole buffer, so a crafted length can't make us read into the next
			// token (issue #183). token_end <= end by the check above.
			const uint8_t *token_end = ptr + len;

			// Error number (4 bytes, LE)
			response.error_number = ptr[0] | (static_cast<uint32_t>(ptr[1]) << 8) |
									(static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
			ptr += 4;

			// State (1 byte) -- disambiguates login failures (18456); see issue #164
			response.error_state = ptr[0];
			ptr++;
			// Class (1 byte)
			ptr++;

			// Message length (2 bytes, LE) - in characters
			uint16_t msg_len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;

			if (ptr + static_cast<size_t>(msg_len) * 2 <= token_end) {
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
			if (ptr + len > end)
				break;	// truncated ENVCHANGE
			// Empty ENVCHANGE: the token type + 2-byte length are already consumed,
			// so skipping preserves loop progress (no infinite loop) and does NOT
			// abandon later tokens (e.g. a following ERROR). Reading env_data[0] with
			// len == 0 would be a 1-byte heap over-read (found by fuzz_login_response).
			if (len == 0)
				continue;
			{
				const uint8_t *env_data = ptr;
				// ENVCHANGE type is first byte
				uint8_t env_type = env_data[0];
				// Debug: log ENVCHANGE type
				MSSQL_PROTO_DEBUG_LOG(2, "ParseLoginResponse: ENVCHANGE type=%d, len=%d", env_type, len);
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
				// Type 20 (0x14) = ROUTING (Azure SQL/Fabric gateway redirection)
				else if (env_type == 20 && len >= 7) {
					MSSQL_PROTO_DEBUG_LOG(1, "ParseLoginResponse: ROUTING ENVCHANGE detected, len=%d", len);
					// ROUTING ENVCHANGE structure:
					// Type (1 byte) = 20
					// ValueLength (2 bytes, LE)
					// Protocol (1 byte) - 0 = TCP
					// ProtocolProperty (2 bytes, LE) - new port
					// AlternateServer length (2 bytes, LE) - in characters
					// AlternateServer (UTF-16LE)
					// OldValue (2 bytes) = 0x0000

					const uint8_t *routing_ptr = env_data + 1;
					const uint8_t *routing_end = env_data + len;

					if (routing_ptr + 2 > routing_end) {
						MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: truncated at value_len");
						ptr += len;
						continue;
					}
					// Skip ValueLength - already covered by outer len
					uint16_t value_len = routing_ptr[0] | (static_cast<uint16_t>(routing_ptr[1]) << 8);
					routing_ptr += 2;
					MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: value_len=%d", value_len);

					if (routing_ptr + 1 > routing_end) {
						MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: truncated at protocol");
						ptr += len;
						continue;
					}
					uint8_t protocol = *routing_ptr++;
					MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: protocol=%d", protocol);
					if (protocol != 0) {  // Only TCP (0) supported
						MSSQL_PROTO_DEBUG_LOG(1, "ROUTING: unsupported protocol %d, expected TCP (0)", protocol);
						ptr += len;
						continue;
					}

					if (routing_ptr + 2 > routing_end) {
						MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: truncated at port");
						ptr += len;
						continue;
					}
					response.routed_port = routing_ptr[0] | (static_cast<uint16_t>(routing_ptr[1]) << 8);
					routing_ptr += 2;
					MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: port=%d", response.routed_port);

					if (routing_ptr + 2 > routing_end) {
						MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: truncated at server_len");
						ptr += len;
						continue;
					}
					uint16_t server_len_chars = routing_ptr[0] | (static_cast<uint16_t>(routing_ptr[1]) << 8);
					routing_ptr += 2;
					MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: server_len_chars=%d", server_len_chars);

					if (routing_ptr + server_len_chars * 2 <= routing_end && server_len_chars > 0) {
						response.routed_server = ReadUTF16LE(routing_ptr, server_len_chars);
						response.has_routing = true;
						MSSQL_PROTO_DEBUG_LOG(1, "ROUTING: parsed server=%s:%d", response.routed_server.c_str(),
											  response.routed_port);
					} else {
						MSSQL_PROTO_DEBUG_LOG(2, "ROUTING: server string out of bounds or empty");
					}
				}
				ptr += len;
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
		} else if (token_type == static_cast<uint8_t>(TokenType::FEDAUTHINFO)) {
			// FEDAUTHINFO token (0xEE) - Azure AD sends STS URL and SPN
			// This is received in ADAL workflow after sending LOGIN7
			// Structure per MS-TDS and go-mssqldb:
			//   TokenLength (4 bytes, DWORD, LE)
			//   CountOfInfoIDs (4 bytes, DWORD, LE)
			//   InfoIDs[]: FedAuthInfoID (1 byte) + DataLength (4 bytes) + DataOffset (4 bytes)
			//   Data: UTF-16LE strings at offsets specified in InfoIDs
			if (ptr + 4 > end)
				break;

			uint32_t token_len = ptr[0] | (static_cast<uint32_t>(ptr[1]) << 8) | (static_cast<uint32_t>(ptr[2]) << 16) |
								 (static_cast<uint32_t>(ptr[3]) << 24);
			ptr += 4;
			MSSQL_PROTO_DEBUG_LOG(1, "FEDAUTHINFO: token_len=%u", token_len);

			if (ptr + token_len > end || token_len < 4)
				break;

			const uint8_t *token_start = ptr;
			const uint8_t *token_end = ptr + token_len;

			// CountOfInfoIDs (4 bytes, LE)
			uint32_t count = token_start[0] | (static_cast<uint32_t>(token_start[1]) << 8) |
							 (static_cast<uint32_t>(token_start[2]) << 16) |
							 (static_cast<uint32_t>(token_start[3]) << 24);
			const uint8_t *info_ptr = token_start + 4;
			MSSQL_PROTO_DEBUG_LOG(2, "FEDAUTHINFO: count=%u", count);

			// Read InfoID entries
			for (uint32_t i = 0; i < count && info_ptr + 9 <= token_end; i++) {
				uint8_t info_id = info_ptr[0];
				uint32_t data_len = info_ptr[1] | (static_cast<uint32_t>(info_ptr[2]) << 8) |
									(static_cast<uint32_t>(info_ptr[3]) << 16) |
									(static_cast<uint32_t>(info_ptr[4]) << 24);
				uint32_t data_offset = info_ptr[5] | (static_cast<uint32_t>(info_ptr[6]) << 8) |
									   (static_cast<uint32_t>(info_ptr[7]) << 16) |
									   (static_cast<uint32_t>(info_ptr[8]) << 24);
				info_ptr += 9;

				MSSQL_PROTO_DEBUG_LOG(2, "FEDAUTHINFO: info_id=%d, data_len=%u, data_offset=%u", info_id, data_len,
									  data_offset);

				// Data is at token_start + data_offset. Validate WITHOUT overflow:
				// data_offset + data_len can wrap (both attacker-controlled uint32),
				// so a wrapped sum could pass "<= token_len" while data_offset alone
				// points past token_end -> ReadUTF16LE reads wild memory (SEGV,
				// found by fuzz_login_response). Rearranged so neither side wraps.
				if (data_offset <= token_len && data_len <= token_len - data_offset) {
					const uint8_t *opt_data = token_start + data_offset;
					// Convert from UTF-16LE (data_len is in bytes, 2 bytes per char)
					uint16_t char_count = static_cast<uint16_t>(data_len / 2);
					std::string value = ReadUTF16LE(opt_data, char_count);

					if (info_id == FEDAUTHINFO_OPT_STS_URL) {
						response.sts_url = value;
						MSSQL_PROTO_DEBUG_LOG(1, "FEDAUTHINFO: STS_URL=%s", value.c_str());
					} else if (info_id == FEDAUTHINFO_OPT_SPN) {
						response.server_spn = value;
						MSSQL_PROTO_DEBUG_LOG(1, "FEDAUTHINFO: SPN=%s", value.c_str());
					}
				}
			}

			response.has_fedauth_info = true;
			ptr = token_end;
		} else if (token_type == static_cast<uint8_t>(TokenType::SSPI)) {
			// SSPI token (0xED) -- Integrated Auth continuation, Spec 042
			// [MS-TDS] 2.2.7.21: TokenType(1) + USHORT length(2) + Data(variable)
			if (ptr + 2 > end) {
				break;
			}
			uint16_t blob_len = ptr[0] | (static_cast<uint16_t>(ptr[1]) << 8);
			ptr += 2;
			if (ptr + blob_len > end) {
				MSSQL_PROTO_DEBUG_LOG(1, "SSPI token truncated: claim=%u, available=%zu", blob_len,
									  static_cast<size_t>(end - ptr));
				break;
			}
			MSSQL_PROTO_DEBUG_LOG(1, "SSPI token: blob_len=%u", blob_len);
			response.has_sspi_token = true;
			response.sspi_token.assign(ptr, ptr + blob_len);
			ptr += blob_len;
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

std::vector<TdsPacket> TdsProtocol::SplitIntoPackets(const TdsPacket &message, size_t max_packet_size) {
	std::vector<TdsPacket> packets;

	const PacketType type = message.GetType();
	const std::vector<uint8_t> &payload = message.GetPayload();

	// TDS packet header is 8 bytes, so payload per packet is capped at
	// (max_packet_size - 8). Guard against a degenerate max_packet_size.
	const size_t max_payload = (max_packet_size > TDS_HEADER_SIZE) ? (max_packet_size - TDS_HEADER_SIZE)
																   : (TDS_DEFAULT_PACKET_SIZE - TDS_HEADER_SIZE);

	// Fits in a single packet -- return the original (already EOM) untouched so
	// the common path is byte-identical to the pre-fix behavior.
	if (payload.size() <= max_payload) {
		packets.push_back(message);
		return packets;
	}

	size_t offset = 0;
	while (offset < payload.size()) {
		size_t chunk_size = std::min(max_payload, payload.size() - offset);
		bool is_last = (offset + chunk_size >= payload.size());

		TdsPacket packet(type);
		// EOM flag only on the last packet; intermediate packets clear it.
		if (!is_last) {
			packet.SetEndOfMessage(false);
		}
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

//===----------------------------------------------------------------------===//
// FEDAUTH Protocol Methods (T016, T017)
//===----------------------------------------------------------------------===//

TdsPacket TdsProtocol::BuildPreloginWithFedAuth(bool use_encrypt, bool fedauth_required) {
	TdsPacket packet(PacketType::PRELOGIN);

	// Build option headers matching go-mssqldb structure exactly:
	// VERSION: 6 bytes
	// ENCRYPTION: 1 byte
	// INSTOPT: 1 byte (null-terminated empty instance name)
	// THREADID: 4 bytes (all zeros)
	// MARS: 1 byte (disabled)
	// TRACEID: 36 bytes (go-mssqldb sends this - 16 bytes connid + 16 bytes activityid + 4 bytes sequence)
	// FEDAUTHREQUIRED: 1 byte (when fedauth_required is true)
	// TERMINATOR: 0 bytes

	// Calculate headers size
	// Each header is 5 bytes: option(1) + offset(2) + length(2)
	// VERSION(5) + ENCRYPTION(5) + INSTOPT(5) + THREADID(5) + MARS(5) + TRACEID(5) + TERMINATOR(1) = 31 bytes base
	// With FEDAUTH: add FEDAUTHREQUIRED(5) = 36 bytes
	uint16_t headers_size = fedauth_required ? 36 : 31;
	uint16_t data_offset = headers_size;

	// Data layout (after headers):
	// VERSION: 6 bytes at data_offset
	// ENCRYPTION: 1 byte at data_offset + 6
	// INSTOPT: 1 byte at data_offset + 7
	// THREADID: 4 bytes at data_offset + 8
	// MARS: 1 byte at data_offset + 12
	// TRACEID: 36 bytes at data_offset + 13
	// FEDAUTHREQUIRED: 1 byte at data_offset + 49 (if included)

	// VERSION option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::VERSION));
	packet.AppendUInt16BE(data_offset);	 // offset
	packet.AppendUInt16BE(6);			 // length

	// ENCRYPTION option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::ENCRYPTION));
	packet.AppendUInt16BE(data_offset + 6);	 // offset
	packet.AppendUInt16BE(1);				 // length

	// INSTOPT option header (instance name - empty, just null terminator)
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::INSTOPT));
	packet.AppendUInt16BE(data_offset + 7);	 // offset
	packet.AppendUInt16BE(1);				 // length (just the null terminator)

	// THREADID option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::THREADID));
	packet.AppendUInt16BE(data_offset + 8);	 // offset
	packet.AppendUInt16BE(4);				 // length

	// MARS option header
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::MARS));
	packet.AppendUInt16BE(data_offset + 12);  // offset
	packet.AppendUInt16BE(1);				  // length

	// TRACEID option header (go-mssqldb sends this with connection/activity IDs)
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::TRACEID));
	packet.AppendUInt16BE(data_offset + 13);  // offset
	packet.AppendUInt16BE(36);				  // length (16 + 16 + 4)

	// FEDAUTHREQUIRED option header (if requested)
	if (fedauth_required) {
		packet.AppendByte(static_cast<uint8_t>(PreloginOption::FEDAUTHREQUIRED));
		packet.AppendUInt16BE(data_offset + 49);  // offset (after TRACEID)
		packet.AppendUInt16BE(1);				  // length
	}

	// TERMINATOR
	packet.AppendByte(static_cast<uint8_t>(PreloginOption::TERMINATOR));

	// VERSION data: UL_VERSION (4 bytes LE) + US_SUBBUILD (2 bytes LE)
	// Per go-mssqldb: uses driver version 1.9.0.6 (0x01090006)
	// Format: byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24), 0, 0
	// go-mssqldb outputs: 06 00 09 01 00 00
	packet.AppendByte(0x06);  // v & 0xFF
	packet.AppendByte(0x00);  // (v >> 8) & 0xFF
	packet.AppendByte(0x09);  // (v >> 16) & 0xFF
	packet.AppendByte(0x01);  // (v >> 24) & 0xFF
	packet.AppendByte(0x00);  // sub-build low
	packet.AppendByte(0x00);  // sub-build high

	// ENCRYPTION data: request encryption
	// For Azure AD, we must use encryption
	if (use_encrypt) {
		packet.AppendByte(static_cast<uint8_t>(EncryptionOption::ENCRYPT_ON));
	} else {
		packet.AppendByte(static_cast<uint8_t>(EncryptionOption::ENCRYPT_NOT_SUP));
	}

	// INSTOPT data: null-terminated empty instance name
	packet.AppendByte(0x00);  // null terminator

	// THREADID data: 4 bytes, all zeros (per go-mssqldb)
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);

	// MARS data: disabled (0)
	packet.AppendByte(0x00);

	// TRACEID data: 36 bytes (16 bytes connid + 16 bytes activityid + 4 bytes sequence)
	// Use random UUIDs like go-mssqldb does
	// For simplicity, use fixed random-looking values (connection will still be unique per socket)
	// connid (16 bytes) - random UUID
	packet.AppendByte(0x4d);
	packet.AppendByte(0x11);
	packet.AppendByte(0x08);
	packet.AppendByte(0x69);
	packet.AppendByte(0x83);
	packet.AppendByte(0x05);
	packet.AppendByte(0x40);
	packet.AppendByte(0x4c);
	packet.AppendByte(0x8d);
	packet.AppendByte(0x4c);
	packet.AppendByte(0x40);
	packet.AppendByte(0x8d);
	packet.AppendByte(0x68);
	packet.AppendByte(0x05);
	packet.AppendByte(0x32);
	packet.AppendByte(0xcd);
	// activityid (16 bytes) - random UUID
	packet.AppendByte(0xe4);
	packet.AppendByte(0x56);
	packet.AppendByte(0xda);
	packet.AppendByte(0xab);
	packet.AppendByte(0x27);
	packet.AppendByte(0xff);
	packet.AppendByte(0x42);
	packet.AppendByte(0x3d);
	packet.AppendByte(0xa6);
	packet.AppendByte(0xb9);
	packet.AppendByte(0x88);
	packet.AppendByte(0x7b);
	packet.AppendByte(0x29);
	packet.AppendByte(0x50);
	packet.AppendByte(0x6b);
	packet.AppendByte(0x81);
	// sequence (4 bytes) - always 0
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);
	packet.AppendByte(0x00);

	// FEDAUTHREQUIRED data (if included)
	// MS-TDS: B_FEDAUTHREQUIRED = 0x01 when client requires FEDAUTH
	if (fedauth_required) {
		packet.AppendByte(0x01);  // FEDAUTH required
	}

	return packet;
}

TdsPacket TdsProtocol::BuildLogin7WithFedAuth(const std::string &client_hostname, const std::string &server_name,
											  const std::string &database, const std::vector<uint8_t> &fedauth_token,
											  bool fedauth_echo, const std::string &app_name, uint32_t packet_size) {
	TdsPacket packet(PacketType::LOGIN7);

	// LOGIN7 with FEDAUTH uses feature extensions (MS-TDS 2.2.7)
	// Structure:
	//   Fixed header (94 bytes)
	//   Variable-length strings (HostName, AppName, ServerName, Database, etc.)
	//   Feature extensions (pointed to by Extension field at offset 36)
	//
	// Per go-mssqldb and MS-TDS spec:
	//   HostName = client workstation name (identifies the client machine)
	//   ServerName = TDS server name (may include instance name for routing)

	// Spec 043: every variable LOGIN7 string field is encoded via the
	// EncodeLogin7VarField helper so UTF-8 -> UTF-16LE conversion and
	// cch*/ib* bookkeeping use UTF-16 code-unit counts, not UTF-8 byte
	// counts. FEDAUTH does not send username/password so those fields
	// stay empty (helper returns cch=0 and emits zero bytes).
	uint16_t var_offset = 94;
	Login7VarField field_hostname = EncodeLogin7VarField("HostName", client_hostname, var_offset);
	Login7VarField field_username = EncodeLogin7VarField("UserName", "", var_offset);
	Login7VarField field_password = EncodeLogin7VarField("Password", "", var_offset);
	Login7VarField field_appname = EncodeLogin7VarField("AppName", app_name, var_offset);
	Login7VarField field_servername = EncodeLogin7VarField("ServerName", server_name, var_offset);

	// CltIntName (unused) - points to same offset, length 0
	uint16_t cltintname_offset = var_offset;

	// Language (unused) - points to same offset, length 0
	uint16_t language_offset = var_offset;

	Login7VarField field_database = EncodeLogin7VarField("Database", database, var_offset);

	// SSPI, AtchDBFile, ChangePassword - all unused, point to current offset
	uint16_t sspi_attrdb_chgpwd_offset = var_offset;

	// Per MS-TDS 2.2.6.3 and go-mssqldb implementation:
	// The Extension DWORD comes AFTER all variable strings.
	// ibExtension points to this DWORD, cbExtension = 4 (size of DWORD).
	// The DWORD contains the offset to the actual feature extensions.
	// Feature extensions immediately follow the DWORD.
	//
	// Structure: [all strings] [DWORD] [FeatureExtensions]

	uint16_t extension_dword_offset = var_offset;
	var_offset += 4;  // DWORD that contains offset to feature extensions

	// Feature extensions start immediately after the DWORD
	uint32_t feature_ext_offset = var_offset;

	// Feature extensions for Azure SQL (per go-mssqldb):
	// 1. FEDAUTH (0x02): For authentication
	// 2. TERMINATOR (0xFF)
	// Note: go-mssqldb does NOT include AZURESQLSUPPORT, only FEDAUTH

	// FEDAUTH extension: FeatureId(1) + FeatureDataLen(4) + FeatureData(variable)
	// Per go-mssqldb tds.go, FEDAUTH FeatureData format for SECURITYTOKEN:
	//   Options (1 byte): (library << 1) | echo - where library=0x01 for SECURITYTOKEN
	//   TokenLen (4 bytes, LE): Length of the UTF-16LE token
	//   Token (variable): UTF-16LE encoded access token
	// FeatureDataLen = 1 (Options) + 4 (TokenLen) + token.size()
	uint32_t fedauth_data_len = 5 + static_cast<uint32_t>(fedauth_token.size());
	uint32_t fedauth_ext_len = 1 + 4 + fedauth_data_len;  // FeatureId + FeatureDataLen + FeatureData

	// Total length calculation
	// var_offset includes: Fixed header (94) + all variable strings + ExtensionDWORD (4)
	// Add: FEDAUTH extension + Terminator (1)
	uint32_t total_length = var_offset + fedauth_ext_len + 1;

	// Build fixed header (94 bytes)

	// Offset 0: Length (4 bytes, LE)
	packet.AppendUInt32LE(total_length);

	// Offset 4: TDSVersion (4 bytes, LE)
	packet.AppendUInt32LE(TDS_VERSION_7_4);

	// Offset 8: PacketSize (4 bytes, LE)
	packet.AppendUInt32LE(packet_size);

	// Offset 12: ClientProgVer (4 bytes, LE)
	packet.AppendUInt32LE(0x00000001);

	// Offset 16: ClientPID (4 bytes, LE)
	packet.AppendUInt32LE(static_cast<uint32_t>(GET_PID()));

	// Offset 20: ConnectionID (4 bytes, LE)
	packet.AppendUInt32LE(0);

	// Offset 24: OptionFlags1 (1 byte)
	// Bit 5 (0x20): USE_DB - switch to database on login
	// Bit 7 (0x80): SET_LANG - use language from login
	// Per go-mssqldb: both flags are set (0xA0)
	uint8_t flags1 = 0x20 | 0x80;  // USE_DB | SET_LANG
	packet.AppendByte(flags1);

	// Offset 25: OptionFlags2 (1 byte)
	// Bit 1: fODBC - ANSI compatibility
	// Bit 7: INTEGRATED_SECURITY - NOT used, we use FEDAUTH instead
	uint8_t flags2 = 0x02;	// fODBC only
	packet.AppendByte(flags2);

	// Offset 26: TypeFlags (1 byte)
	// Bit 4: fReadOnlyIntent - 0 for read/write, 1 for read-only
	// For Azure SQL with FEDAUTH, use read/write by default
	uint8_t type_flags = 0x00;	// Read/write intent
	packet.AppendByte(type_flags);

	// Offset 27: OptionFlags3 (1 byte)
	// Bit 4 (0x10): fExtension - indicates feature extension data is present (MS-TDS 2.2.6.3)
	// Per go-mssqldb: only set fExtension for FEDAUTH, don't set reserved bits
	uint8_t flags3 = 0x10;	// fExtension only
	packet.AppendByte(flags3);

	// Offset 28: ClientTimeZone (4 bytes, LE)
	packet.AppendUInt32LE(0);

	// Offset 32: ClientLCID (4 bytes, LE)
	packet.AppendUInt32LE(0x0409);	// en-US

	// Offset 36-93: Offset/Length pairs for variable fields

	// HostName
	packet.AppendUInt16LE(field_hostname.ib);
	packet.AppendUInt16LE(field_hostname.cch);

	// UserName (empty for FEDAUTH)
	packet.AppendUInt16LE(field_username.ib);
	packet.AppendUInt16LE(field_username.cch);

	// Password (empty for FEDAUTH)
	packet.AppendUInt16LE(field_password.ib);
	packet.AppendUInt16LE(field_password.cch);

	// AppName
	packet.AppendUInt16LE(field_appname.ib);
	packet.AppendUInt16LE(field_appname.cch);

	// ServerName
	packet.AppendUInt16LE(field_servername.ib);
	packet.AppendUInt16LE(field_servername.cch);

	// Extension - ibExtension points to the DWORD containing feature extension offset
	// Per go-mssqldb: cbExtension = 4 (just the DWORD size, not including extension data)
	packet.AppendUInt16LE(extension_dword_offset);
	packet.AppendUInt16LE(4);  // cbExtension = 4 (DWORD size only)

	// CltIntName (unused)
	packet.AppendUInt16LE(cltintname_offset);
	packet.AppendUInt16LE(0);

	// Language (unused)
	packet.AppendUInt16LE(language_offset);
	packet.AppendUInt16LE(0);

	// Database
	packet.AppendUInt16LE(field_database.ib);
	packet.AppendUInt16LE(field_database.cch);

	// ClientID (6 bytes) - MAC address, zeros
	for (int i = 0; i < 6; i++) {
		packet.AppendByte(0);
	}

	// SSPI (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// AtchDBFile (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// ChangePassword (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// cbSSPILong (4 bytes)
	packet.AppendUInt32LE(0);

	// =====================================
	// Variable data section (encoded UTF-16LE bytes from the helper)
	// =====================================
	packet.AppendPayload(field_hostname.utf16le_bytes);
	// UserName / Password are empty for FEDAUTH (zero bytes).
	packet.AppendPayload(field_appname.utf16le_bytes);
	packet.AppendPayload(field_servername.utf16le_bytes);
	// CltIntName / Language remain empty (zero bytes).
	packet.AppendPayload(field_database.utf16le_bytes);

	// SSPI - empty (no data written)
	// AtchDBFile - empty (no data written)
	// ChangePassword - empty (no data written)

	// ExtensionDWORD - contains offset to feature extensions
	// Per go-mssqldb: this DWORD comes after ALL variable strings
	packet.AppendUInt32LE(feature_ext_offset);

	// =====================================
	// Feature Extensions (per go-mssqldb)
	// =====================================

	// FEDAUTH extension (MS-TDS 2.2.7.1)
	// FeatureId: 0x02
	packet.AppendByte(static_cast<uint8_t>(FeatureExtId::FEDAUTH));

	// FeatureDataLen (4 bytes, LE)
	packet.AppendUInt32LE(fedauth_data_len);

	// FEDAUTH Feature Data (per go-mssqldb tds.go featureExtFedAuth.toBytes()):
	//   Options (1 byte): (FedAuthLibrary << 1) | FedAuthEcho
	//     FedAuthLibrarySecurityToken = 0x01
	//     FedAuthEcho = bit 0, set if server's FEDAUTHREQUIRED was non-zero
	//     So: (0x01 << 1) | echo = 0x02 or 0x03
	//   TokenLen (4 bytes, LE): Length of the UTF-16LE token
	//   Token (variable): UTF-16LE encoded access token
	uint8_t fedauth_options = (0x01 << 1) | (fedauth_echo ? 1 : 0);	 // SECURITYTOKEN | echo
	packet.AppendByte(fedauth_options);
	packet.AppendUInt32LE(static_cast<uint32_t>(fedauth_token.size()));

	// Token (UTF-16LE encoded access token)
	packet.AppendPayload(fedauth_token);

	// Feature terminator
	packet.AppendByte(static_cast<uint8_t>(FeatureExtId::TERMINATOR));

	return packet;
}

TdsPacket TdsProtocol::BuildLogin7WithADAL(const std::string &client_hostname, const std::string &server_name,
										   const std::string &database, bool fedauth_echo, const std::string &app_name,
										   uint32_t packet_size) {
	TdsPacket packet(PacketType::LOGIN7);

	// LOGIN7 with ADAL FEDAUTH workflow (per go-mssqldb implementation)
	// ADAL flow: LOGIN7 contains small FEDAUTH extension (just options + workflow bytes)
	// Server responds with FEDAUTHINFO token, client sends token in separate FEDAUTH_TOKEN packet

	// Spec 043: encode every variable LOGIN7 string field via the helper
	// so cch* / ib* use UTF-16 code units, not UTF-8 byte counts. ADAL
	// FEDAUTH carries no username/password.
	uint16_t var_offset = 94;
	Login7VarField field_hostname = EncodeLogin7VarField("HostName", client_hostname, var_offset);
	Login7VarField field_username = EncodeLogin7VarField("UserName", "", var_offset);
	Login7VarField field_password = EncodeLogin7VarField("Password", "", var_offset);
	Login7VarField field_appname = EncodeLogin7VarField("AppName", app_name, var_offset);
	Login7VarField field_servername = EncodeLogin7VarField("ServerName", server_name, var_offset);

	uint16_t cltintname_offset = var_offset;
	uint16_t language_offset = var_offset;

	Login7VarField field_database = EncodeLogin7VarField("Database", database, var_offset);

	uint16_t sspi_attrdb_chgpwd_offset = var_offset;

	// Extension DWORD offset
	uint16_t extension_dword_offset = var_offset;
	var_offset += 4;  // DWORD pointing to feature extensions

	// Feature extensions start here
	uint32_t feature_ext_offset = var_offset;

	// ADAL FEDAUTH extension is small: FeatureId(1) + FeatureDataLen(4) + Options(1) + Workflow(1)
	// Total: 1 + 4 + 2 = 7 bytes, plus terminator = 8 bytes
	uint32_t fedauth_data_len = 2;						  // Options (1 byte) + Workflow (1 byte)
	uint32_t fedauth_ext_len = 1 + 4 + fedauth_data_len;  // FeatureId + FeatureDataLen + FeatureData

	// Total length = fixed header + variable strings + extension DWORD + FEDAUTH extension + terminator
	uint32_t total_length = var_offset + fedauth_ext_len + 1;

	// Build fixed header (94 bytes)

	// Offset 0: Length (4 bytes, LE)
	packet.AppendUInt32LE(total_length);

	// Offset 4: TDSVersion (4 bytes, LE)
	packet.AppendUInt32LE(TDS_VERSION_7_4);

	// Offset 8: PacketSize (4 bytes, LE)
	packet.AppendUInt32LE(packet_size);

	// Offset 12: ClientProgVer (4 bytes, LE)
	packet.AppendUInt32LE(0x00000001);

	// Offset 16: ClientPID (4 bytes, LE)
	packet.AppendUInt32LE(static_cast<uint32_t>(GET_PID()));

	// Offset 20: ConnectionID (4 bytes, LE)
	packet.AppendUInt32LE(0);

	// Offset 24: OptionFlags1 (1 byte)
	// Per go-mssqldb: USE_DB (0x20) | SET_LANG (0x80) = 0xA0
	uint8_t flags1 = 0x20 | 0x80;  // USE_DB | SET_LANG
	packet.AppendByte(flags1);

	// Offset 25: OptionFlags2 (1 byte)
	// Per go-mssqldb: fODBC (0x02) only
	uint8_t flags2 = 0x02;	// fODBC
	packet.AppendByte(flags2);

	// Offset 26: TypeFlags (1 byte)
	uint8_t type_flags = 0x00;	// Read/write intent
	packet.AppendByte(type_flags);

	// Offset 27: OptionFlags3 (1 byte)
	// Per go-mssqldb debug output: OptionFlags3 = 0x00 in the login struct,
	// but fExtension (0x10) is added in sendLogin() when feature extensions are present.
	// The packet builder will set this correctly based on extension presence.
	// NOTE: go-mssqldb shows 0x00 in debug but actually sends 0x10 in the wire packet.
	uint8_t flags3 = 0x10;	// fExtension - indicates feature extension data is present
	packet.AppendByte(flags3);

	// Offset 28: ClientTimeZone (4 bytes, LE)
	packet.AppendUInt32LE(0);

	// Offset 32: ClientLCID (4 bytes, LE)
	packet.AppendUInt32LE(0x0409);	// en-US

	// Offset 36-93: Offset/Length pairs for variable fields

	// HostName
	packet.AppendUInt16LE(field_hostname.ib);
	packet.AppendUInt16LE(field_hostname.cch);

	// UserName (empty for FEDAUTH)
	packet.AppendUInt16LE(field_username.ib);
	packet.AppendUInt16LE(field_username.cch);

	// Password (empty for FEDAUTH)
	packet.AppendUInt16LE(field_password.ib);
	packet.AppendUInt16LE(field_password.cch);

	// AppName
	packet.AppendUInt16LE(field_appname.ib);
	packet.AppendUInt16LE(field_appname.cch);

	// ServerName
	packet.AppendUInt16LE(field_servername.ib);
	packet.AppendUInt16LE(field_servername.cch);

	// Extension
	packet.AppendUInt16LE(extension_dword_offset);
	packet.AppendUInt16LE(4);  // cbExtension = 4 (DWORD size)

	// CltIntName (unused)
	packet.AppendUInt16LE(cltintname_offset);
	packet.AppendUInt16LE(0);

	// Language (unused)
	packet.AppendUInt16LE(language_offset);
	packet.AppendUInt16LE(0);

	// Database
	packet.AppendUInt16LE(field_database.ib);
	packet.AppendUInt16LE(field_database.cch);

	// ClientID (6 bytes) - MAC address, zeros
	for (int i = 0; i < 6; i++) {
		packet.AppendByte(0);
	}

	// SSPI (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// AtchDBFile (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// ChangePassword (unused)
	packet.AppendUInt16LE(sspi_attrdb_chgpwd_offset);
	packet.AppendUInt16LE(0);

	// cbSSPILong (4 bytes)
	packet.AppendUInt32LE(0);

	// =====================================
	// Variable data section (UTF-16LE bytes via the helper)
	// =====================================
	packet.AppendPayload(field_hostname.utf16le_bytes);
	// UserName / Password empty.
	packet.AppendPayload(field_appname.utf16le_bytes);
	packet.AppendPayload(field_servername.utf16le_bytes);
	// CltIntName / Language empty.
	packet.AppendPayload(field_database.utf16le_bytes);
	// SSPI / AtchDBFile / ChangePassword empty.

	// ExtensionDWORD - offset to feature extensions
	packet.AppendUInt32LE(feature_ext_offset);

	// =====================================
	// Feature Extensions (ADAL FEDAUTH)
	// =====================================

	// FEDAUTH extension with ADAL workflow
	// FeatureId: 0x02
	packet.AppendByte(static_cast<uint8_t>(FeatureExtId::FEDAUTH));

	// FeatureDataLen (4 bytes, LE) = 2 (options + workflow)
	packet.AppendUInt32LE(fedauth_data_len);

	// FEDAUTH Feature Data for ADAL workflow (per go-mssqldb):
	//   Options (1 byte): (FedAuthLibrary << 1) | FedAuthEcho
	//     FedAuthLibraryADAL = 0x02
	//     So: (0x02 << 1) | echo = 0x04 or 0x05
	//   Workflow (1 byte): FedAuthADALWorkflowPassword = 0x01
	uint8_t fedauth_options = (FEDAUTH_LIBRARY_ADAL << 1) | (fedauth_echo ? 1 : 0);
	packet.AppendByte(fedauth_options);
	packet.AppendByte(FEDAUTH_ADAL_WORKFLOW_PASSWORD);	// Service Principal uses Password workflow

	// Feature terminator
	packet.AppendByte(static_cast<uint8_t>(FeatureExtId::TERMINATOR));

	return packet;
}

TdsPacket TdsProtocol::BuildFedAuthToken(const std::vector<uint8_t> &token_utf16le, const std::vector<uint8_t> &nonce) {
	TdsPacket packet(PacketType::FEDAUTH_TOKEN);

	// FEDAUTH_TOKEN packet format (per go-mssqldb sendFedAuthInfo):
	//   DataLen (4 bytes, LE): 4 + token_len + nonce_len
	//   TokenLen (4 bytes, LE): length of UTF-16LE token
	//   Token (variable): UTF-16LE encoded access token
	//   Nonce (optional): 32-byte nonce if present

	uint32_t token_len = static_cast<uint32_t>(token_utf16le.size());
	uint32_t nonce_len = static_cast<uint32_t>(nonce.size());
	uint32_t data_len = 4 + token_len + nonce_len;

	// DataLen
	packet.AppendUInt32LE(data_len);

	// TokenLen
	packet.AppendUInt32LE(token_len);

	// Token
	packet.AppendPayload(token_utf16le);

	// Nonce (if present)
	if (!nonce.empty()) {
		packet.AppendPayload(nonce);
	}

	return packet;
}

std::vector<TdsPacket> TdsProtocol::BuildFedAuthTokenMultiPacket(const std::vector<uint8_t> &token_utf16le,
																 size_t max_packet_size,
																 const std::vector<uint8_t> &nonce) {
	std::vector<TdsPacket> packets;

	// Build the full FEDAUTH_TOKEN payload first
	// Format: DataLen (4) + TokenLen (4) + Token + Nonce (optional)
	uint32_t token_len = static_cast<uint32_t>(token_utf16le.size());
	uint32_t nonce_len = static_cast<uint32_t>(nonce.size());
	uint32_t data_len = 4 + token_len + nonce_len;

	std::vector<uint8_t> payload;
	payload.reserve(8 + token_utf16le.size() + nonce.size());

	// DataLen (4 bytes, LE)
	payload.push_back(data_len & 0xFF);
	payload.push_back((data_len >> 8) & 0xFF);
	payload.push_back((data_len >> 16) & 0xFF);
	payload.push_back((data_len >> 24) & 0xFF);

	// TokenLen (4 bytes, LE)
	payload.push_back(token_len & 0xFF);
	payload.push_back((token_len >> 8) & 0xFF);
	payload.push_back((token_len >> 16) & 0xFF);
	payload.push_back((token_len >> 24) & 0xFF);

	// Token
	payload.insert(payload.end(), token_utf16le.begin(), token_utf16le.end());

	// Nonce (if present)
	if (!nonce.empty()) {
		payload.insert(payload.end(), nonce.begin(), nonce.end());
	}

	// TDS packet header is 8 bytes, so max payload per packet is (max_packet_size - 8)
	size_t max_payload = max_packet_size - TDS_HEADER_SIZE;

	// If it fits in a single packet, use single packet
	if (payload.size() <= max_payload) {
		TdsPacket packet(PacketType::FEDAUTH_TOKEN);
		packet.AppendPayload(payload);
		packets.push_back(std::move(packet));
		return packets;
	}

	// Split into multiple packets
	size_t offset = 0;
	while (offset < payload.size()) {
		size_t chunk_size = std::min(max_payload, payload.size() - offset);
		bool is_last = (offset + chunk_size >= payload.size());

		TdsPacket packet(PacketType::FEDAUTH_TOKEN);

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

}  // namespace tds
}  // namespace duckdb

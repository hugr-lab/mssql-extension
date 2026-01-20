// Debug program for testing multi-packet TDS messages
// Build: g++ -std=c++17 -I../src/include -o debug_multipacket debug_multipacket.cpp
//        ../build/release/extension/mssql/libmssql_extension.a
// Or run via make debug_multipacket

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// TDS constants
constexpr size_t TDS_HEADER_SIZE = 8;
constexpr size_t TDS_DEFAULT_PACKET_SIZE = 4096;
constexpr size_t TDS_MAX_PACKET_SIZE = 32767;

// TDS packet types
constexpr uint8_t TDS_TYPE_PRELOGIN = 0x12;
constexpr uint8_t TDS_TYPE_LOGIN7 = 0x10;
constexpr uint8_t TDS_TYPE_SQL_BATCH = 0x01;
constexpr uint8_t TDS_TYPE_TABULAR_RESULT = 0x04;

// Status flags
constexpr uint8_t TDS_STATUS_EOM = 0x01;
constexpr uint8_t TDS_STATUS_NORMAL = 0x00;

// Token types
constexpr uint8_t TDS_TOKEN_DONE = 0xFD;
constexpr uint8_t TDS_TOKEN_DONEPROC = 0xFE;
constexpr uint8_t TDS_TOKEN_DONEINPROC = 0xFF;
constexpr uint8_t TDS_TOKEN_ERROR = 0xAA;
constexpr uint8_t TDS_TOKEN_LOGINACK = 0xAD;
constexpr uint8_t TDS_TOKEN_ENVCHANGE = 0xE3;
constexpr uint8_t TDS_TOKEN_COLMETADATA = 0x81;
constexpr uint8_t TDS_TOKEN_ROW = 0xD1;

void hexdump(const uint8_t *data, size_t len, const char *prefix = "", size_t max_len = 100) {
	printf("%s", prefix);
	for (size_t i = 0; i < len && i < max_len; i++) {
		printf("%02x ", data[i]);
		if ((i + 1) % 16 == 0)
			printf("\n%s", prefix);
	}
	if (len > max_len)
		printf("... (%zu more bytes)", len - max_len);
	printf("\n");
}

std::vector<uint8_t> utf16le_encode(const std::string &str) {
	std::vector<uint8_t> result;
	for (char c : str) {
		result.push_back(static_cast<uint8_t>(c));
		result.push_back(0);
	}
	return result;
}

std::vector<uint8_t> build_tds_header(uint8_t type, uint8_t status, uint16_t length, uint8_t pkt_id) {
	std::vector<uint8_t> header(8);
	header[0] = type;
	header[1] = status;
	header[2] = (length >> 8) & 0xFF;  // Big-endian
	header[3] = length & 0xFF;
	header[4] = 0;	// SPID high
	header[5] = 0;	// SPID low
	header[6] = pkt_id;
	header[7] = 0;	// Window
	return header;
}

std::vector<uint8_t> build_prelogin(bool encrypt = false) {
	// PRELOGIN packet
	std::vector<uint8_t> payload;

	// Options: VERSION, ENCRYPTION
	// Option table: 5 bytes per option (token + offset + length) + 1 byte terminator
	// Total option table = 5 + 5 + 1 = 11 bytes
	uint16_t data_offset = 11;	// Data starts after option table

	// VERSION option (offset to VERSION data, length=6)
	payload.push_back(0x00);  // Token
	payload.push_back(data_offset >> 8);
	payload.push_back(data_offset & 0xFF);
	payload.push_back(0);  // Length high
	payload.push_back(6);  // Length low

	// ENCRYPTION option (offset to ENCRYPTION data, length=1)
	uint16_t enc_offset = data_offset + 6;	// After VERSION data
	payload.push_back(0x01);				// Token
	payload.push_back(enc_offset >> 8);
	payload.push_back(enc_offset & 0xFF);
	payload.push_back(0);  // Length high
	payload.push_back(1);  // Length low

	// Terminator
	payload.push_back(0xFF);

	// VERSION data: 0x07, 0x00, 0x00, 0x00, 0x00, 0x00
	payload.push_back(0x07);  // TDS 7.x
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);
	payload.push_back(0x00);

	// ENCRYPTION data: ENCRYPT_NOT_SUP (0x02) or ENCRYPT_ON (0x01)
	payload.push_back(encrypt ? 0x01 : 0x02);

	// Build packet (packet_id will be set by caller)
	std::vector<uint8_t> packet;
	auto header = build_tds_header(TDS_TYPE_PRELOGIN, TDS_STATUS_EOM, TDS_HEADER_SIZE + payload.size(),
								   0);	// Default, caller should set
	packet.insert(packet.end(), header.begin(), header.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

void set_packet_id(std::vector<uint8_t> &packet, uint8_t pkt_id) {
	if (packet.size() >= 7) {
		packet[6] = pkt_id;
	}
}

std::vector<uint8_t> build_login7(const std::string &host, const std::string &user, const std::string &pass,
								  const std::string &db, uint32_t packet_size = 4096) {
	// LOGIN7 packet - corrected layout matching TDS spec
	std::vector<uint8_t> payload;

	// Prepare variable-length strings (UTF-16LE)
	std::vector<uint8_t> host_utf16 = utf16le_encode(host);
	std::vector<uint8_t> user_utf16 = utf16le_encode(user);
	std::vector<uint8_t> pass_utf16 = utf16le_encode(pass);
	std::vector<uint8_t> db_utf16 = utf16le_encode(db);
	std::vector<uint8_t> app_utf16 = utf16le_encode("DuckDB MSSQL Extension");

	// Password encoding: swap nibbles, XOR 0xA5
	for (auto &b : pass_utf16) {
		b = ((b << 4) & 0xF0) | ((b >> 4) & 0x0F);
		b ^= 0xA5;
	}

	// Calculate offsets (variable data starts at byte 94)
	uint16_t var_offset = 94;

	uint16_t hostname_offset = var_offset;
	uint16_t hostname_len = host.length();
	var_offset += host_utf16.size();

	uint16_t username_offset = var_offset;
	uint16_t username_len = user.length();
	var_offset += user_utf16.size();

	uint16_t password_offset = var_offset;
	uint16_t password_len = pass.length();
	var_offset += pass_utf16.size();

	uint16_t appname_offset = var_offset;
	uint16_t appname_len = 22;	// "DuckDB MSSQL Extension" (22 chars)
	var_offset += app_utf16.size();

	uint16_t servername_offset = var_offset;
	uint16_t servername_len = host.length();
	var_offset += host_utf16.size();

	// Unused fields point to current offset with length 0
	uint16_t unused_offset = var_offset;
	uint16_t cltintname_offset = var_offset;
	uint16_t language_offset = var_offset;

	uint16_t database_offset = var_offset;
	uint16_t database_len = db.length();
	var_offset += db_utf16.size();

	// Total LOGIN7 payload length
	uint32_t total_length = var_offset;

	// Build fixed header (94 bytes)
	payload.resize(94, 0);

	// Offset 0-3: Length (total LOGIN7 payload length, LE)
	payload[0] = total_length & 0xFF;
	payload[1] = (total_length >> 8) & 0xFF;
	payload[2] = (total_length >> 16) & 0xFF;
	payload[3] = (total_length >> 24) & 0xFF;

	// Offset 4-7: TDS Version: 7.4 (0x74000004, LE)
	payload[4] = 0x04;
	payload[5] = 0x00;
	payload[6] = 0x00;
	payload[7] = 0x74;

	// Offset 8-11: Packet size (LE)
	payload[8] = packet_size & 0xFF;
	payload[9] = (packet_size >> 8) & 0xFF;
	payload[10] = (packet_size >> 16) & 0xFF;
	payload[11] = (packet_size >> 24) & 0xFF;

	// Offset 12-15: Client version (LE)
	payload[12] = 0x01;
	payload[13] = 0x00;
	payload[14] = 0x00;
	payload[15] = 0x00;

	// Offset 16-19: Client PID (LE)
	uint32_t pid = static_cast<uint32_t>(getpid());
	payload[16] = pid & 0xFF;
	payload[17] = (pid >> 8) & 0xFF;
	payload[18] = (pid >> 16) & 0xFF;
	payload[19] = (pid >> 24) & 0xFF;

	// Offset 20-23: Connection ID (0 for new connection)
	// Already zeros

	// Offset 24: OptionFlags1 (USE_DB = 0x20)
	payload[24] = 0x20;

	// Offset 25: OptionFlags2
	payload[25] = 0x00;

	// Offset 26: TypeFlags
	payload[26] = 0x00;

	// Offset 27: OptionFlags3
	payload[27] = 0x00;

	// Offset 28-31: ClientTimeZone (0)
	// Already zeros

	// Offset 32-35: ClientLCID (0x0409 = en-US)
	payload[32] = 0x09;
	payload[33] = 0x04;
	payload[34] = 0x00;
	payload[35] = 0x00;

	// Offset 36-93: Offset/Length pairs for variable fields

	// HostName (36-39)
	payload[36] = hostname_offset & 0xFF;
	payload[37] = (hostname_offset >> 8) & 0xFF;
	payload[38] = hostname_len & 0xFF;
	payload[39] = (hostname_len >> 8) & 0xFF;

	// UserName (40-43)
	payload[40] = username_offset & 0xFF;
	payload[41] = (username_offset >> 8) & 0xFF;
	payload[42] = username_len & 0xFF;
	payload[43] = (username_len >> 8) & 0xFF;

	// Password (44-47)
	payload[44] = password_offset & 0xFF;
	payload[45] = (password_offset >> 8) & 0xFF;
	payload[46] = password_len & 0xFF;
	payload[47] = (password_len >> 8) & 0xFF;

	// AppName (48-51)
	payload[48] = appname_offset & 0xFF;
	payload[49] = (appname_offset >> 8) & 0xFF;
	payload[50] = appname_len & 0xFF;
	payload[51] = (appname_len >> 8) & 0xFF;

	// ServerName (52-55)
	payload[52] = servername_offset & 0xFF;
	payload[53] = (servername_offset >> 8) & 0xFF;
	payload[54] = servername_len & 0xFF;
	payload[55] = (servername_len >> 8) & 0xFF;

	// Unused/Extension (56-59) - length 0
	payload[56] = unused_offset & 0xFF;
	payload[57] = (unused_offset >> 8) & 0xFF;
	payload[58] = 0;
	payload[59] = 0;

	// CltIntName (60-63) - length 0
	payload[60] = cltintname_offset & 0xFF;
	payload[61] = (cltintname_offset >> 8) & 0xFF;
	payload[62] = 0;
	payload[63] = 0;

	// Language (64-67) - length 0
	payload[64] = language_offset & 0xFF;
	payload[65] = (language_offset >> 8) & 0xFF;
	payload[66] = 0;
	payload[67] = 0;

	// Database (68-71)
	payload[68] = database_offset & 0xFF;
	payload[69] = (database_offset >> 8) & 0xFF;
	payload[70] = database_len & 0xFF;
	payload[71] = (database_len >> 8) & 0xFF;

	// ClientID (72-77) - MAC address, 6 bytes of zeros
	// Already zeros

	// SSPI (78-81) - length 0
	payload[78] = var_offset & 0xFF;
	payload[79] = (var_offset >> 8) & 0xFF;
	payload[80] = 0;
	payload[81] = 0;

	// AtchDBFile (82-85) - length 0
	payload[82] = var_offset & 0xFF;
	payload[83] = (var_offset >> 8) & 0xFF;
	payload[84] = 0;
	payload[85] = 0;

	// ChangePassword (86-89) - length 0
	payload[86] = var_offset & 0xFF;
	payload[87] = (var_offset >> 8) & 0xFF;
	payload[88] = 0;
	payload[89] = 0;

	// cbSSPILong (90-93) - 4 bytes, 0
	// Already zeros

	// Append variable data in order
	payload.insert(payload.end(), host_utf16.begin(), host_utf16.end());
	payload.insert(payload.end(), user_utf16.begin(), user_utf16.end());
	payload.insert(payload.end(), pass_utf16.begin(), pass_utf16.end());
	payload.insert(payload.end(), app_utf16.begin(), app_utf16.end());
	payload.insert(payload.end(), host_utf16.begin(), host_utf16.end());  // ServerName
	payload.insert(payload.end(), db_utf16.begin(), db_utf16.end());

	// Build TDS packet (packet_id will be set by caller)
	std::vector<uint8_t> packet;
	auto header = build_tds_header(TDS_TYPE_LOGIN7, TDS_STATUS_EOM, TDS_HEADER_SIZE + payload.size(), 0);
	packet.insert(packet.end(), header.begin(), header.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

std::vector<uint8_t> build_all_headers() {
	// ALL_HEADERS section (22 bytes)
	std::vector<uint8_t> headers;

	// TotalLength = 22 (little-endian)
	headers.push_back(22);
	headers.push_back(0);
	headers.push_back(0);
	headers.push_back(0);

	// Transaction Descriptor Header
	// HeaderLength = 18
	headers.push_back(18);
	headers.push_back(0);
	headers.push_back(0);
	headers.push_back(0);

	// HeaderType = 0x0002
	headers.push_back(0x02);
	headers.push_back(0x00);

	// TransactionDescriptor = 0 (8 bytes)
	for (int i = 0; i < 8; i++)
		headers.push_back(0);

	// OutstandingRequestCount = 1
	headers.push_back(1);
	headers.push_back(0);
	headers.push_back(0);
	headers.push_back(0);

	return headers;
}

std::vector<std::vector<uint8_t>> build_sql_batch_packets(const std::string &sql, size_t max_packet_size,
														  uint8_t &start_pkt_id, bool include_all_headers = true) {
	std::vector<std::vector<uint8_t>> packets;

	// Build payload: optionally ALL_HEADERS + SQL(UTF-16LE)
	std::vector<uint8_t> payload;
	auto sql_utf16 = utf16le_encode(sql);

	if (include_all_headers) {
		auto headers = build_all_headers();
		payload.insert(payload.end(), headers.begin(), headers.end());
	}
	payload.insert(payload.end(), sql_utf16.begin(), sql_utf16.end());

	size_t max_payload = max_packet_size - TDS_HEADER_SIZE;

	// Split into packets
	size_t offset = 0;
	while (offset < payload.size()) {
		size_t chunk_size = std::min(max_payload, payload.size() - offset);
		bool is_last = (offset + chunk_size >= payload.size());

		std::vector<uint8_t> packet;
		uint16_t pkt_len = TDS_HEADER_SIZE + chunk_size;
		uint8_t status = is_last ? TDS_STATUS_EOM : TDS_STATUS_NORMAL;

		auto header = build_tds_header(TDS_TYPE_SQL_BATCH, status, pkt_len, start_pkt_id++);
		packet.insert(packet.end(), header.begin(), header.end());
		packet.insert(packet.end(), payload.begin() + offset, payload.begin() + offset + chunk_size);

		packets.push_back(std::move(packet));
		offset += chunk_size;
	}

	return packets;
}

bool send_all(int fd, const uint8_t *data, size_t len) {
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, data + sent, len - sent, 0);
		if (n <= 0)
			return false;
		sent += n;
	}
	return true;
}

std::vector<uint8_t> receive_response(int fd, int timeout_ms = 10000) {
	std::vector<uint8_t> response;
	uint8_t buffer[8192];

	while (true) {
		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int ret = poll(&pfd, 1, timeout_ms);
		if (ret <= 0) {
			printf("Poll timeout or error\n");
			break;
		}

		if (pfd.revents & POLLIN) {
			ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
			if (n <= 0) {
				if (n == 0)
					printf("Connection closed\n");
				else
					printf("Recv error: %s\n", strerror(errno));
				break;
			}
			response.insert(response.end(), buffer, buffer + n);

			// Check if we have a complete packet with EOM
			if (response.size() >= 2 && (response[1] & TDS_STATUS_EOM)) {
				// Check if we have the full packet
				if (response.size() >= 4) {
					uint16_t pkt_len = (response[2] << 8) | response[3];
					if (response.size() >= pkt_len) {
						break;	// Got complete response
					}
				}
			}
		}

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			printf("Poll error: POLLERR=%d POLLHUP=%d POLLNVAL=%d\n", !!(pfd.revents & POLLERR),
				   !!(pfd.revents & POLLHUP), !!(pfd.revents & POLLNVAL));
			break;
		}

		timeout_ms = 1000;	// Shorter timeout for subsequent reads
	}

	return response;
}

int main(int argc, char *argv[]) {
	const char *host = "127.0.0.1";
	int port = 1433;
	const char *user = "sa";
	const char *pass = "TestPassword1";
	const char *db = "TestDB";
	int sql_length = 3000;	// Length of SQL to generate

	if (argc > 1)
		sql_length = atoi(argv[1]);

	printf("=== TDS Multi-Packet Debug Tool ===\n");
	printf("Target: %s:%d\n", host, port);
	printf("SQL length: %d characters\n", sql_length);
	printf("\n");

	// Connect
	printf("[1] Connecting to server...\n");
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host, &addr.sin_addr);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}

	// Set TCP_NODELAY like the working implementation
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

	printf("    Connected!\n\n");

	// Track packet IDs like the working implementation
	uint8_t next_packet_id = 0;

	// PRELOGIN
	printf("[2] Sending PRELOGIN...\n");
	auto prelogin = build_prelogin(false);
	set_packet_id(prelogin, next_packet_id++);
	printf("    Packet (%zu bytes, pkt_id=%u):\n", prelogin.size(), prelogin[6]);
	hexdump(prelogin.data(), prelogin.size(), "    ");

	if (!send_all(fd, prelogin.data(), prelogin.size())) {
		printf("    Failed to send PRELOGIN\n");
		return 1;
	}

	auto response = receive_response(fd);
	printf("    Response (%zu bytes):\n", response.size());
	hexdump(response.data(), response.size(), "    ");

	if (response.empty()) {
		printf("    No response received!\n");
		return 1;
	}
	printf("    PRELOGIN OK\n\n");

	// LOGIN7
	printf("[3] Sending LOGIN7...\n");
	auto login7 = build_login7(host, user, pass, db, TDS_MAX_PACKET_SIZE);
	set_packet_id(login7, next_packet_id++);
	printf("    Packet (%zu bytes, pkt_id=%u):\n", login7.size(), login7[6]);
	hexdump(login7.data(), login7.size(), "    ", 300);	 // Show all bytes

	if (!send_all(fd, login7.data(), login7.size())) {
		printf("    Failed to send LOGIN7\n");
		return 1;
	}

	response = receive_response(fd);
	printf("    Response (%zu bytes):\n", response.size());
	hexdump(response.data(), response.size(), "    ", 300);	 // Show more bytes

	// Look for packet size ENVCHANGE (type 4)
	uint32_t negotiated_packet_size = TDS_DEFAULT_PACKET_SIZE;	// Default
	for (size_t i = 8; i < response.size() - 5; i++) {
		if (response[i] == 0xE3) {	// ENVCHANGE
			uint16_t len = response[i + 1] | (response[i + 2] << 8);
			uint8_t env_type = response[i + 3];
			if (env_type == 4 && len >= 3) {  // Packet size
				uint8_t new_len = response[i + 4];
				if (new_len > 0 && i + 5 + new_len * 2 <= response.size()) {
					std::string packet_size_str;
					for (uint8_t j = 0; j < new_len; j++) {
						packet_size_str += static_cast<char>(response[i + 5 + j * 2]);
					}
					negotiated_packet_size = std::stoul(packet_size_str);
					printf("    Negotiated packet size: %u\n", negotiated_packet_size);
				}
			}
		}
	}

	// Check for LOGINACK
	bool login_ok = false;
	for (size_t i = 8; i < response.size(); i++) {
		if (response[i] == TDS_TOKEN_LOGINACK) {
			login_ok = true;
			break;
		}
		if (response[i] == TDS_TOKEN_ERROR) {
			printf("    LOGIN ERROR detected at offset %zu\n", i);
			break;
		}
	}

	if (!login_ok) {
		printf("    LOGIN7 failed!\n");
		return 1;
	}
	printf("    LOGIN7 OK\n\n");

	// Single-packet SQL Batch test
	printf("[4] Testing SINGLE-PACKET SQL Batch...\n");
	std::string simple_sql = "SELECT 1";
	uint8_t pkt_id = 1;
	auto packets = build_sql_batch_packets(simple_sql, negotiated_packet_size, pkt_id);

	printf("    SQL: %s\n", simple_sql.c_str());
	printf("    Packet count: %zu\n", packets.size());
	printf("    Packet (%zu bytes):\n", packets[0].size());
	hexdump(packets[0].data(), packets[0].size(), "    ");

	if (!send_all(fd, packets[0].data(), packets[0].size())) {
		printf("    Failed to send SQL\n");
		return 1;
	}

	response = receive_response(fd);
	printf("    Response (%zu bytes):\n", response.size());
	hexdump(response.data(), response.size(), "    ");

	if (response.empty()) {
		printf("    Single-packet SQL FAILED!\n");
		return 1;
	}
	printf("    Single-packet SQL OK\n\n");

	// Multi-packet SQL Batch test
	printf("[5] Testing MULTI-PACKET SQL Batch (with ALL_HEADERS)...\n");
	std::string comment(sql_length, 'x');
	std::string multi_sql = "SELECT 1 /* " + comment + " */";

	// Reset packet ID for new message - each message starts fresh
	pkt_id = 1;
	packets = build_sql_batch_packets(multi_sql, negotiated_packet_size, pkt_id, true);

	printf("    SQL length: %zu bytes (%zu UTF-16LE + 22 headers = %zu payload)\n", multi_sql.length(),
		   multi_sql.length() * 2, multi_sql.length() * 2 + 22);
	printf("    Packet count: %zu\n", packets.size());

	for (size_t i = 0; i < packets.size(); i++) {
		printf("    Packet %zu/%zu (%zu bytes): type=0x%02x status=0x%02x length=%u pkt_id=%u\n", i + 1, packets.size(),
			   packets[i].size(), packets[i][0], packets[i][1], (packets[i][2] << 8) | packets[i][3], packets[i][6]);
		if (i == 0) {
			printf("    First packet header + first 50 bytes:\n");
			hexdump(packets[i].data(), std::min<size_t>(50, packets[i].size()), "      ");
		}
	}

	// Try sending packets individually
	printf("\n    Sending %zu packets individually...\n", packets.size());
	for (size_t i = 0; i < packets.size(); i++) {
		printf("    Sending packet %zu/%zu (%zu bytes)...\n", i + 1, packets.size(), packets[i].size());
		if (!send_all(fd, packets[i].data(), packets[i].size())) {
			printf("    Failed to send packet %zu\n", i + 1);
			return 1;
		}
	}
	printf("    All packets sent, waiting for response...\n");

	response = receive_response(fd, 5000);
	printf("    Response (%zu bytes):\n", response.size());
	if (!response.empty()) {
		hexdump(response.data(), response.size(), "    ");
	}

	if (response.empty()) {
		printf("\n    *** MULTI-PACKET SQL (with ALL_HEADERS) FAILED - No response! ***\n");
		printf("    Server likely closed the connection.\n");

		// Reconnect and try without ALL_HEADERS
		printf("\n[6] Reconnecting for test without ALL_HEADERS...\n");
		close(fd);

		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			int flag = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

			// Quick PRELOGIN + LOGIN7
			prelogin = build_prelogin(false);
			set_packet_id(prelogin, 0);
			send_all(fd, prelogin.data(), prelogin.size());
			receive_response(fd);

			login7 = build_login7(host, user, pass, db, TDS_MAX_PACKET_SIZE);
			set_packet_id(login7, 1);
			send_all(fd, login7.data(), login7.size());
			response = receive_response(fd);

			if (!response.empty()) {
				printf("    Reconnected OK\n");

				printf("\n[7] Testing MULTI-PACKET SQL Batch (WITHOUT ALL_HEADERS)...\n");
				pkt_id = 1;
				packets = build_sql_batch_packets(multi_sql, TDS_DEFAULT_PACKET_SIZE, pkt_id, false);

				printf("    SQL length: %zu bytes (%zu UTF-16LE, no headers)\n", multi_sql.length(),
					   multi_sql.length() * 2);
				printf("    Packet count: %zu\n", packets.size());

				for (size_t i = 0; i < packets.size(); i++) {
					printf("    Sending packet %zu/%zu (%zu bytes)...\n", i + 1, packets.size(), packets[i].size());
					if (!send_all(fd, packets[i].data(), packets[i].size())) {
						printf("    Failed to send packet %zu\n", i + 1);
						break;
					}
				}
				printf("    Waiting for response...\n");

				response = receive_response(fd, 5000);
				printf("    Response (%zu bytes):\n", response.size());
				if (!response.empty()) {
					hexdump(response.data(), response.size(), "    ");
					printf("    Multi-packet SQL (no ALL_HEADERS) OK!\n");
				} else {
					printf("\n    *** MULTI-PACKET SQL (no ALL_HEADERS) also FAILED! ***\n");
				}
			}
		}
	} else {
		printf("    Multi-packet SQL OK!\n");
	}

	printf("\n[8] Cleanup...\n");
	close(fd);
	printf("    Done.\n");

	return response.empty() ? 1 : 0;
}

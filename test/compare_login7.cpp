// Quick comparison of LOGIN7 packets
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

void hexdump(const char *label, const uint8_t *data, size_t len) {
	std::cout << label << " (" << len << " bytes):\n";
	for (size_t i = 0; i < len; i++) {
		std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
		if ((i + 1) % 16 == 0)
			std::cout << "\n";
	}
	std::cout << std::dec << "\n\n";
}

std::vector<uint8_t> utf16le_encode(const std::string &str) {
	std::vector<uint8_t> result;
	for (char c : str) {
		result.push_back(static_cast<uint8_t>(c));
		result.push_back(0);
	}
	return result;
}

// My debug version
std::vector<uint8_t> debug_build_login7(const std::string &host, const std::string &user, const std::string &pass,
										const std::string &db, uint32_t packet_size = 4096) {
	std::vector<uint8_t> payload;

	std::vector<uint8_t> host_utf16 = utf16le_encode(host);
	std::vector<uint8_t> user_utf16 = utf16le_encode(user);
	std::vector<uint8_t> pass_utf16 = utf16le_encode(pass);
	std::vector<uint8_t> db_utf16 = utf16le_encode(db);
	std::vector<uint8_t> app_utf16 = utf16le_encode("Debug");

	// Password encoding
	for (auto &b : pass_utf16) {
		b = ((b << 4) & 0xF0) | ((b >> 4) & 0x0F);
		b ^= 0xA5;
	}

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
	uint16_t appname_len = 5;
	var_offset += app_utf16.size();

	uint16_t servername_offset = var_offset;
	uint16_t servername_len = host.length();
	var_offset += host_utf16.size();

	uint16_t unused_offset = var_offset;
	uint16_t cltintname_offset = var_offset;
	uint16_t language_offset = var_offset;

	uint16_t database_offset = var_offset;
	uint16_t database_len = db.length();
	var_offset += db_utf16.size();

	uint32_t total_length = var_offset;

	payload.resize(94, 0);

	// Length
	payload[0] = total_length & 0xFF;
	payload[1] = (total_length >> 8) & 0xFF;
	payload[2] = (total_length >> 16) & 0xFF;
	payload[3] = (total_length >> 24) & 0xFF;

	// TDS Version 7.4
	payload[4] = 0x04;
	payload[5] = 0x00;
	payload[6] = 0x00;
	payload[7] = 0x74;

	// Packet size
	payload[8] = packet_size & 0xFF;
	payload[9] = (packet_size >> 8) & 0xFF;
	payload[10] = (packet_size >> 16) & 0xFF;
	payload[11] = (packet_size >> 24) & 0xFF;

	// Client version
	payload[12] = 0x01;
	payload[13] = 0x00;
	payload[14] = 0x00;
	payload[15] = 0x00;

	// Client PID
	payload[16] = 0x01;
	payload[17] = 0x00;
	payload[18] = 0x00;
	payload[19] = 0x00;

	payload[24] = 0x20;	 // OptionFlags1
	payload[32] = 0x09;	 // LCID
	payload[33] = 0x04;

	// HostName
	payload[36] = hostname_offset & 0xFF;
	payload[37] = (hostname_offset >> 8) & 0xFF;
	payload[38] = hostname_len & 0xFF;
	payload[39] = (hostname_len >> 8) & 0xFF;

	// UserName
	payload[40] = username_offset & 0xFF;
	payload[41] = (username_offset >> 8) & 0xFF;
	payload[42] = username_len & 0xFF;
	payload[43] = (username_len >> 8) & 0xFF;

	// Password
	payload[44] = password_offset & 0xFF;
	payload[45] = (password_offset >> 8) & 0xFF;
	payload[46] = password_len & 0xFF;
	payload[47] = (password_len >> 8) & 0xFF;

	// AppName
	payload[48] = appname_offset & 0xFF;
	payload[49] = (appname_offset >> 8) & 0xFF;
	payload[50] = appname_len & 0xFF;
	payload[51] = (appname_len >> 8) & 0xFF;

	// ServerName
	payload[52] = servername_offset & 0xFF;
	payload[53] = (servername_offset >> 8) & 0xFF;
	payload[54] = servername_len & 0xFF;
	payload[55] = (servername_len >> 8) & 0xFF;

	// Unused
	payload[56] = unused_offset & 0xFF;
	payload[57] = (unused_offset >> 8) & 0xFF;
	payload[58] = 0;
	payload[59] = 0;

	// CltIntName
	payload[60] = cltintname_offset & 0xFF;
	payload[61] = (cltintname_offset >> 8) & 0xFF;
	payload[62] = 0;
	payload[63] = 0;

	// Language
	payload[64] = language_offset & 0xFF;
	payload[65] = (language_offset >> 8) & 0xFF;
	payload[66] = 0;
	payload[67] = 0;

	// Database
	payload[68] = database_offset & 0xFF;
	payload[69] = (database_offset >> 8) & 0xFF;
	payload[70] = database_len & 0xFF;
	payload[71] = (database_len >> 8) & 0xFF;

	// SSPI
	payload[78] = var_offset & 0xFF;
	payload[79] = (var_offset >> 8) & 0xFF;
	payload[80] = 0;
	payload[81] = 0;

	// AtchDBFile
	payload[82] = var_offset & 0xFF;
	payload[83] = (var_offset >> 8) & 0xFF;
	payload[84] = 0;
	payload[85] = 0;

	// ChangePassword
	payload[86] = var_offset & 0xFF;
	payload[87] = (var_offset >> 8) & 0xFF;
	payload[88] = 0;
	payload[89] = 0;

	payload.insert(payload.end(), host_utf16.begin(), host_utf16.end());
	payload.insert(payload.end(), user_utf16.begin(), user_utf16.end());
	payload.insert(payload.end(), pass_utf16.begin(), pass_utf16.end());
	payload.insert(payload.end(), app_utf16.begin(), app_utf16.end());
	payload.insert(payload.end(), host_utf16.begin(), host_utf16.end());
	payload.insert(payload.end(), db_utf16.begin(), db_utf16.end());

	return payload;
}

int main() {
	auto payload = debug_build_login7("127.0.0.1", "sa", "TestPassword1", "TestDB");

	std::cout << "LOGIN7 Payload Analysis:\n\n";

	std::cout << "Fixed header (first 36 bytes):\n";
	hexdump("", payload.data(), 36);

	std::cout << "Offset/Length pairs (bytes 36-93):\n";
	for (int i = 36; i < 90; i += 4) {
		uint16_t offset = payload[i] | (payload[i + 1] << 8);
		uint16_t len = payload[i + 2] | (payload[i + 3] << 8);
		const char *names[] = {"HostName", "UserName",	 "Password",	  "AppName",  "ServerName",
							   "Unused",   "CltIntName", "Language",	  "Database", "ClientID(6)",
							   "SSPI",	   "AtchDBFile", "ChangePassword"};
		int idx = (i - 36) / 4;
		if (idx == 9) {
			std::cout << "  ClientID: ";
			for (int j = 0; j < 6; j++)
				std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)payload[72 + j] << " ";
			std::cout << "\n";
			i += 2;	 // ClientID is 6 bytes, not 4
			continue;
		}
		std::cout << "  " << names[idx] << ": offset=" << std::dec << offset << ", len=" << len << "\n";
	}

	std::cout << "\nVariable data (starting at byte 94):\n";
	hexdump("", payload.data() + 94, payload.size() - 94);

	std::cout << "Total payload size: " << payload.size() << " bytes\n";
	uint32_t stored_len = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
	std::cout << "Stored length field: " << stored_len << " bytes\n";

	return 0;
}

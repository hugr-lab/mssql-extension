// test/cpp/test_login_error_state.cpp
// Unit tests for LOGIN7 ERROR-token State capture (issue #164).
//
// These tests do NOT require a running SQL Server instance -- they drive
// TdsProtocol::ParseLoginResponse() with hand-built token streams.
//
// Background (issue #164): SQL Server rejects a LOGIN7 with error 18456
// "Login failed for user 'x'" for many DISTINCT reasons -- bad password, an
// inaccessible default/initial database, a disabled login, etc. The only field
// that disambiguates them is the ERROR token's State byte (MS-TDS §2.2.7.10).
// The parser used to read that byte and immediately discard it, so every 18456
// collapsed to "check username and password" even when the password was fine
// (the reporter could authenticate against a different DB in the same instance).
// These tests pin the State byte into LoginResponse::error_state.
//
// Compile manually (macOS/brew example):
//   c++ -std=c++17 -Isrc/include \
//       -I/opt/homebrew/opt/simdutf/include \
//       test/cpp/test_login_error_state.cpp \
//       src/tds/tds_packet.cpp \
//       src/tds/tds_protocol.cpp \
//       src/tds/encoding/utf16.cpp \
//       -L/opt/homebrew/opt/simdutf/lib -lsimdutf \
//       -o build/test/test_login_error_state
//
// Run:
//   ./build/test/test_login_error_state

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tds/tds_protocol.hpp"
#include "tds/tds_types.hpp"

using namespace duckdb;
using namespace duckdb::tds;

namespace {

int g_checks = 0;

#define CHECK(cond)                                                                                             \
	do {                                                                                                        \
		g_checks++;                                                                                             \
		if (!(cond)) {                                                                                          \
			std::cerr << "ASSERT FAILED: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
			std::exit(1);                                                                                       \
		}                                                                                                       \
	} while (0)

void AppendU16LE(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFF));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32LE(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFF));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
	buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
	buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// Encode an ASCII string as UTF-16LE (matches what SQL Server sends for the
// ASCII-range characters used in these fixtures, and what ReadUTF16LE decodes).
void AppendUtf16LE(std::vector<uint8_t> &buf, const std::string &s) {
	for (char c : s) {
		buf.push_back(static_cast<uint8_t>(c));
		buf.push_back(0x00);
	}
}

// Build a complete ERROR token (0xAA) per MS-TDS §2.2.7.10 (TDS 7.2+ layout,
// 4-byte line number). Returns the token bytes including the 0xAA type byte and
// the 2-byte length field.
std::vector<uint8_t> BuildErrorToken(uint32_t number, uint8_t state, uint8_t error_class, const std::string &message,
									 const std::string &server_name = "SQLTEST", const std::string &proc_name = "",
									 uint32_t line_number = 1) {
	// Token body (everything the 2-byte Length field covers).
	std::vector<uint8_t> body;
	AppendU32LE(body, number);								   // Number
	body.push_back(state);									   // State
	body.push_back(error_class);							   // Class (severity)
	AppendU16LE(body, static_cast<uint16_t>(message.size()));  // MsgText length (chars)
	AppendUtf16LE(body, message);							   // MsgText
	body.push_back(static_cast<uint8_t>(server_name.size()));  // ServerName length (chars)
	AppendUtf16LE(body, server_name);						   // ServerName
	body.push_back(static_cast<uint8_t>(proc_name.size()));	   // ProcName length (chars)
	AppendUtf16LE(body, proc_name);							   // ProcName
	AppendU32LE(body, line_number);							   // LineNumber (4 bytes, TDS 7.2+)

	std::vector<uint8_t> token;
	token.push_back(static_cast<uint8_t>(TokenType::ERROR_TOKEN));
	AppendU16LE(token, static_cast<uint16_t>(body.size()));
	token.insert(token.end(), body.begin(), body.end());
	return token;
}

// Append a minimal DONE token (0xFD) with the ERROR status bit set. 8 bytes of
// payload follow the token type (Status 2 + CurCmd 2 + RowCount 4).
void AppendDoneToken(std::vector<uint8_t> &buf) {
	buf.push_back(static_cast<uint8_t>(TokenType::DONE));
	AppendU16LE(buf, 0x0002);  // DONE_ERROR
	AppendU16LE(buf, 0x0000);  // CurCmd
	AppendU32LE(buf, 0x0000);  // RowCount
}

//===----------------------------------------------------------------------===//
// Tests
//===----------------------------------------------------------------------===//

// State 40: "could not open database specified in login" -- the reporter's most
// likely case. Correct credentials, wrong/inaccessible initial catalog. Before
// the fix this was indistinguishable from a bad password.
void TestState40DatabaseInaccessible() {
	auto stream = BuildErrorToken(18456, 40, 14, "Login failed for user 'app_user'.");
	AppendDoneToken(stream);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_number == 18456);
	CHECK(resp.error_state == 40);
	CHECK(resp.error_message == "Login failed for user 'app_user'.");
	std::cout << "  [ok] state 40 (database inaccessible) captured" << std::endl;
}

// State 8: genuine password mismatch. Same error number, different state -- this
// is exactly the case the old code conflated with state 40.
void TestState8PasswordMismatch() {
	auto stream = BuildErrorToken(18456, 8, 14, "Login failed for user 'app_user'.");

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_number == 18456);
	CHECK(resp.error_state == 8);
	std::cout << "  [ok] state 8 (password mismatch) captured and distinct from state 40" << std::endl;
}

// A non-login server error (e.g. 4060 cannot open database) also carries a State;
// verify it round-trips too and does not get clobbered.
void TestState4060CannotOpenDatabase() {
	auto stream = BuildErrorToken(4060, 1, 11, "Cannot open database \"reporting\" requested by the login.");

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_number == 4060);
	CHECK(resp.error_state == 1);
	std::cout << "  [ok] state captured for non-18456 error (4060)" << std::endl;
}

// Regression: a successful LOGINACK response must leave error_state at its 0
// default (no ERROR token present).
void TestSuccessLeavesStateZero() {
	// Minimal LOGINACK token: type + len + interface(1) + tdsver(4) +
	// prognamelen(1) + progname + progver(4).
	std::vector<uint8_t> body;
	body.push_back(0x01);			// Interface
	AppendU32LE(body, 0x74000004);	// TDS 7.4 version (value is not asserted here)
	body.push_back(0x00);			// ProgName length (0 chars)
	AppendU32LE(body, 0x00000000);	// ProgVersion

	std::vector<uint8_t> stream;
	stream.push_back(static_cast<uint8_t>(TokenType::LOGINACK));
	AppendU16LE(stream, static_cast<uint16_t>(body.size()));
	stream.insert(stream.end(), body.begin(), body.end());
	AppendDoneToken(stream);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	CHECK(resp.error_number == 0);
	CHECK(resp.error_state == 0);
	std::cout << "  [ok] successful login leaves error_state == 0" << std::endl;
}

// Hardening (issue #183): a crafted ERROR token whose declared MsgText length
// runs past the token's own extent must NOT absorb bytes from following tokens.
// The token here truthfully declares len=16 (2-char message + filler) but claims
// msg_len=20 chars; 60 bytes of padding follow so the *buffer* has room for the
// over-read. The clamp to token_end must reject it -> error_message stays empty,
// while error_number / error_state are still captured. Also a no-crash guard.
void TestOverlongMsgLenClampedToToken() {
	std::vector<uint8_t> body;
	AppendU32LE(body, 18456);	// Number
	body.push_back(40);			// State
	body.push_back(14);			// Class
	AppendU16LE(body, 20);		// MsgText length claims 20 chars (40 bytes) -- a lie
	AppendUtf16LE(body, "Hi");	// ...but only 2 chars (4 bytes) actually present
	// Filler so the token body reaches the mandatory len >= 14.
	body.insert(body.end(), {0xAA, 0xBB, 0xCC, 0xDD});

	std::vector<uint8_t> stream;
	stream.push_back(static_cast<uint8_t>(TokenType::ERROR_TOKEN));
	AppendU16LE(stream, static_cast<uint16_t>(body.size()));  // truthful token length
	stream.insert(stream.end(), body.begin(), body.end());
	// Trailing bytes beyond the token: without the clamp, msg_len=20 would read
	// into these (bounded only by buffer end). With the clamp it cannot.
	stream.insert(stream.end(), 60, 0x00);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_number == 18456);
	CHECK(resp.error_state == 40);
	CHECK(resp.error_message.empty());	// over-long msg_len clamped, no absorption
	std::cout << "  [ok] over-long msg_len clamped to token extent (issue #183)" << std::endl;
}

// Hardening (issue #183, sibling of the ERROR clamp): a LOGINACK whose ServerName
// length runs past the token's own extent must not absorb following bytes. Token
// truthfully declares len=16 (interface + tdsver + namelen + "SQL"(6) + progver)
// but ServerName claims 10 chars (20 bytes); 30 trailing bytes follow. The clamp
// to loginack_end must reject it -> server_name empty, login still successful.
void TestLoginAckOverlongServerNameClamped() {
	std::vector<uint8_t> body;
	body.push_back(0x01);			// Interface
	AppendU32LE(body, 0x74000004);	// TDS 7.4 version
	body.push_back(10);				// ServerName length claims 10 chars -- a lie
	AppendUtf16LE(body, "SQL");		// ...but only 3 chars (6 bytes) present
	AppendU32LE(body, 0);			// ProgVersion

	std::vector<uint8_t> stream;
	stream.push_back(static_cast<uint8_t>(TokenType::LOGINACK));
	AppendU16LE(stream, static_cast<uint16_t>(body.size()));  // truthful token length (16)
	stream.insert(stream.end(), body.begin(), body.end());
	stream.insert(stream.end(), 30, 0x00);	// room a naive parser could read into

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	CHECK(resp.server_name.empty());  // over-long name_len clamped to token extent
	CHECK(resp.error_state == 0);
	std::cout << "  [ok] over-long LOGINACK server name clamped to token extent (issue #183)" << std::endl;
}

// Regression for the crash fuzz_login_response found (issue #164 harness): a
// FEDAUTHINFO token (0xEE) whose InfoID declares data_offset + data_len that
// OVERFLOWS uint32 passed the old "data_offset + data_len <= token_len" check
// while data_offset alone pointed far past the token, so ReadUTF16LE dereferenced
// wild memory (SEGV). The overflow-safe bound must reject it without crashing and
// without populating sts_url / server_spn.
void TestFedAuthInfoOffsetOverflowRejected() {
	// token body = CountOfInfoIDs(4) + one InfoID{ id(1), data_len(4), data_offset(4) }
	std::vector<uint8_t> tok;
	AppendU32LE(tok, 1);		   // CountOfInfoIDs = 1
	tok.push_back(0x01);		   // FedAuthInfoID = STS_URL
	AppendU32LE(tok, 0x20);		   // DataLength = 32 bytes
	AppendU32LE(tok, 0xFFFFFFF0);  // DataOffset -> 0xFFFFFFF0 + 0x20 wraps to 0x10

	std::vector<uint8_t> stream;
	stream.push_back(static_cast<uint8_t>(TokenType::FEDAUTHINFO));
	AppendU32LE(stream, static_cast<uint32_t>(tok.size()));	 // TokenLength (4 bytes)
	stream.insert(stream.end(), tok.begin(), tok.end());

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);  // must not crash

	CHECK(resp.has_fedauth_info == true);  // token was seen...
	CHECK(resp.sts_url.empty());		   // ...but the wild-offset field was rejected
	CHECK(resp.server_spn.empty());
	std::cout << "  [ok] FEDAUTHINFO wild data_offset rejected without OOB (fuzz regression)" << std::endl;
}

// Regression for a second crash fuzz_login_response found: a zero-length
// ENVCHANGE (0xE3 + len 0x0000) as the last token makes ptr == end, and the
// old `ptr + len <= end` guard let the code read env_data[0] one byte past the
// heap buffer. The 3-byte input `E3 00 00` is the minimized reproducer. Must
// parse without OOB.
void TestEnvChangeZeroLenNoOverread() {
	std::vector<uint8_t> stream = {static_cast<uint8_t>(TokenType::ENVCHANGE), 0x00, 0x00};

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);  // must not OOB

	CHECK(resp.success == false);  // no LOGINACK -> not a successful login
	CHECK(resp.has_routing == false);
	std::cout << "  [ok] zero-length ENVCHANGE at buffer tail no longer over-reads (fuzz regression)" << std::endl;
}

}  // namespace

int main() {
	std::cout << "test_login_error_state (issue #164: capture 18456 State byte)" << std::endl;

	TestState40DatabaseInaccessible();
	TestState8PasswordMismatch();
	TestState4060CannotOpenDatabase();
	TestSuccessLeavesStateZero();
	TestOverlongMsgLenClampedToToken();
	TestLoginAckOverlongServerNameClamped();
	TestFedAuthInfoOffsetOverflowRejected();
	TestEnvChangeZeroLenNoOverread();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}

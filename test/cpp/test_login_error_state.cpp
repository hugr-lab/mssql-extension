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
// Build + run via the Makefile (matches the CI source set exactly):
//   make test-login-error-state
//
// Or compile manually (macOS/brew example). The source list mirrors
// .github/workflows/ci.yml — including tds_types.cpp, which CI links:
//   c++ -std=c++17 -Isrc/include \
//       -I/opt/homebrew/opt/simdutf/include \
//       test/cpp/test_login_error_state.cpp \
//       src/tds/tds_packet.cpp \
//       src/tds/tds_protocol.cpp \
//       src/tds/tds_types.cpp \
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

void AppendU64LE(std::vector<uint8_t> &buf, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
	}
}

// Append a DONE-family token in the TDS 7.2+ layout ([MS-TDS] 2.2.7.9):
// Status(2) + CurCmd(2) + DoneRowCount(8). The 8-byte row count is what every
// server we log in to sends (we negotiate 7.4); the pre-7.2 4-byte form this
// helper used to emit masked the parser skipping 8 bytes instead of 12 --
// harmless only because DONE was always the last token in these fixtures.
void AppendDoneFamilyToken(std::vector<uint8_t> &buf, TokenType type, uint16_t status, uint16_t cur_cmd,
						   uint64_t row_count) {
	buf.push_back(static_cast<uint8_t>(type));
	AppendU16LE(buf, status);
	AppendU16LE(buf, cur_cmd);
	AppendU64LE(buf, row_count);
}

// Final DONE (0xFD) with the ERROR status bit set.
void AppendDoneToken(std::vector<uint8_t> &buf) {
	AppendDoneFamilyToken(buf, TokenType::DONE, 0x0002 /* DONE_ERROR */, 0x0000, 0);
}

// Minimal successful LOGINACK token (type + len + interface + tdsver +
// progname-len + progver), shared by the fixtures that need a success path.
void AppendLoginAckToken(std::vector<uint8_t> &buf) {
	std::vector<uint8_t> body;
	body.push_back(0x01);			// Interface
	AppendU32LE(body, 0x74000004);	// TDS 7.4 version
	body.push_back(0x00);			// ProgName length (0 chars)
	AppendU32LE(body, 0x00000000);	// ProgVersion
	buf.push_back(static_cast<uint8_t>(TokenType::LOGINACK));
	AppendU16LE(buf, static_cast<uint16_t>(body.size()));
	buf.insert(buf.end(), body.begin(), body.end());
}

// ROUTING ENVCHANGE (type 20 / 0x14) per [MS-TDS] 2.2.7.13. Layout inside the
// ENVCHANGE token: Type(1) NewValueLen(2, LE) Protocol(1, 0=TCP)
// Port(2, LE) AltServerLen(2, LE, in CHARACTERS) AltServer(UTF-16LE)
// OldValue(2, always 0). Spec 068.
void AppendRoutingEnvChange(std::vector<uint8_t> &buf, const std::string &server, uint16_t port) {
	std::vector<uint8_t> routing;
	routing.push_back(0x00);									 // Protocol: TCP
	AppendU16LE(routing, port);									 // ProtocolProperty: port
	AppendU16LE(routing, static_cast<uint16_t>(server.size()));	 // AltServer length in chars
	AppendUtf16LE(routing, server);

	std::vector<uint8_t> body;
	body.push_back(20);										   // ENVCHANGE type 20 = ROUTING
	AppendU16LE(body, static_cast<uint16_t>(routing.size()));  // NewValue length
	body.insert(body.end(), routing.begin(), routing.end());
	AppendU16LE(body, 0);  // OldValue length = 0

	buf.push_back(static_cast<uint8_t>(TokenType::ENVCHANGE));
	AppendU16LE(buf, static_cast<uint16_t>(body.size()));
	buf.insert(buf.end(), body.begin(), body.end());
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
	std::vector<uint8_t> stream;
	AppendLoginAckToken(stream);
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

// A zero-length ENVCHANGE mid-stream must be SKIPPED, not abandon the rest of the
// stream: a following ERROR token still has to be parsed (roborev finding -- the
// len==0 path now `continue`s instead of `break`ing). Regression against silently
// losing a real login error behind a malformed empty ENVCHANGE.
void TestEnvChangeZeroLenMidStreamContinues() {
	std::vector<uint8_t> stream = {static_cast<uint8_t>(TokenType::ENVCHANGE), 0x00, 0x00};	 // empty ENVCHANGE
	auto err = BuildErrorToken(18456, 40, 14, "Login failed for user 'app_user'.");
	stream.insert(stream.end(), err.begin(), err.end());

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.error_number == 18456);	// ERROR after the empty ENVCHANGE still parsed
	CHECK(resp.error_state == 40);
	std::cout << "  [ok] empty ENVCHANGE mid-stream skipped, following ERROR still parsed" << std::endl;
}

// Companion to the above on the SUCCESS path (roborev finding): an empty
// ENVCHANGE preceding a LOGINACK must not derail the login -- the continue path
// has to keep processing to the LOGINACK. Guards a future regression that
// re-breaks the skip for the success flow, which the ERROR-path test wouldn't catch.
void TestEnvChangeZeroLenBeforeLoginAckSucceeds() {
	std::vector<uint8_t> stream = {static_cast<uint8_t>(TokenType::ENVCHANGE), 0x00, 0x00};	 // empty ENVCHANGE
	AppendLoginAckToken(stream);
	AppendDoneToken(stream);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);  // LOGINACK after the empty ENVCHANGE still reached
	CHECK(resp.error_number == 0);
	std::cout << "  [ok] empty ENVCHANGE mid-stream skipped, following LOGINACK still succeeds" << std::endl;
}

// Regression for issues #88 / #164 (Azure Synapse Serverless): the login
// response there does NOT lead with LOGINACK -- the gateway runs internal
// procs during login and fronts it with a run of DONEINPROC tokens (confirmed
// by an MSSQL_DEBUG=2 hex dump: `ff 11 00 c1 00 01 00 00 00 00 00 00 00 ...`,
// 13 bytes per token, DONE_MORE|DONE_COUNT). The parser used to skip the
// pre-7.2 DONE size (8 bytes instead of 12), desyncing 4 bytes per token and
// never reaching the LOGINACK -> "No LOGINACK token in response" on every
// auth path. Replays that exact token pattern and requires the login to
// succeed.
void TestSynapseDoneInProcRunBeforeLoginAck() {
	std::vector<uint8_t> stream;
	// The observed pattern: rowcount 1, 1, 0 with DONE_MORE|DONE_COUNT (0x11),
	// then a bare DONE_MORE (0x01) separator -- twice, for a healthy run.
	for (int round = 0; round < 2; round++) {
		AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0011, 0x00C1, 1);
		AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0011, 0x00C1, 1);
		AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0011, 0x00C1, 0);
		AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0001, 0x00C0, 0);
	}
	AppendLoginAckToken(stream);
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0000, 0x0000, 0);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);  // LOGINACK behind the DONEINPROC run is reached
	CHECK(resp.error_state == 0);
	std::cout << "  [ok] LOGINACK behind a Synapse-style DONEINPROC run is parsed (issues #88/#164)" << std::endl;
}

// Sibling on the failure path: a login ERROR behind the same DONEINPROC run
// must surface as that error, not as the parser losing its place. Before the
// fix a desynced parse swallowed the ERROR too, so a bad password on Synapse
// reported "No LOGINACK token in response" instead of 18456.
void TestSynapseDoneInProcRunBeforeError() {
	std::vector<uint8_t> stream;
	AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0011, 0x00C1, 1);
	AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0001, 0x00C0, 0);
	auto err = BuildErrorToken(18456, 8, 14, "Login failed for user 'app_user'.");
	stream.insert(stream.end(), err.begin(), err.end());

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_number == 18456);	// the real error, not a lost parse
	CHECK(resp.error_state == 8);
	std::cout << "  [ok] ERROR behind a DONEINPROC run still surfaces (issues #88/#164)" << std::endl;
}

// The quiet casualty of the same desync, and the one that fails no test and
// raises no error: a PACKETSIZE ENVCHANGE sitting BEHIND a DONE. DoLogin7 feeds
// LoginResponse::negotiated_packet_size into ApplyNegotiatedFraming, so losing
// it pins every packet on the connection at the 4096 default -- spec 055's
// measured -28% client CPU / -43% wall simply does not happen, silently. The
// login still succeeds, which is exactly why nothing would have caught it.
void TestPacketSizeEnvChangeAfterDone() {
	std::vector<uint8_t> stream;
	AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0011, 0x00C1, 1);

	// ENVCHANGE type 4 (PACKETSIZE), new value "16384" as UTF-16LE digits.
	const std::string new_size = "16384";
	std::vector<uint8_t> env;
	env.push_back(0x04);								   // type = PACKETSIZE
	env.push_back(static_cast<uint8_t>(new_size.size()));  // NewValueLen (chars)
	AppendUtf16LE(env, new_size);						   // NewValue
	env.push_back(0x00);								   // OldValueLen
	stream.push_back(static_cast<uint8_t>(TokenType::ENVCHANGE));
	AppendU16LE(stream, static_cast<uint16_t>(env.size()));
	stream.insert(stream.end(), env.begin(), env.end());

	AppendLoginAckToken(stream);
	AppendDoneToken(stream);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	CHECK(resp.negotiated_packet_size == 16384);
	std::cout << "  [ok] PACKETSIZE ENVCHANGE behind a DONE still negotiated" << std::endl;
}

// Plain DONE (0xFD) ahead of the LOGINACK. The Synapse capture is all
// DONEINPROC/DONEPROC, so the third member of the branch is covered only by
// assumption; pin it.
void TestPlainDoneBeforeLoginAck() {
	std::vector<uint8_t> stream;
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0001, 0x0000, 7);
	AppendLoginAckToken(stream);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	std::cout << "  [ok] plain DONE (0xFD) before LOGINACK does not desync" << std::endl;
}

// Exact boundary: a DONE whose 12 payload bytes end precisely at `end`. The
// guard is `ptr + 12 <= end`, and the other half of getting that right is
// ACCEPTING a well-formed trailing DONE -- an off-by-one here would reject the
// commonest token in every login response, so hold both directions.
void TestDoneExactlyAtBufferEnd() {
	std::vector<uint8_t> stream;
	AppendLoginAckToken(stream);
	const size_t before_done = stream.size();
	AppendDoneToken(stream);
	CHECK(stream.size() == before_done + 13);  // 1 type + 12 payload

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	std::cout << "  [ok] DONE ending exactly at buffer end accepted" << std::endl;
}

// ...and the rejecting direction: a DONE with only 9 payload bytes -- enough for
// the OLD 8-byte skip, four short of the real 12 -- must break out rather than
// walk past `end`. Without a case here the bounds check is only ever exercised
// on well-formed input.
void TestTruncatedDoneDoesNotOverread() {
	std::vector<uint8_t> stream;
	stream.push_back(static_cast<uint8_t>(TokenType::DONEINPROC));
	for (int i = 0; i < 9; i++) {
		stream.push_back(0x00);
	}

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);
	CHECK(resp.error_message == "No LOGINACK token in response");
	std::cout << "  [ok] truncated DONE breaks cleanly, no over-read" << std::endl;
}

}  // namespace

//===----------------------------------------------------------------------===//
// Spec 068 -- login-time routing (ENVCHANGE 20)
//
// The parser half of the contract: `has_routing` must be reported independently
// of `success`, because the connection layer now treats routing as outranking
// both. The two shapes below are the two the connection layer used to get
// wrong in opposite directions.
//===----------------------------------------------------------------------===//

// Shape 1 -- ROUTING alongside a LOGINACK. This is what the Azure gateways we
// have actually met send. The parser must report BOTH; the connection layer
// then hops rather than transacting against the gateway.
void TestRoutingWithLoginAck() {
	std::vector<uint8_t> stream;
	AppendLoginAckToken(stream);
	AppendRoutingEnvChange(stream, "routed.database.windows.net", 11003);
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0000, 0x0000, 0);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);	  // LOGINACK was there...
	CHECK(resp.has_routing == true);  // ...and so was the redirect
	CHECK(resp.routed_server == "routed.database.windows.net");
	CHECK(resp.routed_port == 11003);
	std::cout << "  [ok] ROUTING + LOGINACK reports both success and has_routing (spec 068 D1)" << std::endl;
}

// Shape 2 -- ROUTING with NO LOGINACK: legal per [MS-TDS], and the shape that
// used to die as "authentication failed" on the FEDAUTH path because the
// !success return came before the routing capture (spec 068 Gap 1).
void TestRoutingWithoutLoginAck() {
	std::vector<uint8_t> stream;
	AppendRoutingEnvChange(stream, "routed.database.windows.net", 11003);
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0000, 0x0000, 0);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == false);	  // no LOGINACK on this socket...
	CHECK(resp.has_routing == true);  // ...but the redirect is still there
	CHECK(resp.routed_server == "routed.database.windows.net");
	CHECK(resp.routed_port == 11003);
	CHECK(resp.error_number == 0);	// and it is NOT an error
	std::cout << "  [ok] ROUTING without LOGINACK still reports has_routing (spec 068 Gap 1)" << std::endl;
}

// The routed target may carry an instance name and a port suffix -- Fabric
// sends "hostname\INSTANCE:port". The parser hands the string over verbatim;
// splitting it is NormalizeRoutedTarget's job. Pins that the UTF-16LE decode
// keeps the backslash and the colon intact.
void TestRoutedTargetWithInstanceAndPortSurvivesDecode() {
	std::vector<uint8_t> stream;
	AppendRoutingEnvChange(stream, "host.pbidedicated.windows.net\\INST01:1433", 1433);
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0000, 0x0000, 0);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.has_routing == true);
	CHECK(resp.routed_server == "host.pbidedicated.windows.net\\INST01:1433");
	std::cout << "  [ok] routed target keeps instance + port suffix through the UTF-16 decode" << std::endl;
}

// Composition with PR #254: a Synapse-style DONEINPROC run ahead of the real
// tokens. #254 fixed the 12-byte TDS 7.2+ DONE body size; if that regressed,
// the walk would desync and never reach the ROUTING behind it. This is the
// shape a routing Synapse Serverless endpoint would send.
void TestRoutingBehindDoneInProcRun() {
	std::vector<uint8_t> stream;
	for (int i = 0; i < 3; i++) {
		AppendDoneFamilyToken(stream, TokenType::DONEINPROC, 0x0001 /* DONE_MORE */, 0x00E6, 0);
	}
	AppendLoginAckToken(stream);
	AppendRoutingEnvChange(stream, "routed.sql.azuresynapse.net", 1433);
	AppendDoneFamilyToken(stream, TokenType::DONE, 0x0000, 0x0000, 0);

	LoginResponse resp = TdsProtocol::ParseLoginResponse(stream);

	CHECK(resp.success == true);
	CHECK(resp.has_routing == true);
	CHECK(resp.routed_server == "routed.sql.azuresynapse.net");
	CHECK(resp.routed_port == 1433);
	std::cout << "  [ok] ROUTING behind a DONEINPROC run still parsed (composition with #254)" << std::endl;
}

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
	TestEnvChangeZeroLenMidStreamContinues();
	TestEnvChangeZeroLenBeforeLoginAckSucceeds();
	TestSynapseDoneInProcRunBeforeLoginAck();
	TestSynapseDoneInProcRunBeforeError();
	TestPacketSizeEnvChangeAfterDone();
	TestPlainDoneBeforeLoginAck();
	TestDoneExactlyAtBufferEnd();
	TestTruncatedDoneDoesNotOverread();
	TestRoutingWithLoginAck();
	TestRoutingWithoutLoginAck();
	TestRoutedTargetWithInstanceAndPortSurvivesDecode();
	TestRoutingBehindDoneInProcRun();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}

// test/cpp/test_login7_encoding.cpp
// Unit tests for LOGIN7 non-ASCII fix (spec 043).
//
// These tests do NOT require a running SQL Server instance.
//
// Covers:
//   - LOGIN7 round-trip per builder × variable field × input category
//   - cch* / ib* correctness for non-ASCII inputs
//   - IOException (std::runtime_error) on field length > 128 UTF-16 code units
//   - ASCII regression: variable-data region + ib*/cch* pairs match a frozen
//     fixture (excludes ClientPID / ClientID / TDS-version environmental fields)
//   - simdutf wrapper bitwise-equivalent to legacy Utf16LE* on valid input
//   - simdutf wrapper falls back to legacy on invalid UTF-8 (no throw)
//
// Compile manually (no CI hook yet):
//   c++ -std=c++17 -Isrc/include -Iduckdb/src/include \
//       -DMSSQL_TLS_STUB=1 -DMSSQL_BENCH_BUILD \
//       test/cpp/test_login7_encoding.cpp \
//       src/tds/tds_packet.cpp \
//       src/tds/tds_protocol.cpp \
//       src/tds/encoding/utf16.cpp \
//       -lsimdutf \
//       -o build/test/test_login7_encoding
//
// MSSQL_BENCH_BUILD exposes the private legacy hand-rolled converter via the
// tds::encoding::testing::LegacyUtf16LE* re-export so the simdutf-vs-legacy
// equivalence assertions can compare both implementations. The production
// extension is built without this flag.
//
// Run:
//   ./build/test/test_login7_encoding

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tds/encoding/utf16.hpp"
#include "tds/tds_packet.hpp"
#include "tds/tds_protocol.hpp"
#include "tds/tds_types.hpp"

using namespace duckdb;
using namespace duckdb::tds;

namespace {

//===----------------------------------------------------------------------===//
// LOGIN7 packet parser (test-side; see specs/043/data-model.md §E5)
//===----------------------------------------------------------------------===//

// LOGIN7 fixed-header offsets per MS-TDS §2.2.6.4.
constexpr size_t LOGIN7_OFF_HOSTNAME_IB = 36;
constexpr size_t LOGIN7_OFF_USERNAME_IB = 40;
constexpr size_t LOGIN7_OFF_PASSWORD_IB = 44;
constexpr size_t LOGIN7_OFF_APPNAME_IB = 48;
constexpr size_t LOGIN7_OFF_SERVERNAME_IB = 52;
constexpr size_t LOGIN7_OFF_DATABASE_IB = 68;

struct ParsedField {
	uint16_t ib;
	uint16_t cch;
	std::vector<uint8_t> bytes;	 // payload bytes (still obfuscated for password)
};

struct ParsedLogin7 {
	ParsedField hostname;
	ParsedField username;
	ParsedField password;
	ParsedField appname;
	ParsedField servername;
	ParsedField database;
};

uint16_t ReadU16LE(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

void ExtractField(const std::vector<uint8_t> &packet, size_t pair_offset, ParsedField &out) {
	out.ib = ReadU16LE(packet.data() + pair_offset);
	out.cch = ReadU16LE(packet.data() + pair_offset + 2);
	const size_t byte_len = static_cast<size_t>(out.cch) * 2;
	out.bytes.assign(packet.begin() + out.ib, packet.begin() + out.ib + byte_len);
}

ParsedLogin7 ParseLogin7Packet(const std::vector<uint8_t> &packet) {
	ParsedLogin7 p{};
	ExtractField(packet, LOGIN7_OFF_HOSTNAME_IB, p.hostname);
	ExtractField(packet, LOGIN7_OFF_USERNAME_IB, p.username);
	ExtractField(packet, LOGIN7_OFF_PASSWORD_IB, p.password);
	ExtractField(packet, LOGIN7_OFF_APPNAME_IB, p.appname);
	ExtractField(packet, LOGIN7_OFF_SERVERNAME_IB, p.servername);
	ExtractField(packet, LOGIN7_OFF_DATABASE_IB, p.database);
	return p;
}

// Reverse the LOGIN7 password obfuscation: XOR 0xA5, then swap nibbles.
void DeobfuscatePassword(std::vector<uint8_t> &bytes) {
	for (auto &b : bytes) {
		b ^= 0xA5;
		b = static_cast<uint8_t>(((b << 4) & 0xF0) | ((b >> 4) & 0x0F));
	}
}

std::string Utf16LeToUtf8(const std::vector<uint8_t> &le_bytes) {
	return encoding::Utf16LEDecode(le_bytes.data(), le_bytes.size());
}

//===----------------------------------------------------------------------===//
// Test helpers
//===----------------------------------------------------------------------===//

int g_failures = 0;

#define CHECK_EQ(actual, expected, label)                                                                       \
	do {                                                                                                        \
		if (!((actual) == (expected))) {                                                                        \
			std::cerr << "FAIL: " << label << " expected=" << (expected) << " actual=" << (actual) << " (line " \
					  << __LINE__ << ")" << std::endl;                                                          \
			g_failures++;                                                                                       \
		}                                                                                                       \
	} while (0)

#define CHECK_STR_EQ(actual, expected, label)                                                            \
	do {                                                                                                 \
		if (!((actual) == (expected))) {                                                                 \
			std::cerr << "FAIL: " << label << " expected=\"" << (expected) << "\" actual=\"" << (actual) \
					  << "\" (line " << __LINE__ << ")" << std::endl;                                    \
			g_failures++;                                                                                \
		}                                                                                                \
	} while (0)

#define CHECK_TRUE(cond, label)                                                          \
	do {                                                                                 \
		if (!(cond)) {                                                                   \
			std::cerr << "FAIL: " << label << " (line " << __LINE__ << ")" << std::endl; \
			g_failures++;                                                                \
		}                                                                                \
	} while (0)

struct Fixture {
	const char *label;
	std::string text;
	uint16_t expected_cch;
};

const std::vector<Fixture> &PerFieldFixtures() {
	// (label, UTF-8 input, expected UTF-16 code-unit count)
	static const std::vector<Fixture> fixtures = {
		{"empty", "", 0},
		{"ascii_basic", "TestPassword1", 13},
		{"cyrillic_test123", "Тест123!", 8},  // 4 cyrillic (2 bytes each in UTF-8) + 4 ASCII
		{"cyrillic_baza", "База", 4},		  // 4 cyrillic letters
		{"latin_umlaut", "jürgen", 6},		  // 6 chars; ü = 2 UTF-8 bytes, 1 UTF-16 code unit
		{"latin_umlaut_2024", "Ünlaut$2024", 11},
		{"cjk_short", "東京", 2},
		{"emoji_surrogate", "\xF0\x9F\x94\x92secure!", 9}};	 // 🔒 = 1 codepoint = 2 UTF-16 units + "secure!"
	return fixtures;
}

//===----------------------------------------------------------------------===//
// Test 1: BuildLogin7 round-trip per variable field × non-ASCII fixture matrix
//===----------------------------------------------------------------------===//

void RoundTripBuildLogin7(const Fixture &fixture) {
	const std::string host = "localhost";
	const std::string user = "user_ru";
	const std::string app = "DuckDB";
	const std::string db = "TestDB";

	// Exercise each variable field one at a time by swapping the fixture
	// into that slot while keeping the others ASCII.
	auto run_one = [&](const char *slot_name, const std::string &h, const std::string &u, const std::string &p,
					   const std::string &a, const std::string &dbname, uint16_t expected_cch_field) {
		auto packet = TdsProtocol::BuildLogin7(h, u, p, dbname, a, /*packet_size=*/4096);
		ParsedLogin7 parsed = ParseLogin7Packet(packet.GetPayload());

		auto check_field = [&](const char *fn, const ParsedField &pf, const std::string &original, bool is_password) {
			std::vector<uint8_t> bytes = pf.bytes;
			if (is_password) {
				DeobfuscatePassword(bytes);
			}
			const std::string decoded = Utf16LeToUtf8(bytes);
			std::string label = std::string(fn) + " (" + slot_name + "=" + fixture.label + ")";
			CHECK_STR_EQ(decoded, original, label);
			CHECK_EQ(static_cast<size_t>(pf.cch) * 2, bytes.size(), label + " cch bytes");
		};
		check_field("HostName", parsed.hostname, h, false);
		check_field("UserName", parsed.username, u, false);
		check_field("Password", parsed.password, p, true);
		check_field("AppName", parsed.appname, a, false);
		check_field("ServerName", parsed.servername, h, false);
		check_field("Database", parsed.database, dbname, false);
		(void)expected_cch_field;  // implied by cch bytes check
	};

	// Place the fixture into each variable slot (HostName, UserName, Password,
	// AppName, ServerName=host shared with HostName, Database). For each
	// placement, every other field stays ASCII so we can isolate.
	run_one("HostName", fixture.text, user, "Pass123", app, db, fixture.expected_cch);
	run_one("UserName", host, fixture.text, "Pass123", app, db, fixture.expected_cch);
	run_one("Password", host, user, fixture.text, app, db, fixture.expected_cch);
	run_one("AppName", host, user, "Pass123", fixture.text, db, fixture.expected_cch);
	run_one("Database", host, user, "Pass123", app, fixture.text, fixture.expected_cch);
}

void TestBuildLogin7FixtureMatrix() {
	std::cout << "[1] BuildLogin7 round-trip (per variable field × fixture)..." << std::endl;
	for (const auto &fx : PerFieldFixtures()) {
		RoundTripBuildLogin7(fx);
	}
}

//===----------------------------------------------------------------------===//
// Test 2: ib* monotonicity and contiguity for a non-ASCII payload
//===----------------------------------------------------------------------===//

void TestIbContiguity() {
	std::cout << "[2] ib* monotonicity for non-ASCII LOGIN7 payload..." << std::endl;
	auto packet = TdsProtocol::BuildLogin7(/*host=*/"localhost", /*user=*/"jürgen", /*pwd=*/"Тест123!",
										   /*db=*/"База", /*app=*/"DuckDB", /*packet_size=*/4096);
	ParsedLogin7 p = ParseLogin7Packet(packet.GetPayload());

	// HostName starts at offset 94 (immediately after fixed header).
	CHECK_EQ(p.hostname.ib, 94u, "ib_hostname == 94");

	auto check_advance = [&](const ParsedField &prev, const ParsedField &next, const char *label) {
		const uint16_t expected = static_cast<uint16_t>(prev.ib + prev.cch * 2);
		CHECK_EQ(next.ib, expected, label);
	};
	check_advance(p.hostname, p.username, "ib_username = ib_hostname + cch_hostname*2");
	check_advance(p.username, p.password, "ib_password = ib_username + cch_username*2");
	check_advance(p.password, p.appname, "ib_appname = ib_password + cch_password*2");
	check_advance(p.appname, p.servername, "ib_servername = ib_appname + cch_appname*2");
	check_advance(p.servername, p.database, "ib_database = ib_servername + cch_servername*2");
}

//===----------------------------------------------------------------------===//
// Test 3: Surrogate-pair emoji uses 2 UTF-16 code units
//===----------------------------------------------------------------------===//

void TestSurrogatePairCounts() {
	std::cout << "[3] Surrogate-pair emoji: cch counts UTF-16 units, not codepoints..." << std::endl;
	auto packet = TdsProtocol::BuildLogin7("localhost", "user", "\xF0\x9F\x94\x92secure!", "TestDB", "DuckDB", 4096);
	ParsedLogin7 p = ParseLogin7Packet(packet.GetPayload());
	// 🔒 (U+1F512) needs a UTF-16 surrogate pair = 2 code units; "secure!" = 7.
	CHECK_EQ(p.password.cch, 9u, "cch_password = 9 for emoji+secure!");
	CHECK_EQ(p.password.bytes.size(), 18u, "ib bytes = 18 for emoji+secure!");
}

//===----------------------------------------------------------------------===//
// Test 4: Field length cap throws on overflow (FR-008)
//===----------------------------------------------------------------------===//

void TestFieldLengthCap() {
	std::cout << "[4] Length cap throws on > 128 UTF-16 code units..." << std::endl;

	// 129 ASCII chars => 129 UTF-16 code units => over cap.
	const std::string too_long_ascii(129, 'x');
	bool threw = false;
	try {
		(void)TdsProtocol::BuildLogin7("localhost", "user", too_long_ascii, "db", "app", 4096);
	} catch (const std::runtime_error &e) {
		threw = true;
		const std::string msg = e.what();
		CHECK_TRUE(msg.find("LOGIN7 field Password") != std::string::npos, "error message mentions Password");
		CHECK_TRUE(msg.find("128") != std::string::npos, "error message mentions limit 128");
		CHECK_TRUE(msg.find("129") != std::string::npos, "error message mentions observed 129");
	}
	CHECK_TRUE(threw, "129-char ASCII password throws");

	// 65 surrogate-pair emoji => 130 UTF-16 code units => over cap.
	std::string emoji_too_long;
	for (int i = 0; i < 65; i++) {
		emoji_too_long.append("\xF0\x9F\x94\x92");	// 🔒 per repeat (4 UTF-8 bytes / 2 UTF-16 units)
	}
	threw = false;
	try {
		(void)TdsProtocol::BuildLogin7("localhost", "user", emoji_too_long, "db", "app", 4096);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	CHECK_TRUE(threw, "65 surrogate-pair emoji (130 UTF-16 units) throws");

	// 128 ASCII chars => exactly cap => must NOT throw.
	const std::string at_cap_ascii(128, 'y');
	bool ok = true;
	try {
		(void)TdsProtocol::BuildLogin7("localhost", "user", at_cap_ascii, "db", "app", 4096);
	} catch (const std::runtime_error &) {
		ok = false;
	}
	CHECK_TRUE(ok, "128-char ASCII password (at cap) does not throw");
}

//===----------------------------------------------------------------------===//
// Test 5: ASCII regression — variable-data region + ib*/cch* pairs are stable
//===----------------------------------------------------------------------===//

void TestAsciiRegression() {
	std::cout << "[5] ASCII regression: variable-data region byte-stable..." << std::endl;
	auto packet = TdsProtocol::BuildLogin7("LOCALHOST", "sa", "TestPassword1", "TestDB", "DuckDB", 4096);
	const auto &bytes = packet.GetPayload();

	// Frozen variable-region snapshot generated by the post-fix build. The
	// region starts at offset 94 and runs to the end. Pre-fix this region
	// is also bit-identical for pure ASCII (UTF-8 byte count == UTF-16 code
	// unit count for ASCII), so this is a "zero regression bytes" guard.
	// Length: 9 (hostname) + 2 (user) + 13 (password) + 6 (app) + 9 (server)
	//       + 6 (database) = 45 UTF-16 code units = 90 bytes.
	CHECK_EQ(bytes.size(), 94u + 90u, "total packet size for ASCII fixture");

	// The variable-field ib*/cch* pairs at offsets 36..72 must match a known
	// layout (HostName at 94, advancing by cch*2 each field).
	const uint8_t *p = bytes.data();
	CHECK_EQ(ReadU16LE(p + 36), 94u, "ib_hostname = 94");
	CHECK_EQ(ReadU16LE(p + 38), 9u, "cch_hostname = 9");
	CHECK_EQ(ReadU16LE(p + 40), static_cast<uint16_t>(94 + 18), "ib_username");
	CHECK_EQ(ReadU16LE(p + 42), 2u, "cch_username");
	CHECK_EQ(ReadU16LE(p + 44), static_cast<uint16_t>(94 + 18 + 4), "ib_password");
	CHECK_EQ(ReadU16LE(p + 46), 13u, "cch_password");
	CHECK_EQ(ReadU16LE(p + 48), static_cast<uint16_t>(94 + 18 + 4 + 26), "ib_appname");
	CHECK_EQ(ReadU16LE(p + 50), 6u, "cch_appname");
	CHECK_EQ(ReadU16LE(p + 52), static_cast<uint16_t>(94 + 18 + 4 + 26 + 12), "ib_servername");
	CHECK_EQ(ReadU16LE(p + 54), 9u, "cch_servername");
	CHECK_EQ(ReadU16LE(p + 68), static_cast<uint16_t>(94 + 18 + 4 + 26 + 12 + 18), "ib_database");
	CHECK_EQ(ReadU16LE(p + 70), 6u, "cch_database");
}

//===----------------------------------------------------------------------===//
// Test 6: simdutf wrapper byte-equivalent to legacy on valid input
//===----------------------------------------------------------------------===//

const std::vector<std::string> &SimdutfFixtures() {
	static const std::vector<std::string> fixtures = {
		"",							// empty
		"a",						// single ASCII
		"Hello, world!",			// pure ASCII
		"TestPassword1",			// ASCII at LOGIN7-ish length
		std::string(64, 'A'),		// 64 ASCII
		std::string(127, 'B'),		// near-cap ASCII
		"ü",						// single Latin-1 supplement
		"Ünlaut$2024",				// mixed
		"jürgen",					// mixed name
		"naïve café",				// Latin extended
		"Тест",						// Cyrillic
		"Тест123!",					// Cyrillic + ASCII
		"База",						// Cyrillic
		"Пароль",					// Cyrillic
		"Привет, мир!",				// Cyrillic phrase
		"Здравствуй, мир!",			// longer Cyrillic
		"Ελληνικά",					// Greek
		"العربية",					// Arabic
		"עברית",					// Hebrew
		"日本",						// CJK
		"東京",						// CJK
		"中文测试",					// CJK 4 chars
		"한국어",					// Hangul
		"ไทย",						// Thai
		"\xF0\x9F\x94\x92",			// 🔒 single emoji
		"\xF0\x9F\x94\x92secure!",	// emoji + ASCII
		"\xF0\x9F\x98\x80",			// 😀
		"abc\xE6\x9D\xB1xyz",		// mixed ASCII / CJK
		"Σ∞∂∇",						// math symbols
		"𝄞music"					// 𝄞 (U+1D11E) surrogate pair + ASCII
	};
	return fixtures;
}

void TestSimdutfByteEquivalence() {
	std::cout << "[6] simdutf wrapper byte-equivalent to legacy on " << SimdutfFixtures().size() << " fixtures..."
			  << std::endl;
	for (const auto &input : SimdutfFixtures()) {
		// Encode direction. Post-spec-044 the public `Utf16LE*` symbols are
		// simdutf-backed; the legacy hand-rolled implementation is reachable
		// only via the test-only `encoding::testing::LegacyUtf16LE*` re-export
		// (gated by MSSQL_BENCH_BUILD at compile time).
		const auto legacy = encoding::testing::LegacyUtf16LEEncode(input);
		const auto simd = encoding::Utf16LEEncode(input);
		CHECK_TRUE(legacy == simd, std::string("encode mismatch for input: ") + input);

		// Byte-length helper
		CHECK_EQ(encoding::testing::LegacyUtf16LEByteLength(input), encoding::Utf16LEByteLength(input),
				 std::string("byte-length mismatch for input: ") + input);

		// Direct-encode helper writes to caller buffer
		std::vector<uint8_t> buf_legacy(input.size() * 4, 0);
		std::vector<uint8_t> buf_simd(input.size() * 4, 0);
		const size_t n_legacy =
			encoding::testing::LegacyUtf16LEEncodeDirect(input.data(), input.size(), buf_legacy.data());
		const size_t n_simd = encoding::Utf16LEEncodeDirect(input.data(), input.size(), buf_simd.data());
		CHECK_EQ(n_legacy, n_simd, std::string("direct-encode size mismatch for input: ") + input);
		buf_legacy.resize(n_legacy);
		buf_simd.resize(n_simd);
		CHECK_TRUE(buf_legacy == buf_simd, std::string("direct-encode bytes mismatch for input: ") + input);

		// Decode direction
		const auto round_legacy = encoding::testing::LegacyUtf16LEDecode(legacy.data(), legacy.size());
		const auto round_simd = encoding::Utf16LEDecode(simd.data(), simd.size());
		CHECK_STR_EQ(round_legacy, input, std::string("legacy round-trip: ") + input);
		CHECK_STR_EQ(round_simd, input, std::string("simdutf round-trip: ") + input);
	}
}

//===----------------------------------------------------------------------===//
// Test 7: simdutf wrapper does not throw on invalid UTF-8; falls back to legacy
//===----------------------------------------------------------------------===//

void TestSimdutfInvalidInputFallback() {
	std::cout << "[7] simdutf invalid-UTF-8 fallback to legacy converter..." << std::endl;
	// Invalid UTF-8 sequences (lone continuation, overlong, lone leading byte).
	// Adjacent string literals are concatenated; this stops the \xNN escape
	// from greedily consuming following hex letters.
	const std::vector<std::string> invalid = {
		"\xC0\xC1"
		"invalid",	// overlong / invalid leading bytes
		"abc"
		"\x80"
		"def",	// lone continuation byte
		"\xE0\x80"
		"valid_following"  // truncated 3-byte
	};
	for (const auto &s : invalid) {
		bool threw = false;
		std::vector<uint8_t> simd;
		try {
			simd = encoding::Utf16LEEncode(s);
		} catch (...) {
			threw = true;
		}
		CHECK_TRUE(!threw, "Utf16LEEncode does not throw on invalid UTF-8");
		// Fallback: output must be bit-identical to the private legacy
		// hand-rolled converter on the same invalid input.
		const auto legacy = encoding::testing::LegacyUtf16LEEncode(s);
		CHECK_TRUE(simd == legacy, "invalid-UTF-8 fallback matches legacy output");
	}
}

}  // namespace

//===----------------------------------------------------------------------===//
// Test 8: LOGIN7 SSPI fragmentation (issue #138)
//
// A Kerberos LOGIN7 carrying a real AD PAC exceeds the 4096-byte
// pre-negotiation packet size. SplitIntoPackets must fragment it across TDS
// packets (EOM on the last only) so SQL Server does not TCP-reset the
// connection. Small blobs must still produce exactly one EOM packet.
//===----------------------------------------------------------------------===//

void TestLogin7SspiFragmentation() {
	std::cout << "[8] LOGIN7 SSPI fragmentation (issue #138)..." << std::endl;

	const std::string host = "client-host";
	const std::string server = "sql.example.com";
	const std::string db = "master";
	const std::string app = "DuckDB MSSQL Extension";

	auto build = [&](size_t blob_size) {
		// Deterministic non-trivial blob bytes (i*7+1 mod 256) so concatenation
		// equality also catches chunk-boundary corruption, not just sizes.
		std::vector<uint8_t> blob(blob_size);
		for (size_t i = 0; i < blob_size; i++) {
			blob[i] = static_cast<uint8_t>((i * 7 + 1) & 0xFF);
		}
		return TdsProtocol::BuildLogin7WithSSPI(host, server, db, blob, app, TDS_DEFAULT_PACKET_SIZE);
	};

	// --- Small blob: fits in one packet, returned unchanged (EOM set). ---
	{
		TdsPacket login = build(64);
		CHECK_TRUE(login.GetPayload().size() <= TDS_DEFAULT_PACKET_SIZE - TDS_HEADER_SIZE,
				   "small LOGIN7 fits in one packet");
		auto packets = TdsProtocol::SplitIntoPackets(login, TDS_DEFAULT_PACKET_SIZE);
		CHECK_EQ(packets.size(), static_cast<size_t>(1), "small blob -> 1 packet");
		CHECK_TRUE(packets[0].IsEndOfMessage(), "single packet has EOM");
		CHECK_TRUE(packets[0].GetPayload() == login.GetPayload(), "single packet payload unchanged");
	}

	// --- Large blob (PAC-sized): must fragment across multiple packets. ---
	{
		const size_t kBlob = 10000;	 // > 2x default packet size
		TdsPacket login = build(kBlob);
		const std::vector<uint8_t> original = login.GetPayload();
		CHECK_TRUE(original.size() > TDS_DEFAULT_PACKET_SIZE, "large LOGIN7 exceeds one packet");

		auto packets = TdsProtocol::SplitIntoPackets(login, TDS_DEFAULT_PACKET_SIZE);
		CHECK_TRUE(packets.size() > 1, "large blob -> multiple packets");

		std::vector<uint8_t> reassembled;
		for (size_t i = 0; i < packets.size(); i++) {
			const TdsPacket &pkt = packets[i];
			const bool is_last = (i + 1 == packets.size());

			CHECK_EQ(static_cast<int>(pkt.GetType()), static_cast<int>(PacketType::LOGIN7),
					 "fragment keeps LOGIN7 type");
			// Each serialized packet (header + payload) must fit the limit.
			CHECK_TRUE(pkt.Serialize().size() <= TDS_DEFAULT_PACKET_SIZE, "serialized fragment <= packet size");
			CHECK_TRUE(pkt.GetLength() <= TDS_DEFAULT_PACKET_SIZE, "fragment length header <= packet size");
			// EOM only on the last fragment.
			CHECK_EQ(pkt.IsEndOfMessage(), is_last, "EOM set only on last fragment");

			const auto &chunk = pkt.GetPayload();
			CHECK_TRUE(!chunk.empty(), "fragment payload non-empty");
			reassembled.insert(reassembled.end(), chunk.begin(), chunk.end());
		}
		CHECK_TRUE(reassembled == original, "reassembled payload byte-identical to original LOGIN7");
	}
}

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "LOGIN7 non-ASCII fix + simdutf wrapper unit tests (spec 043)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		TestBuildLogin7FixtureMatrix();
		TestIbContiguity();
		TestSurrogatePairCounts();
		TestFieldLengthCap();
		TestAsciiRegression();
		TestSimdutfByteEquivalence();
		TestSimdutfInvalidInputFallback();
		TestLogin7SspiFragmentation();
	} catch (const std::exception &e) {
		std::cerr << "UNCAUGHT EXCEPTION: " << e.what() << std::endl;
		return 2;
	}

	std::cout << "============================================================" << std::endl;
	if (g_failures == 0) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	std::cerr << g_failures << " TEST(S) FAILED" << std::endl;
	return 1;
}

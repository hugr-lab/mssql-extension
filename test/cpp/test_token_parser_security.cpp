// test/cpp/test_token_parser_security.cpp
//
// Security regression tests for the TDS token-stream decoder (duckdb::tds::TokenParser).
// The decoder consumes attacker-controllable bytes from a (possibly malicious or
// MITM'd) SQL Server, so a server that under-declares a token length must never
// make the parser read past its buffer.
//
// These reproduce a heap-buffer-overflow found by fuzzing (fuzz/fuzz_tds_tokens):
// an ERROR (0xAA) or INFO (0xAB) token whose LE16 length field is smaller than the
// fixed number[4]+state[1]+severity[1] prefix. Without the fix the fixed-field
// reads run past the buffer; built with -fsanitize=address this test crashes on a
// regression. It does NOT require a running SQL Server.
//
// Build/run: see the `test-token-parser-security` Makefile target (ASan+UBSan).

#include <cstdint>
#include <iostream>
#include <vector>

#include "tds/tds_token_parser.hpp"

using namespace duckdb::tds;

static int g_failures = 0;

// Feed a raw token stream and drain the parser. The only assertion is "does not
// crash / does not hang" — a memory-safety regression aborts under ASan; a logic
// regression that loops forever is caught by the iteration cap.
static void DrainNoCrash(const char *name, const std::vector<uint8_t> &bytes) {
	TokenParser parser;
	parser.Feed(bytes.data(), bytes.size());
	int steps = 0;
	for (; steps < 100000; ++steps) {
		ParsedTokenType t = parser.TryParseNext();
		if (t == ParsedTokenType::NeedMoreData || t == ParsedTokenType::None) {
			break;
		}
	}
	if (steps >= 100000) {
		std::cerr << "FAIL: " << name << " did not terminate" << std::endl;
		++g_failures;
		return;
	}
	std::cout << "ok: " << name << std::endl;
}

int main() {
	// --- the exact fuzz finding + its minimization ---
	DrainNoCrash("error_underlen_min", {0xAA, 0x00, 0x00});	 // ERROR, token_length=0
	DrainNoCrash("error_underlen_8b", {0xAA, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00});
	DrainNoCrash("info_underlen_min", {0xAB, 0x00, 0x00});	// INFO, token_length=0

	// --- boundary token_lengths 1..5 (all < the 6-byte fixed prefix) ---
	for (uint16_t len = 1; len <= 5; ++len) {
		std::vector<uint8_t> err{0xAA, static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
		err.insert(err.end(), len, 0x00);  // exactly token_length body bytes present
		DrainNoCrash("error_len_boundary", err);
		std::vector<uint8_t> info{0xAB, static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>(len >> 8)};
		info.insert(info.end(), len, 0x00);
		DrainNoCrash("info_len_boundary", info);
	}

	// --- truncated header / empty ---
	DrainNoCrash("empty", {});
	DrainNoCrash("error_truncated_header", {0xAA, 0x05});  // length byte missing

	// --- sanity: a well-formed-enough ERROR token still parses without crashing.
	// number[4] state[1] severity[1] msglen(US_VARCHAR=0) servername[1]=0 procname[1]=0 line[4]
	{
		std::vector<uint8_t> body = {/*number*/ 0x01,		  0x00, 0x00, 0x00,
									 /*state*/ 0x01,
									 /*severity*/ 0x10,
									 /*msglen LE16*/ 0x00,	  0x00,
									 /*servername len*/ 0x00,
									 /*procname len*/ 0x00,
									 /*line*/ 0x00,			  0x00, 0x00, 0x00};
		std::vector<uint8_t> err{0xAA, static_cast<uint8_t>(body.size() & 0xFF),
								 static_cast<uint8_t>(body.size() >> 8)};
		err.insert(err.end(), body.begin(), body.end());
		DrainNoCrash("error_wellformed", err);
	}

	// --- FindBeginTxnDescriptor: ENVCHANGE transaction-descriptor scan ---
	// (replaced the connection provider's hand-rolled `offset += token_len - 1` loop)
	{
		uint8_t out[8] = {0};
		// Valid BEGIN_TRANS: E3 len 08 08 <desc 1..8> 00  (token_len = 11)
		std::vector<uint8_t> valid = {0xE3, 0x0B, 0x00, 0x08, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0x00};
		bool found = FindBeginTxnDescriptor(valid.data(), valid.size(), out);
		bool good = found;
		for (int i = 0; i < 8; ++i) {
			good = good && out[i] == static_cast<uint8_t>(i + 1);
		}
		if (!good) {
			std::cerr << "FAIL: FindBeginTxnDescriptor did not extract a valid descriptor" << std::endl;
			++g_failures;
		} else {
			std::cout << "ok: findtxn_valid" << std::endl;
		}

		// Malformed: must never read past the buffer or hang; result is don't-care.
		const std::vector<std::vector<uint8_t>> malformed = {
			{0xE3, 0x00, 0x00},								   // token_len == 0 (underflow shape)
			{0xE3, 0x05, 0x00, 0x08, 0x08},					   // claims more body than present
			{0xE3, 0x0B, 0x00, 0x08, 0x04, 1, 2, 3, 4, 0x00},  // new_len != 8
			{0xE3, 0xFF, 0xFF, 0x08, 0x08, 1, 2, 3, 4},		   // huge token_len, short buffer
			{0xE3},											   // truncated header
			{0xFD, 0x00},									   // DONE then nothing
			{},
		};
		for (const auto &v : malformed) {
			uint8_t d[8] = {0};
			(void)FindBeginTxnDescriptor(v.data(), v.size(), d);
		}
		std::cout << "ok: findtxn_malformed_no_oob" << std::endl;
	}

	if (g_failures) {
		std::cerr << g_failures << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "All token-parser security tests passed (no OOB)." << std::endl;
	return 0;
}

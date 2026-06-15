// Fuzz harness: ENVCHANGE transaction-descriptor scan.
//
// tds::FindBeginTxnDescriptor() scans a server's BEGIN TRANSACTION response (raw,
// attacker-controllable bytes) for the ENVCHANGE BEGIN_TRANS (0x08) 8-byte
// transaction descriptor. It replaced a hand-rolled inline loop in the connection
// provider that used an `offset += token_len - 1` pattern (a uint16 underflow
// shape). This harness drives that scan directly to prove no token length the
// server advertises can cause an out-of-bounds read or a non-terminating scan.
//
// The function is a pure byte scan and does not throw; any sanitizer report or
// signal is a real bug.

#include <cstddef>
#include <cstdint>

#include "tds/tds_token_parser.hpp"

static volatile std::size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	uint8_t descriptor[8] = {0};
	bool found = duckdb::tds::FindBeginTxnDescriptor(data, size, descriptor);
	if (found) {
		for (int i = 0; i < 8; ++i) {
			g_sink += descriptor[i];
		}
	}
	return 0;
}

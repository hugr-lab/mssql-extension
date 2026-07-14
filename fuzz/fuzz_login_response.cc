// Fuzz harness: LOGIN7 response parser (TdsProtocol::ParseLoginResponse).
//
// This is the token stream a (possibly malicious or MITM'd) SQL Server returns
// to a LOGIN7 request: LOGINACK, ENVCHANGE (ROUTING / PACKETSIZE), ERROR / INFO,
// FEDAUTHINFO, SSPI. The client trusts the server's declared token and field
// lengths before authentication completes, so every length here is attacker-
// controlled. This is a distinct code path from fuzz_tds_tokens (post-login
// result-set decoding via TokenParser).
//
// Motivated by issues #164 (capture the 18456 State byte) and #183 (clamp the
// ERROR-token MsgText length to the token's own extent). Seeds live in
// corpus/login_response/.
//
// The input bytes are the raw token stream (the payload after the 8-byte TDS
// packet header) — exactly what DoLogin7 hands the parser from ReceiveMessage.
//
// std::exception on malformed input is benign; only a sanitizer report / signal
// is a real bug. We never catch(...).

#include <cstddef>
#include <cstdint>
#include <exception>
#include <vector>

#include "tds/tds_protocol.hpp"

static volatile std::size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	try {
		std::vector<uint8_t> buf(data, data + size);
		duckdb::tds::LoginResponse resp = duckdb::tds::TdsProtocol::ParseLoginResponse(buf);

		// Touch decoded structures so the work is observable to the optimiser.
		g_sink += resp.success ? 1u : 0u;
		g_sink += resp.error_number;
		g_sink += resp.error_state;
		g_sink += resp.error_message.size();
		g_sink += resp.server_name.size();
		g_sink += resp.routed_server.size();
		g_sink += resp.sts_url.size();
		g_sink += resp.sspi_token.size();
	} catch (const std::exception &) {
		// Rejected malformed login response — expected, not a bug.
	}
	return 0;
}

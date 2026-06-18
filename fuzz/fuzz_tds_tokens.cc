// Fuzz harness: TDS result-set / token-stream decoder (the main attack surface).
//
// duckdb::tds::TokenParser consumes the token stream a (possibly malicious or
// MITM'd) SQL Server sends back: COLMETADATA (column types/lengths), ROW / NBCROW
// (per-column type+length decoding), ENVCHANGE, DONE, ERROR, INFO. The client
// trusts the server's declared types and lengths, so this is where length
// arithmetic / type confusion bugs live.
//
// The input bytes are a raw token stream (the payload after the 8-byte TDS packet
// header — exactly what TdsConnection feeds the parser from packet.GetPayload()).
// We drive the same TryParseNext() loop the connection does.
//
// std::exception on malformed input is benign; only a sanitizer report / signal
// is a real bug. We never catch(...).

#include <cstddef>
#include <cstdint>
#include <exception>

#include "tds/tds_token_parser.hpp"

static volatile std::size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	try {
		duckdb::tds::TokenParser parser;
		parser.Feed(data, size);

		// Pull tokens until the parser is out of data. The iteration cap is a
		// safety net against a hypothetical non-consuming parse loop; a correct
		// parser reaches NeedMoreData/None well before it.
		for (int i = 0; i < 1000000; ++i) {
			duckdb::tds::ParsedTokenType t = parser.TryParseNext();
			if (t == duckdb::tds::ParsedTokenType::NeedMoreData || t == duckdb::tds::ParsedTokenType::None) {
				break;
			}
			// Touch decoded structures so the work is observable to the optimiser.
			if (t == duckdb::tds::ParsedTokenType::ColMetadata) {
				g_sink += parser.GetColumnMetadata().size();
			} else if (t == duckdb::tds::ParsedTokenType::Row) {
				g_sink += parser.GetRow().values.size();
			}
		}
	} catch (const std::exception &) {
		// Rejected malformed token stream — expected, not a bug.
	}
	return 0;
}

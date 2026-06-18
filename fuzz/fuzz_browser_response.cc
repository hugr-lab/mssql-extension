// Fuzz harness: SQL Browser (UDP/1434) response parser.
//
// duckdb::mssql::ParseBrowserResponse() decodes a datagram from a *remote,
// attacker-controllable* SQL Browser service: opcode 0x05 + LE16 body length +
// ';'-delimited body. A rogue/MITM browser fully controls these bytes, so this
// is a real client-side attack surface with a clean (data,len) seam and zero
// DuckDB dependencies — the pipeline proof-of-concept.
//
// Contract: the parser throws std::runtime_error on malformed input. That is the
// documented, *benign* outcome. Only an ASan/UBSan report or a signal indicates
// a genuine memory-safety bug, so we swallow std::exception but never the
// sanitizer abort (we do NOT catch(...) — a sanitizer error is not a C++
// exception and must propagate to crash the process).

#include <cstddef>
#include <cstdint>
#include <exception>

#include "connection/instance_resolver.hpp"

static volatile std::size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	try {
		auto records = duckdb::mssql::ParseBrowserResponse(data, size);
		// Touch results so the decode can't be optimised away.
		g_sink += records.size();
		for (const auto &r : records) {
			g_sink += r.instance_name.size() + r.server_name.size() + r.version.size();
			g_sink += r.tcp_port + (r.tcp_enabled ? 1u : 0u) + (r.is_clustered ? 2u : 0u);
		}
	} catch (const std::exception &) {
		// Rejected malformed datagram — expected, not a bug.
	}
	return 0;
}

// Fuzz harness: UTF-16LE decoders.
//
// The TDS layer decodes server-supplied UTF-16LE for column names, string
// values and ENVCHANGE payloads. Two implementations exist and are both reached
// on attacker-controlled bytes:
//   - Utf16LEDecode()        — simdutf-backed, with a hand-rolled fallback for
//                              invalid input (the path most likely to mishandle
//                              odd/truncated byte counts and surrogates).
//   - testing::LegacyUtf16LEDecode() — the legacy hand-rolled converter.
//
// Feeding raw bytes (including odd lengths and lone surrogates) exercises the
// length/surrogate arithmetic directly. std::exception is benign; only a
// sanitizer report / signal is a real bug.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "tds/encoding/utf16.hpp"

static volatile std::size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	try {
		// Production decode path — the real server->client attack surface.
		std::string a = duckdb::tds::encoding::Utf16LEDecode(data, size);
		g_sink += a.size();
#ifdef MSSQL_BENCH_BUILD
		// The legacy hand-rolled converter + encode path are only declared under
		// MSSQL_BENCH_BUILD; the fuzz build defines it so these are reachable.
		std::string b = duckdb::tds::encoding::testing::LegacyUtf16LEDecode(data, size);
		g_sink += b.size();
		// LegacyUtf16LEEncodeDirect: UTF-8 -> UTF-16LE into a caller buffer sized
		// by LegacyUtf16LEByteLength. Exercise the length/encode arithmetic.
		std::string in(reinterpret_cast<const char *>(data), size);
		size_t need = duckdb::tds::encoding::testing::LegacyUtf16LEByteLength(in);
		std::vector<uint8_t> out(need);
		size_t wrote =
			duckdb::tds::encoding::testing::LegacyUtf16LEEncodeDirect(in.data(), in.size(), out.empty() ? nullptr : out.data());
		g_sink += wrote;
#endif
	} catch (const std::exception &) {
		// Rejected malformed input — expected, not a bug.
	}
	return 0;
}

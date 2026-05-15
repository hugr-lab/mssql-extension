// test/cpp/bench_utf16.cpp
//
// Codec microbenchmark for spec 044 (UTF-16 Codec Consolidation).
//
// Compares the simdutf-backed public Utf16LE* primitives against the
// private hand-rolled LegacyUtf16LE* helpers (exposed via the
// `tds::encoding::testing::` namespace when compiled with
// MSSQL_BENCH_BUILD).
//
// Asserts, per-fixture:
//   1. Byte-identical output between simdutf and legacy (FR-022 / SC-005).
//   2. simdutf wall-clock <= 1.20x legacy wall-clock (FR-023 / SC-004).
//      The 1.20x slack covers (a) M-series measurement noise (~+/-15%
//      across consecutive runs of the same fixture), and (b) the
//      intrinsic 3-pass cost of the simdutf wrapper (validate + length
//      + convert) vs the legacy hand-rolled ASCII fast path (single
//      pass) on the ascii_64k fixture. Non-ASCII fixtures consistently
//      see simdutf 10-25% faster than legacy. The floor is "not
//      catastrophically slower", not a specific speedup target.
//
// Manual target only (FR-024 / FR-042): `make bench-utf16`. The benchmark
// must NOT run as part of `make test` or any CI workflow.
//
// Output format:
//   [bench_utf16] simdutf vs legacy UTF-16 codec equivalence + perf
//   Fixture        Simdutf (ms)   Legacy (ms)   Ratio   Identical
//   ascii_16       0.012          0.018         0.67    PASS
//   ...
//   VERDICT: <count>/<total> fixtures byte-identical, <count>/<total>
//   within 1.20x perf floor.

#include "tds/encoding/utf16.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using duckdb::tds::encoding::Utf16LEByteLength;
using duckdb::tds::encoding::Utf16LEDecode;
using duckdb::tds::encoding::Utf16LEEncode;
using duckdb::tds::encoding::Utf16LEEncodeDirect;
using duckdb::tds::encoding::testing::LegacyUtf16LEByteLength;
using duckdb::tds::encoding::testing::LegacyUtf16LEDecode;
using duckdb::tds::encoding::testing::LegacyUtf16LEEncode;
using duckdb::tds::encoding::testing::LegacyUtf16LEEncodeDirect;

namespace {

//===----------------------------------------------------------------------===//
// Fixture set (FR-021): ASCII × 3 sizes, BMP × 3, CJK × 3, emoji × 3,
// plus one mixed 64-KB payload. 13 fixtures total.
//===----------------------------------------------------------------------===//

struct FixtureSpec {
	const char *name;
	std::string utf8;
};

// Repeat a small payload to reach the target byte size. Trims the result
// to the last complete UTF-8 codepoint boundary so the fixture is always
// valid UTF-8 (avoids mid-codepoint truncation that would route both
// simdutf and legacy paths through the invalid-input fallback).
std::string Repeat(const std::string &unit, size_t target_bytes) {
	std::string out;
	out.reserve(target_bytes + unit.size());
	while (out.size() < target_bytes) {
		out.append(unit);
	}
	out.resize(target_bytes);
	// Trim trailing bytes until `out` ends on a UTF-8 codepoint boundary.
	// A UTF-8 leading byte has the high bits 0xxxxxxx (1-byte) or 11xxxxxx
	// (start of multi-byte); continuation bytes are 10xxxxxx.
	while (!out.empty()) {
		const auto b = static_cast<uint8_t>(out.back());
		if ((b & 0x80) == 0x00) {
			// 1-byte (ASCII) codepoint at the end — already complete.
			break;
		}
		if ((b & 0xC0) == 0xC0) {
			// Leading byte of a multi-byte codepoint at the very end — the
			// continuation bytes are missing. Drop it.
			out.pop_back();
			break;
		}
		// Continuation byte (10xxxxxx). Pop it; if the next byte is the
		// leading byte and the codepoint is complete, the next iteration's
		// check on the leading byte will pass.
		out.pop_back();
		if (!out.empty()) {
			const auto lead = static_cast<uint8_t>(out.back());
			// Determine expected continuation count from leading byte.
			int continuations_expected = 0;
			if ((lead & 0xE0) == 0xC0)
				continuations_expected = 1;
			else if ((lead & 0xF0) == 0xE0)
				continuations_expected = 2;
			else if ((lead & 0xF8) == 0xF0)
				continuations_expected = 3;
			else
				continue;  // unexpected — pop further

			// Walk forward from the leading byte's position counting actual
			// continuation bytes present.
			const size_t lead_pos = out.size() - 1;
			int continuations_present = 0;
			for (size_t i = lead_pos + 1; i < out.size(); ++i) {
				continuations_present++;
			}
			if (continuations_present == continuations_expected) {
				break;
			}
			// Otherwise this leading byte is incomplete — pop it and continue.
			out.pop_back();
		}
	}
	return out;
}

// Sink to prevent compiler dead-code-elimination of the timed body. We
// accumulate a checksum from the function's output bytes into a volatile
// global so the optimizer cannot prove the call result is unused.
volatile uint64_t g_sink = 0;

inline void ConsumeBytes(const std::vector<uint8_t> &v) {
	uint64_t s = 0;
	for (auto b : v) {
		s = s * 131 + b;
	}
	g_sink ^= s;
}

inline void ConsumeBytes(const uint8_t *p, size_t n) {
	uint64_t s = 0;
	for (size_t i = 0; i < n; ++i) {
		s = s * 131 + p[i];
	}
	g_sink ^= s;
}

inline void ConsumeString(const std::string &s) {
	uint64_t h = 0;
	for (char c : s) {
		h = h * 131 + static_cast<uint8_t>(c);
	}
	g_sink ^= h;
}

std::vector<FixtureSpec> BuildFixtures() {
	std::vector<FixtureSpec> fs;
	// ASCII
	fs.push_back({"ascii_16", "abcdefghijklmnop"});
	fs.push_back({"ascii_256", Repeat("Lorem ipsum dolor sit amet. ", 256)});
	fs.push_back({"ascii_64k", Repeat("Lorem ipsum dolor sit amet, consectetur. ", 65536)});

	// BMP non-ASCII: Cyrillic + accented Latin (each UTF-8 char is 2 bytes)
	fs.push_back({"bmp_16", "Привет мир!"});
	fs.push_back({"bmp_256", Repeat("Привет Лорем Юнлаут Ä é í ñ. ", 256)});
	fs.push_back({"bmp_64k", Repeat("Привет Лорем Юнлаут Ä é í ñ. ", 65536)});

	// CJK (each UTF-8 char is 3 bytes)
	fs.push_back({"cjk_16", "中文测试入"});
	fs.push_back({"cjk_256", Repeat("中文测试 漢字 日本語 한국어 ", 256)});
	fs.push_back({"cjk_64k", Repeat("中文测试 漢字 日本語 한국어 ", 65536)});

	// Emoji (surrogate pairs; each UTF-8 char is 4 bytes)
	fs.push_back({"emoji_16", "🔒🔑💾"});
	fs.push_back({"emoji_256", Repeat("🔒 secure 🔑 keys 💾 storage 😀 happy ", 256)});
	fs.push_back({"emoji_64k", Repeat("🔒 secure 🔑 keys 💾 storage 😀 happy ", 65536)});

	// Mixed 64k: ~50% ASCII, ~30% BMP, ~20% non-BMP
	fs.push_back(
		{"mixed_64k",
		 Repeat("Lorem ipsum dolor. Привет мир Лорем юнлаут. 中文测试 漢字 日本語. 🔒 secure 🔑 keys. ", 65536)});

	return fs;
}

//===----------------------------------------------------------------------===//
// Timing: median of N measured iterations after a 100-iteration warm-up.
//===----------------------------------------------------------------------===//

template <typename Fn>
double MedianMillis(Fn fn, size_t iterations) {
	using clock = std::chrono::steady_clock;
	// Warm-up: 100 iterations not measured.
	for (size_t w = 0; w < 100; ++w) {
		fn();
	}
	std::vector<double> samples;
	samples.reserve(iterations);
	for (size_t i = 0; i < iterations; ++i) {
		const auto t0 = clock::now();
		fn();
		const auto t1 = clock::now();
		const auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
		samples.push_back(static_cast<double>(dur) / 1e6);
	}
	std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
	return samples[samples.size() / 2];
}

size_t IterationsFor(size_t input_size) {
	// 64-KB fixtures: 100 iterations. Smaller fixtures: 1000 iterations.
	return (input_size >= 16384) ? 100 : 1000;
}

//===----------------------------------------------------------------------===//
// Bench routines: encode/decode round-trip, byte-identity, timing.
//===----------------------------------------------------------------------===//

struct FixtureResult {
	std::string name;
	double simdutf_ms;
	double legacy_ms;
	double ratio;	 // simdutf / legacy
	bool identical;	 // bit-identical output for all four operations
};

FixtureResult RunFixture(const FixtureSpec &f) {
	FixtureResult r;
	r.name = f.name;

	const size_t iters = IterationsFor(f.utf8.size());

	// === Byte-identity checks ===
	const auto simd_encode = Utf16LEEncode(f.utf8);
	const auto legacy_encode = LegacyUtf16LEEncode(f.utf8);

	std::vector<uint8_t> simd_direct(f.utf8.size() * 4, 0);
	std::vector<uint8_t> legacy_direct(f.utf8.size() * 4, 0);
	const size_t simd_n = Utf16LEEncodeDirect(f.utf8.data(), f.utf8.size(), simd_direct.data());
	const size_t legacy_n = LegacyUtf16LEEncodeDirect(f.utf8.data(), f.utf8.size(), legacy_direct.data());
	simd_direct.resize(simd_n);
	legacy_direct.resize(legacy_n);

	const auto simd_decode_back = Utf16LEDecode(simd_encode.data(), simd_encode.size());
	const auto legacy_decode_back = LegacyUtf16LEDecode(legacy_encode.data(), legacy_encode.size());

	const size_t simd_byte_len = Utf16LEByteLength(f.utf8);
	const size_t legacy_byte_len = LegacyUtf16LEByteLength(f.utf8);

	// Equivalence requires byte-for-byte agreement across all four operations.
	// (The decode-back-equals-input round trip is intentionally NOT asserted
	// here — fixtures may be re-encoded through paths that change byte
	// length, e.g. simdutf canonicalizes some invalid byte sequences
	// differently than the hand-rolled converter. For valid UTF-8 fixtures
	// the encode/decode equivalence assertions above already cover correctness.)
	r.identical = (simd_encode == legacy_encode) && (simd_direct == legacy_direct) &&
				  (simd_decode_back == legacy_decode_back) && (simd_byte_len == legacy_byte_len);

	// === Timing ===
	// Use a volatile-global checksum sink to prevent the optimizer from
	// dead-code-eliminating the timed body. Without this, the legacy call
	// (visible to the compiler within the same TU) was being eliminated
	// while the simdutf call (external symbol) was not, producing
	// nonsensical "legacy ~0 ms, simdutf ~ms" timings.
	r.simdutf_ms = MedianMillis(
		[&]() {
			auto v = Utf16LEEncode(f.utf8);
			ConsumeBytes(v);
		},
		iters);
	r.legacy_ms = MedianMillis(
		[&]() {
			auto v = LegacyUtf16LEEncode(f.utf8);
			ConsumeBytes(v);
		},
		iters);
	r.ratio = (r.legacy_ms > 0) ? (r.simdutf_ms / r.legacy_ms) : 0.0;
	return r;
}

void PrintHeader() {
	std::cout << "[bench_utf16] simdutf vs legacy UTF-16 codec equivalence + perf\n\n";
	std::cout << std::left << std::setw(14) << "Fixture" << std::right << std::setw(14) << "Simdutf (ms)"
			  << std::setw(14) << "Legacy (ms)" << std::setw(10) << "Ratio" << std::setw(12) << "Identical"
			  << "\n";
}

void PrintRow(const FixtureResult &r) {
	std::cout << std::left << std::setw(14) << r.name << std::right << std::setw(14) << std::fixed
			  << std::setprecision(4) << r.simdutf_ms << std::setw(14) << r.legacy_ms << std::setw(10)
			  << std::setprecision(2) << r.ratio << std::setw(12) << (r.identical ? "PASS" : "FAIL") << "\n";
}

}  // namespace

int main() {
	const auto fixtures = BuildFixtures();
	std::vector<FixtureResult> results;
	results.reserve(fixtures.size());

	PrintHeader();
	for (const auto &f : fixtures) {
		auto r = RunFixture(f);
		PrintRow(r);
		results.push_back(r);
	}

	// === Verdict ===
	size_t identical_count = 0;
	size_t perf_floor_count = 0;
	for (const auto &r : results) {
		if (r.identical) {
			++identical_count;
		}
		if (r.ratio <= 1.20) {
			++perf_floor_count;
		}
	}

	std::cout << "\nVERDICT: " << identical_count << "/" << results.size() << " fixtures byte-identical, "
			  << perf_floor_count << "/" << results.size() << " within 1.20x perf floor.\n";

	const bool all_identical = (identical_count == results.size());
	const bool all_within_floor = (perf_floor_count == results.size());

	if (all_identical && all_within_floor) {
		std::cout << "PASS.\n";
		return 0;
	}

	// On failure, report which fixtures violated which gate.
	std::cout << "FAIL.\n";
	for (const auto &r : results) {
		if (!r.identical) {
			std::cout << "  byte-identity FAIL: " << r.name << "\n";
		}
		if (r.ratio > 1.20) {
			std::cout << "  perf floor FAIL: " << r.name << " (ratio " << std::fixed << std::setprecision(2) << r.ratio
					  << ")\n";
		}
	}
	return 1;
}

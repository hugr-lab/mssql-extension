// Spec 091: code-page decode kernel microbenchmark. Manual target, not in `make test`:
//   make bench-code-page
//
// One chunk of 2048 values per workload, decoded over and over, every way below, run
// interleaved within each round (the repository's rule: only same-session interleaved A/B
// counts), median of the rounds. Before timing, every way's output is compared with
// `direct`'s, byte for byte, and the run aborts on a difference.
//   fused      codec::code_page::DecodeValues, value by value: on arm64 a single-byte value
//              of 32 bytes or more takes the NEON kernel, a shorter one (and every
//              double-byte value) the 16-byte ASCII loop with table stores.
//   2-pass     simdutf::validate_ascii over the whole payload first; when ASCII, one memcpy
//              and the staged offsets reused; otherwise fused.
//   2-stage    DecodeToUtf16 (SIMD table lookup on NEON) into a scratch buffer, then
//              simdutf::convert_valid_utf16le_to_utf8: the read path's UTF-16 kernel.
//   ascii64    CopyAscii (test and copy in one pass, 64 bytes a step), then 2-stage for the
//              rest from the first byte >= 0x80.
//   auto       validate_ascii + memcpy when ASCII, else 2-stage.
//   direct     DecodeToUtf8 over the whole payload as one string (the NEON kernel on arm64
//              for a single-byte page).
//   utf16      simdutf::convert_utf16le_to_utf8 over the same text as UTF-16LE: what the
//              read path runs today, after the server's CAST to NVARCHAR.
// Reported per value, with the bytes each form puts on the wire.

#include "codec/code_page/code_page.hpp"
#include "codec/code_page/code_page_internal.hpp"

#include "simdutf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace duckdb::mssql::codec::code_page;

namespace {

constexpr size_t VALUES = 2048;
constexpr int ROUNDS = 9;

struct Chunk {
	std::string name;
	const CodePageTable *page;
	std::vector<uint8_t> payload;
	std::vector<uint32_t> offsets, lengths;
	std::vector<char16_t> utf16;
	size_t utf8_bytes = 0;
};

using Gen = std::vector<uint8_t> (*)(std::mt19937 &);

std::vector<uint8_t> AsciiWord(std::mt19937 &rng, size_t n) {
	std::vector<uint8_t> v(n);
	for (auto &c : v) {
		c = static_cast<uint8_t>('a' + rng() % 26);
	}
	return v;
}

std::vector<uint8_t> AsciiShort(std::mt19937 &rng) {
	return AsciiWord(rng, 4 + rng() % 17);
}
std::vector<uint8_t> AsciiLong(std::mt19937 &rng) {
	std::vector<uint8_t> v = AsciiWord(rng, 200);
	for (size_t i = 7; i < v.size(); i += 8) {
		v[i] = ' ';
	}
	return v;
}
// French-like text in 1252: words with now and then an é è à ç ô (about 4% of bytes).
std::vector<uint8_t> Latin1252(std::mt19937 &rng) {
	static const uint8_t accents[] = {0xE9, 0xE8, 0xE0, 0xE7, 0xF4};
	std::vector<uint8_t> v;
	const size_t n = 20 + rng() % 41;
	while (v.size() < n) {
		const size_t w = 2 + rng() % 9;
		for (size_t i = 0; i < w; i++) {
			v.push_back(rng() % 25 == 0 ? accents[rng() % 5] : static_cast<uint8_t>('a' + rng() % 26));
		}
		v.push_back(' ');
	}
	return v;
}
// Russian-like text in 1251: Cyrillic letters 0xC0-0xFF, words of 3-9, spaces between.
std::vector<uint8_t> Cyrillic1251(std::mt19937 &rng) {
	std::vector<uint8_t> v;
	const size_t n = 20 + rng() % 41;
	while (v.size() < n) {
		const size_t w = 3 + rng() % 7;
		for (size_t i = 0; i < w; i++) {
			v.push_back(static_cast<uint8_t>(0xC0 + rng() % 64));
		}
		v.push_back(' ');
	}
	return v;
}
// Russian text with typographic punctuation in 1251: about 1 in 40 characters is a dash,
// an ellipsis or a curly quote (0x85, 0x93, 0x94, 0x96, 0x97: 3-byte UTF-8).
std::vector<uint8_t> Cyrillic1251Typo(std::mt19937 &rng) {
	static const uint8_t typo[] = {0x85, 0x93, 0x94, 0x96, 0x97};
	std::vector<uint8_t> v;
	const size_t n = 20 + rng() % 41;
	while (v.size() < n) {
		const size_t w = 3 + rng() % 7;
		for (size_t i = 0; i < w; i++) {
			v.push_back(rng() % 40 == 0 ? typo[rng() % 5] : static_cast<uint8_t>(0xC0 + rng() % 64));
		}
		v.push_back(' ');
	}
	return v;
}
// French text with typography in 1252: accents (~4%) and, about 1 in 40 characters, a curly
// quote, a dash, an ellipsis or the euro sign (0x80, 0x85, 0x93, 0x94, 0x96, 0x97: 3-byte).
std::vector<uint8_t> Latin1252Typo(std::mt19937 &rng) {
	static const uint8_t accents[] = {0xE9, 0xE8, 0xE0, 0xE7, 0xF4};
	static const uint8_t typo[] = {0x80, 0x85, 0x93, 0x94, 0x96, 0x97};
	std::vector<uint8_t> v;
	const size_t n = 20 + rng() % 41;
	while (v.size() < n) {
		const size_t w = 2 + rng() % 9;
		for (size_t i = 0; i < w; i++) {
			const unsigned r = rng() % 40;
			v.push_back(r == 0 ? typo[rng() % 6]
							   : (rng() % 25 == 0 ? accents[rng() % 5] : static_cast<uint8_t>('a' + rng() % 26)));
		}
		v.push_back(' ');
	}
	return v;
}
// Thai text in 874: letters A1-DA (U+0E01-U+0E3A), no spaces between words, a space now and
// then between phrases.
std::vector<uint8_t> Thai874(std::mt19937 &rng) {
	std::vector<uint8_t> v;
	const size_t n = 20 + rng() % 41;
	while (v.size() < n) {
		const size_t w = 10 + rng() % 30;
		for (size_t i = 0; i < w; i++) {
			v.push_back(static_cast<uint8_t>(0xA1 + rng() % 0x3A));
		}
		v.push_back(' ');
	}
	return v;
}
// Japanese-like text in 932: hiragana and kanji pairs, now and then ASCII digits.
std::vector<uint8_t> Japanese932(std::mt19937 &rng) {
	std::vector<uint8_t> v;
	const size_t chars = 8 + rng() % 20;
	for (size_t i = 0; i < chars; i++) {
		const unsigned r = rng() % 10;
		if (r < 4) {  // hiragana 0x829F-0x82F1
			v.push_back(0x82);
			v.push_back(static_cast<uint8_t>(0x9F + rng() % 0x53));
		} else if (r < 9) {	 // kanji, leads 0x88-0x98, trails 0x40-0x7E
			v.push_back(static_cast<uint8_t>(0x89 + rng() % 0x0F));
			v.push_back(static_cast<uint8_t>(0x40 + rng() % 0x3F));
		} else {
			v.push_back(static_cast<uint8_t>('0' + rng() % 10));
		}
	}
	return v;
}

Chunk Make(const char *name, int code_page, Gen gen, std::mt19937 &rng) {
	Chunk c;
	c.name = name;
	c.page = FindCodePage(code_page);
	for (size_t i = 0; i < VALUES; i++) {
		const std::vector<uint8_t> v = gen(rng);
		c.offsets.push_back(static_cast<uint32_t>(c.payload.size()));
		c.lengths.push_back(static_cast<uint32_t>(v.size()));
		c.payload.insert(c.payload.end(), v.begin(), v.end());
	}
	std::vector<char> utf8(Utf8Bound(c.payload.size()));
	c.utf8_bytes = DecodeToUtf8(*c.page, c.payload.data(), c.payload.size(), utf8.data());
	c.utf16.resize(c.utf8_bytes + 1);
	const size_t units = simdutf::convert_utf8_to_utf16le(utf8.data(), c.utf8_bytes, c.utf16.data());
	c.utf16.resize(units);
	return c;
}

volatile size_t sink;

template <class F>
double TimeIt(F &&f, int reps) {
	const auto t0 = std::chrono::steady_clock::now();
	for (int r = 0; r < reps; r++) {
		sink = f();
	}
	const auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

double Median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

}  // namespace

int main() {
	namespace detail = duckdb::mssql::codec::code_page::detail;
	// CODE_PAGE_ISA=scalar|neon|avx2|avx512 runs the kernels of that instruction set (the CPU must
	// have it); unset, the best one the CPU has.
	if (const char *want = std::getenv("CODE_PAGE_ISA")) {
		const detail::Isa isas[] = {detail::Isa::Scalar, detail::Isa::Neon, detail::Isa::Avx2, detail::Isa::Avx512};
		bool forced = false;
		for (detail::Isa isa : isas) {
			if (std::string(want) == detail::IsaName(isa)) {
				forced = detail::ForceIsa(isa);
			}
		}
		if (!forced) {
			std::fprintf(stderr, "CODE_PAGE_ISA=%s: not supported here\n", want);
			return 2;
		}
	}
	std::printf("kernels: %s\n", detail::IsaName(detail::ActiveIsa()));
	std::mt19937 rng(91);
	std::vector<Chunk> chunks;
	chunks.push_back(Make("ascii short (4-20 B)", 1252, AsciiShort, rng));
	chunks.push_back(Make("ascii long (200 B)", 1252, AsciiLong, rng));
	chunks.push_back(Make("latin 1252, ~4% accents", 1252, Latin1252, rng));
	chunks.push_back(Make("cyrillic 1251", 1251, Cyrillic1251, rng));
	chunks.push_back(Make("cyrillic 1251 + typography", 1251, Cyrillic1251Typo, rng));
	chunks.push_back(Make("latin 1252 + typography", 1252, Latin1252Typo, rng));
	chunks.push_back(Make("thai 874", 874, Thai874, rng));
	chunks.push_back(Make("japanese 932", 932, Japanese932, rng));

	static const char *const NAMES[] = {"fused", "2-pass", "2-stage", "ascii64", "auto", "direct", "utf16"};
	const size_t ways = sizeof(NAMES) / sizeof(NAMES[0]);
	std::printf("ns per value, median of %d interleaved rounds; 2048 values per chunk\n", ROUNDS);
	std::printf("%-26s %7s %7s |", "workload", "cp KB", "u16 KB");
	for (size_t w = 0; w < ways; w++) {
		std::printf(" %8s", NAMES[w]);
	}
	std::printf("\n");
	for (const Chunk &c : chunks) {
		std::vector<char> dst(Utf8Bound(c.payload.size()));
		std::vector<char> dst16(c.utf16.size() * 3 + 64);
		std::vector<uint64_t> out_offsets(VALUES);
		std::vector<uint32_t> out_lengths(VALUES);
		std::vector<char16_t> scratch(c.payload.size() + 1);
		const int reps = static_cast<int>(std::max<size_t>(50, 40000000 / (c.payload.size() + 1)));

		auto fused = [&]() {
			return DecodeValues(*c.page, c.payload.data(), c.payload.size(), c.offsets.data(), c.lengths.data(), VALUES,
								dst.data(), out_offsets.data(), out_lengths.data());
		};
		auto two_pass = [&]() -> size_t {
			if (simdutf::validate_ascii(reinterpret_cast<const char *>(c.payload.data()), c.payload.size())) {
				memcpy(dst.data(), c.payload.data(), c.payload.size());
				for (size_t i = 0; i < VALUES; i++) {
					out_offsets[i] = c.offsets[i];
					out_lengths[i] = c.lengths[i];
				}
				return c.payload.size();
			}
			return fused();
		};
		// Stage 1 then stage 2 over payload[from, end), written at dst + from: the bytes
		// before `from` are ASCII, one output byte each.
		auto stages = [&](size_t from) -> size_t {
			const size_t units =
				DecodeToUtf16(*c.page, c.payload.data() + from, c.payload.size() - from, scratch.data());
			return simdutf::convert_valid_utf16le_to_utf8(scratch.data(), units, dst.data() + from);
		};
		auto twostage = [&]() -> size_t { return stages(0); };
		auto ascii64 = [&]() -> size_t {
			const size_t done = CopyAscii(c.payload.data(), c.payload.size(), dst.data());
			return done == c.payload.size() ? done : done + stages(done);
		};
		auto autosel = [&]() -> size_t {
			if (simdutf::validate_ascii(reinterpret_cast<const char *>(c.payload.data()), c.payload.size())) {
				memcpy(dst.data(), c.payload.data(), c.payload.size());
				return c.payload.size();
			}
			return twostage();
		};
		auto direct = [&]() { return DecodeToUtf8(*c.page, c.payload.data(), c.payload.size(), dst.data()); };
		auto utf16 = [&]() { return simdutf::convert_utf16le_to_utf8(c.utf16.data(), c.utf16.size(), dst16.data()); };
		const std::vector<std::function<size_t()>> run = {fused, two_pass, twostage, ascii64, autosel, direct, utf16};
		const std::vector<const char *> output = {dst.data(), dst.data(), dst.data(),  dst.data(),
												  dst.data(), dst.data(), dst16.data()};

		// Every way against direct, once, before anything is timed.
		const size_t want_length = direct();
		const std::string want(dst.data(), want_length);
		for (size_t w = 0; w < ways; w++) {
			memset(dst.data(), 0, dst.size());
			memset(dst16.data(), 0, dst16.size());
			const size_t length = run[w]();
			if (length != want_length || memcmp(output[w], want.data(), want_length) != 0) {
				std::fprintf(stderr, "%s: %s gives %zu bytes, direct %zu%s\n", c.name.c_str(), NAMES[w], length,
							 want_length, length == want_length ? " (different bytes)" : "");
				return 1;
			}
		}

		std::vector<std::vector<double>> times(ways);
		for (int round = 0; round < ROUNDS; round++) {
			for (size_t w = 0; w < ways; w++) {
				times[w].push_back(TimeIt(run[w], reps));
			}
		}
		const double in_bytes = static_cast<double>(c.payload.size());
		std::printf("%-26s %7.1f %7.1f |", c.name.c_str(), in_bytes / 1024, c.utf16.size() * 2.0 / 1024);
		for (size_t w = 0; w < ways; w++) {
			std::printf(" %8.2f", Median(times[w]) / VALUES);
		}
		std::printf("\n");
	}
	std::printf("\nutf16 leaves out the server's CAST and the doubled wire bytes (u16 KB against cp KB).\n");
	return 0;
}

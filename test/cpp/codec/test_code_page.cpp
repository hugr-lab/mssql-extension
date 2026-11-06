// test/cpp/codec/test_code_page.cpp
// Unit tests for codec::code_page (spec 091). No SQL Server needed.
//
// The reference is the server's own answers in tools/code_pages/data/, read
// here directly — not the generated tables — so the test checks the emitter
// and the kernel together:
//   - every single byte and every double-byte pair decodes as the server read it;
//   - every high byte of a single-byte page in blocks of every length the NEON
//     kernel splits differently (alone and alternated with ASCII), every two
//     high bytes in one 32-byte step, and every double-byte pair repeated in
//     16/32/48-byte runs decode as a reference decoder built from the same
//     answers (lead byte at the end of a value dropped, a 0x00 after a lead
//     byte read on its own);
//   - the strings the server read whole (cpNNN.strings.psv, double-byte pages)
//     decode as it read them: a check that does not go through that model;
//   - random strings and random runs of values decode as the reference;
//   - no decode writes past its returned length + STORE_SLACK (a canary
//     follows), DecodeToUtf16 none past n units;
//   - ExactUtf8Length agrees with the decode;
//   - collation lookups by id, by name and by wire key, for every collation.
//
// Run from the repository root (the data path is relative):
//   make test-code-page             the platform's SIMD form (NEON / SSE2)
//   make test-code-page-portable    the 8-byte SWAR form

#include "codec/code_page/code_page.hpp"
#include "codec/code_page/code_page_internal.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace duckdb::mssql::codec::code_page;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                             \
	do {                                                             \
		if (!(cond)) {                                               \
			std::cerr << "FAIL " << __LINE__ << ": " << msg << "\n"; \
			failures++;                                              \
		}                                                            \
	} while (0)

const std::string DATA = "tools/code_pages/data/";

std::vector<uint8_t> Bytes(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 2 <= hex.size(); i += 2) {
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	}
	return out;
}

std::vector<uint32_t> CodePoints(const std::string &utf16le_hex) {
	const std::vector<uint8_t> raw = Bytes(utf16le_hex);
	std::vector<uint32_t> cps;
	for (size_t i = 0; i + 2 <= raw.size(); i += 2) {
		cps.push_back(raw[i] | (static_cast<uint32_t>(raw[i + 1]) << 8));
	}
	return cps;
}

std::string Utf8(const std::vector<uint32_t> &cps) {
	std::string s;
	for (uint32_t cp : cps) {
		if (cp < 0x80) {
			s += static_cast<char>(cp);
		} else if (cp < 0x800) {
			s += static_cast<char>(0xC0 | (cp >> 6));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			s += static_cast<char>(0xE0 | (cp >> 12));
			s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			s += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	return s;
}

//! The server's answers for one page, and a decoder built only from them.
struct Reference {
	int code_page = 0;
	std::vector<std::vector<uint32_t>> single = std::vector<std::vector<uint32_t>>(256);
	//! Indexed (lead << 8) | trail; only leads >= 0x80 are probed.
	std::vector<std::vector<uint32_t>> pairs = std::vector<std::vector<uint32_t>>(65536);
	std::vector<uint32_t> pair_keys;
	bool lead[256] = {};
	bool double_byte = false;

	bool Load(int cp) {
		code_page = cp;
		std::ifstream in(DATA + "cp" + std::to_string(cp) + ".psv");
		if (!in) {
			return false;
		}
		std::string line;
		while (std::getline(in, line)) {
			const size_t bar = line.find('|');
			const std::string key = line.substr(0, bar);
			const std::vector<uint32_t> cps = CodePoints(line.substr(bar + 1));
			const uint32_t v = static_cast<uint32_t>(std::stoul(key, nullptr, 16));
			if (key.size() == 2) {
				single[v] = cps;
			} else {
				pairs[v] = cps;
				pair_keys.push_back(v);
			}
		}
		for (int b = 0x80; b < 256; b++) {
			lead[b] = single[b].empty();
			double_byte = double_byte || lead[b];
		}
		return true;
	}

	std::string Decode(const uint8_t *s, size_t n) const {
		std::vector<uint32_t> out;
		size_t i = 0;
		while (i < n) {
			const uint8_t b = s[i];
			if (double_byte && lead[b]) {
				if (i + 1 == n) {
					break;	// dropped
				}
				const uint8_t t = s[i + 1];
				const std::vector<uint32_t> &cps = pairs[(uint32_t(b) << 8) | t];
				if (t == 0) {
					out.push_back(cps[0]);	// the replacement; the 0x00 reads on its own next
					i += 1;
				} else {
					out.insert(out.end(), cps.begin(), cps.end());
					i += 2;
				}
				continue;
			}
			out.insert(out.end(), single[b].begin(), single[b].end());
			i++;
		}
		return Utf8(out);
	}

	std::string Decode(const std::vector<uint8_t> &s) const {
		return Decode(s.data(), s.size());
	}
};

const unsigned char CANARY = 0xA5;
const char16_t CANARY16 = 0xA5A5;

//! True when every byte of out from `from` on is still the canary.
bool CanaryIntact(const std::vector<char> &out, size_t from) {
	for (size_t i = from; i < out.size(); i++) {
		if (static_cast<unsigned char>(out[i]) != CANARY) {
			return false;
		}
	}
	return true;
}

//! Decode through the kernel into Utf8Bound(n) bytes followed by a canary; *overrun when
//! anything at or past the returned length + STORE_SLACK was written.
std::string KernelDecode(const CodePageTable &page, const std::vector<uint8_t> &in, bool *overrun) {
	std::vector<char> out(Utf8Bound(in.size()) + 64, static_cast<char>(CANARY));
	const size_t n = DecodeToUtf8(page, in.data(), in.size(), out.data());
	if (n > in.size() * MAX_UTF8_PER_BYTE || !CanaryIntact(out, n + STORE_SLACK)) {
		*overrun = true;
	}
	return std::string(out.data(), n < out.size() ? n : out.size());
}

std::string Utf16ToUtf8(const char16_t *u, size_t n) {
	std::vector<uint32_t> cps(u, u + n);
	return Utf8(cps);
}

//! Decode `in` every way the module offers and compare each with `want`:
//!   DecodeToUtf8 (canary at the returned length + STORE_SLACK);
//!   into a buffer sized by ExactUtf8Length + STORE_SLACK, which must equal want's length;
//!   DecodeValues as one value with high bytes after it, which a read-ahead may touch but
//!   the decode must not take;
//!   DecodeToUtf16 (nothing written past n units) then UTF-8.
//! Returns false on any difference; *overrun on a write past an envelope.
bool Check(const CodePageTable &page, const std::vector<uint8_t> &in, const std::string &want, bool *overrun) {
	bool ok = KernelDecode(page, in, overrun) == want;

	const size_t exact = ExactUtf8Length(page, in.data(), in.size());
	ok = ok && exact == want.size();
	std::vector<char> tight(exact + STORE_SLACK + 32, static_cast<char>(CANARY));
	const size_t tight_n = DecodeToUtf8(page, in.data(), in.size(), tight.data());
	ok = ok && tight_n == want.size() && memcmp(tight.data(), want.data(), want.size()) == 0;
	if (!CanaryIntact(tight, exact + STORE_SLACK)) {
		*overrun = true;
	}

	std::vector<uint8_t> staged(in);
	staged.insert(staged.end(), static_cast<size_t>(24), static_cast<uint8_t>(0xFF));
	const uint32_t offset = 0, length = static_cast<uint32_t>(in.size());
	uint64_t out_offset = 99;
	uint32_t out_length = 99;
	std::vector<char> values(Utf8Bound(in.size()) + 64, static_cast<char>(CANARY));
	const size_t total =
		DecodeValues(page, staged.data(), staged.size(), &offset, &length, 1, values.data(), &out_offset, &out_length);
	ok = ok && total == want.size() && out_offset == 0 && out_length == want.size() &&
		 memcmp(values.data(), want.data(), want.size()) == 0;
	if (!CanaryIntact(values, total + STORE_SLACK)) {
		*overrun = true;
	}

	std::vector<char16_t> units(in.size() + 32, CANARY16);
	const size_t u = DecodeToUtf16(page, in.data(), in.size(), units.data());
	ok = ok && u <= in.size() && Utf16ToUtf8(units.data(), u) == want;
	for (size_t i = in.size(); i < units.size(); i++) {
		if (units[i] != CANARY16) {
			*overrun = true;
		}
	}
	return ok;
}

void TestEveryByteAndPair(const Reference &ref, const CodePageTable &page) {
	bool overrun = false;
	int mismatches = 0;
	for (int b = 0; b < 256; b++) {
		const std::vector<uint8_t> in(1, static_cast<uint8_t>(b));
		mismatches += !Check(page, in, Utf8(ref.single[b]), &overrun);
	}
	for (uint32_t key : ref.pair_keys) {
		const std::vector<uint8_t> in = {static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key & 0xFF)};
		mismatches += !Check(page, in, Utf8(ref.pairs[key]), &overrun);
	}
	CHECK(mismatches == 0, "cp" << ref.code_page << ": " << mismatches << " bytes/pairs differ from the server");
	CHECK(!overrun, "cp" << ref.code_page << ": a byte/pair decode wrote past its envelope");
}

//! The NEON kernel splits its input in 32-byte steps, then a 16-byte block, then a tail, and
//! picks a tier per step (ASCII, the page's byte + delta run, 1/2-byte planes, Block3), so
//! the blocks here put every high byte in every one of those positions:
//!   single-byte: each byte 0x80-0xFF repeated, alone and alternated with ASCII, at lengths
//!   either side of each boundary; every two high bytes in one 32-byte step, ASCII around
//!   (each pair of tiers meets in one step);
//!   double-byte: every probed pair repeated in 16/32/48-byte runs.
void TestBlocks(const Reference &ref, const CodePageTable &page) {
	bool overrun = false;
	int mismatches = 0;
	if (!ref.double_byte) {
		static const size_t LENGTHS[] = {16, 17, 31, 32, 33, 48, 64, 96};
		for (int b = 0x80; b < 0x100; b++) {
			for (size_t len : LENGTHS) {
				const std::vector<uint8_t> alone(len, static_cast<uint8_t>(b));
				std::vector<uint8_t> mixed(len);
				for (size_t i = 0; i < len; i++) {
					mixed[i] = (i % 2) ? static_cast<uint8_t>('a' + i % 26) : static_cast<uint8_t>(b);
				}
				mismatches += !Check(page, alone, ref.Decode(alone), &overrun);
				mismatches += !Check(page, mixed, ref.Decode(mixed), &overrun);
			}
		}
		// [0, 32) ASCII, [32, 64) ASCII but for the two high bytes, then a 16-byte block and
		// a 5-byte tail of ASCII. The positions vary with the bytes, so both halves of the
		// step and both in one half all occur.
		for (int a = 0x80; a < 0x100; a++) {
			for (int b = 0x80; b < 0x100; b++) {
				std::vector<uint8_t> in(32 + 32 + 16 + 5);
				for (size_t i = 0; i < in.size(); i++) {
					in[i] = static_cast<uint8_t>('A' + i % 26);
				}
				const size_t pa = (static_cast<size_t>(a) * 7) % 32;
				size_t pb = (static_cast<size_t>(b) * 11 + 5) % 32;
				if (pb == pa) {
					pb = (pa + 16) % 32;
				}
				in[32 + pa] = static_cast<uint8_t>(a);
				in[32 + pb] = static_cast<uint8_t>(b);
				mismatches += !Check(page, in, ref.Decode(in), &overrun);
			}
		}
	} else {
		static const size_t RUNS[] = {16, 32, 48};
		for (uint32_t key : ref.pair_keys) {
			for (size_t len : RUNS) {
				std::vector<uint8_t> in(len);
				for (size_t i = 0; i < len; i += 2) {
					in[i] = static_cast<uint8_t>(key >> 8);
					in[i + 1] = static_cast<uint8_t>(key & 0xFF);
				}
				mismatches += !Check(page, in, ref.Decode(in), &overrun);
			}
		}
	}
	CHECK(mismatches == 0, "cp" << ref.code_page << ": " << mismatches << " blocks differ from the reference");
	CHECK(!overrun, "cp" << ref.code_page << ": a block decode wrote past its envelope");
}

//! cpNNN.strings.psv: strings of 3-12 bytes the server read whole. Compared with the
//! server's answer directly, not with the reference decoder, so a rule the pair model gets
//! wrong shows here. Absent until the generator has probed them.
void TestServerStrings(const Reference &ref, const CodePageTable &page) {
	const std::string name = "cp" + std::to_string(ref.code_page) + ".strings.psv";
	std::ifstream in(DATA + name);
	if (!in) {
		std::cout << "test_code_page: " << name << " absent (make code-pages-generate probes it); "
				  << "cp" << ref.code_page << "'s server-string check skipped\n";
		return;
	}
	bool overrun = false;
	int strings = 0, mismatches = 0, stage_one = 0;
	std::string line;
	while (std::getline(in, line)) {
		const size_t bar = line.find('|');
		const std::vector<uint8_t> bytes = Bytes(line.substr(0, bar));
		const std::vector<uint32_t> cps = CodePoints(line.substr(bar + 1));
		strings++;
		if (!Check(page, bytes, Utf8(cps), &overrun)) {
			mismatches++;
		}
		std::vector<char16_t> units(bytes.size() + 32, CANARY16);
		const size_t u = DecodeToUtf16(page, bytes.data(), bytes.size(), units.data());
		if (std::vector<uint32_t>(units.begin(), units.begin() + u) != cps) {
			stage_one++;
		}
	}
	CHECK(strings > 0, name << " is empty");
	CHECK(mismatches == 0, name << ": " << mismatches << " of " << strings << " strings differ from the server");
	CHECK(stage_one == 0, name << ": " << stage_one << " stage-1 decodes differ from the server's UTF-16");
	CHECK(!overrun, name << ": a decode wrote past its envelope");
}

std::vector<uint8_t> RandomBytes(std::mt19937 &rng, const Reference &ref, size_t n) {
	std::vector<uint8_t> s;
	s.reserve(n);
	if (!ref.double_byte) {
		// Dense blocks as well as sparse ones: 35% single high bytes, and 10% of positions
		// start a run of 16-64 high bytes drawn from a random slice of 0x80-0xFF, so a run
		// may lie in the page's byte + delta run (the arithmetic tier) or hold a 3-byte
		// character (Block3).
		while (s.size() < n) {
			const unsigned r = rng() % 100;
			if (r < 10) {
				const unsigned lo = 0x80 + rng() % 128;
				const unsigned width = 1 + rng() % (0x100 - lo);
				const size_t run = 16 + rng() % 49;
				for (size_t k = 0; k < run && s.size() < n; k++) {
					s.push_back(static_cast<uint8_t>(lo + rng() % width));
				}
			} else if (r < 45) {
				s.push_back(static_cast<uint8_t>(0x80 + rng() % 128));
			} else if (r < 50) {
				s.push_back(0);
			} else {
				s.push_back(static_cast<uint8_t>(0x20 + rng() % 0x5F));	 // printable ASCII
			}
		}
		return s;
	}
	std::vector<uint8_t> leads;
	for (int b = 0x80; b < 256; b++) {
		if (ref.lead[b]) {
			leads.push_back(static_cast<uint8_t>(b));
		}
	}
	for (size_t i = 0; i < n; i++) {
		const unsigned r = rng() % 100;
		if (r < 45) {
			s.push_back(static_cast<uint8_t>(0x20 + rng() % 0x5F));	 // printable ASCII
		} else if (r < 70) {
			s.push_back(leads[rng() % leads.size()]);
		} else if (r < 75) {
			s.push_back(0);	 // NUL, including after a lead byte
		} else {
			s.push_back(static_cast<uint8_t>(rng() % 256));
		}
	}
	return s;
}

void TestRandomStrings(const Reference &ref, const CodePageTable &page, std::mt19937 &rng) {
	bool overrun = false;
	int mismatches = 0;
	for (int iter = 0; iter < 3000; iter++) {
		const size_t n = (iter % 10 == 0) ? rng() % 300 : rng() % 40;
		const std::vector<uint8_t> in = RandomBytes(rng, ref, n);
		mismatches += !Check(page, in, ref.Decode(in), &overrun);
	}
	CHECK(mismatches == 0, "cp" << ref.code_page << ": " << mismatches << " random strings differ");
	CHECK(!overrun, "cp" << ref.code_page << ": a random decode wrote past its envelope");
}

//! Values back to back, as staging holds them; the buffer ends exactly at the last value,
//! so the tail takes the short path, and short values read into their neighbours. Values
//! of 32 bytes or more take the NEON kernel on arm64, shorter ones the 16-byte loop.
void TestRandomValues(const Reference &ref, const CodePageTable &page, std::mt19937 &rng) {
	int mismatches = 0;
	bool overrun = false;
	for (int iter = 0; iter < 300; iter++) {
		const size_t count = 1 + rng() % 200;
		std::vector<uint8_t> buf;
		std::vector<uint32_t> offsets(count), lengths(count);
		for (size_t i = 0; i < count; i++) {
			const size_t n = (rng() % 8 == 0) ? 0 : (rng() % 4 == 0 ? rng() % 160 : rng() % 48);
			const std::vector<uint8_t> v = RandomBytes(rng, ref, n);
			offsets[i] = static_cast<uint32_t>(buf.size());
			lengths[i] = static_cast<uint32_t>(n);
			buf.insert(buf.end(), v.begin(), v.end());
		}
		std::vector<char> out(Utf8Bound(buf.size()) + 64, static_cast<char>(CANARY));
		std::vector<uint64_t> out_offsets(count);
		std::vector<uint32_t> out_lengths(count);
		const size_t total = DecodeValues(page, buf.data(), buf.size(), offsets.data(), lengths.data(), count,
										  out.data(), out_offsets.data(), out_lengths.data());
		size_t expect_offset = 0;
		for (size_t i = 0; i < count; i++) {
			const std::string want = ref.Decode(buf.data() + offsets[i], lengths[i]);
			const std::string got(out.data() + out_offsets[i], out_lengths[i]);
			if (got != want || out_offsets[i] != expect_offset) {
				mismatches++;
			}
			expect_offset += want.size();
		}
		if (total != expect_offset) {
			mismatches++;
		}
		if (!CanaryIntact(out, total + STORE_SLACK)) {
			overrun = true;
		}
	}
	CHECK(mismatches == 0, "cp" << ref.code_page << ": " << mismatches << " values differ in DecodeValues");
	CHECK(!overrun, "cp" << ref.code_page << ": DecodeValues wrote past its returned length + STORE_SLACK");
}

void TestCopyAscii(std::mt19937 &rng) {
	int mismatches = 0;
	bool overrun = false;
	for (int iter = 0; iter < 2000; iter++) {
		const size_t n = rng() % 400;
		std::vector<uint8_t> in(n);
		for (auto &c : in) {
			c = static_cast<uint8_t>(rng() % 0x80);
		}
		size_t want = n;
		if (n && rng() % 2) {
			want = rng() % n;
			in[want] = static_cast<uint8_t>(0x80 | (rng() % 0x80));
		}
		std::vector<char> out(n + 32, static_cast<char>(CANARY));
		const size_t got = CopyAscii(in.data(), n, out.data());
		if (got != want || (got && memcmp(out.data(), in.data(), got) != 0)) {
			mismatches++;
		}
		if (!CanaryIntact(out, n)) {
			overrun = true;
		}
	}
	CHECK(mismatches == 0, "CopyAscii: " << mismatches << " buffers wrong");
	CHECK(!overrun, "CopyAscii wrote past n bytes");
}

void TestLookups() {
	const char name[] = "SQL_Latin1_General_CP1_CI_AS";
	const CollationRow *row = FindCollationByName(name, strlen(name));
	CHECK(row && row->code_page == 1252, "SQL_Latin1_General_CP1_CI_AS -> 1252");
	CHECK(row && ((static_cast<uint32_t>(row->collation_id) >> 24) == 52), "its SortId is 52");
	const char lower[] = "sql_latin1_general_cp1_ci_as";
	CHECK(FindCollationByName(lower, strlen(lower)) == row, "name lookup is case-insensitive");
	CHECK(row && FindCollationById(row->collation_id) == row, "lookup by CollationId");
	const char prefix[] = "SQL_Latin1_General_CP1_CI_A";
	CHECK(FindCollationByName(prefix, strlen(prefix)) == nullptr, "a prefix is not a name");
	const char longer[] = "SQL_Latin1_General_CP1_CI_ASX";
	CHECK(FindCollationByName(longer, strlen(longer)) == nullptr, "a name with more after it is not a name");
	const char cyr[] = "Cyrillic_General_CI_AS";
	const CollationRow *c = FindCollationByName(cyr, strlen(cyr));
	CHECK(c && c->code_page == 1251 && c->lcid == 1049, "Cyrillic_General_CI_AS -> 1251, LCID 1049");
	CHECK(CodePageForWireCollation(0x0409, false, 52) == 1252, "wire (0x0409, SortId 52) -> 1252");
	CHECK(CodePageForWireCollation(0x0409, false, 106) == 1251, "wire (0x0409, SortId 106) -> 1251");
	CHECK(CodePageForWireCollation(0x0411, false, 0) == 932, "wire (LCID 0x0411) -> 932");
	CHECK(CodePageForWireCollation(0x0409, true, 0) == CODE_PAGE_UTF8, "wire (0x0409, UTF-8) -> 65001");
	CHECK(CodePageForWireCollation(0x0409, false, 0) == 1252, "wire (0x0409) -> 1252");
	CHECK(CodePageForWireCollation(0x7FFFF, false, 0) == CODE_PAGE_NOT_FOUND, "an unknown LCID -> not found");
	CHECK(FindCodePage(CODE_PAGE_UTF8) == nullptr && FindCodePage(CODE_PAGE_UNICODE_ONLY) == nullptr &&
			  FindCodePage(1252) != nullptr && FindCodePage(1200) == nullptr,
		  "tables exist for code-page pages only");
}

//! Every collation finds itself by id, by name and by its upper-cased name, and its wire
//! key (LCID, the _UTF8 suffix, SortId = CollationId >> 24) gives its code page.
void TestEveryCollation() {
	int by_id = 0, by_name = 0, by_upper = 0, by_wire = 0;
	for (size_t i = 0; i < tables::COLLATION_COUNT; i++) {
		const CollationRow &row = tables::COLLATIONS[i];
		const size_t length = strlen(row.name);
		std::string upper(row.name);
		for (char &ch : upper) {
			ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
		}
		const bool utf8 = length >= 5 && strcmp(row.name + length - 5, "_UTF8") == 0;
		const uint8_t sort_id = static_cast<uint8_t>(static_cast<uint32_t>(row.collation_id) >> 24);
		by_id += FindCollationById(row.collation_id) != &row;
		by_name += FindCollationByName(row.name, length) != &row;
		by_upper += FindCollationByName(upper.data(), upper.size()) != &row;
		by_wire += CodePageForWireCollation(row.lcid, utf8, sort_id) != static_cast<int32_t>(row.code_page);
	}
	CHECK(tables::COLLATION_COUNT > 5000, "the collation table is short: " << tables::COLLATION_COUNT);
	CHECK(by_id == 0, by_id << " collations not found by CollationId");
	CHECK(by_name == 0, by_name << " collations not found by name");
	CHECK(by_upper == 0, by_upper << " collations not found by upper-cased name");
	CHECK(by_wire == 0, by_wire << " collations' wire keys give another code page");
}

//! What the single-byte kernels take for granted (the AVX-512 one tells a byte's length
//! from its top bit and its first UTF-8 byte alone): 0x00-0x7F read as themselves, and
//! 0x80-0xFF as 2 bytes (lead C2-DF) or 3 (lead E0-EF).
void TestSingleByteShape(const CodePageTable &page) {
	int bad = 0;
	for (unsigned b = 0; b < 0x100; b++) {
		const uint32_t packed = page.single[b];
		const unsigned length = packed >> 24, lead = packed & 0xFF;
		if (b < 0x80) {
			bad += packed != ((1u << 24) | b);
		} else {
			bad += !((length == 2 && lead >= 0xC2 && lead <= 0xDF) || (length == 3 && lead >= 0xE0 && lead <= 0xEF));
		}
	}
	CHECK(bad == 0, "cp" << page.code_page << ": " << bad << " bytes outside the single-byte kernels' shape");
}

}  // namespace

//! Every page through the kernels of one instruction set.
int RunPages(std::mt19937 &rng) {
	for (size_t i = 0; i < tables::CODE_PAGE_COUNT; i++) {
		const CodePageTable &page = tables::CODE_PAGES[i];
		const int cp = page.code_page;
		CHECK(static_cast<size_t>(page.index) == i, "cp" << cp << " index " << int(page.index) << ", at " << i);
		CHECK(FindCodePage(cp) == &page, "FindCodePage(" << cp << ")");
		Reference ref;
		if (!ref.Load(cp)) {
			std::cerr << "cannot read " << DATA << "cp" << cp << ".psv (run from the repository root)\n";
			return 2;
		}
		CHECK(IsDoubleByte(page) == ref.double_byte, "cp" << cp << " double-byte flag");
		if (!ref.double_byte) {
			TestSingleByteShape(page);
		}
		TestEveryByteAndPair(ref, page);
		TestBlocks(ref, page);
		if (ref.double_byte) {
			TestServerStrings(ref, page);
		}
		TestRandomStrings(ref, page, rng);
		TestRandomValues(ref, page, rng);
	}
	return 0;
}

int main() {
	namespace detail = duckdb::mssql::codec::code_page::detail;
	std::mt19937 rng(91);
	CHECK(tables::CODE_PAGE_COUNT == 16, "16 code pages, not " << tables::CODE_PAGE_COUNT);
	// The kernels of every instruction set this CPU runs: the best one, and the portable
	// scalar path, which a CPU without the SIMD one takes.
	const detail::Isa best = detail::DetectIsa();
	std::vector<detail::Isa> isas = {best};
	if (best == detail::Isa::Avx512) {
		isas.push_back(detail::Isa::Avx2);
	}
	if (best != detail::Isa::Scalar) {
		isas.push_back(detail::Isa::Scalar);
	}
	for (detail::Isa isa : isas) {
		CHECK(detail::ForceIsa(isa), "cannot force " << detail::IsaName(isa));
		std::cout << "kernels: " << detail::IsaName(isa) << "\n";
		const int rc = RunPages(rng);
		if (rc != 0) {
			return rc;
		}
	}
	detail::ForceIsa(best);
	TestCopyAscii(rng);
	TestLookups();
	TestEveryCollation();
	if (failures > 0) {
		std::cerr << "\n" << failures << " assertion(s) failed.\n";
		return 1;
	}
	std::cout << "test_code_page: all assertions passed\n";
	return 0;
}

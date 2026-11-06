// Spec 091 / 092 D12: one column of a 2048-row chunk, from the staging to a finished DuckDB
// vector, the way the read path does it today against the way a code-page column would.
// Manual target, not in `make test`:
//   make bench-code-page-column
//
// Both sides start from a ColumnStaging filled as the row stager fills it (not timed) and
// end with the chunk's VARCHAR vector holding every value (timed), on the same text:
//   today      the NVARCHAR path: the server's CAST sends UTF-16LE, staged with a U+0000
//              after each value, and codec::string::DecodeChunkFromStaging converts the
//              column in one simdutf call into the vector's heap, walks the output for the
//              delimiters and writes each string_t -- the production function itself.
//   column     the code-page bytes staged back to back, and code_page::DecodeColumn: one
//              pass of the kernel into the vector's heap, each string_t written from what
//              the kernel recorded (spec 092 D12).
//   values     code_page::DecodeValues (value by value) into the heap, then a loop over the
//              values writing each string_t: the two-loop form DecodeColumn replaces.
// Each cell is a page's text at one value length, interleaved rounds (the repository's
// rule: only same-session interleaved A/B counts), median of the rounds, after checking
// that every way's vector holds the same strings. Reported per chunk and per value, with
// the bytes each form puts on the wire. CELL=<text>/<lengths> (e.g. CELL=thai, CELL=/4-16)
// runs only the matching cells.

#include "codec/code_page/code_page.hpp"
#include "codec/code_page/code_page_internal.hpp"
#include "codec/staging/column_staging.hpp"
#include "codec/string_codec.hpp"
#include "tds/tds_column_metadata.hpp"
#include "tds/tds_types.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#include <simdutf.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace duckdb;
namespace code_page = duckdb::mssql::codec::code_page;
namespace staging = duckdb::mssql::codec::staging;

namespace {

constexpr size_t ROWS = STANDARD_VECTOR_SIZE;
constexpr int ROUNDS = 9;

// Text generators: n bytes of a page's text. Words of letters with spaces between, as the
// per-value benchmark's (bench_code_page.cpp) but cut to the length asked for.
using Gen = uint8_t (*)(std::mt19937 &);

uint8_t AsciiByte(std::mt19937 &rng) {
	return static_cast<uint8_t>('a' + rng() % 26);
}
// French-like in 1252: about 4% é è à ç ô.
uint8_t LatinByte(std::mt19937 &rng) {
	static const uint8_t accents[] = {0xE9, 0xE8, 0xE0, 0xE7, 0xF4};
	return rng() % 25 == 0 ? accents[rng() % 5] : AsciiByte(rng);
}
// ... and, 1 character in 40, a curly quote, a dash, an ellipsis or the euro sign (3 bytes).
uint8_t LatinTypoByte(std::mt19937 &rng) {
	static const uint8_t typo[] = {0x80, 0x85, 0x93, 0x94, 0x96, 0x97};
	return rng() % 40 == 0 ? typo[rng() % 6] : LatinByte(rng);
}
// Russian-like in 1251: Cyrillic letters C0-FF.
uint8_t CyrillicByte(std::mt19937 &rng) {
	return static_cast<uint8_t>(0xC0 + rng() % 64);
}
uint8_t CyrillicTypoByte(std::mt19937 &rng) {
	static const uint8_t typo[] = {0x85, 0x93, 0x94, 0x96, 0x97};
	return rng() % 40 == 0 ? typo[rng() % 5] : CyrillicByte(rng);
}
// Thai in 874: letters A1-DA, no spaces between words.
uint8_t ThaiByte(std::mt19937 &rng) {
	return static_cast<uint8_t>(0xA1 + rng() % 0x3A);
}

std::vector<uint8_t> Text(std::mt19937 &rng, Gen gen, size_t n, size_t word) {
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; i++) {
		v[i] = (i + 1) % word == 0 ? ' ' : gen(rng);
	}
	return v;
}

struct Cell {
	std::string name;
	const code_page::CodePageTable *page;
	size_t min_length, max_length;
	unsigned null_percent;
	staging::ColumnStaging cp;	 // the code-page bytes, back to back
	staging::ColumnStaging u16;	 // the same text as UTF-16LE, U+0000 after each value
	size_t utf8_bytes = 0;
	size_t cp_bytes = 0, u16_bytes = 0;
};

void Fill(Cell &c, Gen gen, size_t word, std::mt19937 &rng) {
	c.cp.Configure(staging::StagingKind::Var, 0);
	c.u16.Configure(staging::StagingKind::Var, 0);
	std::vector<char> utf8;
	std::vector<char16_t> utf16;
	for (size_t row = 0; row < ROWS; row++) {
		if (rng() % 100 < c.null_percent) {
			c.cp.AppendNull();
			c.u16.AppendNull();
			continue;
		}
		const size_t n = c.min_length + rng() % (c.max_length - c.min_length + 1);
		const std::vector<uint8_t> v = Text(rng, gen, n, word);
		c.cp.AppendVar(v.data(), static_cast<uint32_t>(v.size()));
		utf8.resize(code_page::Utf8Bound(v.size()));
		const size_t u8 = code_page::DecodeToUtf8(*c.page, v.data(), v.size(), utf8.data());
		utf16.resize(u8 + 1);
		const size_t units = simdutf::convert_utf8_to_utf16le(utf8.data(), u8, utf16.data());
		c.u16.AppendVarDelimited(reinterpret_cast<const uint8_t *>(utf16.data()), static_cast<uint32_t>(units * 2));
		c.utf8_bytes += u8;
		c.cp_bytes += v.size();
		c.u16_bytes += units * 2;
	}
}

tds::ColumnMetadata NVarcharColumn() {
	tds::ColumnMetadata col;
	col.type_id = tds::TDS_TYPE_NVARCHAR;
	return col;
}

void Today(const Cell &c, DataChunk &chunk) {
	chunk.Reset();
	static const tds::ColumnMetadata col = NVarcharColumn();
	mssql::codec::string::DecodeChunkFromStaging(c.u16, ROWS, col, chunk.data[0]);
	chunk.SetChildCardinality(ROWS);
}

void Column(const Cell &c, DataChunk &chunk) {
	chunk.Reset();
	Vector &out = chunk.data[0];
	string_t blob = StringVector::EmptyString(out, code_page::Utf8Bound(c.cp.PayloadSize()));
	code_page::DecodeColumn(*c.page, c.cp.buffer.data(), c.cp.PayloadSize(), c.cp.offsets.data(), c.cp.lengths.data(),
							c.cp.validity_words.data(), ROWS, blob.GetDataWriteable(),
							reinterpret_cast<code_page::StringView16 *>(FlatVector::GetDataMutable<string_t>(out)),
							string_t::INLINE_LENGTH);
	chunk.SetChildCardinality(ROWS);
}

// Value by value, then a loop over the values for the string_t (scratch for the offsets).
void Values(const Cell &c, DataChunk &chunk, std::vector<uint64_t> &out_offsets, std::vector<uint32_t> &out_lengths) {
	chunk.Reset();
	Vector &out = chunk.data[0];
	string_t blob = StringVector::EmptyString(out, code_page::Utf8Bound(c.cp.PayloadSize()));
	char *dst = blob.GetDataWriteable();
	// DecodeValues takes every row: a NULL row's slots are whatever Configure left (zero).
	code_page::DecodeValues(*c.page, c.cp.buffer.data(), c.cp.PayloadSize(), c.cp.offsets.data(), c.cp.lengths.data(),
							ROWS, dst, out_offsets.data(), out_lengths.data());
	string_t *result = FlatVector::GetDataMutable<string_t>(out);
	for (size_t row = 0; row < ROWS; row++) {
		if (c.cp.IsValid(row)) {
			result[row] = string_t(dst + out_offsets[row], out_lengths[row]);
		}
	}
	chunk.SetChildCardinality(ROWS);
}

bool SameStrings(const Cell &c, DataChunk &a, DataChunk &b, const char *what) {
	const string_t *x = FlatVector::GetData<string_t>(a.data[0]);
	const string_t *y = FlatVector::GetData<string_t>(b.data[0]);
	for (size_t row = 0; row < ROWS; row++) {
		if (!c.cp.IsValid(row)) {
			continue;
		}
		if (!(x[row] == y[row]) || x[row].GetString() != y[row].GetString()) {
			std::fprintf(stderr, "%s: %s differs from today at row %zu\n", c.name.c_str(), what, row);
			return false;
		}
	}
	return true;
}

volatile size_t sink;

template <class F>
double TimeIt(F &&f, int reps) {
	const auto t0 = std::chrono::steady_clock::now();
	for (int r = 0; r < reps; r++) {
		f();
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
	namespace detail = code_page::detail;
	if (const char *want = std::getenv("CODE_PAGE_ISA")) {
		const detail::Isa isas[] = {detail::Isa::Scalar, detail::Isa::Neon, detail::Isa::Avx2, detail::Isa::Avx512};
		bool found = false;
		for (detail::Isa isa : isas) {
			if (std::strcmp(want, detail::IsaName(isa)) == 0) {
				found = detail::ForceIsa(isa);
			}
		}
		if (!found) {
			std::fprintf(stderr, "CODE_PAGE_ISA=%s: not supported here\n", want);
			return 2;
		}
	}
	std::printf("kernels: %s; %zu rows a chunk, median of %d interleaved rounds\n",
				detail::IsaName(detail::ActiveIsa()), ROWS, ROUNDS);
	struct Spec {
		const char *name;
		int page;
		Gen gen;
		size_t word;
	};
	const Spec specs[] = {{"ascii", 1252, AsciiByte, 8},
						  {"latin 1252", 1252, LatinByte, 7},
						  {"1252 + typography", 1252, LatinTypoByte, 7},
						  {"cyrillic 1251", 1251, CyrillicByte, 7},
						  {"1251 + typography", 1251, CyrillicTypoByte, 7},
						  {"thai 874", 874, ThaiByte, 25}};
	struct Lengths {
		const char *name;
		size_t lo, hi;
	};
	const Lengths lengths[] = {{"4-16", 4, 16}, {"20-60", 20, 60}, {"200-1000", 200, 1000}};
	std::printf("%-20s %-9s %5s | %10s %10s %10s | %8s %8s %8s | %6s | %s\n", "text", "bytes", "nulls", "today us",
				"column us", "values us", "today", "column", "values", "x", "wire cp / utf16");
	std::vector<uint64_t> out_offsets(ROWS);
	std::vector<uint32_t> out_lengths(ROWS);
	for (const Spec &spec : specs) {
		for (const Lengths &len : lengths) {
			for (unsigned nulls : {0u, 10u}) {
				// CELL=<text>/<lengths> runs only the cells whose name contains it.
				const std::string cell = std::string(spec.name) + "/" + len.name;
				if (std::getenv("CELL") && cell.find(std::getenv("CELL")) == std::string::npos) {
					continue;
				}
				// Each cell's own seed, so a CELL= run decodes the same text as the full run.
				std::mt19937 rng(static_cast<uint32_t>(std::hash<std::string>()(cell)) + nulls);
				Cell c;
				c.name = spec.name;
				c.page = code_page::FindCodePage(spec.page);
				c.min_length = len.lo;
				c.max_length = len.hi;
				c.null_percent = nulls;
				Fill(c, spec.gen, spec.word, rng);
				DataChunk a, b, v;
				a.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
				b.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
				v.Initialize(Allocator::DefaultAllocator(), {LogicalType::VARCHAR});
				Today(c, a);
				Column(c, b);
				Values(c, v, out_offsets, out_lengths);
				if (!SameStrings(c, a, b, "column") || !SameStrings(c, a, v, "values")) {
					return 1;
				}
				const size_t per_value = (len.lo + len.hi) / 2;
				const int reps = per_value > 100 ? 40 : 300;
				std::vector<double> t_today, t_column, t_values;
				for (int round = 0; round < ROUNDS; round++) {
					t_today.push_back(TimeIt([&] { Today(c, a); }, reps));
					t_column.push_back(TimeIt([&] { Column(c, b); }, reps));
					t_values.push_back(TimeIt([&] { Values(c, v, out_offsets, out_lengths); }, reps));
				}
				const double today = Median(t_today), column = Median(t_column), values = Median(t_values);
				std::printf("%-20s %-9s %4u%% | %10.1f %10.1f %10.1f | %8.2f %8.2f %8.2f | %6.2f | %zu / %zu\n",
							spec.name, len.name, nulls, today / 1000, column / 1000, values / 1000, today / ROWS,
							column / ROWS, values / ROWS, today / column, c.cp_bytes, c.u16_bytes);
			}
		}
	}
	return 0;
}

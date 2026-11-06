// SQL Server code pages (spec 091): the AVX2 kernel for single-byte pages. Every function
// here is compiled for AVX2 + BMI2 by its own target attribute, so nothing in this file is
// reached on a CPU without them: code_page.cpp calls in only when detail::ActiveIsa() says
// AVX2. See codec/code_page/code_page.hpp for the contract.
//
// 32 bytes a step, the first tier that holds:
//   A  every byte ASCII                    -> stored as it is;
//   B  1252 / 1251 / 874, every non-ASCII  -> the page's arithmetic (Latin-1 for 1252 A0-FF,
//      byte in the page's letter range        U+0410+ for 1251 C0-FF, Thai for 874) and the
//                                             simdutf-style compaction;
//   C  anything else, any page             -> each byte's packed UTF-8 [b0 b1 b2 length] by
//                                             vpgatherdd, 8 bytes a gather, packed 4
//                                             characters a pshufb through Compaction3.
// The tail under 32 bytes goes through the packed table a byte at a time.

#include "codec/code_page/code_page_internal.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#define MSSQL_AVX2
#else
#define MSSQL_AVX2 __attribute__((target("avx2,bmi2")))
#endif

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {
namespace detail {

namespace {

constexpr size_t STEP = 32;

//! The scalar tail: each byte's packed UTF-8 from the table, one 4-byte store.
inline char *Tail(const uint32_t *single, const uint8_t *p, const uint8_t *end, char *d) {
	for (; p < end; ++p) {
		const uint32_t packed = single[*p];
		memcpy(d, &packed, sizeof(packed));
		d += packed >> 24;
	}
	return d;
}

//! Two 16-byte rows as one 256-bit register, low lane first.
MSSQL_AVX2 inline __m256i Rows(const uint8_t *low, const uint8_t *high) {
	return _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i *>(low))),
								   _mm_loadu_si128(reinterpret_cast<const __m128i *>(high)), 1);
}

MSSQL_AVX2 inline void Store16(char *d, __m128i v) {
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d), v);
}

//! Pack 32 characters of at most 2 UTF-8 bytes, given their first / second byte planes and
//! the 32-bit mask of the ones with a second byte: four 8-character groups through
//! Compaction, two per pshufb (one per 128-bit lane).
MSSQL_AVX2 inline char *Pack2(const Compaction &c, __m256i first, __m256i second, uint32_t multi, char *d) {
	// lo: chars 0-7 (lane 0) and 16-23 (lane 1); hi: chars 8-15 and 24-31.
	const __m256i lo = _mm256_unpacklo_epi8(first, second), hi = _mm256_unpackhi_epi8(first, second);
	const unsigned m0 = multi & 0xFF, m1 = (multi >> 8) & 0xFF, m2 = (multi >> 16) & 0xFF, m3 = multi >> 24;
	const __m256i out_lo = _mm256_shuffle_epi8(lo, Rows(c.shuffle[m0], c.shuffle[m2]));
	const __m256i out_hi = _mm256_shuffle_epi8(hi, Rows(c.shuffle[m1], c.shuffle[m3]));
	Store16(d, _mm256_castsi256_si128(out_lo));
	d += c.length[m0];
	Store16(d, _mm256_castsi256_si128(out_hi));
	d += c.length[m1];
	Store16(d, _mm256_extracti128_si256(out_lo, 1));
	d += c.length[m2];
	Store16(d, _mm256_extracti128_si256(out_hi, 1));
	return d + c.length[m3];
}

//! Pack 32 characters of 1-3 UTF-8 bytes from three byte planes: a length code (0, 1, 2)
//! per character from its first byte, eight 4-character groups of 32-bit lanes
//! [b0 b1 b2 0], each through Compaction3.
MSSQL_AVX2 inline char *Pack3(const Compaction3 &c, __m256i first, __m256i second, __m256i third, char *d) {
	const __m256i sign = _mm256_set1_epi8(static_cast<char>(0x80));
	const __m256i biased = _mm256_xor_si256(first, sign);					  // unsigned order as signed
	const __m256i multi = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x3F));  // first >= C0
	const __m256i three = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x5F));  // first >= E0
	const __m256i code = _mm256_sub_epi8(_mm256_setzero_si256(), _mm256_add_epi8(multi, three));
	// c0 + 4 c1 + 16 c2 + 64 c3 per 4 characters: pairs by maddubs, then pairs of pairs.
	const __m256i pairs = _mm256_maddubs_epi16(code, _mm256_set1_epi32(0x40100401));
	uint32_t index[8];
	_mm256_storeu_si256(reinterpret_cast<__m256i *>(index), _mm256_madd_epi16(pairs, _mm256_set1_epi16(1)));
	const __m256i fs_lo = _mm256_unpacklo_epi8(first, second), fs_hi = _mm256_unpackhi_epi8(first, second);
	const __m256i zero = _mm256_setzero_si256();
	const __m256i t_lo = _mm256_unpacklo_epi8(third, zero), t_hi = _mm256_unpackhi_epi8(third, zero);
	// Group g (lane 0) holds chars 4g..4g+3, (lane 1) chars 16+4g..; index[] is in char order.
	const __m256i groups[4] = {_mm256_unpacklo_epi16(fs_lo, t_lo), _mm256_unpackhi_epi16(fs_lo, t_lo),
							   _mm256_unpacklo_epi16(fs_hi, t_hi), _mm256_unpackhi_epi16(fs_hi, t_hi)};
	__m256i out[4];
	for (unsigned g = 0; g < 4; g++) {
		out[g] = _mm256_shuffle_epi8(groups[g], Rows(c.shuffle[index[g]], c.shuffle[index[4 + g]]));
	}
	for (unsigned g = 0; g < 4; g++) {
		Store16(d, _mm256_castsi256_si128(out[g]));
		d += c.length[index[g]];
	}
	for (unsigned g = 0; g < 4; g++) {
		Store16(d, _mm256_extracti128_si256(out[g], 1));
		d += c.length[index[4 + g]];
	}
	return d;
}

//! Tier C: 32 bytes through the packed table by gathers. Each gathered dword is a byte's
//! [b0 b1 b2 length], so a 128-bit lane already holds 4 characters as Compaction3 wants
//! them; the length code is length - 1.
MSSQL_AVX2 inline char *Gathered(const Compaction3 &c, const uint32_t *single, const uint8_t *p, char *d) {
	const __m256i shifts = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
	const __m256i one = _mm256_set1_epi32(1);
	for (unsigned k = 0; k < 4; k++) {
		const __m256i bytes = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(p + 8 * k)));
		const __m256i packed = _mm256_i32gather_epi32(reinterpret_cast<const int *>(single), bytes, 4);
		const __m256i code = _mm256_sub_epi32(_mm256_srli_epi32(packed, 24), one);
		const __m256i weighted = _mm256_sllv_epi32(code, shifts);
		const __m256i sum2 = _mm256_hadd_epi32(weighted, weighted);
		const __m256i sum4 = _mm256_hadd_epi32(sum2, sum2);	 // dword 0 of each lane
		const unsigned lo = static_cast<unsigned>(_mm256_extract_epi32(sum4, 0));
		const unsigned hi = static_cast<unsigned>(_mm256_extract_epi32(sum4, 4));
		const __m256i out = _mm256_shuffle_epi8(packed, Rows(c.shuffle[lo], c.shuffle[hi]));
		Store16(d, _mm256_castsi256_si128(out));
		d += c.length[lo];
		Store16(d, _mm256_extracti128_si256(out, 1));
		d += c.length[hi];
	}
	return d;
}

enum class Page { Generic, Cp1252, Cp1251, Cp874 };

template <Page PAGE>
MSSQL_AVX2 char *Kernel(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d) {
	const Compaction &c2 = GetCompaction();
	const Compaction3 &c3 = GetCompaction3();
	const uint32_t *single = page.single;
	const __m256i sign = _mm256_set1_epi8(static_cast<char>(0x80));
	const __m256i k3f = _mm256_set1_epi8(0x3F);
	while (static_cast<size_t>(end - p) >= STEP) {
		const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
		const uint32_t high = static_cast<uint32_t>(_mm256_movemask_epi8(v));  // the non-ASCII bytes
		if (high == 0) {
			_mm256_storeu_si256(reinterpret_cast<__m256i *>(d), v);
			p += STEP;
			d += STEP;
			continue;
		}
		const __m256i biased = _mm256_xor_si256(v, sign);  // unsigned order as signed
		if (PAGE == Page::Cp1252) {
			// A0-FF: biased >= 0x20. Every non-ASCII byte there -> Latin-1, all 2-byte.
			const __m256i latin = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x1F));
			if (static_cast<uint32_t>(_mm256_movemask_epi8(latin)) == high) {
				const __m256i lead = _mm256_add_epi8(_mm256_set1_epi8(static_cast<char>(0xC2)),
													 _mm256_and_si256(_mm256_srli_epi16(v, 6), _mm256_set1_epi8(1)));
				const __m256i first = _mm256_blendv_epi8(v, lead, v);  // the sign bit marks non-ASCII
				const __m256i second = _mm256_or_si256(_mm256_and_si256(v, k3f), sign);
				d = Pack2(c2, first, second, high, d);
				p += STEP;
				continue;
			}
		} else if (PAGE == Page::Cp1251) {
			// C0-FF: biased >= 0x40 -> U+0410 + (byte - C0): D0 / D1 (byte >= F0), then
			// 0x80 | ((byte + 0x10) & 0x3F).
			const __m256i letters = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x3F));
			if (static_cast<uint32_t>(_mm256_movemask_epi8(letters)) == high) {
				const __m256i upper = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x6F));  // >= F0
				const __m256i lead = _mm256_sub_epi8(_mm256_set1_epi8(static_cast<char>(0xD0)), upper);
				const __m256i first = _mm256_blendv_epi8(v, lead, v);
				const __m256i second =
					_mm256_or_si256(_mm256_and_si256(_mm256_add_epi8(v, _mm256_set1_epi8(0x10)), k3f), sign);
				d = Pack2(c2, first, second, high, d);
				p += STEP;
				continue;
			}
		} else if (PAGE == Page::Cp874) {
			// A1-FB except DB-DE -> U+0E01-U+0E5B: E0, B8 / B9 (byte >= E0), then
			// 0x80 | ((byte + 0x20) & 0x3F).
			const __m256i in_range = _mm256_and_si256(_mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x20)),
													  _mm256_cmpgt_epi8(_mm256_set1_epi8(0x7C), biased));
			const __m256i hole = _mm256_and_si256(_mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x5A)),
												  _mm256_cmpgt_epi8(_mm256_set1_epi8(0x5F), biased));
			const __m256i thai = _mm256_andnot_si256(hole, in_range);
			if (static_cast<uint32_t>(_mm256_movemask_epi8(thai)) == high) {
				const __m256i upper = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x5F));  // >= E0
				const __m256i first = _mm256_blendv_epi8(v, _mm256_set1_epi8(static_cast<char>(0xE0)), v);
				const __m256i second = _mm256_sub_epi8(_mm256_set1_epi8(static_cast<char>(0xB8)), upper);
				const __m256i third =
					_mm256_or_si256(_mm256_and_si256(_mm256_add_epi8(v, _mm256_set1_epi8(0x20)), k3f), sign);
				d = Pack3(c3, first, second, third, d);
				p += STEP;
				continue;
			}
		}
		d = Gathered(c3, single, p, d);
		p += STEP;
	}
	return Tail(single, p, end, d);
}

}  // namespace

MSSQL_AVX2 char *DecodeSingleByteAvx2(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d) {
	switch (page.code_page) {
	case 1252:
		return Kernel<Page::Cp1252>(page, p, end, d);
	case 1251:
		return Kernel<Page::Cp1251>(page, p, end, d);
	case 874:
		return Kernel<Page::Cp874>(page, p, end, d);
	default:
		return Kernel<Page::Generic>(page, p, end, d);
	}
}

}  // namespace detail
}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

#endif

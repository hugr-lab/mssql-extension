// SQL Server code pages (spec 091): the AVX2 kernel for single-byte pages. Every function
// here is compiled for AVX2 + BMI2 + POPCNT by its own target attribute, so nothing in this file is
// reached on a CPU without them: code_page.cpp calls in only when detail::ActiveIsa() says
// AVX2. See codec/code_page/code_page.hpp for the contract.
//
// 32 bytes a step, the first tier that holds:
//   A  every byte ASCII                    -> stored as it is;
//   B  1252 / 1251 / 874, every non-ASCII  -> the page's arithmetic (Latin-1 for 1252 A0-FF,
//      byte in the page's letter range        U+0410+ for 1251 C0-FF, Thai for 874) and the
//                                             simdutf-style compaction;
//   W  1252 / 1251, the rest               -> the arithmetic, and the bytes outside the run
//                                             (1252 80-9F, 1251 80-BF) from a window of the
//                                             page's byte planes: 2 / 4 vpshufb tables;
//   C  anything else, any page             -> the byte planes' 128 entries: 8 vpshufb tables
//                                             and a blend tree, the third plane only for a
//                                             step holding a 3-byte character.
// Packed by Compaction (1/2-byte steps) or Compaction3. No gather: vpgatherdd measured 3x
// slower than these tables on AMD Zen 2/3 (and no faster on Intel before Ice Lake).
// The tail under 32 bytes goes through the packed table a byte at a time.

#include "codec/code_page/code_page_internal.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstring>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define MSSQL_AVX2
#else
#define MSSQL_AVX2 __attribute__((target("avx2,bmi2,popcnt")))
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

//! DecodeColumn's record of 32 bytes at `at`: which read as 2+ UTF-8 bytes (bit i for byte
//! i) and which as 3, as the two 16-byte entries StepRecorder documents, one 64-bit store.
inline void Record32(StepRecorder rec, const uint8_t *at, uint32_t multi, uint32_t three) {
	const uint64_t pair = static_cast<uint64_t>((multi & 0xFFFFu) | (three << 16)) |
						  (static_cast<uint64_t>((multi >> 16) | (three & 0xFFFF0000u)) << 32);
	memcpy(rec.masks + rec.Index(at), &pair, sizeof(pair));
}

//! The tail under 32 bytes, recorded (DecodeColumn): its masks from the packed table.
inline char *TailRecorded(const uint32_t *single, const uint8_t *p, const uint8_t *end, char *d, StepRecorder rec) {
	if (p == end) {
		return d;
	}
	const uint8_t *from = p;
	uint32_t multi = 0, three = 0;
	for (unsigned i = 0; p < end; ++p, ++i) {
		const uint32_t packed = single[*p];
		memcpy(d, &packed, sizeof(packed));
		const unsigned length = packed >> 24;
		multi |= static_cast<uint32_t>(length > 1) << i;
		three |= static_cast<uint32_t>(length > 2) << i;
		d += length;
	}
	Record32(rec, from, multi, three);
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

//! A page's three UTF-8 byte planes of 0x80-0xFF as vpshufb tables: table h of a plane holds
//! the bytes 0x80 + 16 h .. + 15, in both 128-bit lanes (vpshufb looks up within a lane).
struct Planes256 {
	uint8_t first[8][32];
	uint8_t second[8][32];
	uint8_t third[8][32];
};

const Planes256 &PlanesFor(const CodePageTable &page) {
	static const std::vector<Planes256> planes = [] {
		std::vector<Planes256> all(tables::CODE_PAGE_COUNT);
		for (size_t i = 0; i < tables::CODE_PAGE_COUNT; i++) {
			for (unsigned b = 0; b < 128; b++) {
				const uint32_t packed = tables::CODE_PAGES[i].single[0x80 + b];
				for (unsigned lane = 0; lane < 2; lane++) {
					all[i].first[b / 16][16 * lane + b % 16] = static_cast<uint8_t>(packed & 0xFF);
					all[i].second[b / 16][16 * lane + b % 16] = static_cast<uint8_t>((packed >> 8) & 0xFF);
					all[i].third[b / 16][16 * lane + b % 16] = static_cast<uint8_t>((packed >> 16) & 0xFF);
				}
			}
		}
		return all;
	}();
	return planes[page.index];
}

//! idx's bits 4, 5 and 6 moved to bit 7, where vpblendvb reads its choice: the blend tree's
//! selectors (idx = byte - 0x80 = the byte with its top bit flipped).
struct Selectors {
	__m256i bit4, bit5, bit6;
};

MSSQL_AVX2 inline Selectors SelectorsOf(__m256i idx) {
	Selectors s;
	s.bit4 = _mm256_slli_epi16(idx, 3);
	s.bit5 = _mm256_slli_epi16(idx, 2);
	s.bit6 = _mm256_slli_epi16(idx, 1);
	return s;
}

//! The first TABLES (2, 4 or 8) 16-entry tables of a plane looked up by idx: vpshufb in each,
//! then a blend tree on idx's bits 4-6. An idx with bit 7 set (an ASCII byte) reads 0 from
//! every table; with fewer than 8 tables, an idx past them reads some table: the caller
//! masks it.
template <unsigned TABLES>
MSSQL_AVX2 inline __m256i Lookup(const uint8_t (*plane)[32], __m256i idx, const Selectors &sel) {
	__m256i t[TABLES];
	for (unsigned h = 0; h < TABLES; h++) {
		t[h] = _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(plane[h])), idx);
	}
	const __m256i r01 = _mm256_blendv_epi8(t[0], t[1], sel.bit4);
	if (TABLES == 2) {
		return r01;
	}
	const __m256i r03 = _mm256_blendv_epi8(r01, _mm256_blendv_epi8(t[2 % TABLES], t[3 % TABLES], sel.bit4), sel.bit5);
	if (TABLES == 4) {
		return r03;
	}
	const __m256i r45 = _mm256_blendv_epi8(t[4 % TABLES], t[5 % TABLES], sel.bit4);
	const __m256i r67 = _mm256_blendv_epi8(t[6 % TABLES], t[7 % TABLES], sel.bit4);
	return _mm256_blendv_epi8(r03, _mm256_blendv_epi8(r45, r67, sel.bit5), sel.bit6);
}

//! Pack a step from its first / second planes (and the third, looked up only when a 3-byte
//! character is there), recording its masks for DecodeColumn.
template <unsigned TABLES, bool REC>
MSSQL_AVX2 inline char *PackPlanes(const Compaction &c2, const Compaction3 &c3, const Planes256 &planes, __m256i first,
								   __m256i second, __m256i idx, const Selectors &sel, uint32_t high, const uint8_t *p,
								   char *d, StepRecorder rec) {
	const __m256i sign = _mm256_set1_epi8(static_cast<char>(0x80));
	const uint32_t three = static_cast<uint32_t>(
		_mm256_movemask_epi8(_mm256_cmpgt_epi8(_mm256_xor_si256(first, sign), _mm256_set1_epi8(0x5F))));  // >= E0
	if (REC) {
		Record32(rec, p, high, three);
	}
	if (three == 0) {
		return Pack2(c2, first, second, high, d);
	}
	// Past a window the third plane reads some table's byte: Compaction3 keeps a third byte
	// only for a 3-byte first byte (>= E0), which the window's bytes alone have.
	return Pack3(c3, first, second, Lookup<TABLES>(planes.third, idx, sel), d);
}

enum class Page { Generic, Cp1252, Cp1251, Cp874 };

//! REC: record each 32 bytes' masks for DecodeColumn. Every byte >= 0x80 reads as 2 or 3
//! UTF-8 bytes (test_code_page checks the tables), so the multi-byte mask is the high bits.
template <Page PAGE, bool REC>
MSSQL_AVX2 char *Kernel(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d, StepRecorder rec) {
	const Compaction &c2 = GetCompaction();
	const Compaction3 &c3 = GetCompaction3();
	const Planes256 &planes = PlanesFor(page);
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
				if (REC) {
					Record32(rec, p, high, 0);
				}
				d = Pack2(c2, first, second, high, d);
				p += STEP;
				continue;
			}
			// Tier W: Latin-1 for A0-FF, the 80-9F window (tables 0-1) for the rest.
			const __m256i lead = _mm256_add_epi8(_mm256_set1_epi8(static_cast<char>(0xC2)),
												 _mm256_and_si256(_mm256_srli_epi16(v, 6), _mm256_set1_epi8(1)));
			const __m256i window = _mm256_andnot_si256(latin, _mm256_cmpgt_epi8(_mm256_setzero_si256(), v));
			const Selectors sel = SelectorsOf(biased);
			const __m256i first =
				_mm256_blendv_epi8(_mm256_blendv_epi8(v, lead, v), Lookup<2>(planes.first, biased, sel), window);
			const __m256i second = _mm256_blendv_epi8(_mm256_or_si256(_mm256_and_si256(v, k3f), sign),
													  Lookup<2>(planes.second, biased, sel), window);
			d = PackPlanes<2, REC>(c2, c3, planes, first, second, biased, sel, high, p, d, rec);
			p += STEP;
			continue;
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
				if (REC) {
					Record32(rec, p, high, 0);
				}
				d = Pack2(c2, first, second, high, d);
				p += STEP;
				continue;
			}
			// Tier W: the letters by arithmetic, the 80-BF window (tables 0-3) for the rest.
			const __m256i upper = _mm256_cmpgt_epi8(biased, _mm256_set1_epi8(0x6F));  // >= F0
			const __m256i lead = _mm256_sub_epi8(_mm256_set1_epi8(static_cast<char>(0xD0)), upper);
			const __m256i window = _mm256_andnot_si256(letters, _mm256_cmpgt_epi8(_mm256_setzero_si256(), v));
			const Selectors sel = SelectorsOf(biased);
			const __m256i first =
				_mm256_blendv_epi8(_mm256_blendv_epi8(v, lead, v), Lookup<4>(planes.first, biased, sel), window);
			const __m256i second = _mm256_blendv_epi8(
				_mm256_or_si256(_mm256_and_si256(_mm256_add_epi8(v, _mm256_set1_epi8(0x10)), k3f), sign),
				Lookup<4>(planes.second, biased, sel), window);
			d = PackPlanes<4, REC>(c2, c3, planes, first, second, biased, sel, high, p, d, rec);
			p += STEP;
			continue;
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
				if (REC) {
					Record32(rec, p, high, high);  // Thai: every non-ASCII byte 3 bytes
				}
				d = Pack3(c3, first, second, third, d);
				p += STEP;
				continue;
			}
		}
		// Tier C: the planes' 128 entries for every non-ASCII byte.
		const Selectors sel = SelectorsOf(biased);
		const __m256i first = _mm256_blendv_epi8(v, Lookup<8>(planes.first, biased, sel), v);
		const __m256i second = Lookup<8>(planes.second, biased, sel);
		d = PackPlanes<8, REC>(c2, c3, planes, first, second, biased, sel, high, p, d, rec);
		p += STEP;
	}
	return REC ? TailRecorded(single, p, end, d, rec) : Tail(single, p, end, d);
}

template <bool REC>
MSSQL_AVX2 char *KernelFor(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d, StepRecorder rec) {
	switch (page.code_page) {
	case 1252:
		return Kernel<Page::Cp1252, REC>(page, p, end, d, rec);
	case 1251:
		return Kernel<Page::Cp1251, REC>(page, p, end, d, rec);
	case 874:
		return Kernel<Page::Cp874, REC>(page, p, end, d, rec);
	default:
		return Kernel<Page::Generic, REC>(page, p, end, d, rec);
	}
}

}  // namespace

MSSQL_AVX2 char *DecodeSingleByteAvx2(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d) {
	const StepRecorder none = {nullptr, nullptr};
	return KernelFor<false>(page, p, end, d, none);
}

MSSQL_AVX2 char *DecodeSingleByteAvx2(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d,
									  StepRecorder recorder) {
	return KernelFor<true>(page, p, end, d, recorder);
}

MSSQL_AVX2 void StartsFromMasksX86(const uint32_t *masks, size_t granules, uint16_t *starts) {
	unsigned extra = 0;
	for (size_t g = 0; g < granules; g++) {
		starts[g] = static_cast<uint16_t>(16 * g + extra);
		extra += static_cast<unsigned>(_mm_popcnt_u32(masks[g]));
	}
}

}  // namespace detail
}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

#endif

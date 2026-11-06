// SQL Server code pages (spec 091): the AVX-512 VBMI2 kernel for single-byte pages. Every
// function here is compiled for AVX-512 (F, BW, VBMI, VBMI2) + BMI2 + POPCNT by its own
// target attribute and reached only when detail::ActiveIsa() says AVX512. See
// codec/code_page/code_page.hpp for the contract.
//
// One kernel for every single-byte page, 64 bytes a step:
//   - all ASCII: stored as it is;
//   - otherwise the UTF-8 bytes of 0x80-0xFF from three 128-entry planes (first, second,
//     third byte), each one vpermi2b over two 64-byte registers -- the whole high half of a
//     page in one instruction a plane, so no page needs a special case;
//   - packed by vpcompressb, a 32-character half at a time: the planes interleaved as
//     (first, second) pairs, or as (first, second, third) triplets for a half that holds a
//     3-byte character, compressed under a mask that keeps every first byte, a multi-byte
//     character's second and a 3-byte one's third (pdep spreads the character masks onto
//     the byte positions);
//   - the step under 64 bytes the same way, loaded and stored under a mask: no scalar tail,
//     no byte read past the input or written past the output, so a value of any length
//     goes through the kernel.
// Every single-byte page reads 0x00-0x7F as ASCII and 0x80-0xFF as 2 or 3 UTF-8 bytes
// (checked over the generated tables by test_code_page), so a byte's top bit says whether
// it has a second byte and its first byte (>= 0xE0) whether it has a third.

#include "codec/code_page/code_page_internal.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <immintrin.h>

#include <cstring>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define MSSQL_AVX512
#define MSSQL_AVX512_INLINE __forceinline
#else
#define MSSQL_AVX512 __attribute__((target("avx512f,avx512bw,avx512vbmi,avx512vbmi2,bmi,bmi2,popcnt")))
#define MSSQL_AVX512_INLINE \
	inline __attribute__((always_inline, target("avx512f,avx512bw,avx512vbmi,avx512vbmi2,bmi,bmi2,popcnt")))
#endif

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {
namespace detail {

namespace {

constexpr size_t STEP = 64;

//! A page's three UTF-8 byte planes of 0x80-0xFF, each as vpermi2b's two 64-byte tables
//! (80-BF, C0-FF) indexed by the byte's low 7 bits. Loaded unaligned: a vector of these is
//! not over-aligned before C++17.
struct Planes512 {
	uint8_t first[128];
	uint8_t second[128];
	uint8_t third[128];
};

const Planes512 &PlanesFor(const CodePageTable &page) {
	static const std::vector<Planes512> planes = [] {
		std::vector<Planes512> all(tables::CODE_PAGE_COUNT);
		for (size_t i = 0; i < tables::CODE_PAGE_COUNT; i++) {
			for (size_t b = 0; b < 128; b++) {
				const uint32_t packed = tables::CODE_PAGES[i].single[0x80 + b];
				all[i].first[b] = static_cast<uint8_t>(packed & 0xFF);
				all[i].second[b] = static_cast<uint8_t>((packed >> 8) & 0xFF);
				all[i].third[b] = static_cast<uint8_t>((packed >> 16) & 0xFF);
			}
		}
		return all;
	}();
	return planes[page.index];
}

//! vpermi2b index vectors, the same for every page.
//!   pairs[h]: (first, second) of characters [32h, 32h + 32): byte 2j is first[32h + j]
//!     (table a), 2j + 1 is second[32h + j] (table b, bit 6).
//!   triplets[g]: (first, second, third) of characters [16g, 16g + 16), from the pair vector
//!     of their half (table a) and the third plane (table b): byte 3j / 3j + 1 the pair of
//!     character 16g + j, 3j + 2 its third byte. Bytes 48-63 unused.
struct Indices {
	uint8_t pairs[2][64];
	uint8_t triplets[4][64];
	Indices() {
		for (unsigned h = 0; h < 2; h++) {
			for (unsigned j = 0; j < 32; j++) {
				pairs[h][2 * j] = static_cast<uint8_t>(32 * h + j);
				pairs[h][2 * j + 1] = static_cast<uint8_t>(64 + 32 * h + j);
			}
		}
		for (unsigned g = 0; g < 4; g++) {
			for (unsigned j = 0; j < 16; j++) {
				const unsigned in_pairs = 2 * (16 * (g % 2) + j);
				triplets[g][3 * j] = static_cast<uint8_t>(in_pairs);
				triplets[g][3 * j + 1] = static_cast<uint8_t>(in_pairs + 1);
				triplets[g][3 * j + 2] = static_cast<uint8_t>(64 + 16 * g + j);
			}
			for (unsigned j = 48; j < 64; j++) {
				triplets[g][j] = 0;
			}
		}
	}
};

const Indices &GetIndices() {
	static const Indices indices;
	return indices;
}

//! A page prepared for the kernel: what stays in registers across a DecodeValues loop.
struct Kernel512 {
	__m512i first_lo, first_hi, second_lo, second_hi, third_lo, third_hi;
	__m512i pairs[2];
	__m512i triplets[4];
};

MSSQL_AVX512_INLINE Kernel512 Prepare(const CodePageTable &page) {
	const Planes512 &planes = PlanesFor(page);
	const Indices &indices = GetIndices();
	Kernel512 k;
	k.first_lo = _mm512_loadu_si512(planes.first);
	k.first_hi = _mm512_loadu_si512(planes.first + 64);
	k.second_lo = _mm512_loadu_si512(planes.second);
	k.second_hi = _mm512_loadu_si512(planes.second + 64);
	k.third_lo = _mm512_loadu_si512(planes.third);
	k.third_hi = _mm512_loadu_si512(planes.third + 64);
	for (unsigned h = 0; h < 2; h++) {
		k.pairs[h] = _mm512_loadu_si512(indices.pairs[h]);
	}
	for (unsigned g = 0; g < 4; g++) {
		k.triplets[g] = _mm512_loadu_si512(indices.triplets[g]);
	}
	return k;
}

//! Store the first `n` (<= 64) bytes of v at d, and nothing past them.
MSSQL_AVX512_INLINE char *StoreN(char *d, __m512i v, uint64_t n) {
	_mm512_mask_storeu_epi8(d, _cvtu64_mask64(_bzhi_u64(~0ull, static_cast<unsigned>(n))), v);
	return d + n;
}

//! DecodeColumn's record of the 64 bytes at `at`: their multi-byte and 3-byte masks as the
//! four 16-byte entries StepRecorder documents (each entry: 16 bits of each), two stores.
MSSQL_AVX512_INLINE void Record64(const StepRecorder &rec, const uint8_t *at, uint64_t multi, uint64_t three) {
	const uint64_t MULTI_SLOTS = 0x0000FFFF0000FFFFull, THREE_SLOTS = 0xFFFF0000FFFF0000ull;
	const uint64_t entries[2] = {_pdep_u64(multi, MULTI_SLOTS) | _pdep_u64(three, THREE_SLOTS),
								 _pdep_u64(multi >> 32, MULTI_SLOTS) | _pdep_u64(three >> 32, THREE_SLOTS)};
	memcpy(rec.masks + rec.Index(at), entries, sizeof(entries));
}

//! One step: the bytes of v under `valid` (every byte of a full step, the leading ones of
//! the last), read at `at`, written at d. FULL folds the masks of a full step to constants;
//! REC records the step's masks for DecodeColumn (an ASCII step records nothing).
template <bool FULL, bool REC>
MSSQL_AVX512_INLINE char *Step(const Kernel512 &k, __m512i v, uint64_t valid, char *d, const StepRecorder &rec,
							   const uint8_t *at) {
	// Byte positions: every pair's first / second byte, every triplet's first / second / third.
	const uint64_t PAIR_FIRST = 0x5555555555555555ull, PAIR_SECOND = 0xAAAAAAAAAAAAAAAAull;
	const uint64_t TRIPLE_FIRST = 0x0000249249249249ull, TRIPLE_SECOND = 0x0000492492492492ull,
				   TRIPLE_THIRD = 0x0000924924924924ull;
	// Every byte >= 0x80 has 2 or 3 UTF-8 bytes; a masked-out byte was loaded as 0.
	const __mmask64 multi_mask = _mm512_movepi8_mask(v);
	const uint64_t multi = _cvtmask64_u64(multi_mask);
	if (multi == 0) {
		if (FULL) {
			_mm512_storeu_si512(d, v);
			return d + STEP;
		}
		return StoreN(d, v, _mm_popcnt_u64(valid));
	}
	// vpermi2b takes each byte's low 7 bits, bit 6 picking the C0-FF table; the ASCII bytes
	// keep their own value (the merge mask) as their first and only byte.
	const __m512i first = _mm512_mask2_permutex2var_epi8(k.first_lo, v, multi_mask, k.first_hi);
	const __m512i second = _mm512_permutex2var_epi8(k.second_lo, v, k.second_hi);
	const uint64_t three = _cvtmask64_u64(_mm512_cmpge_epu8_mask(first, _mm512_set1_epi8(static_cast<char>(0xE0))));
	if (REC) {
		Record64(rec, at, multi, three);
	}
	__m512i third = _mm512_setzero_si512();
	if (three != 0) {
		third = _mm512_permutex2var_epi8(k.third_lo, v, k.third_hi);
	}
	for (unsigned h = 0; h < 2; h++) {
		const uint64_t half_valid = FULL ? 0xFFFFFFFFull : (valid >> (32 * h)) & 0xFFFFFFFFull;
		if (!FULL && half_valid == 0) {
			break;
		}
		const __m512i pairs = _mm512_permutex2var_epi8(first, k.pairs[h], second);
		const uint64_t half_three = (three >> (32 * h)) & 0xFFFFFFFFull;
		if (half_three == 0) {
			const uint64_t keep =
				(FULL ? PAIR_FIRST : _pdep_u64(half_valid, PAIR_FIRST)) | _pdep_u64(multi >> (32 * h), PAIR_SECOND);
			d = StoreN(d, _mm512_maskz_compress_epi8(_cvtu64_mask64(keep), pairs), _mm_popcnt_u64(keep));
			continue;
		}
		for (unsigned q = 0; q < 2; q++) {
			const unsigned g = 2 * h + q;
			const uint64_t group_valid = FULL ? 0xFFFFull : (half_valid >> (16 * q)) & 0xFFFFull;
			const uint64_t m = (multi >> (16 * g)) & 0xFFFFull, t = (three >> (16 * g)) & 0xFFFFull;
			const uint64_t keep = (FULL ? TRIPLE_FIRST : _pdep_u64(group_valid, TRIPLE_FIRST)) |
								  _pdep_u64(m, TRIPLE_SECOND) | _pdep_u64(t, TRIPLE_THIRD);
			const __m512i triples = _mm512_permutex2var_epi8(pairs, k.triplets[g], third);
			d = StoreN(d, _mm512_maskz_compress_epi8(_cvtu64_mask64(keep), triples), _mm_popcnt_u64(keep));
		}
	}
	return d;
}

template <bool REC>
MSSQL_AVX512_INLINE char *Run(const Kernel512 &k, const uint8_t *p, const uint8_t *end, char *d,
							  const StepRecorder &rec) {
	for (; static_cast<size_t>(end - p) >= STEP; p += STEP) {
		d = Step<true, REC>(k, _mm512_loadu_si512(p), ~0ull, d, rec, p);
	}
	if (p < end) {
		const uint64_t valid = _bzhi_u64(~0ull, static_cast<unsigned>(end - p));
		d = Step<false, REC>(k, _mm512_maskz_loadu_epi8(_cvtu64_mask64(valid), p), valid, d, rec, p);
	}
	return d;
}

}  // namespace

MSSQL_AVX512 char *DecodeSingleByteAvx512(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d) {
	const Kernel512 k = Prepare(page);
	const StepRecorder none = {nullptr, nullptr};
	return Run<false>(k, p, end, d, none);
}

MSSQL_AVX512 char *DecodeSingleByteAvx512(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d,
										  StepRecorder recorder) {
	const Kernel512 k = Prepare(page);
	return Run<true>(k, p, end, d, recorder);
}

MSSQL_AVX512 size_t DecodeValuesSingleByteAvx512(const CodePageTable &page, const uint8_t *src, const uint32_t *offsets,
												 const uint32_t *lengths, size_t count, char *dst,
												 uint64_t *out_offsets, uint32_t *out_lengths) {
	const Kernel512 k = Prepare(page);
	const StepRecorder none = {nullptr, nullptr};
	char *d = dst;
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = src + offsets[i];
		char *start = d;
		d = Run<false>(k, p, p + lengths[i], d, none);
		out_offsets[i] = static_cast<uint64_t>(start - dst);
		out_lengths[i] = static_cast<uint32_t>(d - start);
	}
	return static_cast<size_t>(d - dst);
}

}  // namespace detail
}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

#endif

// SQL Server code pages (spec 091): the decode kernel and the lookups over the
// generated tables. See codec/code_page/code_page.hpp for the contract.

#include "codec/code_page/code_page.hpp"
#include "codec/code_page/code_page_internal.hpp"

#include <atomic>
#include <cassert>

#include <cstring>
#include <vector>

// MSSQL_CODE_PAGE_PORTABLE forces the 8-byte SWAR form, so the tests can run it on a host
// that would otherwise take SSE2 or NEON. NEON means AArch64's: the kernels use vqtbx4q,
// vmaxvq and vld1q_u8_x4, which 32-bit ARM (where __ARM_NEON is defined too) does not have.
#if defined(MSSQL_CODE_PAGE_PORTABLE)
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define MSSQL_CODE_PAGE_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define MSSQL_CODE_PAGE_NEON 1
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_X64)
#include <immintrin.h>	// _xgetbv
#endif
#define MSSQL_CODE_PAGE_ALWAYS_INLINE __forceinline
#else
#define MSSQL_CODE_PAGE_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

// The packed table entries are stored with a 4-byte memcpy that must lay the UTF-8 bytes
// down first, which is a little-endian layout. Every platform the extension builds for is.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "codec/code_page assumes a little-endian target"
#endif

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {

namespace {

constexpr size_t BLOCK = 16;

#if !defined(MSSQL_CODE_PAGE_SSE2) && !defined(MSSQL_CODE_PAGE_NEON)
//! The top bit of every byte of a 64-bit word: the SWAR test for a byte >= 0x80.
constexpr uint64_t HIGH_BITS = 0x8080808080808080ull;
#endif

#if defined(MSSQL_CODE_PAGE_NEON)
//! Set bits of a 32-bit word (DecodeColumn's recorded path, NEON only so far).
inline unsigned PopCount(uint32_t v) {
	return static_cast<unsigned>(__builtin_popcount(v));  // CNT on aarch64
}
#endif

inline unsigned CountTrailingZeros(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
	unsigned long index;
	_BitScanForward64(&index, v);
	return static_cast<unsigned>(index);
#else
	return static_cast<unsigned>(__builtin_ctzll(v));
#endif
}

//! The index of the first byte >= 0x80 among the first `limit` (<= 16) bytes at p,
//! or `limit` when there is none. Reads 16 bytes: the caller guarantees they exist.
inline size_t FirstHighByte(const uint8_t *p, size_t limit) {
#if defined(MSSQL_CODE_PAGE_SSE2)
	uint32_t mask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(p))));
	if (limit < BLOCK) {
		mask &= (1u << limit) - 1u;
	}
	return mask ? CountTrailingZeros(mask) : limit;
#elif defined(MSSQL_CODE_PAGE_NEON)
	// 0xFF for every byte with its top bit set, then four bits per byte (the shrn trick).
	const uint8x16_t high = vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(vld1q_u8(p)), 7));
	uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(high), 4)), 0);
	if (limit < BLOCK) {
		mask &= (uint64_t(1) << (limit * 4)) - 1u;
	}
	return mask ? CountTrailingZeros(mask) / 4 : limit;
#else
	uint64_t w0, w1;
	memcpy(&w0, p, 8);
	memcpy(&w1, p + 8, 8);
	uint64_t m0 = w0 & HIGH_BITS, m1 = w1 & HIGH_BITS;
	if (limit < 8) {
		m0 &= (uint64_t(1) << (limit * 8)) - 1u;
		m1 = 0;
	} else if (limit < BLOCK) {
		m1 &= (uint64_t(1) << ((limit - 8) * 8)) - 1u;
	}
	if (m0) {
		return CountTrailingZeros(m0) / 8;
	}
	return m1 ? 8 + CountTrailingZeros(m1) / 8 : limit;
#endif
}

//! The index of the first byte >= 0x80 among n bytes, or n.
inline size_t FirstNonAscii(const uint8_t *p, size_t n) {
	size_t i = 0;
	for (; i + BLOCK <= n; i += BLOCK) {
		const size_t k = FirstHighByte(p + i, BLOCK);
		if (k < BLOCK) {
			return i + k;
		}
	}
	for (; i < n && p[i] < 0x80; i++) {
	}
	return i;
}

inline void Copy16(char *d, const uint8_t *s) {
#if defined(MSSQL_CODE_PAGE_SSE2)
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d), _mm_loadu_si128(reinterpret_cast<const __m128i *>(s)));
#elif defined(MSSQL_CODE_PAGE_NEON)
	vst1q_u8(reinterpret_cast<uint8_t *>(d), vld1q_u8(s));
#else
	memcpy(d, s, BLOCK);
#endif
}

//! UTF-8 of a BMP code point that is not a surrogate (the generator refuses those).
inline size_t EncodeBmp(uint32_t cp, char *d) {
	if (cp < 0x80) {
		d[0] = static_cast<char>(cp);
		return 1;
	}
	if (cp < 0x800) {
		d[0] = static_cast<char>(0xC0 | (cp >> 6));
		d[1] = static_cast<char>(0x80 | (cp & 0x3F));
		return 2;
	}
	d[0] = static_cast<char>(0xE0 | (cp >> 12));
	d[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
	d[2] = static_cast<char>(0x80 | (cp & 0x3F));
	return 3;
}

inline size_t Utf8LengthBmp(uint32_t cp) {
	return cp < 0x80 ? 1 : (cp < 0x800 ? 2 : 3);
}

//! Decode [p, end) into d; reads stay below `readable` (>= end), which lets a short value
//! take the 16-byte path by reading into whatever follows it. Returns the new output end;
//! writes at most STORE_SLACK bytes past it (16: a block copied whole before a lead byte
//! that ends its value, which then reads as nothing).
//!
//! The loop alternates two modes. In the block mode an ASCII run moves 16 bytes per step
//! (the store copies the whole block; only the ASCII prefix counts, the rest is
//! overwritten). At the first byte >= 0x80 it drops into the table mode, which reads one
//! byte (or a double-byte pair) per step and writes its UTF-8 with one 4-byte store, and
//! returns to the block mode at the next ASCII byte.
template <bool DOUBLE_BYTE>
inline char *DecodeRun(const CodePageTable &page, const uint8_t *p, const uint8_t *end, const uint8_t *readable,
					   char *d) {
	const uint32_t *single = page.single;
	while (p < end) {
		if (static_cast<size_t>(readable - p) >= BLOCK) {
			const size_t remaining = static_cast<size_t>(end - p);
			const size_t limit = remaining < BLOCK ? remaining : BLOCK;
			const size_t ascii = FirstHighByte(p, limit);
			Copy16(d, p);
			p += ascii;
			d += ascii;
			if (ascii == limit) {
				continue;
			}
		} else if (*p < 0x80) {
			*d++ = static_cast<char>(*p++);
			continue;
		}
		// p is at a byte >= 0x80.
		do {
			const uint8_t b = *p;
			if (DOUBLE_BYTE) {
				const uint8_t row = page.lead_row[b];
				if (row) {
					if (p + 1 >= end) {
						// A lead byte that ends its value: the server reads it as nothing.
						return d;
					}
					const uint8_t trail = p[1];
					const uint32_t cp = page.pairs[static_cast<size_t>(row - 1) * 256 + trail];
					// A 0x00 after a lead byte is not consumed: it reads on its own next.
					p += trail ? 2 : 1;
					d += EncodeBmp(cp, d);
					continue;
				}
			}
			const uint32_t packed = single[b];
			memcpy(d, &packed, sizeof(packed));
			d += packed >> 24;
			++p;
		} while (p < end && *p >= 0x80);
	}
	return d;
}

template <bool DOUBLE_BYTE>
size_t DecodeValuesImpl(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
						const uint32_t *lengths, size_t count, char *dst, uint64_t *out_offsets,
						uint32_t *out_lengths) {
	const uint8_t *readable = src + src_size;
	char *d = dst;
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = src + offsets[i];
		char *start = d;
		d = DecodeRun<DOUBLE_BYTE>(page, p, p + lengths[i], readable, d);
		out_offsets[i] = static_cast<uint64_t>(start - dst);
		out_lengths[i] = static_cast<uint32_t>(d - start);
	}
	return static_cast<size_t>(d - dst);
}

using detail::Compaction;
using detail::Compaction3;
using detail::GetCompaction;
using detail::GetCompaction3;
using detail::NoRecord;
using detail::StepRecorder;

#if defined(MSSQL_CODE_PAGE_NEON)
//! A page prepared for the NEON kernels. Each plane holds one byte of the entries of
//! 0x80-0xFF as two 64-entry halves, 0x80-0xBF and 0xC0-0xFF, so one vqtbx4q covers a half:
//!   first / second / third: the UTF-8 bytes a byte reads as (DecodeToUtf8, DecodeValues);
//!   unit_low / unit_high: the bytes of its UTF-16 code unit (DecodeToUtf16).
//! Built once per page, on first use (PlanesFor).
struct NeonPlanes {
	uint8x16x4_t first_lo, first_hi, second_lo, second_hi, third_lo, third_hi;
	//! The first 32 entries (bytes 80-9F) of the three planes as two-register tables:
	//! 1252's specialised kernel looks its exceptions up with vqtbx2q.
	uint8x16x2_t first_w32, second_w32, third_w32;
	uint8x16x4_t unit_low_lo, unit_low_hi, unit_high_lo, unit_high_hi;
	//! The page's longest run of bytes [run_lo, run_lo + run_span] whose code points are
	//! byte + delta, all below U+0800 (1251: C0-FF -> U+0410-U+044F; 1252: A0-FF ->
	//! U+00A0-U+00FF). A block whose non-ASCII bytes all lie in it needs no lookup: its
	//! UTF-8 is arithmetic on the bytes. run_span 0 = no usable run.
	uint8_t run_lo = 0, run_span = 0, delta_low6 = 0, delta_high = 0;
};

NeonPlanes BuildPlanes(const CodePageTable &page) {
	uint8_t first[128], second[128], third[128], unit_low[128], unit_high[128];
	for (size_t b = 0; b < 128; b++) {
		const uint32_t packed = page.single[0x80 + b];
		first[b] = static_cast<uint8_t>(packed & 0xFF);
		second[b] = static_cast<uint8_t>((packed >> 8) & 0xFF);
		third[b] = static_cast<uint8_t>((packed >> 16) & 0xFF);
		unit_low[b] = static_cast<uint8_t>(page.single16[0x80 + b] & 0xFF);
		unit_high[b] = static_cast<uint8_t>(page.single16[0x80 + b] >> 8);
	}
	NeonPlanes planes;
	// The longest run with one delta >= 0 and every code point below U+0800 (2-byte UTF-8).
	unsigned best_lo = 0, best_len = 0, best_delta = 0;
	for (unsigned lo = 0x80; lo < 0x100;) {
		const int delta = static_cast<int>(page.single16[lo]) - static_cast<int>(lo);
		unsigned hi = lo;
		while (hi + 1 < 0x100 && static_cast<int>(page.single16[hi + 1]) - static_cast<int>(hi + 1) == delta) {
			hi++;
		}
		const unsigned len = hi - lo + 1;
		if (delta >= 0 && page.single16[hi] < 0x800 && page.single16[lo] >= 0x80 && len > best_len) {
			best_lo = lo;
			best_len = len;
			best_delta = static_cast<unsigned>(delta);
		}
		lo = hi + 1;
	}
	if (best_len >= 16) {
		planes.run_lo = static_cast<uint8_t>(best_lo);
		planes.run_span = static_cast<uint8_t>(best_len - 1);
		planes.delta_low6 = static_cast<uint8_t>(best_delta & 0x3F);
		planes.delta_high = static_cast<uint8_t>(best_delta >> 6);
	}
	planes.first_lo = vld1q_u8_x4(first);
	planes.first_hi = vld1q_u8_x4(first + 64);
	planes.second_lo = vld1q_u8_x4(second);
	planes.second_hi = vld1q_u8_x4(second + 64);
	planes.third_lo = vld1q_u8_x4(third);
	planes.third_hi = vld1q_u8_x4(third + 64);
	planes.first_w32 = vld1q_u8_x2(first);
	planes.second_w32 = vld1q_u8_x2(second);
	planes.third_w32 = vld1q_u8_x2(third);
	planes.unit_low_lo = vld1q_u8_x4(unit_low);
	planes.unit_low_hi = vld1q_u8_x4(unit_low + 64);
	planes.unit_high_lo = vld1q_u8_x4(unit_high);
	planes.unit_high_hi = vld1q_u8_x4(unit_high + 64);
	return planes;
}

std::vector<NeonPlanes> BuildAllPlanes() {
	std::vector<NeonPlanes> planes;
	planes.reserve(tables::CODE_PAGE_COUNT);
	for (size_t i = 0; i < tables::CODE_PAGE_COUNT; i++) {
		planes.push_back(BuildPlanes(tables::CODE_PAGES[i]));
	}
	return planes;
}

//! The planes of a page, found by its position in tables::CODE_PAGES (CodePageTable::index),
//! so the page must be one FindCodePage returned. Built for every page on the first call; a
//! function-local static, so two threads calling first build them once.
const NeonPlanes &PlanesFor(const CodePageTable &page) {
	static const std::vector<NeonPlanes> planes = BuildAllPlanes();
	return planes[page.index];
}

const uint8_t LANE_BITS[8] = {1, 2, 4, 8, 16, 32, 64, 128};
//! Lane k of each 4-character group weighted 4^k: Block3's length codes summed per group.
const uint8_t GROUP_WEIGHTS[16] = {1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64};

//! What the NEON single-byte kernel reads on every block, set up once per DecodeToUtf8 /
//! DecodeValues call rather than per value: the page's planes, the two compaction tables
//! and the vector constants.
struct NeonKernel {
	explicit NeonKernel(const CodePageTable &page)
		: planes(PlanesFor(page)),
		  compaction(GetCompaction()),
		  compaction3(GetCompaction3()),
		  single(page.single),
		  has_run(planes.run_span != 0),
		  k80(vdupq_n_u8(0x80)),
		  kc0(vdupq_n_u8(0xC0)),
		  ke0(vdupq_n_u8(0xE0)),
		  k3f(vdupq_n_u8(0x3F)),
		  zero(vdupq_n_u8(0)),
		  run_lo(vdupq_n_u8(planes.run_lo)),
		  run_span(vdupq_n_u8(planes.run_span)),
		  run_low6(vdupq_n_u8(planes.delta_low6)),
		  run_lead(vdupq_n_u8(static_cast<uint8_t>(0xC0 + planes.delta_high))),
		  weights(vld1q_u8(GROUP_WEIGHTS)),
		  lane_bits(vld1_u8(LANE_BITS)),
		  lane_bits16(vcombine_u8(vld1_u8(LANE_BITS), vld1_u8(LANE_BITS))),
		  run_wraps(has_run && static_cast<unsigned>(planes.run_lo) + planes.run_span == 0xFF),
		  wrap_span(vdupq_n_u8(static_cast<uint8_t>(0x17F - planes.run_lo))),
		  window(run_wraps && planes.run_lo <= 0xC0) {}
	const NeonPlanes &planes;
	const Compaction &compaction;
	const Compaction3 &compaction3;
	const uint32_t *single;
	const bool has_run;
	const uint8x16_t k80, kc0, ke0, k3f, zero;
	//! The page's run (NeonPlanes::run_lo / run_span), and RunPlanes' addends.
	const uint8x16_t run_lo, run_span, run_low6, run_lead;
	const uint8x16_t weights;
	const uint8x8_t lane_bits;
	const uint8x16_t lane_bits16;
	//! A run that ends at 0xFF (1251 C0-FF, 1252 A0-FF): with ASCII it is one cyclic
	//! interval [run_lo, 0x7F], so "ASCII or in the run" is (byte - run_lo) <= 0x17F - run_lo.
	const bool run_wraps;
	const uint8x16_t wrap_span;
	//! The run covers C0-FF too (1251, 1252), so every byte outside it lies in 80-BF: one
	//! 64-entry lookup per plane, with the run's arithmetic as vqtbx's default, decodes any
	//! block. No byte needs the C0-FF half of the tables.
	const bool window;
};

//! The four 8-lane masks of two blocks (lanes 0x00 / 0xFF) at once: weights, then three
//! pairwise adds, then one lane move. Mask k is byte k of the result.
inline uint32_t Masks4(const NeonKernel &k, uint8x16_t t0, uint8x16_t t1) {
	const uint8x16_t p = vpaddq_u8(vandq_u8(t0, k.lane_bits16), vandq_u8(t1, k.lane_bits16));
	const uint8x16_t q = vpaddq_u8(p, p);
	return vgetq_lane_u32(vreinterpretq_u32_u8(vpaddq_u8(q, q)), 0);
}

//! One bit per lane of 8 lanes that are each 0x00 or 0xFF.
inline unsigned Mask8(const NeonKernel &k, uint8x8_t lanes) {
	return vaddv_u8(vand_u8(lanes, k.lane_bits));
}

//! The UTF-8 first / second byte planes of a block whose non-ASCII bytes all lie in the
//! page's run: code point = byte + delta < U+0800, so
//!   second = 0x80 | ((byte + delta) & 0x3F)
//!   first  = 0xC0 + (byte >> 6) + (delta >> 6) + carry, carry = ((byte & 0x3F) + (delta & 0x3F)) >> 6
//! (byte arithmetic mod 256 is exact for the low six bits). ASCII bytes keep themselves as
//! the first byte; their second byte is never stored.
inline void RunPlanes(const NeonKernel &k, uint8x16_t v, uint8x16_t ascii, uint8x16_t &first, uint8x16_t &second) {
	const uint8x16_t low = vaddq_u8(vandq_u8(v, k.k3f), k.run_low6);  // <= 126
	// (low & 0x3F) | 0x80 in one bit select; run_lead + (byte >> 6) + (low >> 6) in two
	// shift-right-and-accumulates.
	second = vbslq_u8(k.k3f, low, k.k80);
	const uint8x16_t high = vsraq_n_u8(vsraq_n_u8(k.run_lead, v, 6), low, 6);
	first = vbslq_u8(ascii, v, high);
}

//! A block that holds a 3-byte character (U+0800-U+FFFF: the euro sign, the dashes, quotes
//! and ellipsis of 0x80-0x9F, Thai): three byte planes, each character's length code from
//! its first byte, and four 4-character groups of 32-bit lanes [b0 b1 b2 0] packed through
//! Compaction3 (simdutf's 1/2/3-byte packing). `first` is the block's first-byte plane,
//! already looked up with the table indices i0 / i1. (Packing a half with no 3-byte
//! character the cheaper 1/2-byte way was measured: no faster, the branch costs what the
//! shuffle saves.) Writes at most 12 bytes past the returned end.
//! Pack a block of 1/2-byte characters given its two byte planes: two halves of 8, each
//! through Compaction by the mask of its 2-byte leads.
template <class Rec>
inline char *Pack2(const NeonKernel &k, uint8x16_t first, uint8x16_t second, char *d, Rec rec, const uint8_t *at) {
	const uint8x16_t multi = vcgeq_u8(first, k.kc0);
	const unsigned lo = Mask8(k, vget_low_u8(multi)), hi = Mask8(k, vget_high_u8(multi));
	rec.Record16(at, d, lo | (hi << 8), 0);
	vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(vzip1q_u8(first, second), vld1q_u8(k.compaction.shuffle[lo])));
	d += k.compaction.length[lo];
	vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(vzip2q_u8(first, second), vld1q_u8(k.compaction.shuffle[hi])));
	return d + k.compaction.length[hi];
}

//! Pack a block of 1/2/3-byte characters given its three byte planes: a length code per
//! character from its first byte, four 4-character groups of 32-bit lanes [b0 b1 b2 0],
//! each through Compaction3. Writes at most 12 bytes past the returned end.
template <class Rec>
inline char *Pack3(const NeonKernel &k, uint8x16_t first, uint8x16_t second, uint8x16_t third, char *d, Rec rec,
				   const uint8_t *at) {
	// Length code 0 / 1 / 2 (a 2- or 3-byte lead counts once, a 3-byte one twice), lane k of
	// a group weighted 4^k, summed pairwise: one index byte per group, out in one lane move.
	const uint8x16_t multi = vcgeq_u8(first, k.kc0);
	const uint8x16_t three = vcgeq_u8(first, k.ke0);
	if (Rec::ON) {
		// Masks4's bytes: multi's lanes 0-7 and 8-15, then three's: the record word as it is.
		const uint32_t masks = Masks4(k, multi, three);
		rec.Record16(at, d, masks & 0xFFFF, masks >> 16);
	}
	const uint8x16_t code = vsubq_u8(k.zero, vaddq_u8(multi, three));  // each 0xFF counts 1
	const uint8x16_t weighted = vmulq_u8(code, k.weights);
	const uint8x16_t pair = vpaddq_u8(weighted, weighted);
	const uint32_t index = vgetq_lane_u32(vreinterpretq_u32_u8(vpaddq_u8(pair, pair)), 0);
	const uint8x16_t fs_lo = vzip1q_u8(first, second), fs_hi = vzip2q_u8(first, second);
	const uint8x16_t t_lo = vzip1q_u8(third, k.zero), t_hi = vzip2q_u8(third, k.zero);
	const uint8x16_t groups[4] = {
		vreinterpretq_u8_u16(vzip1q_u16(vreinterpretq_u16_u8(fs_lo), vreinterpretq_u16_u8(t_lo))),
		vreinterpretq_u8_u16(vzip2q_u16(vreinterpretq_u16_u8(fs_lo), vreinterpretq_u16_u8(t_lo))),
		vreinterpretq_u8_u16(vzip1q_u16(vreinterpretq_u16_u8(fs_hi), vreinterpretq_u16_u8(t_hi))),
		vreinterpretq_u8_u16(vzip2q_u16(vreinterpretq_u16_u8(fs_hi), vreinterpretq_u16_u8(t_hi)))};
	for (unsigned g = 0; g < 4; g++) {
		const unsigned m = (index >> (8 * g)) & 0xFF;
		vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(groups[g], vld1q_u8(k.compaction3.shuffle[m])));
		d += k.compaction3.length[m];
	}
	return d;
}

template <class Rec>
inline char *Block3(const NeonKernel &k, uint8x16_t i0, uint8x16_t i1, uint8x16_t first, char *d, Rec rec,
					const uint8_t *at) {
	const NeonPlanes &planes = k.planes;
	const uint8x16_t second = vqtbx4q_u8(vqtbx4q_u8(k.zero, planes.second_lo, i0), planes.second_hi, i1);
	const uint8x16_t third = vqtbx4q_u8(vqtbx4q_u8(k.zero, planes.third_lo, i0), planes.third_hi, i1);
	return Pack3(k, first, second, third, d, rec, at);
}

//! The NEON kernel for a single-byte page over [p, end): 32 bytes a step (two 16-byte
//! blocks, two independent lookup chains, so the table lookups overlap), then one 16-byte
//! block, then byte by byte from the table. A step takes the first tier that holds:
//!   all ASCII                        -> the bytes are their UTF-8: stored as they are;
//!   every non-ASCII byte in the run  -> first / second UTF-8 byte by arithmetic (RunPlanes);
//!   every character <= 2 bytes       -> first / second byte planes by vqtbx4q;
//!   a 3-byte character               -> Block3 for each block (Compaction3).
//! The second and third tiers interleave the two planes and pack 8 characters a store by
//! Compaction's shuffle. Returns the new output end; writes at most 12 bytes past it.
//!
//! Inlined into both callers: the NeonKernel is then a local whose address does not escape,
//! so its constants stay in registers across DecodeValues' loop instead of being reloaded
//! after every store through `d` (a char store may alias anything). Measured 3-5% on
//! DecodeValues over 20-60 byte values.
template <class Rec>
MSSQL_CODE_PAGE_ALWAYS_INLINE char *DecodeNeon(const NeonKernel &k, const uint8_t *p, const uint8_t *end, char *d,
											   Rec rec) {
	const NeonPlanes &planes = k.planes;
	const Compaction &compaction = k.compaction;
	while (static_cast<size_t>(end - p) >= 2 * BLOCK) {
		const uint8x16_t v0 = vld1q_u8(p), v1 = vld1q_u8(p + BLOCK);
		if (vmaxvq_u8(vorrq_u8(v0, v1)) < 0x80) {
			vst1q_u8(reinterpret_cast<uint8_t *>(d), v0);
			vst1q_u8(reinterpret_cast<uint8_t *>(d + BLOCK), v1);
			rec.Ascii32(p);
			p += 2 * BLOCK;
			d += 2 * BLOCK;
			continue;
		}
		uint8x16_t f0, f1, s0, s1;
		uint32_t masks;
		const uint8x16_t ascii0 = vcltq_u8(v0, k.k80), ascii1 = vcltq_u8(v1, k.k80);
		uint8x16_t in0, in1;
		if (k.run_wraps) {
			in0 = vcleq_u8(vsubq_u8(v0, k.run_lo), k.wrap_span);
			in1 = vcleq_u8(vsubq_u8(v1, k.run_lo), k.wrap_span);
		} else {
			in0 = vorrq_u8(ascii0, vcleq_u8(vsubq_u8(v0, k.run_lo), k.run_span));
			in1 = vorrq_u8(ascii1, vcleq_u8(vsubq_u8(v1, k.run_lo), k.run_span));
		}
		if (k.has_run && vminvq_u8(vandq_u8(in0, in1)) == 0xFF) {
			// Every non-ASCII byte is in the run: arithmetic, no lookup. Every non-ASCII lane
			// is 2-byte, so the masks are the ASCII masks inverted, in the scalar register.
			RunPlanes(k, v0, ascii0, f0, s0);
			RunPlanes(k, v1, ascii1, f1, s1);
			masks = ~Masks4(k, ascii0, ascii1);
		} else if (k.window) {
			// Bytes 80-BF from the 64-entry half of the tables, everything else keeps the
			// run's arithmetic (an ASCII or run byte indexes past 64, so vqtbx keeps it).
			RunPlanes(k, v0, ascii0, f0, s0);
			RunPlanes(k, v1, ascii1, f1, s1);
			const uint8x16_t w0 = vsubq_u8(v0, k.k80), w1 = vsubq_u8(v1, k.k80);
			f0 = vqtbx4q_u8(f0, planes.first_lo, w0);
			f1 = vqtbx4q_u8(f1, planes.first_lo, w1);
			s0 = vqtbx4q_u8(s0, planes.second_lo, w0);
			s1 = vqtbx4q_u8(s1, planes.second_lo, w1);
			if (vmaxvq_u8(vmaxq_u8(f0, f1)) >= 0xE0) {
				// Only the block that holds a 3-byte character takes the 1/2/3-byte packing.
				d = vmaxvq_u8(f0) >= 0xE0 ? Pack3(k, f0, s0, vqtbx4q_u8(k.zero, planes.third_lo, w0), d, rec, p)
										  : Pack2(k, f0, s0, d, rec, p);
				d = vmaxvq_u8(f1) >= 0xE0 ? Pack3(k, f1, s1, vqtbx4q_u8(k.zero, planes.third_lo, w1), d, rec, p + BLOCK)
										  : Pack2(k, f1, s1, d, rec, p + BLOCK);
				p += 2 * BLOCK;
				continue;
			}
			masks = Masks4(k, vcgeq_u8(f0, k.kc0), vcgeq_u8(f1, k.kc0));
		} else {
			const uint8x16_t a0 = vsubq_u8(v0, k.k80), a1 = vsubq_u8(v0, k.kc0);
			const uint8x16_t b0 = vsubq_u8(v1, k.k80), b1 = vsubq_u8(v1, k.kc0);
			f0 = vqtbx4q_u8(vqtbx4q_u8(v0, planes.first_lo, a0), planes.first_hi, a1);
			f1 = vqtbx4q_u8(vqtbx4q_u8(v1, planes.first_lo, b0), planes.first_hi, b1);
			if (vmaxvq_u8(vmaxq_u8(f0, f1)) >= 0xE0) {
				d = Block3(k, a0, a1, f0, d, rec, p);
				d = Block3(k, b0, b1, f1, d, rec, p + BLOCK);
				p += 2 * BLOCK;
				continue;
			}
			s0 = vqtbx4q_u8(vqtbx4q_u8(k.zero, planes.second_lo, a0), planes.second_hi, a1);
			s1 = vqtbx4q_u8(vqtbx4q_u8(k.zero, planes.second_lo, b0), planes.second_hi, b1);
			masks = Masks4(k, vcgeq_u8(f0, k.kc0), vcgeq_u8(f1, k.kc0));
		}
		const unsigned m0 = masks & 0xFF, m1 = (masks >> 8) & 0xFF;
		const unsigned m2 = (masks >> 16) & 0xFF, m3 = masks >> 24;
		const size_t l0 = compaction.length[m0], l1 = compaction.length[m1], l2 = compaction.length[m2];
		vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(vzip1q_u8(f0, s0), vld1q_u8(compaction.shuffle[m0])));
		vst1q_u8(reinterpret_cast<uint8_t *>(d + l0), vqtbl1q_u8(vzip2q_u8(f0, s0), vld1q_u8(compaction.shuffle[m1])));
		vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1),
				 vqtbl1q_u8(vzip1q_u8(f1, s1), vld1q_u8(compaction.shuffle[m2])));
		vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1 + l2),
				 vqtbl1q_u8(vzip2q_u8(f1, s1), vld1q_u8(compaction.shuffle[m3])));
		rec.Record32(p, d, masks, 0);
		d += l0 + l1 + l2 + compaction.length[m3];
		p += 2 * BLOCK;
	}
	if (static_cast<size_t>(end - p) >= BLOCK) {
		const uint8x16_t v = vld1q_u8(p);
		p += BLOCK;
		if (vmaxvq_u8(v) < 0x80) {
			vst1q_u8(reinterpret_cast<uint8_t *>(d), v);
			d += BLOCK;
		} else {
			const uint8x16_t i0 = vsubq_u8(v, k.k80), i1 = vsubq_u8(v, k.kc0);
			const uint8x16_t first = vqtbx4q_u8(vqtbx4q_u8(v, planes.first_lo, i0), planes.first_hi, i1);
			if (vmaxvq_u8(first) >= 0xE0) {
				d = Block3(k, i0, i1, first, d, rec, p - BLOCK);
			} else {
				const uint8x16_t second = vqtbx4q_u8(vqtbx4q_u8(k.zero, planes.second_lo, i0), planes.second_hi, i1);
				const uint8x16_t two = vcgeq_u8(first, k.kc0);	// a 2-byte lead
				const unsigned m_lo = Mask8(k, vget_low_u8(two)), m_hi = Mask8(k, vget_high_u8(two));
				rec.Record16(p - BLOCK, d, m_lo | (m_hi << 8), 0);
				vst1q_u8(reinterpret_cast<uint8_t *>(d),
						 vqtbl1q_u8(vzip1q_u8(first, second), vld1q_u8(compaction.shuffle[m_lo])));
				d += compaction.length[m_lo];
				vst1q_u8(reinterpret_cast<uint8_t *>(d),
						 vqtbl1q_u8(vzip2q_u8(first, second), vld1q_u8(compaction.shuffle[m_hi])));
				d += compaction.length[m_hi];
			}
		}
	}
	if (Rec::ON && p < end) {
		const uint8_t *from = p;
		const char *start = d;
		unsigned multi = 0, three = 0;
		for (unsigned i = 0; p < end; ++p, ++i) {
			const uint32_t packed = k.single[*p];
			memcpy(d, &packed, sizeof(packed));
			const unsigned length = packed >> 24;
			multi |= static_cast<unsigned>(length > 1) << i;
			three |= static_cast<unsigned>(length > 2) << i;
			d += length;
		}
		rec.Record16(from, start, multi, three);
		return d;
	}
	for (; p < end; ++p) {
		const uint32_t packed = k.single[*p];
		memcpy(d, &packed, sizeof(packed));
		d += packed >> 24;
	}
	return d;
}

//! A value this long goes through the NEON kernel in DecodeValues: one 32-byte step at
//! least. A shorter one takes DecodeRun, whose 16-byte ASCII step may read ahead into the
//! next value, where the NEON kernel would finish it byte by byte.
constexpr size_t NEON_MIN_VALUE = 2 * BLOCK;

//! Code page 1252, specialised (spec 091): bytes A0-FF read as U+00A0-U+00FF (Latin-1), so
//! their UTF-8 is C2 / C3 then 0x80 | (byte & 0x3F); every other byte >= 0x80 lies in
//! 80-9F (the euro sign, the quotes, dashes and ellipsis, Œ Š Ž ...) and is looked up in
//! a 32-entry table (vqtbx2q, two registers). The generic kernel takes the rest (< 32 bytes).
template <class Rec>
MSSQL_CODE_PAGE_ALWAYS_INLINE char *DecodeNeon1252(const NeonKernel &k, const uint8_t *p, const uint8_t *end, char *d,
												   Rec rec) {
	const NeonPlanes &planes = k.planes;
	const uint8x16_t k20 = vdupq_n_u8(0x20);
	while (static_cast<size_t>(end - p) >= 2 * BLOCK) {
		const uint8x16_t v0 = vld1q_u8(p), v1 = vld1q_u8(p + BLOCK);
		if (vmaxvq_u8(vorrq_u8(v0, v1)) < 0x80) {
			vst1q_u8(reinterpret_cast<uint8_t *>(d), v0);
			vst1q_u8(reinterpret_cast<uint8_t *>(d + BLOCK), v1);
			rec.Ascii32(p);
			p += 2 * BLOCK;
			d += 2 * BLOCK;
			continue;
		}
		const uint8x16_t ascii0 = vcltq_u8(v0, k.k80), ascii1 = vcltq_u8(v1, k.k80);
		// Latin-1 arithmetic for every lane; ASCII lanes keep their byte as the first.
		uint8x16_t f0 = vbslq_u8(ascii0, v0, vsraq_n_u8(k.kc0, v0, 6));
		uint8x16_t f1 = vbslq_u8(ascii1, v1, vsraq_n_u8(k.kc0, v1, 6));
		uint8x16_t s0 = vbslq_u8(k.k3f, v0, k.k80), s1 = vbslq_u8(k.k3f, v1, k.k80);
		const uint8x16_t w0 = vsubq_u8(v0, k.k80), w1 = vsubq_u8(v1, k.k80);
		if (vmaxvq_u8(vorrq_u8(vcltq_u8(w0, k20), vcltq_u8(w1, k20))) == 0) {
			// No 80-9F byte: every non-ASCII lane is 2-byte.
			const uint32_t masks = ~Masks4(k, ascii0, ascii1);
			const unsigned m0 = masks & 0xFF, m1 = (masks >> 8) & 0xFF;
			const unsigned m2 = (masks >> 16) & 0xFF, m3 = masks >> 24;
			const Compaction &c = k.compaction;
			const size_t l0 = c.length[m0], l1 = c.length[m1], l2 = c.length[m2];
			vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(vzip1q_u8(f0, s0), vld1q_u8(c.shuffle[m0])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0), vqtbl1q_u8(vzip2q_u8(f0, s0), vld1q_u8(c.shuffle[m1])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1), vqtbl1q_u8(vzip1q_u8(f1, s1), vld1q_u8(c.shuffle[m2])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1 + l2),
					 vqtbl1q_u8(vzip2q_u8(f1, s1), vld1q_u8(c.shuffle[m3])));
			rec.Record32(p, d, masks, 0);
			d += l0 + l1 + l2 + c.length[m3];
			p += 2 * BLOCK;
			continue;
		}
		// 80-9F bytes from the 32-entry tables; everything else keeps the arithmetic (an ASCII
		// or A0-FF byte indexes past 32, so vqtbx keeps it).
		f0 = vqtbx2q_u8(f0, planes.first_w32, w0);
		f1 = vqtbx2q_u8(f1, planes.first_w32, w1);
		s0 = vqtbx2q_u8(s0, planes.second_w32, w0);
		s1 = vqtbx2q_u8(s1, planes.second_w32, w1);
		d = vmaxvq_u8(f0) >= 0xE0 ? Pack3(k, f0, s0, vqtbx2q_u8(k.zero, planes.third_w32, w0), d, rec, p)
								  : Pack2(k, f0, s0, d, rec, p);
		d = vmaxvq_u8(f1) >= 0xE0 ? Pack3(k, f1, s1, vqtbx2q_u8(k.zero, planes.third_w32, w1), d, rec, p + BLOCK)
								  : Pack2(k, f1, s1, d, rec, p + BLOCK);
		p += 2 * BLOCK;
	}
	return DecodeNeon(k, p, end, d, rec);
}

//! Code page 1251, specialised: bytes C0-FF read as U+0410-U+044F (А-я), so their UTF-8 is
//! D0 / D1 (from byte >= F0) then 0x80 | ((byte + 0x10) & 0x3F); every other byte >= 0x80
//! lies in 80-BF (Ё ё Ђ ... № and the typography) and is looked up in the 64-entry half of
//! the planes, the arithmetic as vqtbx's default. The generic kernel takes the rest.
template <class Rec>
MSSQL_CODE_PAGE_ALWAYS_INLINE char *DecodeNeon1251(const NeonKernel &k, const uint8_t *p, const uint8_t *end, char *d,
												   Rec rec) {
	const NeonPlanes &planes = k.planes;
	const uint8x16_t kd0 = vdupq_n_u8(0xD0), kf0 = vdupq_n_u8(0xF0), k10 = vdupq_n_u8(0x10);
	const uint8x16_t kbf = vdupq_n_u8(0xBF);
	while (static_cast<size_t>(end - p) >= 2 * BLOCK) {
		const uint8x16_t v0 = vld1q_u8(p), v1 = vld1q_u8(p + BLOCK);
		if (vmaxvq_u8(vorrq_u8(v0, v1)) < 0x80) {
			vst1q_u8(reinterpret_cast<uint8_t *>(d), v0);
			vst1q_u8(reinterpret_cast<uint8_t *>(d + BLOCK), v1);
			rec.Ascii32(p);
			p += 2 * BLOCK;
			d += 2 * BLOCK;
			continue;
		}
		const uint8x16_t ascii0 = vcltq_u8(v0, k.k80), ascii1 = vcltq_u8(v1, k.k80);
		// D0 + (byte >= F0): the compare's 0xFF counts as -1, so D0 - mask.
		uint8x16_t f0 = vbslq_u8(ascii0, v0, vsubq_u8(kd0, vcgeq_u8(v0, kf0)));
		uint8x16_t f1 = vbslq_u8(ascii1, v1, vsubq_u8(kd0, vcgeq_u8(v1, kf0)));
		uint8x16_t s0 = vbslq_u8(k.k3f, vaddq_u8(v0, k10), k.k80);
		uint8x16_t s1 = vbslq_u8(k.k3f, vaddq_u8(v1, k10), k.k80);
		const uint8x16_t w0 = vsubq_u8(v0, k.k80), w1 = vsubq_u8(v1, k.k80);
		// A byte in 80-BF is (byte - 0x80) <= 0x3F; ASCII wraps to >= 0x80, C0-FF to 0x40-0x7F.
		const uint8x16_t in0 = vcleq_u8(vaddq_u8(w0, vdupq_n_u8(0xC0)), kbf);  // not 80-BF
		const uint8x16_t in1 = vcleq_u8(vaddq_u8(w1, vdupq_n_u8(0xC0)), kbf);
		if (vminvq_u8(vandq_u8(in0, in1)) == 0xFF) {
			// No 80-BF byte: every non-ASCII lane is a letter, 2-byte.
			const uint32_t masks = ~Masks4(k, ascii0, ascii1);
			const unsigned m0 = masks & 0xFF, m1 = (masks >> 8) & 0xFF;
			const unsigned m2 = (masks >> 16) & 0xFF, m3 = masks >> 24;
			const Compaction &c = k.compaction;
			const size_t l0 = c.length[m0], l1 = c.length[m1], l2 = c.length[m2];
			vst1q_u8(reinterpret_cast<uint8_t *>(d), vqtbl1q_u8(vzip1q_u8(f0, s0), vld1q_u8(c.shuffle[m0])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0), vqtbl1q_u8(vzip2q_u8(f0, s0), vld1q_u8(c.shuffle[m1])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1), vqtbl1q_u8(vzip1q_u8(f1, s1), vld1q_u8(c.shuffle[m2])));
			vst1q_u8(reinterpret_cast<uint8_t *>(d + l0 + l1 + l2),
					 vqtbl1q_u8(vzip2q_u8(f1, s1), vld1q_u8(c.shuffle[m3])));
			rec.Record32(p, d, masks, 0);
			d += l0 + l1 + l2 + c.length[m3];
			p += 2 * BLOCK;
			continue;
		}
		f0 = vqtbx4q_u8(f0, planes.first_lo, w0);
		f1 = vqtbx4q_u8(f1, planes.first_lo, w1);
		s0 = vqtbx4q_u8(s0, planes.second_lo, w0);
		s1 = vqtbx4q_u8(s1, planes.second_lo, w1);
		d = vmaxvq_u8(f0) >= 0xE0 ? Pack3(k, f0, s0, vqtbx4q_u8(k.zero, planes.third_lo, w0), d, rec, p)
								  : Pack2(k, f0, s0, d, rec, p);
		d = vmaxvq_u8(f1) >= 0xE0 ? Pack3(k, f1, s1, vqtbx4q_u8(k.zero, planes.third_lo, w1), d, rec, p + BLOCK)
								  : Pack2(k, f1, s1, d, rec, p + BLOCK);
		p += 2 * BLOCK;
	}
	return DecodeNeon(k, p, end, d, rec);
}

//! Code page 874 (Thai), specialised: bytes A1-FB except DB-DE read as U+0E01-U+0E5B, three
//! UTF-8 bytes each, all arithmetic: E0, then B8 / B9 (from byte >= E0), then
//! 0x80 | ((byte + 0x20) & 0x3F). A block of Thai letters alone (Thai puts no space between
//! words) is three planes interleaved by one vst3q_u8, 48 bytes; Thai mixed with ASCII is
//! packed 1 / 3 bytes a character by Pack3; anything else (the euro sign, quotes, the holes)
//! takes the generic kernel's step for that block.
template <class Rec>
MSSQL_CODE_PAGE_ALWAYS_INLINE char *DecodeNeon874(const NeonKernel &k, const uint8_t *p, const uint8_t *end, char *d,
												  Rec rec) {
	const uint8x16_t ka1 = vdupq_n_u8(0xA1), k5a = vdupq_n_u8(0x5A), kdb = vdupq_n_u8(0xDB);
	const uint8x16_t k03 = vdupq_n_u8(0x03), kb8 = vdupq_n_u8(0xB8), k20 = vdupq_n_u8(0x20);
	while (static_cast<size_t>(end - p) >= BLOCK) {
		const uint8x16_t v = vld1q_u8(p);
		if (vmaxvq_u8(v) < 0x80) {
			vst1q_u8(reinterpret_cast<uint8_t *>(d), v);
			p += BLOCK;
			d += BLOCK;
			continue;
		}
		const uint8x16_t thai = vandq_u8(vcleq_u8(vsubq_u8(v, ka1), k5a), vmvnq_u8(vcleq_u8(vsubq_u8(v, kdb), k03)));
		const uint8x16_t ascii = vcltq_u8(v, k.k80);
		if (vminvq_u8(vorrq_u8(thai, ascii)) != 0xFF) {
			// A byte outside Thai and ASCII: the generic kernel's step for this block.
			d = DecodeNeon(k, p, p + BLOCK, d, rec);
			p += BLOCK;
			continue;
		}
		const uint8x16_t second = vsubq_u8(kb8, vcgeq_u8(v, k.ke0));  // 0xFF counts as -1: B9
		const uint8x16_t third = vbslq_u8(k.k3f, vaddq_u8(v, k20), k.k80);
		if (vmaxvq_u8(ascii) == 0) {
			uint8x16x3_t planes;
			planes.val[0] = k.ke0;
			planes.val[1] = second;
			planes.val[2] = third;
			vst3q_u8(reinterpret_cast<uint8_t *>(d), planes);
			rec.Record16(p, d, 0xFFFF, 0xFFFF);
			d += 3 * BLOCK;
		} else {
			d = Pack3(k, vbslq_u8(ascii, v, k.ke0), second, third, d, rec, p);
		}
		p += BLOCK;
	}
	return DecodeNeon(k, p, end, d, rec);
}

//! The specialised kernel of a page, else the generic one.
template <class Rec>
MSSQL_CODE_PAGE_ALWAYS_INLINE char *DecodeNeonFor(const NeonKernel &k, uint16_t code_page, const uint8_t *p,
												  const uint8_t *end, char *d, Rec rec) {
	switch (code_page) {
	case 1251:
		return DecodeNeon1251(k, p, end, d, rec);
	case 1252:
		return DecodeNeon1252(k, p, end, d, rec);
	case 874:
		return DecodeNeon874(k, p, end, d, rec);
	default:
		return DecodeNeon(k, p, end, d, rec);
	}
}

size_t DecodeValuesSingleByteNeon(const CodePageTable &page, const uint8_t *src, size_t src_size,
								  const uint32_t *offsets, const uint32_t *lengths, size_t count, char *dst,
								  uint64_t *out_offsets, uint32_t *out_lengths) {
	const NeonKernel kernel(page);
	NoRecord none;
	const uint8_t *readable = src + src_size;
	char *d = dst;
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = src + offsets[i];
		const uint8_t *end = p + lengths[i];
		char *start = d;
		d = lengths[i] >= NEON_MIN_VALUE ? DecodeNeonFor(kernel, page.code_page, p, end, d, none)
										 : DecodeRun<false>(page, p, end, readable, d);
		out_offsets[i] = static_cast<uint64_t>(start - dst);
		out_lengths[i] = static_cast<uint32_t>(d - start);
	}
	return static_cast<size_t>(d - dst);
}
#endif

//! True when none of the 64 bytes at p has its top bit set; copies them to d either way.
inline bool CopyAscii64(char *d, const uint8_t *p) {
#if defined(MSSQL_CODE_PAGE_SSE2)
	const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
	const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 16));
	const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 32));
	const __m128i e = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 48));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d), a);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 16), b);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), c);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), e);
	return _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a, b), _mm_or_si128(c, e))) == 0;
#elif defined(MSSQL_CODE_PAGE_NEON)
	const uint8x16x4_t v = vld1q_u8_x4(p);
	vst1q_u8_x4(reinterpret_cast<uint8_t *>(d), v);
	const uint8x16_t any = vorrq_u8(vorrq_u8(v.val[0], v.val[1]), vorrq_u8(v.val[2], v.val[3]));
	return vmaxvq_u8(any) < 0x80;
#else
	uint64_t w[8];
	memcpy(w, p, sizeof(w));
	memcpy(d, w, sizeof(w));
	return ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]) & HIGH_BITS) == 0;
#endif
}

//! Widen 16 ASCII bytes to 16 UTF-16 code units (32 bytes) at d.
inline void Widen16(char16_t *d, const uint8_t *s) {
#if defined(MSSQL_CODE_PAGE_SSE2)
	const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s));
	const __m128i zero = _mm_setzero_si128();
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d), _mm_unpacklo_epi8(v, zero));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(d + 8), _mm_unpackhi_epi8(v, zero));
#elif defined(MSSQL_CODE_PAGE_NEON)
	const uint8x16_t v = vld1q_u8(s);
	const uint8x16_t zero = vdupq_n_u8(0);
	vst1q_u8(reinterpret_cast<uint8_t *>(d), vzip1q_u8(v, zero));
	vst1q_u8(reinterpret_cast<uint8_t *>(d + 8), vzip2q_u8(v, zero));
#else
	for (size_t i = 0; i < BLOCK; i++) {
		d[i] = static_cast<char16_t>(s[i]);
	}
#endif
}

//! Stage 1 for a single-byte page: exactly one code unit per byte.
size_t SingleByteToUtf16(const CodePageTable &page, const uint8_t *src, size_t n, char16_t *dst) {
	const uint16_t *units = page.single16;
	size_t i = 0;
#if defined(MSSQL_CODE_PAGE_NEON)
	// The code unit's low and high byte from the page's planes (built once per page), two
	// 64-entry lookups per byte. A byte below 0x80 indexes past both (its offsets wrap to
	// >= 64), so vqtbx keeps it as is: ASCII passes through with no branch.
	const NeonPlanes &planes = PlanesFor(page);
	const uint8x16_t k80 = vdupq_n_u8(0x80), kc0 = vdupq_n_u8(0xC0), zero = vdupq_n_u8(0);
	for (; i + BLOCK <= n; i += BLOCK) {
		const uint8x16_t v = vld1q_u8(src + i);
		const uint8x16_t i0 = vsubq_u8(v, k80), i1 = vsubq_u8(v, kc0);
		const uint8x16_t l = vqtbx4q_u8(vqtbx4q_u8(v, planes.unit_low_lo, i0), planes.unit_low_hi, i1);
		const uint8x16_t h = vqtbx4q_u8(vqtbx4q_u8(zero, planes.unit_high_lo, i0), planes.unit_high_hi, i1);
		vst1q_u8(reinterpret_cast<uint8_t *>(dst + i), vzip1q_u8(l, h));
		vst1q_u8(reinterpret_cast<uint8_t *>(dst + i + 8), vzip2q_u8(l, h));
	}
#else
	// No 128-entry byte shuffle in SSE2: ASCII blocks widen by SIMD, the rest by the table.
	while (i + BLOCK <= n) {
		const size_t ascii = FirstHighByte(src + i, BLOCK);
		if (ascii == BLOCK) {
			Widen16(dst + i, src + i);
			i += BLOCK;
			continue;
		}
		const size_t stop = i + BLOCK;
		for (; i < stop; i++) {
			dst[i] = static_cast<char16_t>(units[src[i]]);
		}
	}
#endif
	for (; i < n; i++) {
		dst[i] = static_cast<char16_t>(units[src[i]]);
	}
	return n;
}

//! Stage 1 for a double-byte page: a pair reads as one code unit, a lead byte that ends the
//! input as none, a 0x00 after a lead byte on its own. Every unit consumes at least one
//! byte, so the 16-unit ASCII store at d never reaches past dst + n.
size_t DoubleByteToUtf16(const CodePageTable &page, const uint8_t *src, size_t n, char16_t *dst) {
	const uint8_t *p = src;
	const uint8_t *end = src + n;
	char16_t *d = dst;
	while (p < end) {
		if (static_cast<size_t>(end - p) >= BLOCK) {
			const size_t ascii = FirstHighByte(p, BLOCK);
			Widen16(d, p);
			p += ascii;
			d += ascii;
			if (ascii == BLOCK) {
				continue;
			}
		} else if (*p < 0x80) {
			*d++ = static_cast<char16_t>(*p++);
			continue;
		}
		do {
			const uint8_t b = *p;
			const uint8_t row = page.lead_row[b];
			if (row) {
				if (p + 1 >= end) {
					return static_cast<size_t>(d - dst);
				}
				const uint8_t trail = p[1];
				*d++ = static_cast<char16_t>(page.pairs[static_cast<size_t>(row - 1) * 256 + trail]);
				p += trail ? 2 : 1;
			} else {
				*d++ = static_cast<char16_t>(page.single16[b]);
				++p;
			}
		} while (p < end && *p >= 0x80);
	}
	return static_cast<size_t>(d - dst);
}

//! ExactUtf8Length's loop: DecodeRun's, counting instead of storing.
template <bool DOUBLE_BYTE>
size_t Utf8LengthImpl(const CodePageTable &page, const uint8_t *src, size_t n) {
	const uint8_t *p = src;
	const uint8_t *end = src + n;
	size_t length = 0;
	while (p < end) {
		if (static_cast<size_t>(end - p) >= BLOCK) {
			const size_t ascii = FirstHighByte(p, BLOCK);
			p += ascii;
			length += ascii;
			if (ascii == BLOCK) {
				continue;
			}
		} else if (*p < 0x80) {
			++p;
			++length;
			continue;
		}
		do {
			const uint8_t b = *p;
			if (DOUBLE_BYTE) {
				const uint8_t row = page.lead_row[b];
				if (row) {
					if (p + 1 >= end) {
						return length;
					}
					const uint8_t trail = p[1];
					length += Utf8LengthBmp(page.pairs[static_cast<size_t>(row - 1) * 256 + trail]);
					p += trail ? 2 : 1;
					continue;
				}
			}
			length += page.single[b] >> 24;
			++p;
		} while (p < end && *p >= 0x80);
	}
	return length;
}

//! Input bytes DecodeColumn decodes before it resolves the values that start in them: their
//! step summaries (8 bytes per 16) sit on the stack, 8 KB, in L1 while they are read back.
constexpr size_t COLUMN_BLOCK = 16 * 1024;
// A block's output positions fit the recorder's 16 bits.
static_assert(COLUMN_BLOCK * MAX_UTF8_PER_BYTE < 65536, "output position in 16 bits");

//! Row r's validity in DuckDB's ValidityMask words (nullptr: all valid).
inline bool RowValid(const uint64_t *validity, size_t row) {
	return validity == nullptr || ((validity[row >> 6] >> (row & 63)) & 1) != 0;
}

//! The view of `length` UTF-8 bytes at `data`, in string_t's layout (StringView16). Reads 12
//! bytes at data whatever the length -- dst has STORE_SLACK past its end -- and selects the
//! inline or the pointer form without a branch.
inline StringView16 MakeView(const char *data, uint32_t length, size_t inline_length) {
	uint64_t head;
	uint32_t tail;
	memcpy(&head, data, sizeof(head));
	memcpy(&tail, data + 8, sizeof(tail));
	const uint64_t head_mask = length >= 8 ? ~uint64_t(0) : ~(~uint64_t(0) << (8 * length));
	const unsigned tail_length = length > 8 ? (length < 12 ? length - 8 : 4) : 0;
	const uint64_t tail_mask = ~(~uint64_t(0) << (8 * tail_length));
	const uint64_t kept = head & head_mask;
	StringView16 inlined, pointed;
	inlined.lo = length | (kept << 32);
	inlined.hi = (kept >> 32) | ((uint64_t(tail) & tail_mask) << 32);
	// DUCKDB_DEBUG_NO_INLINE (inline length 0) keeps the prefix zero.
	pointed.lo = length | (inline_length != 0 ? (head << 32) : 0);
	pointed.hi = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
	return length <= inline_length ? inlined : pointed;
}

//! DecodeColumn value by value: a double-byte page, and a single-byte page with no SIMD
//! kernel to record steps.
template <bool DOUBLE_BYTE>
size_t DecodeColumnByValue(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
						   const uint32_t *lengths, const uint64_t *validity, size_t count, char *dst,
						   StringView16 *views, size_t inline_length) {
	const uint8_t *readable = src + src_size;
#if defined(__x86_64__) || defined(_M_X64)
	const detail::Isa isa = detail::ActiveIsa();
#endif
	char *d = dst;
	for (size_t row = 0; row < count; row++) {
		if (!RowValid(validity, row)) {
			views[row].lo = 0;
			views[row].hi = 0;
			continue;
		}
		const uint8_t *p = src + offsets[row];
		const uint8_t *end = p + lengths[row];
		char *start = d;
#if defined(__x86_64__) || defined(_M_X64)
		// Until the x86 kernels record steps: each value through them, as DecodeValues.
		if (!DOUBLE_BYTE && isa == detail::Isa::Avx512) {
			d = detail::DecodeSingleByteAvx512(page, p, end, d);
		} else if (!DOUBLE_BYTE && isa == detail::Isa::Avx2 && lengths[row] >= 32) {
			d = detail::DecodeSingleByteAvx2(page, p, end, d);
		} else {
			d = DecodeRun<DOUBLE_BYTE>(page, p, end, readable, d);
		}
#else
		d = DecodeRun<DOUBLE_BYTE>(page, p, end, readable, d);
#endif
		views[row] = MakeView(start, static_cast<uint32_t>(d - start), inline_length);
	}
	return static_cast<size_t>(d - dst);
}

// Only the NEON kernel records steps so far; the x86 kernels take DecodeColumnByValue.
#if defined(MSSQL_CODE_PAGE_NEON)
//! Each 16 input bytes' output position from the block's: 16 a granule before it and one per
//! mask bit (a 2- or 3-byte character's second byte, a 3-byte one's third). Groups of 8, so
//! it reads masks and writes starts up to 7 past `granules`.
inline void StartsFromMasks(const uint32_t *masks, size_t granules, uint16_t *starts) {
	static const uint16_t LANE_STARTS[8] = {0, 16, 32, 48, 64, 80, 96, 112};
	const uint16x8_t lane_starts = vld1q_u16(LANE_STARTS);
	const uint16x8_t zero = vdupq_n_u16(0);
	unsigned base = 0;	// the output position of granule g's first byte
	for (size_t g = 0; g < granules; g += 8) {
		const uint8x16_t a = vld1q_u8(reinterpret_cast<const uint8_t *>(masks + g));
		const uint8x16_t b = vld1q_u8(reinterpret_cast<const uint8_t *>(masks + g + 4));
		// The bits of each mask, then an inclusive scan over the 8 granules.
		const uint16x8_t bits = vcombine_u16(vmovn_u32(vpaddlq_u16(vpaddlq_u8(vcntq_u8(a)))),
											 vmovn_u32(vpaddlq_u16(vpaddlq_u8(vcntq_u8(b)))));
		uint16x8_t scan = vaddq_u16(bits, vextq_u16(zero, bits, 7));
		scan = vaddq_u16(scan, vextq_u16(zero, scan, 6));
		scan = vaddq_u16(scan, vextq_u16(zero, scan, 4));
		const uint16x8_t before = vsubq_u16(scan, bits);
		vst1q_u16(starts + g, vaddq_u16(vaddq_u16(before, lane_starts), vdupq_n_u16(static_cast<uint16_t>(base))));
		base += 8 * BLOCK + vgetq_lane_u16(scan, 7);
	}
}

//! DecodeColumn on a single-byte page through a recording kernel, COLUMN_BLOCK input bytes
//! at a time: the kernel decodes them and records the masks of each 16 that are not all
//! ASCII (an ASCII step stores nothing: the masks start zeroed, and are zeroed again only
//! after a block that was not all ASCII); each 16's output position is then 16 per granule
//! before it plus the mask bits before it, and each value that starts in the block takes its
//! output offset from those (its view is written once the next value's offset ends it).
//! Nothing reads the input or the output again. `decode(p, end, d, recorder)` is the kernel.
//!
//! Why the masks and not the positions: a second stream of stores in the kernel's loop, beside
//! its output, costs about four cycles a 32-byte step on Apple M4 (a copy loop measured 4-6x
//! slower with one 8-byte store a step to another array), so the ASCII step, the common one,
//! stores nothing.
template <class Decode>
size_t DecodeColumnRecorded(const uint8_t *src, size_t src_size, const uint32_t *offsets, const uint64_t *validity,
							size_t count, char *dst, StringView16 *views, size_t inline_length, Decode decode) {
	// Room for StartsFromMasks' last 8 (it reads and writes whole groups of 8).
	uint32_t masks[COLUMN_BLOCK / BLOCK + 8] = {};
	uint16_t starts[COLUMN_BLOCK / BLOCK + 8];
	char *d = dst;
	size_t row = 0;
	size_t prev_row = 0, prev_out = 0;
	bool have_prev = false;
	size_t b0 = 0;
	for (;;) {
		const size_t b1 = src_size - b0 > COLUMN_BLOCK ? b0 + COLUMN_BLOCK : src_size;
		const bool last = b1 == src_size;
		const size_t block_out = static_cast<size_t>(d - dst);
		const size_t granules = (b1 - b0 + BLOCK - 1) / BLOCK;
		StepRecorder recorder;
		recorder.masks = masks;
		recorder.in = src + b0;
		d = decode(src + b0, src + b1, d, recorder);
		// A block whose output is as long as its input was all ASCII: a value's output offset
		// is its input offset, and no granule positions are needed.
		const bool ascii = static_cast<size_t>(d - dst) - block_out == b1 - b0;
		if (!ascii) {
			StartsFromMasks(masks, granules, starts);
		}
		for (; row < count; row++) {
			if (!RowValid(validity, row)) {
				views[row].lo = 0;
				views[row].hi = 0;
				continue;
			}
			const size_t x = offsets[row];
			if (x >= b1 && !last) {
				break;
			}
			size_t out;
			if (x == b1) {
				out = static_cast<size_t>(d - dst);	 // a value that starts where the input ends: empty
			} else if (ascii) {
				out = block_out + (x - b0);
			} else {
				const size_t k = (x - b0) / BLOCK;
				const unsigned r = static_cast<unsigned>((x - b0) % BLOCK);
				const uint32_t below = (1u << r) - 1;
				// The multi-byte and the 3-byte bytes before the value's first, in one count.
				out = block_out + starts[k] + r + PopCount(masks[k] & (below | (below << 16)));
			}
			if (have_prev) {
				views[prev_row] = MakeView(dst + prev_out, static_cast<uint32_t>(out - prev_out), inline_length);
			}
			prev_row = row;
			prev_out = out;
			have_prev = true;
		}
		if (last) {
			break;
		}
		if (!ascii) {
			memset(masks, 0, granules * sizeof(masks[0]));	// for the next block: an ASCII one writes none
		}
		b0 = b1;
	}
	if (have_prev) {
		views[prev_row] =
			MakeView(dst + prev_out, static_cast<uint32_t>(static_cast<size_t>(d - dst) - prev_out), inline_length);
	}
	return static_cast<size_t>(d - dst);
}

#endif

}  // namespace

namespace detail {

Compaction::Compaction() {
	for (unsigned m = 0; m < 256; m++) {
		unsigned k = 0;
		for (unsigned i = 0; i < 8; i++) {
			shuffle[m][k++] = static_cast<uint8_t>(2 * i);
			if (m & (1u << i)) {
				shuffle[m][k++] = static_cast<uint8_t>(2 * i + 1);
			}
		}
		length[m] = static_cast<uint8_t>(k);
		for (; k < 16; k++) {
			shuffle[m][k] = 0xFF;
		}
	}
}

Compaction3::Compaction3() {
	for (unsigned m = 0; m < 256; m++) {
		unsigned k = 0;
		for (unsigned lane = 0; lane < 4; lane++) {
			const unsigned code = (m >> (2 * lane)) & 3u;
			const unsigned bytes = code == 3 ? 3 : code + 1;
			for (unsigned b = 0; b < bytes; b++) {
				shuffle[m][k++] = static_cast<uint8_t>(4 * lane + b);
			}
		}
		length[m] = static_cast<uint8_t>(k);
		for (; k < 16; k++) {
			shuffle[m][k] = 0xFF;
		}
	}
}

const Compaction &GetCompaction() {
	static const Compaction compaction;
	return compaction;
}

const Compaction3 &GetCompaction3() {
	static const Compaction3 compaction;
	return compaction;
}

Isa DetectIsa() {
#if defined(MSSQL_CODE_PAGE_NEON)
	return Isa::Neon;
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(MSSQL_CODE_PAGE_PORTABLE) && \
	!defined(MSSQL_CODE_PAGE_NO_X86_KERNELS)
#if defined(_MSC_VER) && !defined(__clang__)
	int info[4];
	__cpuid(info, 0);
	if (info[0] < 7) {
		return Isa::Scalar;
	}
	__cpuid(info, 1);
	const bool osxsave = (info[2] & (1 << 27)) != 0, avx = (info[2] & (1 << 28)) != 0;
	if (!osxsave || !avx || (_xgetbv(0) & 0x6) != 0x6) {
		return Isa::Scalar;	 // the OS does not save the YMM state
	}
	const bool popcnt = (info[2] & (1 << 23)) != 0;
	const unsigned long long xcr0 = _xgetbv(0);
	__cpuidex(info, 7, 0);
	const bool avx2 = (info[1] & (1 << 5)) != 0, bmi2 = (info[1] & (1 << 8)) != 0;
	const bool avx512f = (info[1] & (1 << 16)) != 0, avx512bw = (info[1] & (1 << 30)) != 0;
	const bool vbmi = (info[2] & (1 << 1)) != 0, vbmi2 = (info[2] & (1 << 6)) != 0;
	// The OS must save the opmask and ZMM state too (XCR0 bits 5-7). AVX2 too: ForceIsa lets
	// the AVX2 kernels run where AVX-512 was found.
	if (avx2 && avx512f && avx512bw && vbmi && vbmi2 && bmi2 && popcnt && (xcr0 & 0xE6) == 0xE6) {
		return Isa::Avx512;
	}
	return avx2 && bmi2 ? Isa::Avx2 : Isa::Scalar;
#else
	__builtin_cpu_init();
	if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vbmi") && __builtin_cpu_supports("avx512vbmi2") &&
		__builtin_cpu_supports("bmi2") && __builtin_cpu_supports("popcnt")) {
		return Isa::Avx512;
	}
	return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") ? Isa::Avx2 : Isa::Scalar;
#endif
#else
	return Isa::Scalar;
#endif
}

namespace {
std::atomic<uint8_t> &IsaState() {
	static std::atomic<uint8_t> state(static_cast<uint8_t>(DetectIsa()));
	return state;
}
}  // namespace

Isa ActiveIsa() {
	return static_cast<Isa>(IsaState().load(std::memory_order_relaxed));
}

bool ForceIsa(Isa isa) {
	const Isa best = DetectIsa();
	// An instruction set the CPU lacks is refused; Scalar always runs.
	const bool supported = isa == Isa::Scalar || isa == best || (best == Isa::Avx512 && isa == Isa::Avx2);
	if (supported) {
		IsaState().store(static_cast<uint8_t>(isa), std::memory_order_relaxed);
	}
	return supported;
}

const char *IsaName(Isa isa) {
	switch (isa) {
	case Isa::Scalar:
		return "scalar";
	case Isa::Neon:
		return "neon";
	case Isa::Avx2:
		return "avx2";
	case Isa::Avx512:
		return "avx512";
	}
	return "?";
}

}  // namespace detail

const CodePageTable *FindCodePage(int32_t code_page) {
	for (size_t i = 0; i < tables::CODE_PAGE_COUNT; i++) {
		if (tables::CODE_PAGES[i].code_page == code_page) {
			return &tables::CODE_PAGES[i];
		}
	}
	return nullptr;
}

bool IsDoubleByte(const CodePageTable &page) {
	return page.lead_row != nullptr;
}

size_t DecodeToUtf8(const CodePageTable &page, const uint8_t *src, size_t n, char *dst) {
	if (IsDoubleByte(page)) {
		return static_cast<size_t>(DecodeRun<true>(page, src, src + n, src + n, dst) - dst);
	}
#if defined(MSSQL_CODE_PAGE_NEON)
	if (detail::ActiveIsa() == detail::Isa::Neon) {
		const NeonKernel kernel(page);
		NoRecord none;
		return static_cast<size_t>(DecodeNeonFor(kernel, page.code_page, src, src + n, dst, none) - dst);
	}
#elif defined(__x86_64__) || defined(_M_X64)
	if (detail::ActiveIsa() == detail::Isa::Avx512) {
		return static_cast<size_t>(detail::DecodeSingleByteAvx512(page, src, src + n, dst) - dst);
	}
	if (detail::ActiveIsa() == detail::Isa::Avx2) {
		return static_cast<size_t>(detail::DecodeSingleByteAvx2(page, src, src + n, dst) - dst);
	}
#endif
	return static_cast<size_t>(DecodeRun<false>(page, src, src + n, src + n, dst) - dst);
}

size_t CopyAscii(const uint8_t *src, size_t n, char *dst) {
	size_t i = 0;
	for (; i + 64 <= n; i += 64) {
		if (!CopyAscii64(dst + i, src + i)) {
			// The block was copied whole; report where its first non-ASCII byte is.
			return i + FirstNonAscii(src + i, 64);
		}
	}
	for (; i < n; i++) {
		if (src[i] >= 0x80) {
			return i;
		}
		dst[i] = static_cast<char>(src[i]);
	}
	return n;
}

size_t DecodeToUtf16(const CodePageTable &page, const uint8_t *src, size_t n, char16_t *dst) {
	return IsDoubleByte(page) ? DoubleByteToUtf16(page, src, n, dst) : SingleByteToUtf16(page, src, n, dst);
}

size_t DecodeValues(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
					const uint32_t *lengths, size_t count, char *dst, uint64_t *out_offsets, uint32_t *out_lengths) {
	if (IsDoubleByte(page)) {
		return DecodeValuesImpl<true>(page, src, src_size, offsets, lengths, count, dst, out_offsets, out_lengths);
	}
#if defined(MSSQL_CODE_PAGE_NEON)
	if (detail::ActiveIsa() == detail::Isa::Neon) {
		return DecodeValuesSingleByteNeon(page, src, src_size, offsets, lengths, count, dst, out_offsets, out_lengths);
	}
#elif defined(__x86_64__) || defined(_M_X64)
	if (detail::ActiveIsa() == detail::Isa::Avx512) {
		return detail::DecodeValuesSingleByteAvx512(page, src, offsets, lengths, count, dst, out_offsets, out_lengths);
	}
	if (detail::ActiveIsa() == detail::Isa::Avx2) {
		const uint8_t *readable = src + src_size;
		char *d = dst;
		for (size_t i = 0; i < count; i++) {
			const uint8_t *p = src + offsets[i];
			const uint8_t *end = p + lengths[i];
			char *start = d;
			d = lengths[i] >= 32 ? detail::DecodeSingleByteAvx2(page, p, end, d)
								 : DecodeRun<false>(page, p, end, readable, d);
			out_offsets[i] = static_cast<uint64_t>(start - dst);
			out_lengths[i] = static_cast<uint32_t>(d - start);
		}
		return static_cast<size_t>(d - dst);
	}
#endif
	return DecodeValuesImpl<false>(page, src, src_size, offsets, lengths, count, dst, out_offsets, out_lengths);
}

size_t DecodeColumn(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
					const uint32_t *lengths, const uint64_t *validity, size_t count, char *dst, StringView16 *views,
					size_t inline_length) {
	assert(inline_length == 0 || inline_length == 12);
#ifndef NDEBUG
	// The recorded path ends a value where the next valid one starts and never reads lengths:
	// the values must lie back to back, ending at src_size, or it and the by-value path differ.
	{
		size_t next = 0;
		for (size_t row = 0; row < count; row++) {
			if (RowValid(validity, row)) {
				assert(offsets[row] == next);
				next += lengths[row];
			}
		}
		assert(next == src_size);
	}
#endif
	if (IsDoubleByte(page)) {
		return DecodeColumnByValue<true>(page, src, src_size, offsets, lengths, validity, count, dst, views,
										 inline_length);
	}
#if defined(MSSQL_CODE_PAGE_NEON)
	if (detail::ActiveIsa() == detail::Isa::Neon) {
		// The kernel is built inside the call, once a block: a local whose address does not
		// escape keeps its constants in registers (a captured one is reloaded after every
		// store through the output, which a char store may alias).
		return DecodeColumnRecorded(src, src_size, offsets, validity, count, dst, views, inline_length,
									[&page](const uint8_t *p, const uint8_t *end, char *d, StepRecorder recorder) {
										const NeonKernel kernel(page);
										return DecodeNeonFor(kernel, page.code_page, p, end, d, recorder);
									});
	}
#endif
	return DecodeColumnByValue<false>(page, src, src_size, offsets, lengths, validity, count, dst, views,
									  inline_length);
}

size_t ExactUtf8Length(const CodePageTable &page, const uint8_t *src, size_t n) {
	return IsDoubleByte(page) ? Utf8LengthImpl<true>(page, src, n) : Utf8LengthImpl<false>(page, src, n);
}

const CollationRow *FindCollationById(int32_t collation_id) {
	size_t lo = 0, hi = tables::COLLATION_COUNT;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const int32_t id = tables::COLLATIONS[mid].collation_id;
		if (id == collation_id) {
			return &tables::COLLATIONS[mid];
		}
		if (id < collation_id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return nullptr;
}

namespace {

inline char AsciiLower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

//! <0, 0, >0 comparing a collation name with (name, length), ASCII case-insensitively.
int CompareName(const char *table_name, const char *name, size_t length) {
	size_t i = 0;
	for (; i < length && table_name[i]; i++) {
		const char a = AsciiLower(table_name[i]), b = AsciiLower(name[i]);
		if (a != b) {
			return static_cast<unsigned char>(a) < static_cast<unsigned char>(b) ? -1 : 1;
		}
	}
	if (i == length) {
		return table_name[i] ? 1 : 0;
	}
	return -1;
}

}  // namespace

const CollationRow *FindCollationByName(const char *name, size_t length) {
	size_t lo = 0, hi = tables::COLLATION_COUNT;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const CollationRow &row = tables::COLLATIONS[tables::COLLATION_BY_NAME[mid]];
		const int c = CompareName(row.name, name, length);
		if (c == 0) {
			return &row;
		}
		if (c < 0) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return nullptr;
}

int32_t CodePageForWireCollation(uint32_t lcid, bool utf8, uint8_t sort_id) {
	if (sort_id) {
		return tables::SORT_ID_CODE_PAGE[sort_id];
	}
	size_t lo = 0, hi = tables::LCID_CODE_PAGE_COUNT;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const tables::LcidCodePage &e = tables::LCID_CODE_PAGES[mid];
		if (e.lcid == lcid && e.utf8 == utf8) {
			return static_cast<int32_t>(e.code_page);
		}
		if (e.lcid < lcid || (e.lcid == lcid && e.utf8 < utf8)) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return CODE_PAGE_NOT_FOUND;
}

}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

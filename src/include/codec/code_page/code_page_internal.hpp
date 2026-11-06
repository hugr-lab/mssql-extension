//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/code_page/code_page_internal.hpp
//
// What the code-page kernels share across translation units (spec 091): the
// compaction tables, the instruction-set choice, and the x86 kernels, which
// live in their own files so each is compiled for its instruction set alone
// (code_page_avx2.cpp, code_page_avx512.cpp: every function marked with its
// target attribute, nothing compiled for AVX2 / AVX-512 that a caller without
// it could reach). Not part of the module's API: code_page.hpp is.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/code_page/code_page.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {
namespace detail {

//! simdutf's compaction for UTF-16 -> UTF-8 applied to code pages: 8 characters interleaved
//! as (first, second) byte pairs, and a mask of which ones have a second byte; the shuffle
//! keeps each first byte and the second bytes the mask names. 256 x 16 bytes, built once.
struct Compaction {
	uint8_t shuffle[256][16];
	uint8_t length[256];
	Compaction();
};

//! simdutf's 1/2/3-byte compaction: 4 characters as 32-bit lanes [b0 b1 b2 0] and a 2-bit
//! length code per character (0, 1, 2 = 1, 2, 3 bytes); the shuffle keeps each lane's
//! first 1-3 bytes. 256 x 16 bytes, built once.
struct Compaction3 {
	uint8_t shuffle[256][16];
	uint8_t length[256];
	Compaction3();
};

//! What a kernel records for DecodeColumn: for each 16 input bytes that are not all ASCII,
//! which of them read as 2 or 3 UTF-8 bytes (bit i for byte i) and which as 3 (bit 16 + i),
//! in masks[j] for the 16 bytes at in + 16 j; the caller zeroes masks[] first, so an ASCII
//! step records nothing. The output positions follow from the masks (16 a granule, one per
//! bit). The kernel names what it records by its input position, so the recorder is a value
//! with nothing to update (passed by value, it stays in registers: a char store through the
//! output could alias a recorder in memory).
struct StepRecorder {
	static constexpr bool ON = true;
	uint32_t *masks;
	const uint8_t *in;
	inline size_t Index(const uint8_t *at) const {
		return static_cast<size_t>(at - in) >> 4;
	}
	//! The 16 input bytes at `at` (fewer at the end of the input), whose output starts at `d`.
	inline void Record16(const uint8_t *at, const char *d, unsigned multi, unsigned three) const {
		PrefetchOutput(d);
		masks[Index(at)] = multi | (three << 16);
	}
	//! The 32 bytes at `at`; the masks bit i for byte i.
	inline void Record32(const uint8_t *at, const char *d, uint32_t multi, uint32_t three) const {
		PrefetchOutput(d);
		const uint64_t pair = static_cast<uint64_t>((multi & 0xFFFFu) | (three << 16)) |
							  (static_cast<uint64_t>((multi >> 16) | (three & 0xFFFF0000u)) << 32);
		memcpy(masks + Index(at), &pair, sizeof(pair));
	}
	//! The output line 512 bytes ahead, for writing. A recorded step's mask store is a second
	//! stream of stores beside the output's, and on Apple M4 that stops the core from
	//! writing whole output lines without reading them first: past L1 every output line is
	//! then read from L2 before it is written (a copy loop with one 8-byte store a step
	//! measured 26 vs 9 us per 512 KB; with this prefetch 12.6). An ASCII step stores no
	//! mask and needs none.
	static inline void PrefetchOutput(const char *d) {
#if defined(__GNUC__) || defined(__clang__)
		// Through the integer: d + 512 may lie past the allocation, which a prefetch tolerates.
		__builtin_prefetch(reinterpret_cast<const char *>(reinterpret_cast<uintptr_t>(d) + 512), 1, 3);
#else
		(void)d;
#endif
	}
	//! 32 ASCII bytes: nothing to record.
	inline void Ascii32(const uint8_t *) const {}
};

//! The recorder of DecodeToUtf8 and DecodeValues: compiles to nothing.
struct NoRecord {
	static constexpr bool ON = false;
	inline void Record16(const uint8_t *, const char *, unsigned, unsigned) const {}
	inline void Record32(const uint8_t *, const char *, uint32_t, uint32_t) const {}
	inline void Ascii32(const uint8_t *) const {}
};

const Compaction &GetCompaction();
const Compaction3 &GetCompaction3();

//! The instruction set the single-byte kernels run with: the best the CPU has, found once
//! (cpuid on x86, NEON on aarch64), or what a test forced.
enum class Isa : uint8_t { Scalar, Neon, Avx2, Avx512 };

Isa ActiveIsa();
//! The best instruction set this CPU supports for the kernels.
Isa DetectIsa();
//! Tests only: run the kernels with `isa` (which the CPU must support; refused otherwise),
//! returning whether it took. ForceIsa(DetectIsa()) restores the default.
bool ForceIsa(Isa isa);
const char *IsaName(Isa isa);

#if defined(__x86_64__) || defined(_M_X64)
//! AVX2 kernels for a single-byte page over [p, end), 32 bytes a step: ASCII stored as it
//! is; 1252 / 1251 / 874 blocks whose non-ASCII bytes are the page's letters by arithmetic;
//! any other block, any page, by vpgatherdd from the packed table and Compaction3. The tail
//! under 32 bytes through the table a byte at a time. Return the new output end; write at
//! most STORE_SLACK bytes past it. Defined in code_page_avx2.cpp.
char *DecodeSingleByteAvx2(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d);
//! The AVX-512 VBMI2 kernel, one for every single-byte page: 64 bytes a step, the three UTF-8
//! byte planes by vpermi2b over the page's whole high half, packed by vpcompressb, the last
//! step loaded and stored under a mask. Same contract, and nothing is written past the
//! returned end. Defined in code_page_avx512.cpp.
char *DecodeSingleByteAvx512(const CodePageTable &page, const uint8_t *p, const uint8_t *end, char *d);
//! DecodeValues for a single-byte page through it, every value whatever its length (the
//! masked last step reads nothing past a value), the page prepared once.
size_t DecodeValuesSingleByteAvx512(const CodePageTable &page, const uint8_t *src, const uint32_t *offsets,
									const uint32_t *lengths, size_t count, char *dst, uint64_t *out_offsets,
									uint32_t *out_lengths);
#endif

}  // namespace detail
}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

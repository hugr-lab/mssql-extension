//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/code_page/code_page.hpp
//
// SQL Server code pages (spec 091): how the server reads the bytes of a
// varchar under each of the 16 code pages it stores varchar in, and which code
// page a collation uses. One module for everything that reads code-page bytes:
// the live read path, a raw mssql_scan, the backup reader. Encoding (UTF-8 to a
// code page, for the write path) is not here yet: it is spec 091's follow-up.
//
// The tables are the server's own answers (tools/code_pages/), so a decode
// here reads a byte exactly as CAST(… AS nvarchar) on the server would,
// invalid double-byte sequences included.
//
// THE KERNEL
// ----------
// Decoding is one pass over the input and one sequential write of the output:
// no validation pass before it, no per-value call through a pointer.
//   - A single-byte page on arm64 takes the NEON kernel, 32 bytes a step (two
//     16-byte blocks, so their table lookups overlap), in tiers: an all-ASCII
//     step is stored as it is; one whose non-ASCII bytes all lie in the page's
//     run of byte + delta (1251 C0-FF, 1252 A0-FF) gets its UTF-8 by
//     arithmetic; one of 1- and 2-byte characters by two TBX (vqtbx4q) byte
//     planes, packed 8 characters a store by a shuffle table (simdutf's UTF-16
//     -> UTF-8 compaction); one holding a 3-byte character by a third plane
//     and simdutf's 1/2/3-byte packing (Compaction3), 4 characters a store.
//   - A single-byte page on x86-64 takes the best kernel the CPU has, chosen
//     once at run time (cpuid; neither is the x86-64 baseline): AVX-512 VBMI2,
//     64 bytes a step, the UTF-8 byte planes of the page's whole high half by
//     vpermi2b and packed by vpcompressb, the last step loaded and stored under
//     a mask (so DecodeValues takes every value through it); else AVX2, 32
//     bytes a step, 1252 / 1251 / 874 by the page's arithmetic and the rest by
//     vpgatherdd from the packed table, packed by pshufb (Compaction /
//     Compaction3). code_page_internal.hpp has the dispatch.
//   - Everything else (a double-byte page; a single-byte page with no SIMD; a
//     value shorter than 32 bytes in DecodeValues on arm64 or under AVX2)
//     alternates a 16-byte ASCII step (one load, one test of the high bits,
//     one store: SSE2 on x86-64 and NEON on arm64, both baseline; two 8-byte
//     SWAR words elsewhere) with a table step for the other bytes:
//     their UTF-8 from a 1 KB table (16 cache lines), written four bytes at a
//     time and advancing by the length, with no branch on it. A double-byte
//     page adds a 256-byte lead table and a code-point table of 64 KB or less,
//     encoded to UTF-8 arithmetically rather than stored pre-encoded (which
//     would be 128 KB per page).
// Callers give the output Utf8Bound(n) bytes: up to 3 per input byte plus
// STORE_SLACK. A decode writes at most STORE_SLACK bytes past the length it
// returns (the 16-byte stores, the 4-byte packed store).
//
// No DuckDB include, so the fuzz harness and the unit test link this module
// alone.
//===----------------------------------------------------------------------===//

#pragma once

#include "codec/code_page/code_page_tables.hpp"

#include <cstddef>
#include <cstdint>

namespace duckdb {
namespace mssql {
namespace codec {
namespace code_page {

using tables::CodePageTable;
using tables::CollationRow;

//! Code page values with a meaning of their own. CODE_PAGE_UNICODE_ONLY is what
//! COLLATIONPROPERTY(…, 'CodePage') answers for a Unicode-only collation;
//! CODE_PAGE_NOT_FOUND is this module's "the generated tables have no such key".
//!
//! Not to be confused with catalog/mssql_code_page.hpp's CODE_PAGE_UNKNOWN,
//! which is 0 and means "cannot tell" (a non-text column, a Unicode-only
//! collation, a page nothing was read for): a different notion with the number
//! this module calls CODE_PAGE_UNICODE_ONLY. mssql::CodePageCanEncode there
//! still answers from its own tables; moving it onto this module's is spec 091
//! D8, a follow-up.
constexpr int32_t CODE_PAGE_UNICODE_ONLY = 0;
constexpr int32_t CODE_PAGE_UTF8 = 65001;
constexpr int32_t CODE_PAGE_NOT_FOUND = -1;

//! Bytes a decode may write past the end of its output (the 16-byte stores and
//! the 4-byte packed store).
constexpr size_t STORE_SLACK = 16;
//! The largest UTF-8 a code-page byte reads as: 3 (U+0800-U+FFFF). A
//! double-byte pair reads as at most 3 too, and a single-byte half-width
//! katakana in 932 also as 3, so 3 per input byte bounds every page.
constexpr size_t MAX_UTF8_PER_BYTE = 3;

//! Output bytes a decode of n input bytes needs.
inline size_t Utf8Bound(size_t n) {
	return n * MAX_UTF8_PER_BYTE + STORE_SLACK;
}

//! The table of a code page, or nullptr: 65001 and 0 have none (UTF-8 is read
//! as is, Unicode-only has no varchar), and neither has any number SQL Server
//! does not store varchar in. Every CodePageTable the functions below take must
//! be one this returned (or an entry of tables::CODE_PAGES): the NEON kernel
//! finds the page's prepared planes by CodePageTable::index.
const CodePageTable *FindCodePage(int32_t code_page);
bool IsDoubleByte(const CodePageTable &page);

//! Decode n code-page bytes into dst, which holds Utf8Bound(n) bytes; returns
//! the UTF-8 length. A lead byte at the end of the input reads as nothing, as
//! the server drops it.
size_t DecodeToUtf8(const CodePageTable &page, const uint8_t *src, size_t n, char *dst);

//! The ASCII fast path of a chunk, for a caller that tries the all-ASCII case
//! first (DecodeToUtf8 and DecodeValues do not call it): copies n bytes to dst
//! while they are ASCII, 64 per step (one pass: the test and the copy are the
//! same loads), and returns how many leading bytes are ASCII (n when all are;
//! dst then holds them, which ARE their UTF-8). dst holds n bytes, and nothing
//! is written past them; past the returned count its content is undefined.
size_t CopyAscii(const uint8_t *src, size_t n, char *dst);

//! Stage 1 of the two-stage decode: n code-page bytes as UTF-16LE code units
//! into dst, which holds n units: n are written for a single-byte page, at most
//! n for a double-byte one, and nothing past dst + n either way (no
//! STORE_SLACK). A double-byte page may write units past the returned count,
//! up to dst + n, as scratch. Returns the units. Stage 2 is the read path's
//! UTF-16 kernel.
size_t DecodeToUtf16(const CodePageTable &page, const uint8_t *src, size_t n, char16_t *dst);

//! Decode `count` values staged back to back in one buffer (the read path's
//! column staging, a backup page's records): value i is
//! src[offsets[i], offsets[i] + lengths[i]), and src_size bounds every read, so
//! a value may be read past its end into the next one but never past the
//! buffer. Output is written sequentially into dst (Utf8Bound of the sum of
//! the lengths), value i at out_offsets[i] with out_lengths[i] bytes. Lead
//! bytes do not run across values: each value ends where its length says.
//! Returns the bytes written.
size_t DecodeValues(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
					const uint32_t *lengths, size_t count, char *dst, uint64_t *out_offsets, uint32_t *out_lengths);

//! A string view of 16 bytes in DuckDB's string_t layout (the Umbra form), which
//! DecodeColumn writes so a chunk's vector needs no loop of its own over the
//! values. Little-endian words:
//!   lo: the length in its low 32 bits, then the value's first 4 bytes;
//!   hi: for a length up to the inline length, the value's bytes 4-11; past
//!       it, the address of the value's first byte.
//! An inlined view is zero past the length (string_t compares the words). With
//! an inline length of 0 (DuckDB's DUCKDB_DEBUG_NO_INLINE) lo carries no
//! prefix. Spec 092's wrapper checks the layout against duckdb::string_t.
struct StringView16 {
	uint64_t lo;
	uint64_t hi;
};

//! Decode one column of a chunk (spec 092 D12): `count` rows, row r valid when
//! bit r of `validity` is set (DuckDB's ValidityMask words; nullptr: all valid)
//! with its value at src[offsets[r], offsets[r] + lengths[r]). The valid rows'
//! values lie back to back in row order and end at src_size, as the read
//! path's binary staging lays them out; a NULL row's offset and length are not
//! read. Writes their UTF-8 back to back into dst (Utf8Bound(src_size) bytes)
//! and row r's view into views[r] -- a NULL row's is empty --, pointing into
//! dst and inline up to `inline_length` bytes (string_t::INLINE_LENGTH: 12, or 0
//! under DUCKDB_DEBUG_NO_INLINE; no other value). On a
//! single-byte page with SIMD the views come from what the kernel records as
//! it decodes (each 16 input bytes' output position and masks), one pass over
//! the bytes and none over the output; otherwise value by value (a double-byte
//! pair must not cross two values). Returns the bytes written to dst.
size_t DecodeColumn(const CodePageTable &page, const uint8_t *src, size_t src_size, const uint32_t *offsets,
					const uint32_t *lengths, const uint64_t *validity, size_t count, char *dst, StringView16 *views,
					size_t inline_length);

//! The exact UTF-8 length of n code-page bytes, for an output too large to
//! size at 3 bytes per input byte (a chunk past ~1.4 GB of payload). A decode
//! into it still needs STORE_SLACK bytes after that length.
size_t ExactUtf8Length(const CodePageTable &page, const uint8_t *src, size_t n);

//! Collations: by the CollationId a backup's catalog stores, and by name
//! (case-insensitive, as SQL Server compares collation names). nullptr when
//! unknown.
const CollationRow *FindCollationById(int32_t collation_id);
const CollationRow *FindCollationByName(const char *name, size_t length);

//! The code page of a collation as the wire carries it (COLMETADATA's five
//! bytes: LCID, flags, version, SortId). A non-zero SortId names a SQL_
//! collation and decides alone; otherwise the LCID and the UTF-8 flag do.
//! CODE_PAGE_NOT_FOUND when the generated tables have no such key.
int32_t CodePageForWireCollation(uint32_t lcid, bool utf8, uint8_t sort_id);

}  // namespace code_page
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb

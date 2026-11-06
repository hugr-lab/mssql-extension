// Fuzz harness: SQL Server code-page decode kernels (spec 091).
//
// A varchar's bytes come from the server (and, for the backup reader, from a file), so
// every kernel meets arbitrary bytes. The first input byte picks one of the generated
// code pages (tables::CODE_PAGES); the rest is the varchar. Each kernel is checked
// against the others, so a disagreement is a bug even when no sanitizer fires:
//   - DecodeToUtf8 writes valid UTF-8, at most MAX_UTF8_PER_BYTE per input byte, and
//     nothing past its returned length + STORE_SLACK (a canary follows);
//   - stage 1 (DecodeToUtf16) writes nothing past n units, and with the UTF-16 conversion
//     gives the same bytes;
//   - ExactUtf8Length gives the same length;
//   - DecodeValues over the input cut into values (0-70 bytes, so both its NEON and its
//     16-byte paths run on arm64) returns the sum of its out_lengths, lays the values out
//     contiguously, writes nothing past that sum + STORE_SLACK, and gives, value by value,
//     what DecodeToUtf8 gives for that value alone;
//   - CopyAscii stops at the first byte >= 0x80 and writes nothing past n.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "codec/code_page/code_page.hpp"
#include "simdutf.h"

using namespace duckdb::mssql::codec::code_page;

namespace {

const unsigned char CANARY = 0xA5;
const char16_t CANARY16 = 0xA5A5;
//! The longest value the input is cut into.
const size_t MAX_VALUE = 70;

void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

void RequireCanary(const std::vector<char> &out, size_t from) {
	for (size_t i = from; i < out.size(); i++) {
		Require(static_cast<unsigned char>(out[i]) == CANARY);
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size == 0) {
		return 0;
	}
	const CodePageTable &page = tables::CODE_PAGES[data[0] % tables::CODE_PAGE_COUNT];
	const uint8_t *in = data + 1;
	const size_t n = size - 1;

	const size_t bound = Utf8Bound(n);
	std::vector<char> utf8(bound + 32, static_cast<char>(CANARY));
	const size_t len = DecodeToUtf8(page, in, n, utf8.data());
	Require(len <= n * MAX_UTF8_PER_BYTE);
	RequireCanary(utf8, len + STORE_SLACK);
	Require(simdutf::validate_utf8(utf8.data(), len));
	Require(ExactUtf8Length(page, in, n) == len);

	std::vector<char16_t> units(n + 16, CANARY16);
	const size_t u = DecodeToUtf16(page, in, n, units.data());
	Require(u <= n);
	for (size_t i = n; i < units.size(); i++) {
		Require(units[i] == CANARY16);
	}
	std::vector<char> via16(u * 3 + 1);
	const size_t len16 = simdutf::convert_valid_utf16le_to_utf8(units.data(), u, via16.data());
	Require(len16 == len && memcmp(via16.data(), utf8.data(), len) == 0);

	// Cut the input into values at the lengths its own bytes give, as staging lays them
	// out; an empty value skips one byte.
	std::vector<uint32_t> offsets, lengths;
	for (size_t pos = 0; pos < n;) {
		const size_t want = in[pos] % (MAX_VALUE + 1);
		const size_t take = want < n - pos ? want : n - pos;
		offsets.push_back(static_cast<uint32_t>(pos));
		lengths.push_back(static_cast<uint32_t>(take));
		pos += take ? take : 1;
	}
	std::vector<char> out(bound + 32, static_cast<char>(CANARY));
	std::vector<uint64_t> out_offsets(offsets.size());
	std::vector<uint32_t> out_lengths(offsets.size());
	const size_t total = DecodeValues(page, in, n, offsets.data(), lengths.data(), offsets.size(), out.data(),
									  out_offsets.data(), out_lengths.data());
	RequireCanary(out, total + STORE_SLACK);
	std::vector<char> one(Utf8Bound(MAX_VALUE) + 32);
	uint64_t next = 0;
	for (size_t i = 0; i < offsets.size(); i++) {
		Require(out_offsets[i] == next);
		next += out_lengths[i];
		const size_t l = DecodeToUtf8(page, in + offsets[i], lengths[i], one.data());
		Require(l == out_lengths[i] && memcmp(one.data(), out.data() + out_offsets[i], l) == 0);
	}
	Require(next == total);

	std::vector<char> copy(n + 16, static_cast<char>(CANARY));
	const size_t ascii = CopyAscii(in, n, copy.data());
	Require(ascii <= n && memcmp(copy.data(), in, ascii) == 0);
	RequireCanary(copy, n);
	for (size_t i = 0; i < ascii; i++) {
		Require(in[i] < 0x80);
	}
	Require(ascii == n || in[ascii] >= 0x80);
	return 0;
}

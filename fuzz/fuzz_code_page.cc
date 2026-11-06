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
//   - DecodeColumn over the input cut into values back to back (empty ones included), with
//     NULL rows between them whose offsets / lengths are garbage, writes each value's UTF-8
//     back to back and each row's string_t view: a valid row's view holds what DecodeToUtf8
//     gives for that value alone (inline up to 12 bytes or 0, the input's choice), a NULL
//     row's is zero, and nothing is written past the total + STORE_SLACK;
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

//! The string a view in string_t's layout holds (StringView16).
std::vector<char> ViewBytes(const StringView16 &view, size_t inline_length) {
	const uint32_t length = static_cast<uint32_t>(view.lo);
	if (length <= inline_length) {
		char bytes[12];
		memcpy(bytes, reinterpret_cast<const char *>(&view.lo) + 4, 4);
		memcpy(bytes + 4, &view.hi, 8);
		return std::vector<char>(bytes, bytes + length);
	}
	const char *data = reinterpret_cast<const char *>(static_cast<uintptr_t>(view.hi));
	return std::vector<char>(data, data + length);
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

	// DecodeColumn: values back to back (an empty one consumes nothing, the value after it
	// at least one byte), a NULL row now and then with garbage offset / length.
	{
		std::vector<uint32_t> col_offsets, col_lengths;
		std::vector<uint64_t> validity;
		bool any_null = false;
		for (size_t pos = 0; pos < n;) {
			const uint8_t b = in[pos];
			const size_t row = col_offsets.size();
			if (validity.size() <= row / 64) {
				validity.push_back(~uint64_t(0));
			}
			if (b % 11 == 3) {
				validity[row / 64] &= ~(uint64_t(1) << (row % 64));
				col_offsets.push_back(0xDEADBEEF);
				col_lengths.push_back(0xDEADBEEF);
				any_null = true;
			}
			const size_t row2 = col_offsets.size();
			if (validity.size() <= row2 / 64) {
				validity.push_back(~uint64_t(0));
			}
			size_t take = b % (MAX_VALUE + 1);
			if (take > n - pos) {
				take = n - pos;
			}
			col_offsets.push_back(static_cast<uint32_t>(pos));
			col_lengths.push_back(static_cast<uint32_t>(take));
			if (take == 0) {  // the empty value, then one of at least a byte so the cut moves on
				const size_t row3 = col_offsets.size();
				if (validity.size() <= row3 / 64) {
					validity.push_back(~uint64_t(0));
				}
				take = 1;
				col_offsets.push_back(static_cast<uint32_t>(pos));
				col_lengths.push_back(1);
			}
			pos += take;
		}
		const size_t count = col_offsets.size();
		const size_t inline_length = (data[0] & 0x80) ? 0 : 12;
		std::vector<char> col(bound + 32, static_cast<char>(CANARY));
		std::vector<StringView16> views(count, StringView16{0x1111, 0x2222});
		const size_t col_total =
			DecodeColumn(page, in, n, col_offsets.data(), col_lengths.data(), any_null ? validity.data() : nullptr,
						 count, col.data(), views.data(), inline_length);
		RequireCanary(col, col_total + STORE_SLACK);
		size_t at = 0;
		for (size_t r = 0; r < count; r++) {
			if (((validity[r / 64] >> (r % 64)) & 1) == 0) {
				Require(views[r].lo == 0 && views[r].hi == 0);
				continue;
			}
			const size_t l = DecodeToUtf8(page, in + col_offsets[r], col_lengths[r], one.data());
			const std::vector<char> got = ViewBytes(views[r], inline_length);
			Require(got.size() == l && (l == 0 || memcmp(got.data(), one.data(), l) == 0));
			Require(l == 0 || memcmp(col.data() + at, one.data(), l) == 0);
			if (l > inline_length) {
				Require(reinterpret_cast<const char *>(static_cast<uintptr_t>(views[r].hi)) == col.data() + at);
			}
			at += l;
		}
		Require(col_total == at);
	}

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

#include "backup/backup_xpress.hpp"

#include "backup/backup_bytes.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr size_t TABLE_BYTES = 256;
static constexpr size_t SYMBOLS = 512;
static constexpr unsigned MAX_CODE = 15;
static constexpr size_t CHUNK = 65536;

[[noreturn]] static void Fail(const std::string &what, size_t in, size_t out) {
	throw FormatError("MS_XPRESS block: " + what + " (input byte " + std::to_string(in) + ", output byte " +
					  std::to_string(out) + ")");
}

//! The canonical code of a 256-byte length table as a 2^15-entry lookup of (symbol << 4 | length);
//! 0 marks a code no symbol has. Lengths are 4 bits, so <= 15 by construction; an over-subscribed
//! table (the lengths' Kraft sum above 1) is refused.
static void BuildTable(const uint8_t *lengths_bytes, std::vector<uint16_t> &table, size_t in, size_t out) {
	uint8_t lengths[SYMBOLS];
	for (size_t i = 0; i < TABLE_BYTES; i++) {
		lengths[2 * i] = lengths_bytes[i] & 0x0F;
		lengths[2 * i + 1] = lengths_bytes[i] >> 4;
	}
	uint32_t kraft = 0;	 // in units of 2^-15
	for (size_t sym = 0; sym < SYMBOLS; sym++) {
		if (lengths[sym]) {
			kraft += 1u << (MAX_CODE - lengths[sym]);
		}
	}
	if (kraft > (1u << MAX_CODE)) {
		Fail("a Huffman table whose code lengths over-subscribe the code space", in, out);
	}
	if (kraft == 0) {
		Fail("a Huffman table with no symbol", in, out);
	}
	table.assign(static_cast<size_t>(1) << MAX_CODE, 0);
	uint32_t code = 0;
	for (unsigned len = 1; len <= MAX_CODE; len++) {
		for (size_t sym = 0; sym < SYMBOLS; sym++) {
			if (lengths[sym] != len) {
				continue;
			}
			const uint32_t span = 1u << (MAX_CODE - len);
			const uint32_t start = code << (MAX_CODE - len);
			const uint16_t entry = static_cast<uint16_t>((sym << 4) | len);
			for (uint32_t i = 0; i < span; i++) {
				table[start + i] = entry;
			}
			code++;
		}
		code <<= 1;
	}
}

void XpressHuffmanDecode(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
	size_t pos = 0;
	size_t produced = 0;
	// A 16-bit word at `at`, zero past the end (the bit window reads up to two words ahead of what
	// it consumes, so the stream may end inside the window).
	auto word = [&](size_t at) -> uint32_t {
		if (at + 2 <= input_size) {
			return static_cast<uint32_t>(input[at] | (input[at + 1] << 8));
		}
		if (at < input_size) {
			return input[at];
		}
		return 0;
	};
	auto byte = [&](size_t at) -> uint32_t {
		if (at >= input_size) {
			Fail("a match length runs past the end of the input", at, produced);
		}
		return input[at];
	};
	std::vector<uint16_t> table;
	int extra = 16;
	// Where the last word the window loaded ends, and where the last long-length byte read ends:
	// the window's unconsumed bits are the tail of its loaded words, which a long-length byte read
	// later may follow.
	size_t window_end = 0;
	size_t bytes_end = 0;
	while (produced < output_size) {
		if (pos > input_size || input_size - pos < TABLE_BYTES) {
			Fail("the input ends before a Huffman table", pos, produced);
		}
		BuildTable(input + pos, table, pos, produced);
		pos += TABLE_BYTES;
		uint32_t bits = (word(pos) << 16) | word(pos + 2);
		pos += 4;
		window_end = pos;
		extra = 16;
		const size_t chunk_end = produced + CHUNK < output_size ? produced + CHUNK : output_size;
		while (produced < chunk_end) {
			// The window may only run a few words past the input's end (its zero padding).
			if (pos > input_size + 4) {
				Fail("the input ends before the output is complete", pos, produced);
			}
			const uint16_t entry = table[(bits >> 17) & 0x7FFF];
			if (entry == 0) {
				Fail("a bit pattern no Huffman code has", pos, produced);
			}
			const unsigned len = entry & 0x0F;
			unsigned sym = entry >> 4;
			bits <<= len;
			extra -= static_cast<int>(len);
			if (extra < 0) {
				bits |= word(pos) << (-extra);
				pos += 2;
				window_end = pos;
				extra += 16;
			}
			if (sym < 256) {
				output[produced++] = static_cast<uint8_t>(sym);
				continue;
			}
			sym -= 256;
			size_t length = sym & 0x0F;
			const unsigned offset_bits = sym >> 4;
			if (length == 15) {
				length = byte(pos);
				pos += 1;
				if (length == 255) {
					length = byte(pos) | (byte(pos + 1) << 8);
					pos += 2;
					if (length == 0) {
						length = byte(pos) | (byte(pos + 1) << 8) | (byte(pos + 2) << 16) |
								 (static_cast<size_t>(byte(pos + 3)) << 24);
						pos += 4;
					}
					if (length < 15) {
						Fail("a long match length below 15", pos, produced);
					}
					length -= 15;
				}
				length += 15;
				bytes_end = pos;
			}
			length += 3;
			size_t distance = 1;
			if (offset_bits) {
				distance = static_cast<size_t>(bits >> (32 - offset_bits)) + (static_cast<size_t>(1) << offset_bits);
				bits <<= offset_bits;
				extra -= static_cast<int>(offset_bits);
				if (extra < 0) {
					bits |= word(pos) << (-extra);
					pos += 2;
					window_end = pos;
					extra += 16;
				}
			}
			if (distance > produced) {
				// D10: every block starts with an empty history.
				Fail("a match " + std::to_string(distance) + " bytes back, before the block's start", pos, produced);
			}
			// Refused rather than cut at the declared size, as the Python prototype's out[:size] would:
			// a declared size is a whole number of 512-byte units, and none of the sample's 181 blocks
			// has a match past it.
			if (length > output_size - produced) {
				Fail("a match of " + std::to_string(length) + " bytes past the block's declared size", pos, produced);
			}
			const uint8_t *from = output + produced - distance;
			uint8_t *to = output + produced;
			if (distance >= length) {
				std::memcpy(to, from, length);
			} else {
				for (size_t i = 0; i < length; i++) {
					to[i] = from[i];  // overlapping: byte by byte, the run repeats
				}
			}
			produced += length;
		}
	}
	// The window holds 16 + extra bits it has not consumed, the tail of its loaded words; every bit
	// it did consume, and every long-length byte, must be in the input. A stream cut short would
	// otherwise decode the zeros the window was filled with past the end: refused here.
	const size_t unconsumed_bytes = static_cast<size_t>(16 + extra) / 8;
	const size_t consumed_end = window_end - unconsumed_bytes > bytes_end ? window_end - unconsumed_bytes : bytes_end;
	if (consumed_end > input_size) {
		Fail("the input ends before the bits the output needs", pos, produced);
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

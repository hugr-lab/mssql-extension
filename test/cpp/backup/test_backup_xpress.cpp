// Spec 085 task 5: XpressHuffmanDecode against an encoder written here.
//
// The encoder emits tokens (a symbol's code, offset bits, the long-length bytes) and lays them out
// by simulating the decoder's window -- two words preloaded, one more word whenever the window
// runs short, long-length bytes read at the current position -- so each byte lands where the
// decoder reads it. The code is canonical over mixed lengths (8, 9 and 10 bits). Checked:
// literals; runs (distance 1); matches with 1..15 offset bits, reaching into the previous 64 KB
// chunk and crossing a chunk's end; the three long-length forms; several chunks, each with its
// table; a last chunk with no padding (the window reads zeros past the end). Refused: a match
// before the block's start (D10), an over-subscribed table, a long length below 15, a match past
// the declared size, and a stream cut short (its zeros decoded as bits). The real blocks are
// checked on the samples: the MS_XPRESS probe decodes byte-for-byte as the Python prototype does.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_xpress.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

template <class F>
std::string ErrorOf(F f) {
	try {
		f();
	} catch (FormatError &e) {
		return e.what();
	}
	return "";
}

using Bytes = std::vector<uint8_t>;

//! A canonical code over `lengths` (as the decoder builds it: by length, then by symbol).
struct Code {
	uint8_t length[512] = {};
	uint32_t code[512] = {};
	Bytes table;  // the 256-byte length table
};

Code MakeCode(const uint8_t (&lengths)[512]) {
	Code c;
	for (int i = 0; i < 512; i++) {
		c.length[i] = lengths[i];
	}
	uint32_t code = 0;
	for (unsigned len = 1; len <= 15; len++) {
		for (unsigned sym = 0; sym < 512; sym++) {
			if (c.length[sym] == len) {
				c.code[sym] = code++;
			}
		}
		code <<= 1;
	}
	c.table.resize(256);
	for (int i = 0; i < 256; i++) {
		c.table[i] = static_cast<uint8_t>(c.length[2 * i] | (c.length[2 * i + 1] << 4));
	}
	return c;
}

struct Token {
	bool is_byte;
	uint32_t value;
	int bits;  // for a bits token
};

//! One chunk's bytes: the table, then the tokens laid out as the decoder reads them. With `last`,
//! cut to the last byte holding a consumed bit (the decoder fills the rest of its window with 0).
Bytes LayOut(const Code &code, const std::vector<Token> &tokens, bool last) {
	std::vector<bool> stream;
	for (const auto &t : tokens) {
		if (!t.is_byte) {
			for (int i = t.bits - 1; i >= 0; i--) {
				stream.push_back(((t.value >> i) & 1) != 0);
			}
		}
	}
	auto word = [&](size_t k) -> uint16_t {
		uint16_t w = 0;
		for (size_t i = 0; i < 16; i++) {
			size_t at = 16 * k + i;
			w = static_cast<uint16_t>((w << 1) | (at < stream.size() && stream[at] ? 1 : 0));
		}
		return w;
	};
	Bytes body;
	auto put_word = [&](size_t at, uint16_t w) {
		if (body.size() < at + 2) {
			body.resize(at + 2, 0);
		}
		body[at] = w & 0xFF;
		body[at + 1] = w >> 8;
	};
	size_t pos = 0;
	size_t next_word = 0;
	put_word(pos, word(next_word++));
	put_word(pos + 2, word(next_word++));
	pos += 4;
	size_t window_end = pos;
	size_t bytes_end = 0;
	int extra = 16;
	for (const auto &t : tokens) {
		if (t.is_byte) {
			if (body.size() < pos + 1) {
				body.resize(pos + 1, 0);
			}
			body[pos++] = static_cast<uint8_t>(t.value);
			bytes_end = pos;
			continue;
		}
		extra -= t.bits;
		if (extra < 0) {
			put_word(pos, word(next_word++));
			pos += 2;
			window_end = pos;
			extra += 16;
		}
	}
	body.resize(pos, 0);
	if (last) {
		// As the decoder counts it: the window's consumed bytes, or the last long-length byte.
		const size_t window_used = window_end - static_cast<size_t>(16 + extra) / 8;
		body.resize(window_used > bytes_end ? window_used : bytes_end);
	}
	Bytes out = code.table;
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

//! A literal (length 0) or a match (length, distance).
struct Item {
	uint8_t literal;
	size_t length;
	size_t distance;
};

struct Block {
	Bytes stream;
	Bytes expected;
};

//! The stream for `items` and the output it decodes to. A chunk closes at a symbol boundary once
//! 64 KB have been written since it opened (so a match may cross its end), as the decoder counts.
Block Build(const Code &code, const std::vector<Item> &items) {
	Block b;
	std::vector<Token> tokens;
	size_t chunk_start = 0;
	auto symbol = [&](unsigned sym) { tokens.push_back(Token{false, code.code[sym], code.length[sym]}); };
	for (size_t n = 0; n < items.size(); n++) {
		const auto &it = items[n];
		if (it.length == 0) {
			symbol(it.literal);
			b.expected.push_back(it.literal);
		} else {
			unsigned offset_bits = 0;
			while ((static_cast<size_t>(2) << offset_bits) <= it.distance) {
				offset_bits++;
			}
			const size_t l3 = it.length - 3;
			symbol(256 + ((offset_bits << 4) | static_cast<unsigned>(l3 < 15 ? l3 : 15)));
			if (l3 >= 15) {
				if (l3 - 15 < 255) {
					tokens.push_back(Token{true, static_cast<uint32_t>(l3 - 15), 0});
				} else if (l3 <= 0xFFFF) {
					tokens.push_back(Token{true, 255, 0});
					tokens.push_back(Token{true, static_cast<uint32_t>(l3 & 0xFF), 0});
					tokens.push_back(Token{true, static_cast<uint32_t>(l3 >> 8), 0});
				} else {
					tokens.push_back(Token{true, 255, 0});
					tokens.push_back(Token{true, 0, 0});
					tokens.push_back(Token{true, 0, 0});
					for (int k = 0; k < 4; k++) {
						tokens.push_back(Token{true, static_cast<uint32_t>((l3 >> (8 * k)) & 0xFF), 0});
					}
				}
			}
			if (offset_bits) {
				tokens.push_back(Token{false,
									   static_cast<uint32_t>(it.distance - (static_cast<size_t>(1) << offset_bits)),
									   static_cast<int>(offset_bits)});
			}
			for (size_t i = 0; i < it.length; i++) {
				b.expected.push_back(b.expected[b.expected.size() - it.distance]);
			}
		}
		const bool more = n + 1 < items.size();
		if (more && b.expected.size() >= chunk_start + 65536) {
			auto chunk = LayOut(code, tokens, false);
			b.stream.insert(b.stream.end(), chunk.begin(), chunk.end());
			tokens.clear();
			chunk_start = b.expected.size();
		}
	}
	auto chunk = LayOut(code, tokens, true);
	b.stream.insert(b.stream.end(), chunk.begin(), chunk.end());
	return b;
}

Bytes Decode(const Bytes &stream, size_t size) {
	Bytes out(size);
	XpressHuffmanDecode(stream.data(), stream.size(), out.data(), out.size());
	return out;
}

Code MixedCode() {
	uint8_t lengths[512];
	for (int s = 0; s < 512; s++) {
		lengths[s] = s < 64 ? 8 : (s < 256 ? 9 : 10);  // Kraft: 0.25 + 0.375 + 0.25
	}
	return MakeCode(lengths);
}

uint32_t rng_state = 12345;
uint32_t Next() {
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

}  // namespace

int main() {
	std::cout << "XpressHuffmanDecode (spec 085 task 5)" << std::endl;
	const Code code = MixedCode();

	{
		std::vector<Item> items;
		for (int i = 0; i < 300; i++) {
			items.push_back(Item{static_cast<uint8_t>(Next()), 0, 0});
		}
		items.push_back(Item{0, 3, 1});		// a run
		items.push_back(Item{0, 17, 2});	// the longest short form
		items.push_back(Item{0, 18, 200});	// long form: one byte
		items.push_back(Item{0, 300, 7});	// long form: the 16-bit length
		items.push_back(Item{0, 20, 255});
		auto b = Build(code, items);
		Check(Decode(b.stream, b.expected.size()) == b.expected,
			  "literals, a run, short matches and the byte / 16-bit long forms, over a mixed-length code");
	}
	{
		// Several chunks; matches with 1..15 offset bits, reaching into the previous chunk and
		// crossing a chunk's end; a 70000-byte match in the 32-bit long form.
		std::vector<Item> items;
		size_t produced = 0;
		for (int i = 0; i < 1000; i++) {
			items.push_back(Item{static_cast<uint8_t>(Next()), 0, 0});
			produced++;
		}
		for (int round = 0; produced < 260000; round++) {
			for (int k = 0; k < 50; k++) {
				items.push_back(Item{static_cast<uint8_t>(Next()), 0, 0});
				produced++;
			}
			const unsigned bits = 1 + round % 15;
			size_t distance = (static_cast<size_t>(1) << bits) + Next() % (static_cast<size_t>(1) << bits);
			if (distance > produced) {
				distance = produced;
			}
			const size_t length = 3 + Next() % 400;
			items.push_back(Item{0, length, distance});
			produced += length;
		}
		items.push_back(Item{0, 70000, 1});
		auto b = Build(code, items);
		Check(b.expected.size() > 3 * 65536 && Decode(b.stream, b.expected.size()) == b.expected,
			  "five chunks, offsets of 1..15 bits across chunks, a 32-bit long length, no padding at the end");
	}
	{
		std::vector<Item> items = {{'x', 0, 0}, {0, 5, 4}};
		auto b = Build(code, items);
		Check(ErrorOf([&]() { Decode(b.stream, 6); }).find("before the block's start") != std::string::npos,
			  "a match before the block's start is refused (D10)");
	}
	{
		Bytes s(256, 0x11);	 // every symbol 1 bit: 512 codes in a space of 2
		s.resize(300, 0);
		Check(ErrorOf([&]() { Decode(s, 10); }).find("over-subscribe") != std::string::npos,
			  "an over-subscribed Huffman table is refused");
	}
	{
		// A 16-bit long length of 10: below 15.
		std::vector<Token> tokens = {{false, code.code['a'], code.length['a']},
									 {false, code.code[256 + 15], code.length[256 + 15]},
									 {true, 255, 0},
									 {true, 10, 0},
									 {true, 0, 0}};
		auto s = LayOut(code, tokens, true);
		Check(ErrorOf([&]() { Decode(s, 100); }).find("below 15") != std::string::npos,
			  "a long match length below 15 is refused");
	}
	{
		std::vector<Item> items = {{'a', 0, 0}, {0, 17, 1}};
		auto b = Build(code, items);
		Check(ErrorOf([&]() { Decode(b.stream, 6); }).find("past the block's declared size") != std::string::npos,
			  "a match past the declared output size is refused");
	}
	{
		std::vector<Item> items;
		for (int i = 0; i < 400; i++) {
			items.push_back(Item{static_cast<uint8_t>(Next()), 0, 0});
		}
		auto b = Build(code, items);
		auto cut = b.stream;
		cut.resize(cut.size() - 6);
		auto e = ErrorOf([&]() { Decode(cut, b.expected.size()); });
		Check(e.find("ends before the bits") != std::string::npos ||
				  e.find("ends before the output") != std::string::npos,
			  "a stream cut short is refused, not decoded from the window's zeros: " + e);
		// Cut by one byte only: decoding can finish on the window's zeros, and the end check refuses.
		auto one = b.stream;
		one.resize(one.size() - 1);
		auto e1 = ErrorOf([&]() { Decode(one, b.expected.size()); });
		Check(e1.find("ends before the bits") != std::string::npos,
			  "a stream one byte short is refused by the end check");
		Check(ErrorOf([&]() { Decode(Bytes(200, 0), 10); }).find("ends before a Huffman table") != std::string::npos,
			  "an input shorter than its table is refused");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}

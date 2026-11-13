//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// test/cpp/backup/test_backup_xpress_diff.cpp
//
// Spec 085 PR 2e: the MS_XPRESS decoder against PR 1's (xpress_reference.hpp), the one a faster
// decoder must agree with. Every compressed block of the committed MS_XPRESS sample, and of any
// container XPRESS_DIFF_FILES names (space-separated; the local bench's backups), decodes to the
// reference's bytes; and thousands of blocks made wrong from them -- bytes flipped, cut short (at
// random, and by each of the last 16 bytes, where the bit window ends), bytes appended, a declared
// size off by some -- are decoded to the same bytes by both or refused by both, for the same reason
// (the message up to its offsets, which a decoder that reads ahead may name otherwise). Neither may
// write past the output it was given: 64 guard bytes after it are checked on every path.
//===----------------------------------------------------------------------===//

#include "backup/backup_bytes.hpp"
#include "backup/backup_xpress.hpp"
#include "xpress_reference.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
	if (!ok) {
		failures++;
	}
}

using Bytes = std::vector<uint8_t>;

Bytes ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Block {
	Bytes payload;
	size_t output = 0;
};

// The chain's compressed blocks, from 8 KB (kind 0x21 compressed / 0x23 stored, 512-byte units of
// output, payload length; 0 means 64 KB), to the first header that is none.
std::vector<Block> CompressedBlocks(const Bytes &file) {
	std::vector<Block> out;
	uint64_t at = 8192;
	while (at + 28 <= file.size() && (file[at] == 0x21 || file[at] == 0x23)) {
		const size_t output = static_cast<size_t>(file[at + 1]) * 512;
		const uint16_t length = static_cast<uint16_t>(file[at + 2] | (file[at + 3] << 8));
		const size_t payload = length == 0 ? 65536 : length;
		if (output == 0 || output > 65536 || at + 28 + payload > file.size()) {
			break;
		}
		if (file[at] == 0x21) {
			Block b;
			b.payload.assign(file.begin() + static_cast<std::ptrdiff_t>(at + 28),
							 file.begin() + static_cast<std::ptrdiff_t>(at + 28 + payload));
			b.output = output;
			out.push_back(std::move(b));
		}
		at += 28 + payload;
	}
	return out;
}

constexpr size_t GUARD = 64;

// The guard bytes after an output: a pattern a decoder is unlikely to write by chance.
uint8_t GuardByte(size_t i) {
	return static_cast<uint8_t>(0xA5 ^ (i * 37));
}

// A refusal's reason: its message up to the offsets it names.
std::string Reason(const std::string &message) {
	const auto at = message.find(" (input byte");
	return at == std::string::npos ? message : message.substr(0, at);
}

// One decode: its output (and guard), or its refusal's reason.
struct Outcome {
	Bytes bytes;
	bool refused = false;
	std::string reason;
	bool guard_intact = true;
};

template <class Decode>
Outcome Run(Decode decode, const uint8_t *input, size_t input_size, size_t output_size) {
	Outcome o;
	o.bytes.resize(output_size + GUARD);
	for (size_t i = 0; i < GUARD; i++) {
		o.bytes[output_size + i] = GuardByte(i);
	}
	try {
		decode(input, input_size, o.bytes.data(), output_size);
	} catch (FormatError &e) {
		o.refused = true;
		o.reason = Reason(e.what());
	}
	for (size_t i = 0; i < GUARD; i++) {
		o.guard_intact = o.guard_intact && o.bytes[output_size + i] == GuardByte(i);
	}
	return o;
}

// One decode by each: true when both give the same bytes, or both refuse for the same reason, and
// neither wrote past the output.
bool Agree(const uint8_t *input, size_t input_size, size_t output_size, bool &refused) {
	const Outcome ours = Run(XpressHuffmanDecode, input, input_size, output_size);
	const Outcome theirs = Run(xpress_reference::Decode, input, input_size, output_size);
	refused = ours.refused;
	if (!ours.guard_intact || !theirs.guard_intact || ours.refused != theirs.refused) {
		return false;
	}
	if (ours.refused) {
		return ours.reason == theirs.reason;
	}
	return std::equal(ours.bytes.begin(), ours.bytes.begin() + static_cast<std::ptrdiff_t>(output_size),
					  theirs.bytes.begin());
}

uint64_t state = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

}  // namespace

int main() {
	std::cout << "MS_XPRESS decoder against PR 1's (spec 085 PR 2e)\n";
	std::vector<std::string> files = {"test/data/backup/full_xpress.bak"};
	if (const char *extra = std::getenv("XPRESS_DIFF_FILES")) {
		std::istringstream in(extra);
		std::string f;
		while (in >> f) {
			files.push_back(f);
		}
	}
	std::vector<Block> sample;
	for (const auto &f : files) {
		const auto blocks = CompressedBlocks(ReadFile(f));
		size_t agree = 0;
		bool refused = false, any_refused = false;
		for (const auto &b : blocks) {
			agree += Agree(b.payload.data(), b.payload.size(), b.output, refused);
			any_refused = any_refused || refused;
		}
		Check(!blocks.empty() && agree == blocks.size() && !any_refused,
			  f + ": " + std::to_string(blocks.size()) + " blocks decode to the reference's bytes (" +
				  std::to_string(agree) + " agree)");
		if (sample.empty()) {
			sample = blocks;
		}
	}

	// Blocks made wrong from the sample's: both decode them alike, or both refuse.
	size_t cases = 0, agree = 0, refusals = 0;
	auto check = [&](const Bytes &input, size_t output) {
		bool refused = false;
		cases++;
		agree += Agree(input.data(), input.size(), output, refused);
		refusals += refused;
	};
	for (size_t i = 0; i < sample.size(); i++) {
		const Block &b = sample[i];
		for (int k = 0; k < 120; k++) {
			Bytes input = b.payload;
			size_t output = b.output;
			switch (k % 5) {
			case 0: {  // a few bytes flipped, anywhere (the table included)
				const int n = 1 + static_cast<int>(Next() % 3);
				for (int j = 0; j < n; j++) {
					input[Next() % input.size()] ^= static_cast<uint8_t>(1 + Next() % 255);
				}
				break;
			}
			case 1:	 // cut short anywhere, the table included
				input.resize(Next() % input.size());
				break;
			case 2:	 // a declared size off by some, either way, within 1..65536
				output = 1 + (output - 1 + 65536 + (Next() % 1025) - 512) % 65536;
				break;
			case 3:	 // a byte flipped in the stream's last 64 bytes (where the bit window ends)
				input[input.size() - 1 - Next() % std::min<size_t>(64, input.size())] ^=
					static_cast<uint8_t>(1 + Next() % 255);
				break;
			default:  // a cut below the 256-byte table
				input.resize(Next() % std::min<size_t>(256, input.size()));
				break;
			}
			check(input, output);
		}
		// Every short cut and every small change of size, and bytes appended (which both ignore).
		for (size_t cut = 1; cut <= 16 && cut < b.payload.size(); cut++) {
			check(Bytes(b.payload.begin(), b.payload.end() - static_cast<std::ptrdiff_t>(cut)), b.output);
		}
		for (int delta = -8; delta <= 8; delta++) {
			const long size = static_cast<long>(b.output) + delta;
			if (delta != 0 && size >= 1 && size <= 65536) {
				check(b.payload, static_cast<size_t>(size));
			}
		}
		for (size_t extra = 1; extra <= 8; extra++) {
			Bytes input = b.payload;
			for (size_t j = 0; j < extra; j++) {
				input.push_back(static_cast<uint8_t>(Next()));
			}
			check(input, b.output);
		}
	}
	Check(cases > 1000 && agree == cases && refusals > 0,
		  std::to_string(cases) +
			  " blocks made wrong decode alike or are refused by both for one reason, neither "
			  "writing past its output (" +
			  std::to_string(agree) + " agree, " + std::to_string(refusals) + " refused)");

	if (failures) {
		std::cout << "FAILED: " << failures << " check(s)\n";
		return 1;
	}
	return 0;
}

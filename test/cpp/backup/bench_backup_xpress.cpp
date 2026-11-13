//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// test/cpp/backup/bench_backup_xpress.cpp
//
// Spec 085 PR 2e: the MS_XPRESS decoder's throughput over every compressed block of real
// containers (an MS_XPRESS backup's chain, read here without the container code: kind, 512-byte
// units of output, payload length). Each block is decoded by the extension's decoder and by the
// reference (xpress_reference.hpp, PR 1's decoder) and the outputs compared; then the extension's
// decoder is timed, best of several passes, as MB/s of output. DuckDB-free. Manual target
// (make bench-backup-xpress FILES="a.bak b.bak"), not in CI.
//===----------------------------------------------------------------------===//

#include "backup/backup_xpress.hpp"
#include "xpress_reference.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace duckdb::mssql::backup;

namespace {

constexpr uint64_t CHAIN_START = 8192;
constexpr size_t BLOCK_HEADER = 28;
constexpr uint8_t KIND_COMPRESSED = 0x21;
constexpr uint8_t KIND_STORED = 0x23;

struct Block {
	const uint8_t *payload;
	size_t payload_size;
	size_t output_size;
};

std::vector<uint8_t> ReadFile(const char *path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::fprintf(stderr, "bench_backup_xpress: cannot open %s\n", path);
		std::exit(2);
	}
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The chain's compressed blocks, header by header from 8 KB to the first header that is none.
void CompressedBlocks(const std::vector<uint8_t> &file, std::vector<Block> &out) {
	uint64_t at = CHAIN_START;
	while (at + BLOCK_HEADER <= file.size()) {
		const uint8_t kind = file[at];
		if (kind != KIND_COMPRESSED && kind != KIND_STORED) {
			break;
		}
		const size_t output = static_cast<size_t>(file[at + 1]) * 512;
		const uint16_t length = static_cast<uint16_t>(file[at + 2] | (file[at + 3] << 8));
		const size_t payload = length == 0 ? 65536 : length;
		if (output == 0 || output > 65536 || at + BLOCK_HEADER + payload > file.size()) {
			break;
		}
		if (kind == KIND_COMPRESSED) {
			out.push_back(Block{file.data() + at + BLOCK_HEADER, payload, output});
		}
		at += BLOCK_HEADER + payload;
	}
}

uint64_t Fnv(const uint8_t *p, size_t n, uint64_t h) {
	for (size_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 1099511628211ull;
	}
	return h;
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: bench_backup_xpress <container.bak>...\n");
		return 2;
	}
	const int passes = std::max(1, std::getenv("PASSES") ? std::atoi(std::getenv("PASSES")) : 5);
	int failed = 0;
	for (int f = 1; f < argc; f++) {
		const auto file = ReadFile(argv[f]);
		std::vector<Block> blocks;
		CompressedBlocks(file, blocks);
		// Every block through both decoders: the same bytes, or a refusal for the same reason (the
		// message up to its offsets). Only the blocks that decode are timed.
		auto reason = [](const std::string &m) {
			const auto at = m.find(" (input byte");
			return at == std::string::npos ? m : m.substr(0, at);
		};
		std::vector<uint8_t> ours(65536), theirs(65536);
		std::vector<Block> decodable;
		size_t differ = 0, refused = 0, in_bytes = 0, out_bytes = 0;
		uint64_t sum = 1469598103934665603ull;
		for (const auto &b : blocks) {
			std::string e1, e2;
			try {
				XpressHuffmanDecode(b.payload, b.payload_size, ours.data(), b.output_size);
			} catch (FormatError &e) {
				e1 = reason(e.what());
			}
			try {
				xpress_reference::Decode(b.payload, b.payload_size, theirs.data(), b.output_size);
			} catch (FormatError &e) {
				e2 = reason(e.what());
			}
			if (e1 != e2 || (e1.empty() && std::memcmp(ours.data(), theirs.data(), b.output_size) != 0)) {
				differ++;
			}
			if (!e1.empty()) {
				refused++;
				continue;
			}
			decodable.push_back(b);
			in_bytes += b.payload_size;
			out_bytes += b.output_size;
			sum = Fnv(ours.data(), b.output_size, sum);
		}
		if (decodable.empty()) {
			std::printf("%s: no compressed block decodes (%zu refused)\n", argv[f], refused);
			failed++;
			continue;
		}
		double best = 1e30;
		for (int p = 0; p < passes; p++) {
			const auto start = std::chrono::steady_clock::now();
			for (const auto &b : decodable) {
				XpressHuffmanDecode(b.payload, b.payload_size, ours.data(), b.output_size);
			}
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			best = s < best ? s : best;
		}
		std::printf(
			"%s: %zu blocks (%zu refused), %.1f MB in, %.1f MB out, best %.3f s = %.0f MB/s out "
			"(%.2f ns/byte); output FNV %016llx; %zu block(s) differ from the reference\n",
			argv[f], decodable.size(), refused, in_bytes / 1e6, out_bytes / 1e6, best, out_bytes / 1e6 / best,
			best * 1e9 / out_bytes, static_cast<unsigned long long>(sum), differ);
		failed += differ != 0;
	}
	return failed ? 1 : 0;
}

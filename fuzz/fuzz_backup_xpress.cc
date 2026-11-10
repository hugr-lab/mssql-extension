// Fuzz harness: the MS_XPRESS (LZ77 + Huffman, [MS-XCA] 2.2) block decoder (spec 085 task 5).
//
// The first two bytes choose the declared output size (1 .. 65536, as a container block's `unit ×
// 512` bounds it); the rest is the compressed payload. The decoder must write exactly that many
// bytes or throw a FormatError: a sanitizer report (a write past the output, a read past the input)
// or any other exception is a real bug.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_xpress.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2) {
		return 0;
	}
	const size_t output = (static_cast<size_t>(data[0]) | (static_cast<size_t>(data[1]) << 8)) + 1;
	std::vector<uint8_t> out(output);
	try {
		XpressHuffmanDecode(data + 2, size - 2, out.data(), out.size());
		g_sink += out[output - 1];
	} catch (FormatError &) {
	}
	return 0;
}

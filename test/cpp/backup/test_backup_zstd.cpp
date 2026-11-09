// Spec 085 task 6: ZstdDecode over frames made by zstd itself.
//
// A block's payload is one frame of exactly the block's output size. Checked: a frame round-trips
// at several sizes and levels; refused: a frame that declares
// another size, bytes after the frame, two frames, a truncated frame, a damaged frame header, a
// buffer smaller than the frame's content, a frame with no declared size, a skippable frame, one
// that needs a dictionary; a checksummed frame decodes and its damaged checksum is refused. SQL
// Server's frames carry no checksum: damage inside a frame is the page layer's to catch. No
// committed test decodes a SQL Server frame yet (no committed sample: task 10); locally the ZSTD
// probe's and E3's data sets decode and parse as HEADERONLY says.

#include <zstd.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_zstd.hpp"

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

Bytes Compress(const Bytes &data, int level) {
	Bytes out(ZSTD_compressBound(data.size()));
	size_t n = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
	out.resize(ZSTD_isError(n) ? 0 : n);
	return out;
}

//! A frame without the content size in its header (as the streaming API writes it).
Bytes CompressStreaming(const Bytes &data) {
	ZSTD_CCtx *c = ZSTD_createCCtx();
	ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0);
	Bytes out(ZSTD_compressBound(data.size()));
	size_t n = ZSTD_compress2(c, out.data(), out.size(), data.data(), data.size());
	ZSTD_freeCCtx(c);
	out.resize(ZSTD_isError(n) ? 0 : n);
	return out;
}

Bytes Data(size_t n) {
	Bytes d(n);
	uint32_t s = 7;
	for (size_t i = 0; i < n; i++) {
		s = s * 1103515245u + 12345u;
		d[i] = (i % 64 < 40) ? static_cast<uint8_t>(i % 13) : static_cast<uint8_t>(s >> 24);
	}
	return d;
}

}  // namespace

int main() {
	std::cout << "ZstdDecode (spec 085 task 6)" << std::endl;
	bool all = true;
	for (size_t size : {512u, 4096u, 65536u}) {
		for (int level : {1, 3, 19}) {
			auto data = Data(size);
			auto frame = Compress(data, level);
			Bytes out(size);
			ZstdDecode(frame.data(), frame.size(), out.data(), out.size());
			all = all && out == data;
		}
	}
	Check(all, "frames of 512 / 4096 / 65536 bytes at levels 1, 3, 19 round-trip");
	{
		auto data = Data(8192);
		auto frame = CompressStreaming(data);
		Bytes out(8192);
		Check(ErrorOf([&]() {
				  ZstdDecode(frame.data(), frame.size(), out.data(), out.size());
			  }).find("does not declare its size") != std::string::npos,
			  "a frame with no declared size is refused (SQL Server declares every frame's)");
	}
	auto data = Data(65536);
	auto frame = Compress(data, 3);
	{
		Bytes out(4096);
		Check(ErrorOf([&]() {
				  ZstdDecode(frame.data(), frame.size(), out.data(), out.size());
			  }).find("declares 65536 bytes in a block of 4096") != std::string::npos,
			  "a frame that declares another size is refused by name");
	}
	{
		auto padded = frame;
		padded.push_back(0);
		padded.push_back(0);
		Bytes out(65536);
		Check(ErrorOf([&]() {
				  ZstdDecode(padded.data(), padded.size(), out.data(), out.size());
			  }).find("2 bytes after the block's one frame") != std::string::npos,
			  "bytes after the frame are refused");
		auto two = frame;
		two.insert(two.end(), frame.begin(), frame.end());
		Check(!ErrorOf([&]() { ZstdDecode(two.data(), two.size(), out.data(), out.size()); }).empty(),
			  "two frames in one block are refused");
		auto cut = frame;
		cut.resize(cut.size() / 2);
		Check(ErrorOf([&]() { ZstdDecode(cut.data(), cut.size(), out.data(), out.size()); }).find("no valid frame") !=
				  std::string::npos,
			  "a truncated frame is refused");
		// SQL Server's frames carry no content checksum (15163 of 15163 seen), so damage inside one
		// can decode to wrong bytes; the page layer's checks catch that. A damaged header cannot.
		auto bad = frame;
		bad[0] ^= 0xFF;
		Check(ErrorOf([&]() { ZstdDecode(bad.data(), bad.size(), out.data(), out.size()); }).find("magic number") !=
				  std::string::npos,
			  "a frame whose header is damaged is refused");
		Bytes skippable = {0x50, 0x2A, 0x4D, 0x18, 4, 0, 0, 0, 1, 2, 3, 4};
		Bytes empty(1);
		Check(ErrorOf([&]() {
				  ZstdDecode(skippable.data(), skippable.size(), empty.data(), 0);
			  }).find("magic number") != std::string::npos,
			  "a skippable frame is refused");
		// A frame whose header names a dictionary (Dictionary_ID_flag 1, one id byte after the
		// descriptor of a single-segment frame): refused before decoding.
		auto with_dict = frame;
		Check((with_dict[4] & 0x20) != 0 && (with_dict[4] & 0x03) == 0, "the test frame is single-segment, no dict id");
		with_dict[4] |= 0x01;
		with_dict.insert(with_dict.begin() + 5, 0x2A);
		auto dict_error = ErrorOf([&]() { ZstdDecode(with_dict.data(), with_dict.size(), out.data(), out.size()); });
		Check(dict_error.find("needs a dictionary") != std::string::npos,
			  "a frame that needs a dictionary is refused: " + dict_error);
		// A frame with a content checksum decodes (zstd checks it).
		ZSTD_CCtx *k = ZSTD_createCCtx();
		ZSTD_CCtx_setParameter(k, ZSTD_c_checksumFlag, 1);
		Bytes checked(ZSTD_compressBound(data.size()));
		size_t kn = ZSTD_compress2(k, checked.data(), checked.size(), data.data(), data.size());
		ZSTD_freeCCtx(k);
		checked.resize(kn);
		ZstdDecode(checked.data(), checked.size(), out.data(), out.size());
		Check(out == data, "a frame with a content checksum decodes");
		checked[checked.size() - 1] ^= 0x01;
		Check(!ErrorOf([&]() { ZstdDecode(checked.data(), checked.size(), out.data(), out.size()); }).empty(),
			  "and its damaged checksum is refused");
		Bytes junk(100, 0x41);
		Check(ErrorOf([&]() { ZstdDecode(junk.data(), junk.size(), out.data(), out.size()); }).find("magic number") !=
				  std::string::npos,
			  "bytes that are no zstd frame are refused");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}

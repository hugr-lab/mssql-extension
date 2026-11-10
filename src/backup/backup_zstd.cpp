#include "backup/backup_zstd.hpp"

#include "backup/backup_bytes.hpp"

#include <string>

// The vcpkg zstd header by its absolute path (CMakeLists.txt): DuckDB's bundled zstd.h, in
// namespace duckdb_zstd, comes first on the include path.
#ifdef MSSQL_ZSTD_HEADER
#include MSSQL_ZSTD_HEADER
#else
#include <zstd.h>
#endif

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

//! One decompression context per thread: the open pass and the scan decode blocks on every thread
//! (D10), and a context is not shareable, but reusable.
struct ThreadContext {
	ZSTD_DCtx *context = nullptr;
	ThreadContext() {}
	~ThreadContext() {
		ZSTD_freeDCtx(context);
	}
	ThreadContext(const ThreadContext &) = delete;
	ThreadContext &operator=(const ThreadContext &) = delete;
};

[[noreturn]] void Fail(const std::string &what) {
	throw FormatError("ZSTD block: " + what);
}

}  // namespace

void ZstdDecode(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
	// A standard zstd frame and nothing else: the vcpkg build also decodes the legacy v0.5-v0.7
	// formats and accepts skippable frames, neither of which SQL Server writes.
	if (input_size < 4 ||
		(static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8) |
		 (static_cast<uint32_t>(input[2]) << 16) | (static_cast<uint32_t>(input[3]) << 24)) != ZSTD_MAGICNUMBER) {
		Fail("not a zstd frame (its magic number is missing)");
	}
	const size_t frame = ZSTD_findFrameCompressedSize(input, input_size);
	if (ZSTD_isError(frame)) {
		Fail(std::string("no valid frame (") + ZSTD_getErrorName(frame) + ")");
	}
	if (frame != input_size) {
		Fail(std::to_string(input_size - frame) + " bytes after the block's one frame");
	}
	const unsigned long long declared = ZSTD_getFrameContentSize(input, input_size);
	if (declared == ZSTD_CONTENTSIZE_ERROR) {
		Fail("a frame header that does not parse");
	}
	// SQL Server declares every frame's size (15163 of 15163); one that does not is not its.
	if (declared == ZSTD_CONTENTSIZE_UNKNOWN) {
		Fail("a frame that does not declare its size");
	}
	if (ZSTD_getDictID_fromFrame(input, input_size) != 0) {
		Fail("a frame that needs a dictionary");
	}
	if (declared != output_size) {
		Fail("a frame that declares " + std::to_string(declared) + " bytes in a block of " +
			 std::to_string(output_size));
	}
	static thread_local ThreadContext thread;
	if (!thread.context) {
		// Created on first use, and again after a failed creation.
		thread.context = ZSTD_createDCtx();
		if (!thread.context) {
			Fail("no decompression context (out of memory)");
		}
	}
	// The output buffer is exactly the block's size: zstd refuses to write past it.
	const size_t written = ZSTD_decompressDCtx(thread.context, output, output_size, input, input_size);
	if (ZSTD_isError(written)) {
		Fail(std::string("the frame does not decode (") + ZSTD_getErrorName(written) + ")");
	}
	if (written != output_size) {
		Fail("the frame decodes to " + std::to_string(written) + " bytes, not the block's " +
			 std::to_string(output_size));
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

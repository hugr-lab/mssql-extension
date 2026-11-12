//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_open_pass.hpp
//
// Spec 085 task 7a, D3a: the open pass's header scans over parallel ranges. A pass that is not
// ranked from its allocation pages (pass 2, a differential's pass, a striped set's) is mapped from
// every group's page headers, so its whole data stream is read: here as waves of large ranges
// (8 MiB by default), each wave fetched side by side on the ASYNC I/O pool (ByteSource::FetchNow,
// through CachingFileSystem: buffer-managed, D17) and then fed in order to the DuckDB-free
// PassHeaderScan. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_source.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! One family file of a ByteSource as a RandomAccess, for the DuckDB-free parsers (MTF walk,
//! header fields, the rank path, the scan's pages). A ReadAt is one fetch through CachingFileSystem,
//! but a short read that starts where the thread's last read of this file ended, or a little past
//! it (a scan's pages, one after another in the file; a chain walk's headers), is served from
//! READ_AHEAD bytes the thread read at once and keeps (PR 2e step 7: one fetch a page cost 12 us of
//! the kernel's time at 16 threads); any other read is read as asked. Reads from several threads are safe (positioned
//! reads on a shared handle; the read-ahead is the thread's own).
class SourceAccess final : public RandomAccess {
public:
	SourceAccess(ByteSource &source, size_t file);
	uint64_t Size() const override;
	void ReadAt(uint64_t offset, uint8_t *dest, size_t count) const override;
	const std::string &Name() const override;
	ByteSource &Source() const {
		return source_;
	}
	size_t File() const {
		return file_;
	}

	//! The fetches made through the source (for the tests: what the read-ahead saved).
	uint64_t Fetches() const {
		return fetches_.load(std::memory_order_relaxed);
	}
	//! A thread's read-ahead (256 KiB) and the gap past its last read that still counts as reading on.
	static constexpr size_t READ_AHEAD = 256 << 10;
	static constexpr uint64_t SEQUENTIAL_GAP = 64 << 10;

private:
	//! `count` bytes at `offset`, fetched as asked.
	void ReadDirect(uint64_t offset, uint8_t *dest, size_t count) const;

	ByteSource &source_;
	size_t file_;
	//! This access among every one made, so a thread's read-ahead is never another file's, even one at
	//! a reused address.
	const uint64_t id_;
	mutable std::atomic<uint64_t> fetches_{0};
};

struct PassScanOptions {
	//! Bytes per range, rounded down to whole 64 KB groups (at least one).
	uint64_t range_bytes = 8 * 1024 * 1024;
	//! Ranges fetched together in one wave; 0 = twice DuckDB's thread count, between 2 and 16, and
	//! no more than an eighth of `memory_limit` at twice each range's bytes (a range that alone
	//! exceeds it is shrunk). A wave waits for its slowest range; a sliding window is a later step.
	size_t ranges_in_flight = 0;
};

//! BuildPassMap's map of the pass whose MQDA data is `length` bytes at `offset` of family file
//! `file`, its groups read as waves of parallel ranges. `ranges_read`, when given, counts them.
//! Checks for an interrupt before each wave.
PassMap BuildPassMapInRanges(ClientContext &context, ByteSource &source, size_t file, uint64_t offset, uint64_t length,
							 uint32_t number, const PassScanOptions &options = PassScanOptions(),
							 uint64_t *ranges_read = nullptr);

//! A PassMapper for BuildPageIndex: a family whose stream is a SourceAccess is scanned in ranges,
//! any other (a container's decoded data set) through BuildPassMap. It keeps `context` by pointer:
//! use it within the call that made it (one BuildPageIndex), never stored with the opened backup.
PassMapper RangedPassMapper(ClientContext &context, const PassScanOptions &options = PassScanOptions());

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

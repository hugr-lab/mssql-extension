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

#include <cstdint>
#include <string>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! One family file of a ByteSource as a RandomAccess, for the DuckDB-free parsers (MTF walk,
//! header fields, the rank path). Each ReadAt is one fetch through CachingFileSystem; reads from
//! several threads are safe (positioned reads on a shared handle).
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

private:
	ByteSource &source_;
	size_t file_;
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

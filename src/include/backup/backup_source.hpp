//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_source.hpp
//
// Spec 085 task 2, D3 / D3a: the backup's files -- every family (stripe) of one media set --
// read through DuckDB's file system only, so s3://, https://, az:// and the rest work with the
// user's DuckDB secrets. Reads are byte ranges through CachingFileSystem (a remote file's ranges
// are cached in buffer-managed blocks, D17), coalesced (backup_ranges) and fetched either
// synchronously or as AsyncTasks for DuckDB's ASYNC I/O pool. The parsers above take the
// fetched bytes as ByteReader spans and never see DuckDB (D15). [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_bytes.hpp"
#include "backup/backup_ranges.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"

#include <string>

namespace duckdb {
class ClientContext;

namespace mssql {
namespace backup {

//! Bytes fetched from one family file, contiguous, pinned while this object lives: the cache's
//! buffers when they hold the range in one piece, else a copy in a buffer-manager allocation
//! (so it is counted against memory_limit too).
class FetchedRange {
public:
	FetchedRange() = default;
	FetchedRange(FetchedRange &&) = default;
	FetchedRange &operator=(FetchedRange &&) = default;

	const uint8_t *Data() const {
		return data_;
	}
	size_t Size() const {
		return size_;
	}
	const ByteRange &Range() const {
		return range_;
	}
	//! True when the cache returned the range in pieces and this holds a contiguous copy.
	bool Copied() const {
		return copy_.get() != nullptr;
	}
	//! The bytes as a span for the parsers; errors name `what` at the file offset.
	ByteReader Reader(const char *what) const {
		return ByteReader(data_, size_, what, range_.offset);
	}
	//! The bytes of `inner` (which this range contains) as a span.
	ByteReader Reader(const ByteRange &inner, const char *what) const;

private:
	friend class ByteSource;

	ByteRange range_;
	const uint8_t *data_ = nullptr;
	size_t size_ = 0;
	FileBufferHandleGroup group_;
	AllocatedData copy_;
};

//! The requests of one CoalescePlan, filled by the fetch tasks; each request's bytes, or the
//! error that stopped it, are read back after the tasks ran.
class FetchBatch {
public:
	explicit FetchBatch(CoalescePlan plan_p);

	const CoalescePlan &Plan() const {
		return plan_;
	}
	//! The fetched bytes of request `i`; rethrows the error its fetch hit.
	const FetchedRange &Request(size_t i) const;
	//! The request holding wanted range `member`, and where it lies in it (set up by the plan).
	size_t RequestOf(size_t member) const;

private:
	friend class ByteSource;

	CoalescePlan plan_;
	vector<FetchedRange> fetched_;
	vector<ErrorData> errors_;
	//! A task's FormatError, kept as one: ErrorData would rethrow it as a DuckDB Exception, past
	//! every catch (FormatError &) above (a fallback, a file named).
	vector<std::string> format_errors_;
	vector<bool> done_;
	vector<size_t> request_of_;
	bool scheduled_ = false;
	mutable mutex lock_;
};

//! Lifetime: the file handles keep the opening ClientContext's file system and query context
//! (CachingFileHandle reopens through them), so a ByteSource -- and every task it made -- must
//! not outlive the ClientContext that opened it. What outlives a statement (spec 085 D9's
//! opened-file cache) keeps the parsed state, not the source, and reopens files per context.
class ByteSource {
public:
	//! The files of `paths`, each a path or a glob (expanded in sorted order; a glob that matches
	//! nothing is an error naming it), opened read-only. Duplicates are refused. The order is the
	//! caller's; which file is which family is the MTF header's to say.
	static unique_ptr<ByteSource> Open(ClientContext &context, const vector<std::string> &paths);

	size_t FileCount() const {
		return files_.size();
	}
	const std::string &Path(size_t file) const;
	uint64_t FileSize(size_t file) const;
	//! The cache key's version: the ETag of a remote file, else its last-modified time (D9).
	std::string VersionTag(size_t file) const;
	bool IsRemote(size_t file) const;

	//! `range`, fetched now on this thread. A range past the file's end is a FormatError naming
	//! the file: a structure in the backup pointed outside it.
	FetchedRange Read(const ByteRange &range, const char *what);

	//! One AsyncTask per request of `batch`, each fetching its range into the batch, for
	//! AsyncResult::FromTasks(…, TaskSchedulerType::ASYNC) or to run in place. A failed fetch is
	//! recorded in the batch, not thrown from the task. A batch is scheduled once.
	vector<unique_ptr<AsyncTask>> FetchTasks(const shared_ptr<FetchBatch> &batch);

	//! Every request of `batch` fetched now, side by side on the caller's thread and the ASYNC
	//! pool (AsyncResult::ExecuteTasksSynchronously): for callers that cannot return BLOCKED.
	void FetchNow(ClientContext &context, const shared_ptr<FetchBatch> &batch);

private:
	struct File {
		std::string path;
		shared_ptr<CachingFileHandle> handle;
		uint64_t size = 0;
	};

	static FetchedRange Fetch(CachingFileHandle &handle, const std::string &path, const ByteRange &range);
	void Check(const ByteRange &range, const char *what) const;

	vector<File> files_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

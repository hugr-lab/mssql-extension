#include "backup/backup_source.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/callback_async_task.hpp"

#include <algorithm>
#include <set>

namespace duckdb {
namespace mssql {
namespace backup {

ByteReader FetchedRange::Reader(const ByteRange &inner, const char *what) const {
	if (inner.family != range_.family || inner.offset < range_.offset || inner.offset > range_.End() ||
		inner.length > range_.End() - inner.offset) {
		throw InternalException("backup: a range read outside the request that fetched it");
	}
	return ByteReader(data_ + (inner.offset - range_.offset), inner.length, what, inner.offset);
}

FetchBatch::FetchBatch(CoalescePlan plan_p) : plan_(std::move(plan_p)) {
	const size_t n = plan_.requests.size();
	fetched_.resize(n);
	errors_.resize(n);
	format_errors_.resize(n);
	done_.assign(n, false);
	size_t wanted = plan_.empty.size();
	for (const auto &request : plan_.requests) {
		wanted += request.members.size();
	}
	request_of_.assign(wanted, static_cast<size_t>(-1));
	for (size_t i = 0; i < n; i++) {
		for (size_t member : plan_.requests[i].members) {
			if (member >= request_of_.size()) {
				throw InternalException("backup: a fetch plan whose members are not 0..n-1");
			}
			request_of_[member] = i;
		}
	}
}

const FetchedRange &FetchBatch::Request(size_t i) const {
	lock_guard<mutex> guard(lock_);
	if (i >= fetched_.size()) {
		throw InternalException("backup: fetch request %llu of %llu", (unsigned long long)i,
								(unsigned long long)fetched_.size());
	}
	if (!format_errors_[i].empty()) {
		throw FormatError(format_errors_[i]);
	}
	if (errors_[i].HasError()) {
		errors_[i].Throw();
	}
	if (!done_[i]) {
		throw InternalException("backup: a fetch request read before its task ran");
	}
	return fetched_[i];
}

size_t FetchBatch::RequestOf(size_t member) const {
	if (member >= request_of_.size() || request_of_[member] == static_cast<size_t>(-1)) {
		throw InternalException("backup: range %llu is in no fetch request (an empty range has no bytes)",
								(unsigned long long)member);
	}
	return request_of_[member];
}

unique_ptr<ByteSource> ByteSource::Open(ClientContext &context, const vector<std::string> &paths) {
	if (paths.empty()) {
		throw InvalidInputException("mssql backup: no file given");
	}
	auto &fs = FileSystem::GetFileSystem(context);
	vector<std::string> expanded;
	for (const auto &path : paths) {
		if (FileSystem::HasGlob(path)) {
			auto matches = fs.GlobFiles(path, FileGlobOptions::ALLOW_EMPTY);
			if (matches.empty()) {
				throw IOException("mssql backup: no file matches \"%s\"", path);
			}
			vector<std::string> names;
			for (auto &match : matches) {
				names.push_back(match.path);
			}
			std::sort(names.begin(), names.end());
			expanded.insert(expanded.end(), names.begin(), names.end());
		} else {
			expanded.push_back(path);
		}
	}
	std::set<std::string> seen;
	for (const auto &path : expanded) {
		if (!seen.insert(path).second) {
			throw InvalidInputException("mssql backup: \"%s\" is given twice", path);
		}
	}
	auto source = make_uniq<ByteSource>();
	auto caching = CachingFileSystem::Get(context);
	for (const auto &path : expanded) {
		File file;
		file.path = path;
		file.handle = shared_ptr<CachingFileHandle>(
			caching.OpenFile(QueryContext(context), OpenFileInfo(path), FileFlags::FILE_FLAGS_READ).release());
		file.size = file.handle->GetFileSize();
		source->files_.push_back(std::move(file));
	}
	return source;
}

const std::string &ByteSource::Path(size_t file) const {
	return files_.at(file).path;
}

uint64_t ByteSource::FileSize(size_t file) const {
	return files_.at(file).size;
}

std::string ByteSource::VersionTag(size_t file) const {
	auto &handle = *files_.at(file).handle;
	auto tag = handle.GetVersionTag();
	if (!tag.empty()) {
		return tag;
	}
	return std::to_string(Timestamp::GetEpochMicroSeconds(handle.GetLastModifiedTime()));
}

bool ByteSource::IsRemote(size_t file) const {
	return files_.at(file).handle->IsRemoteFile();
}

void ByteSource::Check(const ByteRange &range, const char *what) const {
	if (range.family >= files_.size()) {
		throw InternalException("backup: file %llu of %llu", (unsigned long long)range.family,
								(unsigned long long)files_.size());
	}
	const auto &file = files_[range.family];
	if (range.offset > file.size || range.length > file.size - range.offset) {
		throw FormatError("backup file \"" + file.path + "\": " + what + " at offset " + std::to_string(range.offset) +
						  " (" + std::to_string(range.length) + " bytes) runs past the end of the " +
						  std::to_string(file.size) + "-byte file");
	}
}

FetchedRange ByteSource::Fetch(CachingFileHandle &handle, const std::string &path, const ByteRange &range) {
	FetchedRange fetched;
	fetched.range_ = range;
	if (range.length == 0) {
		return fetched;
	}
	fetched.group_ = handle.Read(range.length, range.offset);
	const auto &handles = fetched.group_.GetHandles();
	uint64_t total = 0;
	for (const auto &piece : handles) {
		total += piece.length;
	}
	if (total != range.length) {
		// The file is shorter than it was at open (rewritten, truncated): a named error, not the
		// group's internal one.
		throw FormatError("backup file \"" + path + "\": a read of " + std::to_string(range.length) +
						  " bytes at offset " + std::to_string(range.offset) + " returned " + std::to_string(total) +
						  ": the file changed while it was open");
	}
	if (handles.size() == 1) {
		fetched.data_ = fetched.group_.Ptr();
	} else {
		// The cache returned the range in pieces (it crosses its blocks): one contiguous copy,
		// allocated by the buffer manager's allocator so it is counted.
		fetched.copy_ = handle.GetBufferAllocator().Allocate(range.length);
		fetched.group_.CopyTo(fetched.copy_.get(), range.length);
		fetched.group_ = FileBufferHandleGroup();
		fetched.data_ = fetched.copy_.get();
	}
	fetched.size_ = range.length;
	return fetched;
}

FetchedRange ByteSource::Read(const ByteRange &range, const char *what) {
	Check(range, what);
	return Fetch(*files_[range.family].handle, files_[range.family].path, range);
}

vector<unique_ptr<AsyncTask>> ByteSource::FetchTasks(const shared_ptr<FetchBatch> &batch) {
	{
		lock_guard<mutex> guard(batch->lock_);
		if (batch->scheduled_) {
			// A second run would overwrite ranges a caller may still hold from Request().
			throw InternalException("backup: a fetch batch scheduled twice");
		}
		batch->scheduled_ = true;
	}
	vector<unique_ptr<AsyncTask>> tasks;
	const auto &requests = batch->Plan().requests;
	for (size_t i = 0; i < requests.size(); i++) {
		const auto &range = requests[i].range;
		Check(range, "a coalesced read");
		// The task holds the batch (and so the buffers it fills) and the file's handle, so it does
		// not depend on the caller's frame or the source; it must not outlive the ClientContext.
		auto handle = files_[range.family].handle;
		auto path = files_[range.family].path;
		tasks.push_back(make_uniq<CallbackAsyncTask>(
			[batch, i, range, handle, path]() {
				FetchedRange fetched;
				ErrorData error;
				std::string format_error;
				try {
					fetched = Fetch(*handle, path, range);
				} catch (FormatError &e) {
					format_error = e.what();
				} catch (std::exception &e) {
					error = ErrorData(e);
				}
				lock_guard<mutex> guard(batch->lock_);
				batch->fetched_[i] = std::move(fetched);
				batch->errors_[i] = std::move(error);
				batch->format_errors_[i] = std::move(format_error);
				batch->done_[i] = true;
			},
			range.length));
	}
	return tasks;
}

void ByteSource::FetchNow(ClientContext &context, const shared_ptr<FetchBatch> &batch) {
	auto tasks = FetchTasks(batch);
	if (tasks.empty()) {
		return;
	}
	auto result = AsyncResult::FromTasks(std::move(tasks), TaskSchedulerType::ASYNC);
	result.ExecuteTasksSynchronously(context);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

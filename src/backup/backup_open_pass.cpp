#include "backup/backup_open_pass.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint64_t GROUP_BYTES = static_cast<uint64_t>(PAGE_SIZE) * PAGES_PER_EXTENT;

static uint64_t NextAccessId() {
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1);
}

SourceAccess::SourceAccess(ByteSource &source, size_t file) : source_(source), file_(file), id_(NextAccessId()) {
	if (file >= source.FileCount()) {
		throw FormatError("backup: family file " + std::to_string(file) + " of a source of " +
						  std::to_string(source.FileCount()));
	}
}

uint64_t SourceAccess::Size() const {
	return source_.FileSize(file_);
}

void SourceAccess::ReadDirect(uint64_t offset, uint8_t *dest, size_t count) const {
	ByteRange range;
	range.family = static_cast<uint32_t>(file_);
	range.offset = offset;
	range.length = count;
	auto fetched = source_.Read(range, "a backup structure");
	std::memcpy(dest, fetched.Data(), count);
	fetches_.fetch_add(1, std::memory_order_relaxed);
}

void SourceAccess::ReadAt(uint64_t offset, uint8_t *dest, size_t count) const {
	// The thread's read-aheads, WINDOWS of them: each what it read at once of one access (`bytes`, `size`
	// of them from `base`) and where its last read ended -- a stream. Several, so reads that take turns
	// (a page and its older version in another file, a data page and the LOB pages its rows point to)
	// each keep reading on instead of throwing away a window at every turn. A read that continues none
	// starts a stream with no buffer yet (the least recently used such slot, else the least recently
	// used one); its next read past it fills a window. The buffers' capacity, READ_AHEAD each, stays with
	// the thread (spec 085 § Resources). A read as long as a read-ahead (a container's window) is
	// fetched as asked: buffering it again would only copy it.
	struct Window {
		uint64_t access = 0;
		uint64_t base = 0;
		uint64_t size = 0;
		uint64_t last_end = UINT64_MAX;
		uint64_t used = 0;
		std::vector<uint8_t> bytes;
	};
	static thread_local Window windows[WINDOWS];
	static thread_local uint64_t tick = 0;
	tick++;
	const uint64_t size = Size();
	const bool in_file = count > 0 && offset <= size && count <= size - offset;
	if (in_file) {
		for (auto &w : windows) {
			if (w.access == id_ && w.size > 0 && offset >= w.base && count <= w.size &&
				offset - w.base <= w.size - count) {
				std::memcpy(dest, w.bytes.data() + (offset - w.base), count);
				w.last_end = offset + count;
				w.used = tick;
				return;
			}
		}
		if (count < READ_AHEAD) {
			for (auto &w : windows) {
				if (w.access == id_ && w.last_end != UINT64_MAX && offset >= w.last_end &&
					offset - w.last_end <= SEQUENTIAL_GAP) {
					const uint64_t n = std::min<uint64_t>(READ_AHEAD, size - offset);  // >= count
					w.access = 0;  // not this file's while the read may throw
					w.size = 0;
					w.bytes.resize(static_cast<size_t>(n));
					ReadDirect(offset, w.bytes.data(), static_cast<size_t>(n));
					w.access = id_;
					w.base = offset;
					w.size = n;
					w.used = tick;
					std::memcpy(dest, w.bytes.data(), count);
					w.last_end = offset + count;
					return;
				}
			}
		}
	}
	// As asked (a read past the end is the source's to refuse, naming the file); a stream starts here.
	ReadDirect(offset, dest, count);
	Window *slot = nullptr;
	for (auto &w : windows) {
		if (w.size == 0 && (!slot || w.used < slot->used)) {
			slot = &w;
		}
	}
	if (!slot) {
		for (auto &w : windows) {
			if (!slot || w.used < slot->used) {
				slot = &w;
			}
		}
	}
	slot->access = id_;
	slot->size = 0;
	slot->last_end = offset + count;
	slot->used = tick;
}

const std::string &SourceAccess::Name() const {
	return source_.Path(file_);
}

PassMap BuildPassMapInRanges(ClientContext &context, ByteSource &source, size_t file, uint64_t offset, uint64_t length,
							 uint32_t number, const PassScanOptions &options, uint64_t *ranges_read) {
	PassHeaderScan scan(source.Path(file), source.FileSize(file), offset, length, number);
	uint64_t range_groups = std::max<uint64_t>(1, options.range_bytes / GROUP_BYTES);
	size_t in_flight = options.ranges_in_flight;
	if (in_flight == 0) {
		const size_t threads = static_cast<size_t>(TaskScheduler::GetScheduler(context).NumberOfThreads());
		in_flight = std::min<size_t>(16, std::max<size_t>(2, threads * 2));
		// A wave may pin at most an eighth of memory_limit. A range costs up to twice its bytes:
		// a remote file's cache blocks (2 MiB) never line up with a pass's 2-byte lead-in, so the
		// range comes back in pieces and is copied while they are still pinned (FetchedRange).
		const uint64_t budget = BufferManager::GetBufferManager(context).GetMaxMemory() / 8;
		const uint64_t per_range = 2 * range_groups * GROUP_BYTES;
		if (per_range > budget) {
			range_groups = std::max<uint64_t>(1, budget / (2 * GROUP_BYTES));
			in_flight = 1;
		} else {
			in_flight = std::min<size_t>(in_flight, static_cast<size_t>(budget / per_range));
		}
	}
	// Each wanted range its own request: they are adjacent, and coalescing them would only make
	// requests of more than range_bytes.
	CoalescePolicy policy;
	policy.max_gap = 0;
	policy.max_length = range_groups * GROUP_BYTES;
	const uint64_t groups = scan.GroupCount();
	uint64_t next = 0;
	while (next < groups) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		std::vector<ByteRange> wanted;
		std::vector<uint64_t> firsts;
		for (size_t i = 0; i < in_flight && next < groups; i++) {
			const uint64_t n = std::min(range_groups, groups - next);
			ByteRange r;
			r.family = static_cast<uint32_t>(file);
			r.offset = scan.GroupOffset(next);
			r.length = n * GROUP_BYTES;
			wanted.push_back(r);
			firsts.push_back(next);
			next += n;
		}
		auto batch = make_shared_ptr<FetchBatch>(Coalesce(wanted, policy));
		source.FetchNow(context, batch);
		for (size_t i = 0; i < wanted.size(); i++) {
			const auto &request = batch->Request(batch->RequestOf(i));
			auto reader = request.Reader(wanted[i], "a data pass");
			scan.Feed(firsts[i], reader.Bytes(0, static_cast<size_t>(wanted[i].length)),
					  wanted[i].length / GROUP_BYTES);
		}
		if (ranges_read) {
			*ranges_read += batch->Plan().requests.size();
		}
	}
	return scan.Finish();
}

PassMapper RangedPassMapper(ClientContext &context, const PassScanOptions &options) {
	ClientContext *ctx = &context;
	return [ctx, options](const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number) {
		auto source = dynamic_cast<const SourceAccess *>(&stream);
		if (!source) {
			return BuildPassMap(stream, offset, length, number);
		}
		return BuildPassMapInRanges(*ctx, source->Source(), source->File(), offset, length, number, options);
	};
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

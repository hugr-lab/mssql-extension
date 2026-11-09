#include "backup/backup_ranges.hpp"

#include <algorithm>

namespace duckdb {
namespace mssql {
namespace backup {

CoalescePlan Coalesce(const std::vector<ByteRange> &wanted, const CoalescePolicy &policy) {
	CoalescePlan plan;
	std::vector<size_t> order;
	order.reserve(wanted.size());
	for (size_t i = 0; i < wanted.size(); i++) {
		if (wanted[i].length == 0) {
			plan.empty.push_back(i);
		} else {
			order.push_back(i);
		}
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		const auto &ra = wanted[a];
		const auto &rb = wanted[b];
		if (ra.family != rb.family) {
			return ra.family < rb.family;
		}
		if (ra.offset != rb.offset) {
			return ra.offset < rb.offset;
		}
		if (ra.length != rb.length) {
			// Longer first at one offset, so the shorter ones fall inside it.
			return ra.length > rb.length;
		}
		return a < b;
	});
	for (size_t i : order) {
		const auto &next = wanted[i];
		if (!plan.requests.empty() && !next.Wraps() && !plan.requests.back().range.Wraps()) {
			auto &current = plan.requests.back();
			const uint64_t end = current.range.End();
			const bool same_file = current.range.family == next.family;
			// Sorted by offset, so next.offset >= current.range.offset; within the gap when it starts
			// at or before end + max_gap (overflow-safe: compare the distance past the end).
			const bool close = same_file && (next.offset <= end || next.offset - end <= policy.max_gap);
			const uint64_t merged_end = std::max(end, next.End());
			if (close && merged_end - current.range.offset <= policy.max_length) {
				current.range.length = merged_end - current.range.offset;
				current.members.push_back(i);
				continue;
			}
			if (same_file && next.End() <= end) {
				// Wholly inside the current request (which may already exceed max_length): no new read.
				current.members.push_back(i);
				continue;
			}
		}
		CoalescedRange request;
		request.range = next;
		request.members.push_back(i);
		plan.requests.push_back(std::move(request));
	}
	return plan;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

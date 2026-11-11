//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_ranges.hpp
//
// Spec 085 D3a: the byte ranges a consumer asks for (a unit's pages, a block, a header),
// coalesced into fewer, larger requests -- one HTTP range request each on httpfs -- the way the
// Parquet reader merges its read heads. DuckDB-free (D15): the planning is plain arithmetic,
// the fetching is backup_source's.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

//! `length` bytes at `offset` of the family (stripe) file `family`.
struct ByteRange {
	uint32_t family = 0;
	uint64_t offset = 0;
	uint64_t length = 0;

	//! offset + length; only meaningful when !Wraps().
	uint64_t End() const {
		return offset + length;
	}
	//! True when offset + length overflows: a length no file can hold, from a corrupt structure.
	bool Wraps() const {
		return length > UINT64_MAX - offset;
	}
};

//! When two ranges become one request: on the same file, the second starting no more than
//! `max_gap` bytes past the first's end (the gap is read and thrown away, cheaper than another
//! round trip on object storage), and the merged request no longer than `max_length` (one read
//! pins its whole range, D3a). A single range longer than `max_length` stays one request.
struct CoalescePolicy {
	uint64_t max_gap = 256 * 1024;
	uint64_t max_length = 8 * 1024 * 1024;
};

//! One request and the requests it serves: `members[i]` is the index of a range the caller
//! asked for, which lies at `offset - range.offset` inside it.
struct CoalescedRange {
	ByteRange range;
	std::vector<size_t> members;
};

//! The requests that read every range of `wanted`, in (family, offset) order. Overlapping and
//! duplicate ranges are served by one request; an empty range is served without bytes (it is
//! a member of no request and of `empty` instead). Every non-empty wanted range is a member of
//! exactly one request, which contains it. A range whose end overflows (Wraps) is a request of
//! its own, merged with nothing, so the source's bounds check refuses it by name.
struct CoalescePlan {
	std::vector<CoalescedRange> requests;
	std::vector<size_t> empty;
};

CoalescePlan Coalesce(const std::vector<ByteRange> &wanted, const CoalescePolicy &policy);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

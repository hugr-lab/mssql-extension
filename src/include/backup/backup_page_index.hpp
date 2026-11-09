//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_page_index.hpp
//
// Spec 085 task 7a, D4 / D4a: where each page's latest copy lies in the data stream. A data pass
// (one MSDA block's MQDA stream) is a run of 8 KB units after a 2-byte lead-in, in groups of 8:
// one group per extent, its page k at position k (E12). Units that are not pages -- a zero unit
// (an unallocated page of a copied extent) or a pad (m_type 101) -- hold nothing.
//
// A pass's map takes one of two forms (D4a's resident tier):
//  - ranked: groups in ascending (file, extent) order with empty groups only at the end (every
//    full backup's first pass on one media family, E12): per file a bitmap of the extents the pass
//    carries and a rank directory; an extent's group is its rank. About 21 MB per pass for 10 TB.
//  - runs: (file, first extent, count, first group) where the order breaks (a second pass, a
//    stripe, a differential).
// The later pass wins (D4). DuckDB-free (D15); the reads come through a RandomAccess.
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint32_t PAGE_SIZE = 8192;
static constexpr uint32_t PAGES_PER_EXTENT = 8;

//! Extent `extent` of file `file`.
struct ExtentId {
	uint16_t file = 0;
	uint32_t extent = 0;

	bool operator<(const ExtentId &o) const {
		return file != o.file ? file < o.file : extent < o.extent;
	}
	bool operator==(const ExtentId &o) const {
		return file == o.file && extent == o.extent;
	}
};

//! `count` extents from `first` of one file, held by groups `first_group` .. + count.
struct ExtentRun {
	ExtentId first;
	uint32_t count = 0;
	uint64_t first_group = 0;
};

//! A set of extents of one file as a bitmap with a rank directory: Rank(e) is how many extents of
//! the set lie below e, in O(1) (a count per 512 extents, then a popcount within).
class ExtentBitmap {
public:
	void Add(uint32_t extent);
	bool Contains(uint32_t extent) const;
	//! Extents of the set below `extent`. Valid after Seal().
	uint64_t Rank(uint32_t extent) const;
	//! Builds the rank directory; no Add after it.
	void Seal();
	uint64_t Count() const {
		return count_;
	}
	size_t MemoryBytes() const {
		return words_.capacity() * sizeof(uint64_t) + directory_.capacity() * sizeof(uint64_t);
	}

private:
	std::vector<uint64_t> words_;
	std::vector<uint64_t> directory_;  // set bits before each 8-word block
	uint64_t count_ = 0;
	bool sealed_ = false;

public:
	//! The bytes the bitmap needs to hold `extent` (its words and their directory share).
	static size_t BytesFor(uint32_t extent) {
		const size_t words = static_cast<size_t>(extent) / 64 + 1;
		return words * sizeof(uint64_t) + (words / 8 + 2) * sizeof(uint64_t);
	}
	size_t Words() const {
		return words_.size();
	}
	//! Every extent of the set, ascending.
	template <class F>
	void ForEach(F f) const {
		for (size_t w = 0; w < words_.size(); w++) {
			uint64_t bits = words_[w];
			while (bits) {
				unsigned b = 0;
				while (!((bits >> b) & 1)) {
					b++;  // portable; this walk runs only when a ranked map turns into runs
				}
				f(static_cast<uint32_t>(w * 64 + b));
				bits &= bits - 1;
			}
		}
	}
};

//! One data pass and where its extents lie.
class PassMap {
public:
	//! The group holding `extent`, or false when the pass does not carry it.
	bool Find(const ExtentId &extent, uint64_t &group) const;
	bool Ranked() const {
		return ranked_;
	}
	uint64_t GroupCount() const {
		return groups_;
	}
	uint64_t ExtentCount() const {
		return extents_;
	}
	size_t MemoryBytes() const;
	//! Where the pass's units start in the stream (after its lead-in).
	uint64_t units_offset = 0;
	//! The pass's position among the data set's passes (0 = pass 1).
	uint32_t number = 0;

private:
	friend PassMap BuildPassMap(const RandomAccess &, uint64_t, uint64_t, uint32_t);
	friend class PassMapBuilder;

	bool ranked_ = false;
	uint64_t groups_ = 0;
	uint64_t extents_ = 0;
	// Ranked form: per file (by file id) its bitmap and the group of its first extent.
	std::vector<uint16_t> files_;
	std::vector<ExtentBitmap> bitmaps_;
	std::vector<uint64_t> file_first_group_;
	// Runs form.
	std::vector<ExtentRun> runs_;
};

//! The pass whose MQDA data is `length` bytes at `offset` of `stream`, mapped from its pages'
//! headers: each group's extent from its first page (every page of a group must name that extent
//! at its own position, else a FormatError). Ranked when the order allows, runs otherwise.
PassMap BuildPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number);

//! Where a page's latest copy lies.
struct PageLocation {
	uint32_t pass = 0;
	uint64_t offset = 0;  // in the stream
};

class PageIndex {
public:
	//! The latest copy of page `page` of file `file`: the last pass that carries its extent (D4:
	//! the later pass wins). False when no pass carries it.
	bool Locate(uint16_t file, uint32_t page, PageLocation &location) const;
	const std::vector<PassMap> &Passes() const {
		return passes_;
	}
	size_t MemoryBytes() const;

private:
	friend PageIndex BuildPageIndex(const RandomAccess &, const MtfLayout &);
	std::vector<PassMap> passes_;
};

//! Every data pass of `layout` (each MSDA block's MQDA streams), mapped by BuildPassMap.
PageIndex BuildPageIndex(const RandomAccess &stream, const MtfLayout &layout);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

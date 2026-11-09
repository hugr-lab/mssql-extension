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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
	//! The extent of rank `rank` (0-based) in the set, or false past its count. Valid after Seal().
	bool Select(uint64_t rank, uint32_t &extent) const;
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
					b++;  // portable; the conversion to runs is the only caller
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
	//! The media family (stripe) whose stream holds it.
	uint32_t family = 0;

private:
	friend PassMap BuildPassMap(const RandomAccess &, uint64_t, uint64_t, uint32_t);
	friend bool BuildRankedPassMap(const RandomAccess &, uint64_t, uint64_t, PassMap &, uint64_t *);
	friend class PassMapBuilder;
	friend class RankedPassBuilder;
	friend class PassHeaderScan;

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

//! Maps a pass from its page headers: BuildPassMap's signature. The DuckDB layer gives one that
//! reads a family file as parallel ranges (backup_open_pass, D3a).
class PassMap;
using PassMapper = std::function<PassMap(const RandomAccess &, uint64_t, uint64_t, uint32_t)>;

//! What one 8-unit group's headers name: an extent, or nothing (zero units and pads only).
struct GroupHeader {
	bool found = false;
	ExtentId extent;
};

class PassMapBuilder;

//! A pass mapped from its page headers in pieces, so the DuckDB layer can fetch the pieces as
//! parallel ranges (D3a) and parse them on any thread: ScanGroups is a pure function over the
//! bytes of whole groups; Add takes its results strictly in group order (the map is built in
//! order); Feed is both. Finish checks every group was fed. The rules are BuildPassMap's.
class PassHeaderScan {
public:
	//! The pass whose MQDA data is `length` bytes at `offset` of a stream of `stream_size` bytes
	//! named `name`; the bounds and the unit arithmetic are checked here.
	PassHeaderScan(std::string name, uint64_t stream_size, uint64_t offset, uint64_t length, uint32_t number);
	~PassHeaderScan();
	PassHeaderScan(const PassHeaderScan &) = delete;
	PassHeaderScan &operator=(const PassHeaderScan &) = delete;

	uint64_t GroupCount() const {
		return map_.groups_;
	}
	//! Where group `group` starts in the stream.
	uint64_t GroupOffset(uint64_t group) const;
	//! The groups whose bytes (`groups` × 64 KB) start at stream offset `at`; `name` for errors.
	static void ScanGroups(const std::string &name, uint64_t at, const uint8_t *bytes, uint64_t groups,
						   std::vector<GroupHeader> &out);
	//! Groups `first_group` … in order; anything else is a FormatError.
	void Add(uint64_t first_group, const std::vector<GroupHeader> &groups);
	void Feed(uint64_t first_group, const uint8_t *bytes, uint64_t groups);
	PassMap Finish();

private:
	std::string name_;
	uint64_t offset_ = 0;
	PassMap map_;
	std::unique_ptr<PassMapBuilder> builder_;
	uint64_t next_group_ = 0;
	std::vector<GroupHeader> scratch_;
};

//! The pass whose MQDA data is `length` bytes at `offset` of `stream`, mapped from its pages'
//! headers: each group's extent from its first page (every page of a group must name that extent
//! at its own position, else a FormatError). Ranked when the order allows, runs otherwise.
PassMap BuildPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number);

//! Pass 1 of a full backup on one media family, mapped from its allocation pages alone (E12): the
//! pass is one group per extent of (GAM-allocated extents below the file's Size, plus the extents
//! of the allocation pages), ascending by (file, extent), then pad groups. Reads each file's
//! group 0 (file header, GAM), the group of every later GAM, and a few groups more to check the
//! rule, not every group. False when anything read disagrees with the rule, or a read fails: the
//! caller then maps the pass by its headers (BuildPassMap). `groups_read`, when given, counts the
//! groups it read. One difference from the header scan, harmless: a GAM-allocated extent whose
//! pages are all unallocated is a group of zero units, which this path counts as carried (Locate
//! then returns zero units, which hold no row) and the header scan as empty.
bool BuildRankedPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, PassMap &map,
						uint64_t *groups_read = nullptr);

//! Where a page's latest copy lies.
struct PageLocation {
	static constexpr uint64_t NO_BLOCK = ~uint64_t(0);
	uint32_t family = 0;  // the media family (stripe) whose stream holds the copy
	uint32_t pass = 0;
	uint64_t offset = 0;  // in that family's stream (a container's: its data set), when block is NO_BLOCK
	//! Under trust_layout, a container's pass found by resync: the physical offset of the block
	//! whose decoded group holds the page (unit page % 8); offset is then NO_BLOCK too, so a read
	//! at it fails rather than reading the data set's start (ReadPage handles both).
	uint64_t block = NO_BLOCK;
};

class ContainerStream;
class PassSearch;
struct SeekEntry;

//! A container family's file and chain, for the search under trust_layout (DQ-41).
struct ContainerFamily {
	const RandomAccess *file = nullptr;
	const ContainerStream *stream = nullptr;  // the family's data set
	const std::vector<SeekEntry> *seek = nullptr;
	uint64_t chain_end = 0;
	std::function<void(const uint8_t *, size_t, uint8_t *, size_t)> decoder;
};

//! One media family's data: its stream (a family file, or a container's data set) and its walk;
//! `container` when the stream is a container's data set.
struct FamilyStream {
	const RandomAccess *stream = nullptr;
	const MtfLayout *layout = nullptr;
	const ContainerFamily *container = nullptr;
};

//! How BuildPageIndex maps a full backup's pass 1 and every other pass (D4a, DQ-41).
struct PageIndexOptions {
	//! A full backup: its pass 1 is ranked (one plain family, E12) or searched (a striped set, a
	//! container, a rank that failed), never read whole. Otherwise every pass is header-scanned.
	bool full_backup = false;
	//! A container's pass 1 searched by resync from any offset (DQ-41's trust_layout), not through
	//! its chain walked from known blocks.
	bool trust_layout = false;
	//! The header scans' mapper (the DuckDB layer's reads them as parallel ranges); BuildPassMap
	//! when empty.
	PassMapper mapper;
};

//! One pass of the index: mapped (ranked or by headers) or searched.
struct IndexedPass {
	IndexedPass();
	IndexedPass(IndexedPass &&);
	IndexedPass &operator=(IndexedPass &&);
	~IndexedPass();

	uint32_t number = 0;
	uint32_t family = 0;
	uint64_t data_offset = 0;  // its MQDA stream, for a header scan when a search gives up
	uint64_t data_length = 0;
	uint64_t units_offset = 0;
	uint64_t groups = 0;
	PassMap map;  // when not searched, or once its search gave up (fell_back)
	std::unique_ptr<PassSearch> search;
	bool by_block = false;	// the search's positions are a container's block offsets
	//! The search gave the pass up and `map` (its header scan) answers now. Set once, after the map
	//! is built; the search stays alive for the readers still in it.
	std::atomic<bool> fell_back{false};
	bool Searched() const {
		return search != nullptr && !fell_back.load(std::memory_order_acquire);
	}
};

class PageIndex {
public:
	PageIndex();
	PageIndex(PageIndex &&);
	PageIndex &operator=(PageIndex &&);
	~PageIndex();

	//! The latest copy of page `page` of file `file`: the last pass that carries its extent (D4:
	//! the later pass wins; within one pass an extent is in exactly one family, E12, and a pass
	//! carrying it in two families is a FormatError). False when no pass of any family carries it.
	//! A searched pass reads the file (DQ-41); one whose order breaks is mapped by its headers then.
	//! A searched pass's "absent" rests on its order, which a search sees only where it looks: with
	//! `expected` (the page is live, or an allocation page: a full's pass 1 carries every one, E12),
	//! an answer of absent maps every searched pass by its headers and asks again, so a break the
	//! search did not see cannot hide a page.
	bool Locate(uint16_t file, uint32_t page, PageLocation &location, bool expected = false) const;
	//! The 8 KB at `page`'s location into `out`: false when no pass carries it (`expected` as for
	//! Locate). Its header is the caller's to check (a ranked pass may locate a zero unit, E12).
	bool ReadPage(uint16_t file, uint32_t page, std::vector<uint8_t> &out, bool expected = false) const;

	size_t PassCount() const {
		return passes_.size();
	}
	//! Its `map` is the pass's only while it is not Searched(): a search that gives up replaces it.
	const IndexedPass &Pass(size_t i) const {
		return passes_[i];
	}
	const FamilyStream &Family(size_t f) const {
		return families_[f];
	}
	size_t FamilyCount() const {
		return families_.size();
	}
	size_t MemoryBytes() const;

private:
	friend PageIndex BuildPageIndex(const std::vector<FamilyStream> &, const PageIndexOptions &);
	//! Pass `i`'s position of `extent` when it carries it: a group index, or under trust_layout a
	//! container block's offset (`by_block`). A search that gives up maps the pass by its headers.
	bool FindIn(size_t i, const ExtentId &extent, uint64_t &position, bool &by_block) const;
	//! Pass `i` mapped by its headers, its search given up (under fallback_lock_ inside).
	void FallBack(size_t i) const;
	bool LocateOnce(uint16_t file, uint32_t page, PageLocation &location) const;

	std::vector<FamilyStream> families_;
	mutable std::vector<IndexedPass> passes_;
	mutable std::unique_ptr<std::mutex> fallback_lock_;
};

//! Every data pass of every family (each MSDA block's MQDA stream), ordered by pass number: a full
//! backup's pass 1 ranked or searched, as `options` and the families say; every other pass by its
//! headers.
PageIndex BuildPageIndex(const std::vector<FamilyStream> &families,
						 const PageIndexOptions &options = PageIndexOptions());
//! One family's index; `full_backup` as PageIndexOptions says.
PageIndex BuildPageIndex(const RandomAccess &stream, const MtfLayout &layout, bool full_backup = false);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

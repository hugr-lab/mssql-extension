//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_page.hpp
//
// Spec 085 PR 2b, task 11b: one 8 KB page of a backup -- its 96-byte header (spec 085 § Rules: the
// next-page pointer u32 page @16 / u16 file @20, the slot count @22, the page's own id @32 / @36) and
// the allocation unit it names -- and the PageStore that hands out a LIVE page's latest copy with
// that header checked: it names itself, and a live page is never a zero unit. Every reader of
// records (catalog, rows, IAM chains) takes its pages here, so a page the backup carries but the
// database does not use (E2 R3) never reaches one. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_allocation.hpp"
#include "backup/backup_bytes.hpp"
#include "backup/backup_page_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr size_t PAGE_HEADER_SIZE = 96;

//! m_type values the reader names (others are refused by the reader that expects one of these).
enum class PageType : uint8_t {
	Data = 1,
	Index = 2,
	TextMix = 3,
	TextTree = 4,
	Gam = 8,
	Sgam = 9,
	Iam = 10,
	Pfs = 11,
	Boot = 13,
	FileHeader = 15,
	DiffMap = 16,
	BulkMap = 17,
};

//! A 6-byte page pointer: u32 page, u16 file. (0, 0) is none.
struct PagePointer {
	uint32_t page = 0;
	uint16_t file = 0;

	bool IsNull() const {
		return page == 0 && file == 0;
	}
	bool operator==(const PagePointer &o) const {
		return page == o.page && file == o.file;
	}
	std::string ToString() const;
};

//! The 6-byte pointer at `offset` of `reader`.
PagePointer ReadPagePointer(const ByteReader &reader, size_t offset);

struct PageHeader {
	uint8_t version = 0;
	uint8_t type = 0;
	uint8_t type_flags = 0;
	uint8_t level = 0;
	uint16_t flags = 0;
	uint16_t index_id = 0;	// m_indexId: the allocation unit's high part
	PagePointer prev;
	uint16_t fixed_length = 0;	// pminlen
	PagePointer next;
	uint16_t slot_count = 0;
	uint32_t object_id = 0;	 // m_objId: the allocation unit's low part
	uint16_t free_count = 0;
	uint16_t free_data = 0;
	PagePointer page_id;
	uint16_t ghost_count = 0;

	//! The allocation unit the header names: m_indexId << 48 | m_objId << 16.
	uint64_t AllocationUnit() const {
		return (static_cast<uint64_t>(index_id) << 48) | (static_cast<uint64_t>(object_id) << 16);
	}
};

//! The header of the 8 KB page at `page` (PAGE_SIZE bytes).
PageHeader ReadPageHeader(const uint8_t *page);

//! A live page's latest copy, with its header.
struct Page {
	std::vector<uint8_t> bytes;	 // PAGE_SIZE
	PageHeader header;
};

//! What the backup holds of a page.
enum class PageState : uint8_t {
	NotLive,  // the database does not use it (E2 R3)
	Present,  // live, its latest copy read and checked
	//! Live, and its latest copy is a zero unit in a backup with a later pass (one that ran under
	//! changes; in a one-pass backup such a copy is refused, not Pending): the page became live while the backup ran,
	//! and its content is in the backup's log tail only (RESTORE writes the whole frame and redoes it from the log).
	//! Measured on the load fixture's full, a page of its pass 2; BusinessCentral.DbReader's PROVENANCE ("MQDA region
	//! 2") alike. Readable once spec 090 replays the tail.
	Pending,
};

class PageStore {
public:
	//! Pages located by `index`, alive as `allocation` says; both are kept by reference.
	PageStore(const PageIndex &index, const AllocationMap &allocation);
	PageStore(PageIndex &&, const AllocationMap &) = delete;
	PageStore(const PageIndex &, AllocationMap &&) = delete;

	//! Page `page` of file `file` when it is live (E2 R3; an allocation page always is): its latest
	//! copy, its header read and checked -- version 1, not a pad, and the page's own id. False
	//! when the page is not live. A live page whose copy the backup lacks, a Pending one (a zero unit in
	//! its place), or a header naming another page is a FormatError naming the page.
	bool Read(uint16_t file, uint32_t page, Page &out) const;
	//! Read, with a Pending page (live, its copy a zero unit) answered as such instead of refused.
	PageState Fetch(uint16_t file, uint32_t page, Page &out) const;
	//! Read, with a page that is not live a FormatError too, naming `what` (the reader that needed it).
	void ReadLive(uint16_t file, uint32_t page, Page &out, const char *what) const;

private:
	//! A version-0 copy of live page `want` that is Pending: the backup has a pass after the first,
	//! and the copy is all zeros or names `want`.
	bool IsPendingCopy(const PagePointer &want, const Page &copy) const;

	const PageIndex &index_;
	const AllocationMap &allocation_;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

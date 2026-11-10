//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_allocation.hpp
//
// Spec 085 task 8, D4 / D4a: which pages of a backup are live (E2 R3). A page's rows are read, and
// its header allocation-unit id believed, only if the page is live: the allocation pages
// themselves (the file header 0, PFS, GAM / SGAM / DCM / BCM, file 1's boot page 9) always; any other
// page iff it is below its file's Size, its extent's GAM bit is 0 (allocated) and its PFS byte has
// 0x40. Full backups carry dead pages too (E2 §1.3), which is why this gate exists.
//
// The map reads the latest copy of each allocation page through the page index (PageIndex::ReadPage,
// which may search a pass, DQ-41). It reads each file's header (Size) when it is built; a GAM
// interval's bits (one per extent: about 21 MB for 10 TB in all) are read when a page of that
// interval is first asked about, and kept, so an open reads no GAM it does not need (DQ-41: a
// compressed backup is walked to what it reads); PFS pages are read on demand (one covers 8,088
// pages) and the last few kept. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_mtf.hpp"
#include "backup/backup_page_index.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

class AllocationMap {
public:
	//! The map of data files `files` (their ids: the backup set's data files), read from the latest
	//! copies `index` locates. A file whose header page the backup lacks is refused by name here,
	//! one whose GAM page for an interval it lacks when that interval is first asked about. A
	//! differential alone lacks the allocation pages that did not change: its map is to be built
	//! over its full (spec 085 D12, the overlay of task 24); alone it fails on the first allocation
	//! page it lacks, named. The map keeps `index` (and so its streams) by reference: they must
	//! outlive it.
	AllocationMap(const PageIndex &index, const std::vector<uint16_t> &files);
	AllocationMap(PageIndex &&, const std::vector<uint16_t> &) = delete;

	//! The file's Size in pages, or 0 when the file is not one of the map's.
	uint32_t FileSize(uint16_t file) const;
	//! True for an allocation page of file `file` (always live). The boot page is file 1's only.
	static bool IsAllocationPage(uint16_t file, uint32_t page);
	//! The liveness rule (E2 R3) for page `page` of file `file`.
	bool IsLive(uint16_t file, uint32_t page) const;
	//! The GAM's answer alone: the page's extent is allocated (and below Size).
	bool ExtentAllocated(uint16_t file, uint32_t page) const;

	size_t MemoryBytes() const;

private:
	struct File {
		uint16_t id = 0;
		uint32_t size = 0;
		//! Per GAM interval, its extents' bits (1 = free, as the GAM page holds them), read when the
		//! interval is first asked about; empty until then.
		std::vector<std::vector<uint8_t>> gam_free;
		std::vector<bool> gam_loaded;
	};
	struct PfsPage {
		uint16_t file = 0;
		uint32_t page = 0;
		std::vector<uint8_t> bytes;
	};

	const File *Find(uint16_t file) const;
	uint8_t PfsByte(const File &file, uint32_t page) const;
	//! Whether extent `extent` of `file` is GAM-free: its interval's GAM page read first if needed.
	bool GamFree(const File &file, uint32_t extent) const;
	//! The allocation page (file, page), of m_type `type`; FormatError naming `what` otherwise.
	std::vector<uint8_t> ReadPage(uint16_t file, uint32_t page, uint8_t type, const char *what) const;

	const PageIndex &index_;
	mutable std::vector<File> files_;  // the GAM intervals are filled in on demand, under lock_
	mutable std::mutex lock_;
	mutable std::vector<PfsPage> pfs_cache_;  // the last few PFS pages read
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

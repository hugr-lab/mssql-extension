#include "backup/backup_allocation.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

// Spec 085 § Rules, allocation (E2 layouts): PFS at page 1 and every 8,088 pages, its byte for page
// p at 100 + (p mod 8088), 0x40 = allocated; GAM / SGAM / DCM / BCM at +2 / +3 / +6 / +7 of every
// 511,232-page interval, a 7,988-byte bitmap at 194 (bit i, LSB first, = extent i; 1 = free); the
// file header at page 0, its slot 0 record's u32 Size at +171; the boot page at 1:9 (in another
// file page 9 is an ordinary page).
static constexpr uint32_t PFS_INTERVAL = 8088;
static constexpr size_t PFS_BYTES_AT = 100;
static constexpr uint8_t PFS_ALLOCATED = 0x40;
static constexpr uint32_t GAM_INTERVAL_PAGES = 511232;
static constexpr uint32_t GAM_INTERVAL_EXTENTS = GAM_INTERVAL_PAGES / PAGES_PER_EXTENT;
static constexpr size_t GAM_BITMAP_AT = 194;
static constexpr size_t GAM_BITMAP_BYTES = 7988;
static constexpr size_t FILE_HEADER_ID_AT = 167;
static constexpr size_t FILE_HEADER_SIZE_AT = 171;
static constexpr uint16_t BOOT_FILE = 1;
static constexpr uint32_t BOOT_PAGE = 9;
static constexpr size_t PFS_CACHE_PAGES = 64;

static std::string PageName(uint16_t file, uint32_t page) {
	return std::to_string(file) + ":" + std::to_string(page);
}

std::vector<uint8_t> AllocationMap::ReadPage(uint16_t file, uint32_t page, const char *what) const {
	std::vector<uint8_t> bytes;
	// Every allocation page a map reads is one a full's pass 1 carries (E12): `expected`, so a
	// searched pass cannot answer it absent on an order it does not have.
	if (!index_.ReadPage(file, page, bytes, true)) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) +
						  ") is in no data pass of the backup");
	}
	ByteReader r(bytes.data(), bytes.size(), what, 0);
	if (r.U8(0) != 1 || r.U32(32) != page || r.U16(36) != file) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) + ") is not that page");
	}
	return bytes;
}

AllocationMap::AllocationMap(const PageIndex &index, const std::vector<uint16_t> &files) : index_(index) {
	std::vector<uint16_t> ids = files;
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (uint16_t id : ids) {
		File f;
		f.id = id;
		auto header = ReadPage(id, 0, "the file header page");
		ByteReader h(header.data(), header.size(), "the file header page", 0);
		const uint16_t slot0 = h.U16(PAGE_SIZE - 2);
		if (slot0 < 96 || slot0 + FILE_HEADER_SIZE_AT + 4 > PAGE_SIZE || h.U16(slot0 + FILE_HEADER_ID_AT) != id) {
			h.Fail(PAGE_SIZE - 2, "file " + std::to_string(id) + "'s header has no record naming it");
		}
		f.size = h.U32(slot0 + FILE_HEADER_SIZE_AT);
		// One slot per GAM interval (at most 8,402 for 2^32 pages); the bits are read on demand, so a
		// crafted Size allocates no bitmap for intervals nobody asks about.
		const size_t intervals =
			static_cast<size_t>((static_cast<uint64_t>(f.size) + GAM_INTERVAL_PAGES - 1) / GAM_INTERVAL_PAGES);
		f.gam_free.resize(intervals);
		f.gam_loaded.assign(intervals, false);
		files_.push_back(std::move(f));
	}
}

bool AllocationMap::GamFree(const File &file, uint32_t extent) const {
	const size_t interval = extent / GAM_INTERVAL_EXTENTS;
	const uint32_t within = extent % GAM_INTERVAL_EXTENTS;
	std::lock_guard<std::mutex> guard(lock_);
	auto &f = const_cast<File &>(file);
	if (!f.gam_loaded[interval]) {
		const uint32_t gam_page = static_cast<uint32_t>(interval * GAM_INTERVAL_PAGES + 2);
		auto gam = ReadPage(f.id, gam_page, "a GAM page");
		ByteReader g(gam.data(), gam.size(), "a GAM page", 0);
		const uint8_t *bits = g.Bytes(GAM_BITMAP_AT, GAM_BITMAP_BYTES);
		f.gam_free[interval].assign(bits, bits + GAM_BITMAP_BYTES);
		f.gam_loaded[interval] = true;
	}
	return (f.gam_free[interval][within / 8] >> (within % 8)) & 1;
}

const AllocationMap::File *AllocationMap::Find(uint16_t file) const {
	for (const auto &f : files_) {
		if (f.id == file) {
			return &f;
		}
	}
	return nullptr;
}

uint32_t AllocationMap::FileSize(uint16_t file) const {
	auto f = Find(file);
	return f ? f->size : 0;
}

bool AllocationMap::IsAllocationPage(uint16_t file, uint32_t page) {
	if (page == 0 || page == 1 || (file == BOOT_FILE && page == BOOT_PAGE) || page % PFS_INTERVAL == 0) {
		return true;
	}
	const uint32_t in_interval = page % GAM_INTERVAL_PAGES;
	return in_interval == 2 || in_interval == 3 || in_interval == 6 || in_interval == 7;
}

bool AllocationMap::ExtentAllocated(uint16_t file, uint32_t page) const {
	auto f = Find(file);
	if (!f || page >= f->size) {
		return false;
	}
	return !GamFree(*f, page / PAGES_PER_EXTENT);
}

uint8_t AllocationMap::PfsByte(const File &file, uint32_t page) const {
	const uint32_t interval = page / PFS_INTERVAL;
	const uint32_t pfs_page = interval == 0 ? 1 : interval * PFS_INTERVAL;
	{
		std::lock_guard<std::mutex> guard(lock_);
		for (size_t i = 0; i < pfs_cache_.size(); i++) {
			if (pfs_cache_[i].file == file.id && pfs_cache_[i].page == pfs_page) {
				return pfs_cache_[i].bytes[PFS_BYTES_AT + page % PFS_INTERVAL];
			}
		}
	}
	// Read outside the lock, so one thread's miss does not stall the others' hits; two threads
	// missing the same page both read it, and the cache keeps both copies until they age out.
	PfsPage entry;
	entry.file = file.id;
	entry.page = pfs_page;
	entry.bytes = ReadPage(file.id, pfs_page, "a PFS page");
	const uint8_t value = entry.bytes[PFS_BYTES_AT + page % PFS_INTERVAL];
	std::lock_guard<std::mutex> guard(lock_);
	if (pfs_cache_.size() >= PFS_CACHE_PAGES) {
		pfs_cache_.erase(pfs_cache_.begin());
	}
	pfs_cache_.push_back(std::move(entry));
	return value;
}

bool AllocationMap::IsLive(uint16_t file, uint32_t page) const {
	auto f = Find(file);
	if (!f || page >= f->size) {
		return false;
	}
	if (IsAllocationPage(file, page)) {
		return true;
	}
	return ExtentAllocated(file, page) && (PfsByte(*f, page) & PFS_ALLOCATED) != 0;
}

size_t AllocationMap::MemoryBytes() const {
	std::lock_guard<std::mutex> guard(lock_);
	size_t bytes = files_.capacity() * sizeof(File);
	for (const auto &f : files_) {
		bytes += f.gam_free.capacity() * sizeof(std::vector<uint8_t>) + f.gam_loaded.capacity() / 8;
		for (const auto &interval : f.gam_free) {
			bytes += interval.capacity();
		}
	}
	return bytes + pfs_cache_.capacity() * (sizeof(PfsPage) + PAGE_SIZE);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

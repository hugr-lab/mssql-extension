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
	PageLocation at;
	if (!index_.Locate(file, page, at)) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) +
						  ") is in no data pass of the backup");
	}
	if (at.family >= streams_.size()) {
		throw FormatError("backup: page " + PageName(file, page) + " is in a family the map was not given");
	}
	std::vector<uint8_t> bytes(PAGE_SIZE);
	streams_[at.family]->ReadAt(at.offset, bytes.data(), bytes.size());
	ByteReader r(bytes.data(), bytes.size(), what, at.offset);
	if (r.U8(0) != 1 || r.U32(32) != page || r.U16(36) != file) {
		r.Fail(0, std::string(what) + " (page " + PageName(file, page) + ") is not that page");
	}
	return bytes;
}

AllocationMap::AllocationMap(const RandomAccess &stream, const PageIndex &index, const std::vector<uint16_t> &files)
	: streams_{&stream}, index_(index) {
	Load(files);
}

AllocationMap::AllocationMap(const std::vector<const RandomAccess *> &streams, const PageIndex &index,
							 const std::vector<uint16_t> &files)
	: streams_(streams), index_(index) {
	if (streams_.empty()) {
		throw FormatError("backup: an allocation map over no stream");
	}
	Load(files);
}

void AllocationMap::Load(const std::vector<uint16_t> &files) {
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
		const uint64_t extents = (static_cast<uint64_t>(f.size) + PAGES_PER_EXTENT - 1) / PAGES_PER_EXTENT;
		// Grown interval by interval as each GAM page is read: a crafted Size whose GAM pages the
		// backup lacks is refused before the bitmap is allocated for it.
		for (uint64_t base = 0; base < f.size; base += GAM_INTERVAL_PAGES) {
			auto gam = ReadPage(id, static_cast<uint32_t>(base + 2), "a GAM page");
			const uint64_t interval_end = std::min<uint64_t>(extents, base / PAGES_PER_EXTENT + GAM_INTERVAL_EXTENTS);
			f.gam_free.resize(static_cast<size_t>((interval_end + 7) / 8), 0xFF);
			ByteReader g(gam.data(), gam.size(), "a GAM page", 0);
			const uint8_t *bits = g.Bytes(GAM_BITMAP_AT, GAM_BITMAP_BYTES);
			const uint64_t first = base / PAGES_PER_EXTENT;
			for (uint64_t i = 0; i < GAM_INTERVAL_EXTENTS && first + i < extents; i++) {
				const uint64_t e = first + i;
				const bool free = (bits[i / 8] >> (i % 8)) & 1;
				if (!free) {
					f.gam_free[e / 8] &= static_cast<uint8_t>(~(1u << (e % 8)));
				}
			}
		}
		files_.push_back(std::move(f));
	}
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
	const uint32_t e = page / PAGES_PER_EXTENT;
	return !((f->gam_free[e / 8] >> (e % 8)) & 1);
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
	size_t bytes = files_.capacity() * sizeof(File);
	for (const auto &f : files_) {
		bytes += f.gam_free.capacity();
	}
	return bytes + pfs_cache_.capacity() * (sizeof(PfsPage) + PAGE_SIZE);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

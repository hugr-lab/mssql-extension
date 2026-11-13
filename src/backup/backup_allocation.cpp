#include "backup/backup_allocation.hpp"
#include "backup/backup_record.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <atomic>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

// Spec 085 § Rules, allocation (E2 layouts): PFS at page 1 and every 8,088 pages, its byte for page
// p at 100 + (p mod 8088), 0x40 = allocated; GAM / SGAM / DCM / BCM at pages 2 / 3 / 6 / 7 of the
// first 511,232-page interval and +0 / +1 / +6 / +7 of every later one (E17), a 7,988-byte bitmap
// at 194 (bit i, LSB first, = extent i; 1 = free); the file header at page 0, its slot 0 record's
// Size its variable column 4 (ReadFileHeaderRecord); the boot page at 1:9 (in another file page 9
// is an ordinary page).
static constexpr uint32_t PFS_INTERVAL = 8088;
static constexpr size_t PFS_BYTES_AT = 100;
static constexpr uint8_t PFS_ALLOCATED = 0x40;
static constexpr uint32_t GAM_INTERVAL_PAGES = 511232;
static constexpr uint32_t GAM_INTERVAL_EXTENTS = GAM_INTERVAL_PAGES / PAGES_PER_EXTENT;
static constexpr size_t GAM_BITMAP_AT = 194;
static constexpr size_t GAM_BITMAP_BYTES = 7988;
static constexpr uint16_t BOOT_FILE = 1;
static constexpr uint32_t BOOT_PAGE = 9;
static constexpr size_t PFS_CACHE_PAGES = 64;
static constexpr uint8_t PAGE_TYPE_GAM = 8;
static constexpr uint8_t PAGE_TYPE_PFS = 11;
static constexpr uint8_t PAGE_TYPE_FILE_HEADER = 15;

static std::string PageName(uint16_t file, uint32_t page) {
	return std::to_string(file) + ":" + std::to_string(page);
}

//! `bytes`, the copy of allocation page (file, page), checked: a page, that page, of m_type `type`.
static void CheckAllocationPage(const std::vector<uint8_t> &bytes, uint16_t file, uint32_t page, uint8_t type,
								const char *what) {
	ByteReader r(bytes.data(), bytes.size(), what, 0);
	if (r.U8(0) != 1 || r.U32(32) != page || r.U16(36) != file) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) + ") is not that page");
	}
	// The page is where the format puts this allocation page: if it is of another type, the format's
	// arithmetic does not hold here (as a later interval's +2 / +3 did not, E17).
	if (r.U8(1) != type) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) + ") is a page of type " +
						  std::to_string(r.U8(1)) + ", not " + std::to_string(type));
	}
}

std::vector<uint8_t> AllocationMap::ReadPage(uint16_t file, uint32_t page, uint8_t type, const char *what) const {
	std::vector<uint8_t> bytes;
	// Every allocation page a map reads is one a full's pass 1 carries (E12): `expected`, so a
	// searched pass cannot answer it absent on an order it does not have.
	if (!index_.ReadPage(file, page, bytes, true)) {
		throw FormatError("backup: " + std::string(what) + " (page " + PageName(file, page) +
						  ") is in no data pass of the backup");
	}
	CheckAllocationPage(bytes, file, page, type, what);
	return bytes;
}

namespace {

uint64_t NextMapId() {
	static std::atomic<uint64_t> next{1};
	return next.fetch_add(1);
}

//! A thread's last answers of one map (by its id), read with no lock: a scan's off-row pages are asked
//! about one by one, each a GAM and a PFS lookup, and two locks a page held 8 threads on the map's mutex
//! for a third of their time (PR 3 S7, 200,000 rows of 10 KB off row). A GAM interval's bits never
//! change once loaded (their buffer stays put when the intervals' vector grows); a PFS page is shared
//! with the cache, so it outlives its eviction while the thread keeps it (one page a thread, unaccounted
//! in MemoryBytes).
struct LastGam {
	uint64_t map = 0;
	uint16_t file = 0;
	size_t interval = 0;
	const uint8_t *bits = nullptr;	// null: all free
};
struct LastPfs {
	uint64_t map = 0;
	uint16_t file = 0;
	uint32_t page = 0;
	std::shared_ptr<const std::vector<uint8_t>> bytes;
};
thread_local LastGam last_gam;
thread_local LastPfs last_pfs;

}  // namespace

AllocationMap::AllocationMap(const PageIndex &index, const std::vector<uint16_t> &files)
	: index_(index), id_(NextMapId()) {
	std::vector<uint16_t> ids = files;
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	for (uint16_t id : ids) {
		File f;
		f.id = id;
		auto header = ReadPage(id, 0, PAGE_TYPE_FILE_HEADER, "the file header page");
		const auto record = ReadFileHeaderRecord(header.data(), header.size());
		if (record.file_id != id) {
			throw FormatError("backup: file " + std::to_string(id) + "'s header record names file " +
							  std::to_string(record.file_id));
		}
		f.size = record.size;
		// One slot per GAM interval (at most 8,402 for 2^32 pages); the bits are read on demand, so a
		// crafted Size allocates no bitmap for intervals nobody asks about, and an interval asked about
		// that the backup carries no GAM for keeps none (an empty slot: all free; review of #549).
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
	if (last_gam.map == id_ && last_gam.file == file.id && last_gam.interval == interval) {
		return !last_gam.bits || ((last_gam.bits[within / 8] >> (within % 8)) & 1);
	}
	auto &f = const_cast<File &>(file);
	auto remember = [&]() {
		// Under the lock: the interval is loaded, and its bits are published to this thread.
		last_gam.map = id_;
		last_gam.file = file.id;
		last_gam.interval = interval;
		last_gam.bits = f.gam_free[interval].empty() ? nullptr : f.gam_free[interval].data();
	};
	{
		std::lock_guard<std::mutex> guard(lock_);
		// An interval past the recorded Size: the Size is a lower bound (a stale header after an
		// autogrow; DbReader's PROVENANCE, "File size is a lower bound"), so a page there is asked of
		// its interval's GAM like any other.
		if (interval >= f.gam_free.size()) {
			f.gam_free.resize(interval + 1);
			f.gam_loaded.resize(interval + 1, false);
		}
		if (f.gam_loaded[interval]) {
			remember();
			// An interval the backup carries no GAM for is cached as an empty bitmap: all free.
			return f.gam_free[interval].empty() || ((f.gam_free[interval][within / 8] >> (within % 8)) & 1);
		}
	}
	// Read outside the lock, as PfsByte does: the read may search a pass or map one by its headers
	// (PageIndex::ReadPage, expected), and the other threads' hits must not wait for it. Two threads
	// missing one interval both read it; the first to finish keeps its bits.
	// Page 2 of the first interval, the first page of every later one (E17).
	const uint32_t gam_page = interval == 0 ? 2 : static_cast<uint32_t>(interval * GAM_INTERVAL_PAGES);
	std::vector<uint8_t> bits;	// empty: no GAM, all free -- nothing kept for an interval past the data
	const bool recorded = static_cast<uint64_t>(interval) * GAM_INTERVAL_PAGES < f.size;
	std::vector<uint8_t> gam;
	// An interval within the recorded Size has its GAM in the backup (E12); one past it has one only
	// when the data reached it -- if the backup carries none, nothing there is allocated. Asked with
	// `expected` either way: an absence a searched pass answers on an order it did not check would
	// mark a whole interval free, and its pages not live.
	if (recorded) {
		gam = ReadPage(f.id, gam_page, PAGE_TYPE_GAM, "a GAM page");
	} else if (index_.ReadPage(f.id, gam_page, gam, true)) {
		CheckAllocationPage(gam, f.id, gam_page, PAGE_TYPE_GAM, "a GAM page");
	} else {
		gam.clear();
	}
	if (!gam.empty()) {
		ByteReader g(gam.data(), gam.size(), "a GAM page", 0);
		const uint8_t *b = g.Bytes(GAM_BITMAP_AT, GAM_BITMAP_BYTES);
		bits.assign(b, b + GAM_BITMAP_BYTES);
	}
	std::lock_guard<std::mutex> guard(lock_);
	if (!f.gam_loaded[interval]) {
		f.gam_free[interval] = std::move(bits);
		f.gam_loaded[interval] = true;
	}
	remember();
	return f.gam_free[interval].empty() || ((f.gam_free[interval][within / 8] >> (within % 8)) & 1);
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
	// GAM / SGAM / DCM / BCM: pages 2 / 3 / 6 / 7 of the first interval (0 and 1 are the file header
	// and the PFS), then 0 / 1 / 6 / 7 of every later one (E17: 511232 GAM, 511233 SGAM, 511238 DCM,
	// 511239 BCM, on 2022). Not measured: GAM interval 1011 (page 516855552 = 8088 x 63904, a file past
	// 3.9 TB) starts on a PFS page's place too; which page SQL Server puts there is unknown, and the
	// reader asks it for both types (GamFree, PfsByte), so one of them is a FormatError naming it.
	const uint32_t in_interval = page % GAM_INTERVAL_PAGES;
	if (page < GAM_INTERVAL_PAGES) {
		return in_interval == 2 || in_interval == 3 || in_interval == 6 || in_interval == 7;
	}
	return in_interval == 0 || in_interval == 1 || in_interval == 6 || in_interval == 7;
}

bool AllocationMap::ExtentAllocated(uint16_t file, uint32_t page) const {
	auto f = Find(file);
	if (!f) {
		return false;
	}
	return !GamFree(*f, page / PAGES_PER_EXTENT);
}

uint8_t AllocationMap::PfsByte(const File &file, uint32_t page) const {
	const uint32_t interval = page / PFS_INTERVAL;
	const uint32_t pfs_page = interval == 0 ? 1 : interval * PFS_INTERVAL;
	if (last_pfs.map == id_ && last_pfs.file == file.id && last_pfs.page == pfs_page) {
		return (*last_pfs.bytes)[PFS_BYTES_AT + page % PFS_INTERVAL];
	}
	{
		std::lock_guard<std::mutex> guard(lock_);
		for (size_t i = 0; i < pfs_cache_.size(); i++) {
			if (pfs_cache_[i].file == file.id && pfs_cache_[i].page == pfs_page) {
				last_pfs.map = id_;
				last_pfs.file = file.id;
				last_pfs.page = pfs_page;
				last_pfs.bytes = pfs_cache_[i].bytes;
				return (*pfs_cache_[i].bytes)[PFS_BYTES_AT + page % PFS_INTERVAL];
			}
		}
	}
	// Read outside the lock, so one thread's miss does not stall the others' hits; two threads
	// missing the same page both read it, and the cache keeps both copies until they age out.
	PfsPage entry;
	entry.file = file.id;
	entry.page = pfs_page;
	entry.bytes =
		std::make_shared<const std::vector<uint8_t>>(ReadPage(file.id, pfs_page, PAGE_TYPE_PFS, "a PFS page"));
	const uint8_t value = (*entry.bytes)[PFS_BYTES_AT + page % PFS_INTERVAL];
	last_pfs.map = id_;
	last_pfs.file = file.id;
	last_pfs.page = pfs_page;
	last_pfs.bytes = entry.bytes;
	std::lock_guard<std::mutex> guard(lock_);
	if (pfs_cache_.size() >= PFS_CACHE_PAGES) {
		pfs_cache_.erase(pfs_cache_.begin());
	}
	pfs_cache_.push_back(std::move(entry));
	return value;
}

bool AllocationMap::IsLive(uint16_t file, uint32_t page) const {
	auto f = Find(file);
	if (!f) {
		return false;
	}
	if (IsAllocationPage(file, page)) {
		// Within the recorded Size always; past it (the Size a lower bound) only where the data
		// reached, which the backup's copy of the page says.
		if (page < f->size) {
			return true;
		}
		PageLocation location;
		return index_.Locate(file, page, location, true);
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

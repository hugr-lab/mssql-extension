#include "backup/backup_page_index.hpp"

#include "backup/backup_bytes.hpp"

#include <algorithm>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint64_t GROUP_BYTES = static_cast<uint64_t>(PAGE_SIZE) * PAGES_PER_EXTENT;
static constexpr uint8_t PAGE_HEADER_VERSION = 1;
static constexpr uint8_t PAGE_TYPE_PAD = 101;
// The fields of a page header the map reads: version @0, m_type @1, m_pageId @32, m_fileId @36.
static constexpr size_t HEADER_READ = 40;
// Bits per directory entry: one count per 8 words (512 extents).
static constexpr size_t DIRECTORY_WORDS = 8;

static int PopCount(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_popcountll(x);
#else
	int n = 0;
	while (x) {
		x &= x - 1;
		n++;
	}
	return n;
#endif
}

void ExtentBitmap::Add(uint32_t extent) {
	if (sealed_) {
		throw FormatError("backup page index: an extent added to a sealed bitmap");
	}
	const size_t word = extent / 64;
	if (word >= words_.size()) {
		words_.resize(word + 1, 0);
	}
	const uint64_t bit = static_cast<uint64_t>(1) << (extent % 64);
	if (!(words_[word] & bit)) {
		words_[word] |= bit;
		count_++;
	}
}

bool ExtentBitmap::Contains(uint32_t extent) const {
	const size_t word = extent / 64;
	return word < words_.size() && (words_[word] >> (extent % 64)) & 1;
}

void ExtentBitmap::Seal() {
	sealed_ = true;
	words_.shrink_to_fit();
	directory_.assign((words_.size() + DIRECTORY_WORDS - 1) / DIRECTORY_WORDS + 1, 0);
	uint64_t total = 0;
	for (size_t w = 0; w < words_.size(); w++) {
		if (w % DIRECTORY_WORDS == 0) {
			directory_[w / DIRECTORY_WORDS] = total;
		}
		total += PopCount(words_[w]);
	}
	directory_.back() = total;
}

uint64_t ExtentBitmap::Rank(uint32_t extent) const {
	const size_t word = extent / 64;
	if (word >= words_.size()) {
		return count_;
	}
	const size_t block = word / DIRECTORY_WORDS;
	uint64_t rank = directory_[block];
	for (size_t w = block * DIRECTORY_WORDS; w < word; w++) {
		rank += PopCount(words_[w]);
	}
	const uint32_t within = extent % 64;
	if (within) {
		rank += PopCount(words_[word] & ((static_cast<uint64_t>(1) << within) - 1));
	}
	return rank;
}

bool ExtentBitmap::Select(uint64_t rank, uint32_t &extent) const {
	if (!sealed_ || rank >= count_) {
		return false;
	}
	// The last directory block whose count is <= rank, then word by word, then bit by bit.
	size_t lo = 0;
	size_t hi = directory_.size() - 1;	// the last entry is the total
	while (hi - lo > 1) {
		const size_t mid = lo + (hi - lo) / 2;
		if (directory_[mid] <= rank) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	uint64_t seen = directory_[lo];
	for (size_t w = lo * DIRECTORY_WORDS; w < words_.size(); w++) {
		const uint64_t bits = words_[w];
		const uint64_t in_word = static_cast<uint64_t>(PopCount(bits));
		if (seen + in_word <= rank) {
			seen += in_word;
			continue;
		}
		uint64_t rest = bits;
		for (unsigned b = 0; b < 64; b++) {
			if ((rest >> b) & 1) {
				if (seen == rank) {
					extent = static_cast<uint32_t>(w * 64 + b);
					return true;
				}
				seen++;
			}
		}
	}
	return false;
}

bool PassMap::Find(const ExtentId &extent, uint64_t &group) const {
	if (ranked_) {
		auto at = std::lower_bound(files_.begin(), files_.end(), extent.file);
		if (at == files_.end() || *at != extent.file) {
			return false;
		}
		const size_t f = static_cast<size_t>(at - files_.begin());
		if (!bitmaps_[f].Contains(extent.extent)) {
			return false;
		}
		group = file_first_group_[f] + bitmaps_[f].Rank(extent.extent);
		return true;
	}
	// The last run starting at or before the extent.
	auto it = std::upper_bound(runs_.begin(), runs_.end(), extent,
							   [](const ExtentId &e, const ExtentRun &r) { return e < r.first; });
	if (it == runs_.begin()) {
		return false;
	}
	--it;
	if (it->first.file != extent.file || extent.extent - it->first.extent >= it->count) {
		return false;
	}
	group = it->first_group + (extent.extent - it->first.extent);
	return true;
}

size_t PassMap::MemoryBytes() const {
	size_t bytes = runs_.capacity() * sizeof(ExtentRun) + files_.capacity() * sizeof(uint16_t) +
				   file_first_group_.capacity() * sizeof(uint64_t);
	for (const auto &b : bitmaps_) {
		bytes += b.MemoryBytes();
	}
	return bytes;
}

static std::string WhereName(const std::string &name, uint64_t offset) {
	return "backup file \"" + name + "\" at offset " + std::to_string(offset);
}

static std::string Where(const RandomAccess &stream, uint64_t offset) {
	return WhereName(stream.Name(), offset);
}

// The ranked form's bitmaps may hold at most this many bytes, or 8 runs' worth per extent run,
// whichever is more: a bitmap sized by its highest extent must not outgrow the runs it replaces
// (a sparse pass, or a crafted one naming a new file with a huge extent in every group).
static constexpr size_t RANKED_FLOOR_BYTES = 1 << 20;
static constexpr size_t RANKED_RUN_FACTOR = 8;

//! Builds a pass's map group by group: ranked while the order and the budget allow, runs from
//! the first group that breaks either (the bitmaps so far converted, in group order).
class PassMapBuilder {
public:
	explicit PassMapBuilder(PassMap &map) : map_(map) {}

	void Empty() {
		empty_seen_ = true;
	}

	void Extent(const ExtentId &extent, uint64_t group) {
		if (ranked_) {
			const bool in_order = !have_previous_ || previous_ < extent;
			if (!in_order || empty_seen_ || !FitsBudget(extent)) {
				ToRuns();
			}
		}
		if (ranked_) {
			if (files_.empty() || files_.back() != extent.file) {
				files_.push_back(extent.file);
				bitmaps_.emplace_back();
				first_group_.push_back(group);
				bitmap_bytes_ += ExtentBitmap::BytesFor(0);
			}
			const size_t before = bitmaps_.back().Words();
			bitmaps_.back().Add(extent.extent);
			bitmap_bytes_ += (bitmaps_.back().Words() - before) * sizeof(uint64_t) * 9 / 8;
			if (!have_previous_ || previous_.file != extent.file || previous_.extent + 1 != extent.extent) {
				run_count_++;
			}
		} else {
			AppendRun(extent, group);
		}
		previous_ = extent;
		have_previous_ = true;
	}

	void Finish(const std::string &name, uint64_t offset) {
		if (ranked_) {
			map_.ranked_ = true;
			map_.files_ = std::move(files_);
			map_.file_first_group_ = std::move(first_group_);
			map_.bitmaps_ = std::move(bitmaps_);
			for (auto &b : map_.bitmaps_) {
				b.Seal();
			}
			return;
		}
		std::sort(runs_.begin(), runs_.end(), [](const ExtentRun &a, const ExtentRun &b) { return a.first < b.first; });
		for (size_t i = 1; i < runs_.size(); i++) {
			const auto &a = runs_[i - 1];
			const auto &b = runs_[i];
			if (a.first.file == b.first.file && a.first.extent + a.count > b.first.extent) {
				throw FormatError("backup file \"" + name + "\" at offset " + std::to_string(offset) +
								  ": a data pass that carries extent " + std::to_string(b.first.file) + ":" +
								  std::to_string(b.first.extent) + " twice");
			}
		}
		map_.runs_ = std::move(runs_);
	}

private:
	bool FitsBudget(const ExtentId &extent) const {
		size_t grow = ExtentBitmap::BytesFor(extent.extent);
		if (!files_.empty() && files_.back() == extent.file) {
			const size_t have = bitmaps_.back().Words() * sizeof(uint64_t) * 9 / 8;
			grow = grow > have ? grow - have : 0;
		}
		const size_t budget = std::max(RANKED_FLOOR_BYTES, RANKED_RUN_FACTOR * (run_count_ + 1) * sizeof(ExtentRun));
		return bitmap_bytes_ + grow <= budget;
	}

	//! The ranked groups so far become runs: each file's extents in order, from its first group.
	void ToRuns() {
		ranked_ = false;
		for (size_t f = 0; f < files_.size(); f++) {
			uint64_t group = first_group_[f];
			ExtentId e;
			e.file = files_[f];
			bitmaps_[f].ForEach([&](uint32_t x) {
				e.extent = x;
				AppendRun(e, group++);
			});
		}
		files_.clear();
		bitmaps_.clear();
		first_group_.clear();
	}

	void AppendRun(const ExtentId &extent, uint64_t group) {
		if (!runs_.empty() && runs_.back().first.file == extent.file &&
			runs_.back().first.extent + runs_.back().count == extent.extent &&
			runs_.back().first_group + runs_.back().count == group) {
			runs_.back().count++;
			return;
		}
		ExtentRun run;
		run.first = extent;
		run.count = 1;
		run.first_group = group;
		runs_.push_back(run);
	}

	PassMap &map_;
	bool ranked_ = true;
	bool empty_seen_ = false;
	bool have_previous_ = false;
	ExtentId previous_;
	std::vector<uint16_t> files_;
	std::vector<ExtentBitmap> bitmaps_;
	std::vector<uint64_t> first_group_;
	size_t bitmap_bytes_ = 0;
	size_t run_count_ = 0;
	std::vector<ExtentRun> runs_;
};

// The extent a group's units name, checked unit by unit: a page sits at its own position of its
// extent's group, every page of a group names one extent, a unit is a page (header version 1), a
// pad page, or a zero unit.
static void ScanGroup(const std::string &name, uint64_t group_at, const uint8_t *group_bytes, GroupHeader &out) {
	out.found = false;
	for (uint32_t k = 0; k < PAGES_PER_EXTENT; k++) {
		const uint8_t *unit = group_bytes + static_cast<size_t>(k) * PAGE_SIZE;
		const uint64_t at = group_at + static_cast<uint64_t>(k) * PAGE_SIZE;
		if (unit[0] == 0) {
			// A zero unit (an unallocated page of a copied extent) is all zeros: 13449 of 13451 seen.
			// The other two (a busy backup's second pass, E6 F2) carry a page id and little else: a
			// page caught while it was formatted. That unit is the page's latest copy, an
			// unformatted page the record reader returns no row from; it names its extent like a page.
			bool zeros = true;
			for (size_t i = 1; i < PAGE_SIZE && zeros; i++) {
				zeros = unit[i] == 0;
			}
			if (zeros) {
				continue;
			}
		} else if (unit[0] != PAGE_HEADER_VERSION) {
			throw FormatError(WhereName(name, at) + ": a unit with header version " + std::to_string(unit[0]) +
							  ", neither a page (1) nor a zero unit (0)");
		}
		if (unit[0] == PAGE_HEADER_VERSION && unit[1] == PAGE_TYPE_PAD) {
			continue;
		}
		ByteReader r(unit, HEADER_READ, "a page header", at);
		const uint32_t page = r.U32(32);
		const uint16_t file = r.U16(36);
		if (page % PAGES_PER_EXTENT != k) {
			throw FormatError(WhereName(name, at) + ": page " + std::to_string(file) + ":" + std::to_string(page) +
							  " at position " + std::to_string(k) + " of its extent group");
		}
		ExtentId e;
		e.file = file;
		e.extent = page / PAGES_PER_EXTENT;
		if (out.found && !(e == out.extent)) {
			throw FormatError(WhereName(name, at) + ": an extent group holding pages of two extents");
		}
		out.extent = e;
		out.found = true;
	}
}

PassHeaderScan::PassHeaderScan(std::string name, uint64_t stream_size, uint64_t offset, uint64_t length,
							   uint32_t number)
	: name_(std::move(name)), offset_(offset) {
	if (length < 2 || offset > stream_size || length > stream_size - offset) {
		throw FormatError(WhereName(name_, offset) + ": a data stream that does not fit the file");
	}
	if ((length - 2) % PAGE_SIZE != 0) {
		throw FormatError(WhereName(name_, offset) + ": a data stream of " + std::to_string(length) +
						  " bytes, not a 2-byte lead-in and whole 8 KB units");
	}
	map_.number = number;
	map_.units_offset = offset + 2;
	const uint64_t units = (length - 2) / PAGE_SIZE;
	if (units % PAGES_PER_EXTENT != 0) {
		throw FormatError(WhereName(name_, offset) + ": a data pass of " + std::to_string(units) +
						  " units, not whole 8-unit extent groups");
	}
	map_.groups_ = units / PAGES_PER_EXTENT;
	builder_.reset(new PassMapBuilder(map_));
}

PassHeaderScan::~PassHeaderScan() {}

uint64_t PassHeaderScan::GroupOffset(uint64_t group) const {
	return map_.units_offset + group * GROUP_BYTES;
}

void PassHeaderScan::ScanGroups(const std::string &name, uint64_t at, const uint8_t *bytes, uint64_t groups,
								std::vector<GroupHeader> &out) {
	out.resize(static_cast<size_t>(groups));
	for (uint64_t g = 0; g < groups; g++) {
		ScanGroup(name, at + g * GROUP_BYTES, bytes + g * GROUP_BYTES, out[static_cast<size_t>(g)]);
	}
}

void PassHeaderScan::Add(uint64_t first_group, const std::vector<GroupHeader> &groups) {
	if (!builder_) {
		throw FormatError(WhereName(name_, offset_) + ": a pass scan fed after it finished");
	}
	if (first_group != next_group_) {
		throw FormatError(WhereName(name_, offset_) + ": groups fed out of order (" + std::to_string(first_group) +
						  " after " + std::to_string(next_group_) + ")");
	}
	if (groups.size() > map_.groups_ - next_group_) {
		throw FormatError(WhereName(name_, offset_) + ": " + std::to_string(groups.size()) +
						  " groups fed past the pass's " + std::to_string(map_.groups_));
	}
	for (size_t i = 0; i < groups.size(); i++) {
		if (!groups[i].found) {
			builder_->Empty();
			continue;
		}
		builder_->Extent(groups[i].extent, next_group_ + i);
		map_.extents_++;
	}
	next_group_ += groups.size();
}

void PassHeaderScan::Feed(uint64_t first_group, const uint8_t *bytes, uint64_t groups) {
	if (first_group != next_group_) {
		throw FormatError(WhereName(name_, offset_) + ": groups fed out of order (" + std::to_string(first_group) +
						  " after " + std::to_string(next_group_) + ")");
	}
	if (groups > map_.groups_ - next_group_) {
		throw FormatError(WhereName(name_, offset_) + ": " + std::to_string(groups) + " groups fed past the pass's " +
						  std::to_string(map_.groups_));
	}
	ScanGroups(name_, GroupOffset(first_group), bytes, groups, scratch_);
	Add(first_group, scratch_);
}

PassMap PassHeaderScan::Finish() {
	if (!builder_ || next_group_ != map_.groups_) {
		throw FormatError(WhereName(name_, offset_) + ": a pass scan finished at group " + std::to_string(next_group_) +
						  " of " + std::to_string(map_.groups_));
	}
	builder_->Finish(name_, offset_);
	builder_.reset();
	return std::move(map_);
}

PassMap BuildPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number) {
	PassHeaderScan scan(stream.Name(), stream.Size(), offset, length, number);
	// Reads of up to 16 groups (1 MiB), not one per header: the reader under it may be a container
	// whose every read decodes, or a remote file. The DuckDB layer feeds the same scan from
	// parallel ranges (D3a).
	static constexpr uint64_t GROUPS_PER_READ = 16;
	std::vector<uint8_t> bytes;
	for (uint64_t g = 0; g < scan.GroupCount(); g += GROUPS_PER_READ) {
		const uint64_t n = std::min(GROUPS_PER_READ, scan.GroupCount() - g);
		bytes.resize(static_cast<size_t>(n * GROUP_BYTES));
		stream.ReadAt(scan.GroupOffset(g), bytes.data(), bytes.size());
		scan.Feed(g, bytes.data(), n);
	}
	return scan.Finish();
}

// Allocation-page layout (spec 085 § Rules, allocation): PFS pages at 1 and every 8,088 pages;
// GAM / SGAM / DCM / BCM at +2 / +3 / +6 / +7 of every 511,232-page interval, whose GAM covers
// 63,904 extents with a 7,988-byte bitmap at page offset 194 (bit set = free); the boot page at 9.
static constexpr uint32_t PFS_INTERVAL = 8088;
static constexpr uint32_t GAM_INTERVAL_PAGES = 511232;
static constexpr uint32_t GAM_INTERVAL_EXTENTS = GAM_INTERVAL_PAGES / PAGES_PER_EXTENT;
static constexpr size_t GAM_BITMAP_AT = 194;
static constexpr size_t GAM_BITMAP_BYTES = 7988;
static constexpr uint8_t PAGE_TYPE_FILE_HEADER = 15;
static constexpr uint8_t PAGE_TYPE_GAM = 8;
// The file header page (page 0): its slot 0 record holds u16 file id +167, u32 Size (pages) +171.
static constexpr size_t FILE_HEADER_ID_AT = 167;
static constexpr size_t FILE_HEADER_SIZE_AT = 171;
// Groups checked beside the ones the rule needs, spread over each file; a checked group of zero
// units only (an allocated extent with no allocated page) names nothing, so the next one is read,
// at most this many times.
static constexpr uint32_t RANK_CHECKS_PER_FILE = 16;
static constexpr uint32_t RANK_CHECK_STEPS = 8;

namespace {

//! One group read from the pass: its 8 units, and the extent its pages name (if any).
struct ReadGroup {
	std::vector<uint8_t> bytes;
	bool has_extent = false;
	bool is_pad = true;	 // every unit a pad
	ExtentId extent;
};

//! Reads group `g` of a pass of `groups` groups: false when `g` is past the pass or its units are
//! not pages, pads and zero units at their positions.
bool ReadPassGroup(const RandomAccess &stream, uint64_t units_offset, uint64_t groups, uint64_t g, ReadGroup &out) {
	if (g >= groups) {
		return false;
	}
	out.bytes.resize(GROUP_BYTES);
	stream.ReadAt(units_offset + g * GROUP_BYTES, out.bytes.data(), GROUP_BYTES);
	out.has_extent = false;
	out.is_pad = true;
	for (uint32_t k = 0; k < PAGES_PER_EXTENT; k++) {
		const uint8_t *unit = out.bytes.data() + static_cast<size_t>(k) * PAGE_SIZE;
		if (unit[0] == PAGE_HEADER_VERSION && unit[1] == PAGE_TYPE_PAD) {
			continue;
		}
		out.is_pad = false;
		if (unit[0] == 0) {
			continue;  // a zero unit (BuildPassMap checks its bytes; this path checks the shape)
		}
		if (unit[0] != PAGE_HEADER_VERSION) {
			return false;
		}
		ByteReader r(unit, HEADER_READ, "a page header", units_offset + g * GROUP_BYTES + k * PAGE_SIZE);
		const uint32_t page = r.U32(32);
		ExtentId e;
		e.file = r.U16(36);
		e.extent = page / PAGES_PER_EXTENT;
		if (page % PAGES_PER_EXTENT != k || (out.has_extent && !(e == out.extent))) {
			return false;
		}
		out.extent = e;
		out.has_extent = true;
	}
	return true;
}

//! Page `page_in_extent` of a read group when it is a page of type `type`, else null.
const uint8_t *GroupPage(const ReadGroup &g, uint32_t page_in_extent, uint8_t type) {
	const uint8_t *unit = g.bytes.data() + static_cast<size_t>(page_in_extent) * PAGE_SIZE;
	return unit[0] == PAGE_HEADER_VERSION && unit[1] == type ? unit : nullptr;
}

//! Adds interval `interval`'s extents of a file of `size` pages, in order: its allocation pages'
//! extents (the PFS pages in it, its own first extent, the boot page's in interval 0) and the
//! extents its GAM marks allocated. Every extent added is below the next interval's first.
void AddInterval(uint32_t size, uint32_t interval, const uint8_t *gam_bits, ExtentBitmap &bitmap) {
	const uint64_t first_page = static_cast<uint64_t>(interval) * GAM_INTERVAL_PAGES;
	const uint64_t end_page = std::min<uint64_t>(first_page + GAM_INTERVAL_PAGES, size);
	std::vector<uint32_t> extents;
	extents.push_back(static_cast<uint32_t>(first_page / PAGES_PER_EXTENT));  // pages 0..7: header, PFS, GAM, ...
	if (interval == 0 && size > 9) {
		extents.push_back(9 / PAGES_PER_EXTENT);  // the boot page's extent
	}
	// PFS pages: page 1, then every multiple of 8088.
	for (uint64_t p = (first_page / PFS_INTERVAL + 1) * PFS_INTERVAL; p < end_page; p += PFS_INTERVAL) {
		extents.push_back(static_cast<uint32_t>(p / PAGES_PER_EXTENT));
	}
	const uint32_t first_extent = interval * GAM_INTERVAL_EXTENTS;
	for (uint32_t i = 0; i < GAM_INTERVAL_EXTENTS; i++) {
		// An extent that starts at or past Size is not copied (E12).
		if (static_cast<uint64_t>(first_extent + i) * PAGES_PER_EXTENT >= size) {
			break;
		}
		if (!((gam_bits[i / 8] >> (i % 8)) & 1)) {
			extents.push_back(first_extent + i);
		}
	}
	std::sort(extents.begin(), extents.end());
	for (uint32_t e : extents) {
		bitmap.Add(e);
	}
}

}  // namespace

//! The rank path's body (a friend of PassMap): false when the rule does not hold.
class RankedPassBuilder {
public:
	static bool Build(const RandomAccess &stream, PassMap &result, uint64_t &reads);
};

bool RankedPassBuilder::Build(const RandomAccess &stream, PassMap &result, uint64_t &reads) {
	ReadGroup g;
	uint64_t file_group = 0;  // the group of the current file's extent 0
	uint16_t last_file = 0;
	while (file_group < result.groups_) {
		if (!ReadPassGroup(stream, result.units_offset, result.groups_, file_group, g)) {
			return false;
		}
		reads++;
		if (g.is_pad) {
			break;	// the pass's trailing pads: every file is mapped
		}
		// Group 0 of a file: its extent 0, the file header (page 0) and the first GAM (page 2).
		const uint8_t *header = GroupPage(g, 0, PAGE_TYPE_FILE_HEADER);
		const uint8_t *gam = GroupPage(g, 2, PAGE_TYPE_GAM);
		if (!g.has_extent || g.extent.extent != 0 || !header || !gam ||
			(!result.files_.empty() && g.extent.file <= last_file)) {
			return false;
		}
		const uint16_t file = g.extent.file;
		ByteReader h(header, PAGE_SIZE, "a file header page", 0);
		const uint16_t slot0 = h.U16(PAGE_SIZE - 2);
		if (slot0 < 96 || slot0 + FILE_HEADER_SIZE_AT + 4 > PAGE_SIZE || h.U16(slot0 + FILE_HEADER_ID_AT) != file) {
			return false;
		}
		const uint32_t size = h.U32(slot0 + FILE_HEADER_SIZE_AT);
		// Every PFS page's extent is copied, so a file has at least size / 8088 extents: one whose
		// Size the rest of the pass cannot hold is refused before anything is allocated for it.
		if (size == 0 || size / PFS_INTERVAL + 1 > result.groups_ - file_group) {
			return false;
		}
		ExtentBitmap bitmap;
		std::vector<uint8_t> gam_copy(gam, gam + PAGE_SIZE);
		for (uint32_t interval = 0;; interval++) {
			ByteReader bm(gam_copy.data(), PAGE_SIZE, "a GAM page", 0);
			AddInterval(size, interval, bm.Bytes(GAM_BITMAP_AT, GAM_BITMAP_BYTES), bitmap);
			if (bitmap.Count() > result.groups_ - file_group) {
				return false;
			}
			const uint64_t next_base = static_cast<uint64_t>(interval + 1) * GAM_INTERVAL_PAGES;
			if (next_base >= size) {
				break;
			}
			// The next GAM's extent comes after every extent added so far: its rank is their count.
			const uint32_t next_extent = static_cast<uint32_t>(next_base / PAGES_PER_EXTENT);
			if (!ReadPassGroup(stream, result.units_offset, result.groups_, file_group + bitmap.Count(), g)) {
				return false;
			}
			reads++;
			const uint8_t *next_gam = GroupPage(g, 2, PAGE_TYPE_GAM);
			if (!g.has_extent || g.extent.file != file || g.extent.extent != next_extent || !next_gam) {
				return false;
			}
			gam_copy.assign(next_gam, next_gam + PAGE_SIZE);
		}
		bitmap.Seal();
		const uint64_t count = bitmap.Count();
		// Check groups spread over the file: each names the extent its rank gives.
		for (uint32_t c = 1; c <= RANK_CHECKS_PER_FILE && c < count; c++) {
			uint64_t rank = (count - 1) * c / RANK_CHECKS_PER_FILE;
			bool verified = false;
			for (uint32_t step = 0; step < RANK_CHECK_STEPS && rank < count && !verified; step++, rank++) {
				uint32_t expected = 0;
				if (!bitmap.Select(rank, expected) ||
					!ReadPassGroup(stream, result.units_offset, result.groups_, file_group + rank, g)) {
					return false;
				}
				reads++;
				if (g.is_pad || (g.has_extent && (g.extent.file != file || g.extent.extent != expected))) {
					return false;
				}
				verified = g.has_extent;
			}
		}
		result.files_.push_back(file);
		result.file_first_group_.push_back(file_group);
		result.bitmaps_.push_back(std::move(bitmap));
		result.extents_ += count;
		last_file = file;
		file_group += count;
	}
	return !result.files_.empty();
}

bool BuildRankedPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, PassMap &map,
						uint64_t *groups_read) {
	if (length < 2 || offset > stream.Size() || length > stream.Size() - offset || (length - 2) % GROUP_BYTES != 0) {
		return false;
	}
	PassMap result;
	result.number = 0;
	result.units_offset = offset + 2;
	result.groups_ = (length - 2) / GROUP_BYTES;
	uint64_t reads = 0;
	bool ok = false;
	try {
		ok = RankedPassBuilder::Build(stream, result, reads);
	} catch (FormatError &) {
		// Something read did not parse: the rule does not hold here; the header scan decides.
		ok = false;
	}
	if (groups_read) {
		*groups_read = reads;
	}
	if (!ok) {
		return false;
	}
	result.ranked_ = true;
	map = std::move(result);
	return true;
}

bool PageIndex::Locate(uint16_t file, uint32_t page, PageLocation &location) const {
	ExtentId extent;
	extent.file = file;
	extent.extent = page / PAGES_PER_EXTENT;
	for (size_t i = passes_.size(); i-- > 0;) {
		uint64_t group;
		if (passes_[i].Find(extent, group)) {
			// Within one pass an extent is in one family (E12): the same pass of another family
			// carrying it too is a crafted file, refused here rather than by an open-time pass
			// over every extent.
			for (size_t j = i; j-- > 0 && passes_[j].number == passes_[i].number;) {
				uint64_t other;
				if (passes_[j].Find(extent, other)) {
					throw FormatError("backup: data pass " + std::to_string(passes_[i].number + 1) +
									  " carries extent " + std::to_string(file) + ":" + std::to_string(extent.extent) +
									  " in two media families (" + std::to_string(passes_[j].family + 1) + " and " +
									  std::to_string(passes_[i].family + 1) + ")");
				}
			}
			location.family = passes_[i].family;
			location.pass = passes_[i].number;
			location.offset = passes_[i].units_offset + group * GROUP_BYTES +
							  static_cast<uint64_t>(page % PAGES_PER_EXTENT) * PAGE_SIZE;
			return true;
		}
	}
	return false;
}

size_t PageIndex::MemoryBytes() const {
	size_t bytes = passes_.capacity() * sizeof(PassMap);
	for (const auto &p : passes_) {
		bytes += p.MemoryBytes();
	}
	return bytes;
}

//! One family's passes, appended to `passes` with their family and pass numbers.
static void AddFamilyPasses(const RandomAccess &stream, const MtfLayout &layout, uint32_t family, bool rank_first_pass,
							const PassMapper &mapper, std::vector<PassMap> &passes) {
	uint32_t number = 0;
	if (layout.FindAll("SSET").size() > 1) {
		throw FormatError("backup file \"" + stream.Name() +
						  "\" holds more than one backup set (appended with NOINIT): reading a set by position is "
						  "not supported yet");
	}
	for (const auto &block : layout.blocks) {
		if (block.type != "MSDA") {
			continue;
		}
		const MtfStream *data = nullptr;
		for (const auto &s : block.streams) {
			if (s.id == "MQDA" && s.length > 0) {
				if (s.encryption != 0) {
					throw FormatError(Where(stream, s.header_offset) + ": an encrypted data stream (spec 087)");
				}
				if (data) {
					throw FormatError(Where(stream, block.offset) +
									  ": an MSDA block with two data streams, a layout the reader does not know");
				}
				data = &s;
			}
		}
		// Numbered by the MSDA block's position, not by the data streams met: a family whose pass
		// has no data must not shift its later passes onto another family's earlier ones.
		const uint32_t this_number = number++;
		if (!data) {
			continue;
		}
		PassMap map;
		if (!(this_number == 0 && rank_first_pass &&
			  BuildRankedPassMap(stream, data->data_offset, data->length, map))) {
			map = mapper ? mapper(stream, data->data_offset, data->length, this_number)
						 : BuildPassMap(stream, data->data_offset, data->length, this_number);
		}
		map.number = this_number;
		map.family = family;
		passes.push_back(std::move(map));
	}
}

PageIndex BuildPageIndex(const std::vector<FamilyStream> &families, bool rank_first_pass, const PassMapper &mapper) {
	PageIndex index;
	for (uint32_t f = 0; f < families.size(); f++) {
		AddFamilyPasses(*families[f].stream, *families[f].layout, f, rank_first_pass && families.size() == 1, mapper,
						index.passes_);
	}
	// By pass number, so Locate meets the later pass first going backwards.
	std::stable_sort(index.passes_.begin(), index.passes_.end(),
					 [](const PassMap &a, const PassMap &b) { return a.number < b.number; });
	return index;
}

PageIndex BuildPageIndex(const RandomAccess &stream, const MtfLayout &layout, bool rank_first_pass) {
	FamilyStream one;
	one.stream = &stream;
	one.layout = &layout;
	return BuildPageIndex(std::vector<FamilyStream>{one}, rank_first_pass);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

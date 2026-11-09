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

static std::string Where(const RandomAccess &stream, uint64_t offset) {
	return "backup file \"" + stream.Name() + "\" at offset " + std::to_string(offset);
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

	void Finish(const RandomAccess &stream, uint64_t offset) {
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
				throw FormatError("backup file \"" + stream.Name() + "\" at offset " + std::to_string(offset) +
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

PassMap BuildPassMap(const RandomAccess &stream, uint64_t offset, uint64_t length, uint32_t number) {
	if (length < 2 || offset > stream.Size() || length > stream.Size() - offset) {
		throw FormatError(Where(stream, offset) + ": a data stream that does not fit the file");
	}
	if ((length - 2) % PAGE_SIZE != 0) {
		throw FormatError(Where(stream, offset) + ": a data stream of " + std::to_string(length) +
						  " bytes, not a 2-byte lead-in and whole 8 KB units");
	}
	PassMap map;
	map.number = number;
	map.units_offset = offset + 2;
	const uint64_t units = (length - 2) / PAGE_SIZE;
	if (units % PAGES_PER_EXTENT != 0) {
		throw FormatError(Where(stream, offset) + ": a data pass of " + std::to_string(units) +
						  " units, not whole 8-unit extent groups");
	}
	map.groups_ = units / PAGES_PER_EXTENT;
	PassMapBuilder builder(map);
	// One read per group (64 KB), not one per header: the reader under it may be a container whose
	// every read decodes, or a remote file.
	std::vector<uint8_t> group_bytes(GROUP_BYTES);
	for (uint64_t g = 0; g < map.groups_; g++) {
		const uint64_t group_at = map.units_offset + g * GROUP_BYTES;
		stream.ReadAt(group_at, group_bytes.data(), group_bytes.size());
		bool found = false;
		ExtentId extent;
		for (uint32_t k = 0; k < PAGES_PER_EXTENT; k++) {
			const uint8_t *unit = group_bytes.data() + static_cast<size_t>(k) * PAGE_SIZE;
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
				throw FormatError(Where(stream, at) + ": a unit with header version " + std::to_string(unit[0]) +
								  ", neither a page (1) nor a zero unit (0)");
			}
			if (unit[0] == PAGE_HEADER_VERSION && unit[1] == PAGE_TYPE_PAD) {
				continue;
			}
			ByteReader r(unit, HEADER_READ, "a page header", at);
			const uint32_t page = r.U32(32);
			const uint16_t file = r.U16(36);
			if (page % PAGES_PER_EXTENT != k) {
				throw FormatError(Where(stream, at) + ": page " + std::to_string(file) + ":" + std::to_string(page) +
								  " at position " + std::to_string(k) + " of its extent group");
			}
			ExtentId e;
			e.file = file;
			e.extent = page / PAGES_PER_EXTENT;
			if (found && !(e == extent)) {
				throw FormatError(Where(stream, at) + ": an extent group holding pages of two extents");
			}
			extent = e;
			found = true;
		}
		if (!found) {
			builder.Empty();
			continue;
		}
		builder.Extent(extent, g);
		map.extents_++;
	}
	builder.Finish(stream, offset);
	return map;
}

bool PageIndex::Locate(uint16_t file, uint32_t page, PageLocation &location) const {
	ExtentId extent;
	extent.file = file;
	extent.extent = page / PAGES_PER_EXTENT;
	for (size_t i = passes_.size(); i-- > 0;) {
		uint64_t group;
		if (passes_[i].Find(extent, group)) {
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

PageIndex BuildPageIndex(const RandomAccess &stream, const MtfLayout &layout) {
	PageIndex index;
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
		if (!data) {
			continue;
		}
		index.passes_.push_back(BuildPassMap(stream, data->data_offset, data->length, number++));
	}
	return index;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

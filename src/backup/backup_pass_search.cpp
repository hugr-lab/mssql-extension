#include "backup/backup_pass_search.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr uint64_t GROUP_BYTES = static_cast<uint64_t>(PAGE_SIZE) * PAGES_PER_EXTENT;
static constexpr size_t BLOCK_HEADER = 28;
static constexpr uint32_t BLOCK_OUTPUT = 65536;
static constexpr uint8_t KIND_COMPRESSED = 0x21;
static constexpr uint8_t KIND_STORED = 0x23;
// A resync takes the fourth of four chained plausible headers: a false start meets the true chain
// within a header or two, and the fourth was a true block in 3.6 M probes (E13).
static constexpr int RESYNC_HEADERS = 4;
// What a probe reads at once: the bytes up to the fourth header past a resync (each block at most
// 64 KB + its header) and the block it then decodes.
static constexpr uint64_t PROBE_BYTES = (RESYNC_HEADERS + 1) * (BLOCK_OUTPUT + BLOCK_HEADER) + BLOCK_OUTPUT;

static std::string WhereIn(const std::string &name, uint64_t at) {
	return "backup file \"" + name + "\" at offset " + std::to_string(at);
}

//===--------------------------------------------------------------------===//
// FilePassMedium
//===--------------------------------------------------------------------===//

FilePassMedium::FilePassMedium(const RandomAccess &file, uint64_t units_offset, uint64_t groups)
	: file_(file), units_offset_(units_offset), groups_(groups) {
	if (units_offset > file.Size() || groups > (file.Size() - units_offset) / GROUP_BYTES) {
		throw FormatError(WhereIn(file.Name(), units_offset) + ": a data pass that does not fit the file");
	}
}

uint64_t FilePassMedium::GroupOffset(uint64_t group) const {
	return units_offset_ + group * GROUP_BYTES;
}

uint64_t FilePassMedium::WindowSpan() const {
	return PassSearch::WINDOW_BYTES / GROUP_BYTES;
}

void FilePassMedium::ReadGroups(uint64_t from, uint64_t count, std::vector<PassSample> &out) const {
	std::vector<uint8_t> bytes(static_cast<size_t>(count * GROUP_BYTES));
	file_.ReadAt(GroupOffset(from), bytes.data(), bytes.size());
	bytes_read_ += bytes.size();
	std::vector<GroupHeader> headers;
	PassHeaderScan::ScanGroups(file_.Name(), GroupOffset(from), bytes.data(), count, headers);
	for (uint64_t g = 0; g < count; g++) {
		if (headers[g].found) {
			PassSample sample;
			sample.position = from + g;
			sample.next = from + g + 1;
			sample.extent = headers[g].extent;
			out.push_back(sample);
		}
	}
}

bool FilePassMedium::Probe(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const {
	resumed = at;
	limit = std::min(limit, groups_);
	std::vector<PassSample> found;
	for (uint64_t g = at; g < limit; g += WindowSpan()) {
		ReadGroups(g, std::min(WindowSpan(), limit - g), found);
		if (!found.empty()) {
			sample = found.front();
			return true;
		}
	}
	return false;
}

void FilePassMedium::Window(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const {
	limit = std::min(limit, groups_);
	if (from < limit) {
		ReadGroups(from, limit - from, out);
	}
}

//===--------------------------------------------------------------------===//
// ContainerPassMedium
//===--------------------------------------------------------------------===//

ContainerPassMedium::ContainerPassMedium(const RandomAccess &file, uint64_t begin, uint64_t end, uint64_t chain_end,
										 BlockDecoder decoder)
	: file_(file), begin_(begin), end_(end), chain_end_(chain_end), decoder_(std::move(decoder)) {
	if (begin > end || end > chain_end || chain_end > file.Size()) {
		throw FormatError(WhereIn(file.Name(), begin) + ": a data pass that does not fit the container's chain");
	}
}

uint64_t ContainerPassMedium::WindowSpan() const {
	return PassSearch::WINDOW_BYTES;
}

std::vector<uint8_t> ContainerPassMedium::ReadSpan(uint64_t at, uint64_t count) const {
	if (at >= chain_end_) {
		return std::vector<uint8_t>();
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(std::min(count, chain_end_ - at)));
	file_.ReadAt(at, bytes.data(), bytes.size());
	bytes_read_ += bytes.size();
	return bytes;
}

bool ContainerPassMedium::Plausible(const uint8_t *bytes, uint64_t base, uint64_t at, Block &block) const {
	if (at + BLOCK_HEADER > chain_end_) {
		return false;
	}
	const uint8_t *h = bytes + (at - base);
	const uint8_t kind = h[0];
	const uint32_t units = h[1];
	if ((kind != KIND_COMPRESSED && kind != KIND_STORED) || units == 0 || units > BLOCK_OUTPUT / 512) {
		return false;
	}
	const uint32_t length = static_cast<uint32_t>(h[2]) | (static_cast<uint32_t>(h[3]) << 8);
	block.kind = kind;
	block.payload = length == 0 ? BLOCK_OUTPUT : length;
	if (kind == KIND_STORED && block.payload != units * 512) {
		return false;
	}
	return block.payload <= chain_end_ - at - BLOCK_HEADER;
}

ContainerPassMedium::Block ContainerPassMedium::BlockAt(const std::vector<uint8_t> &bytes, uint64_t base,
														uint64_t position) const {
	uint8_t header[BLOCK_HEADER];
	const uint8_t *h = header;
	if (position >= base && position - base + BLOCK_HEADER <= bytes.size()) {
		h = bytes.data() + (position - base);
	} else {
		if (position + BLOCK_HEADER > chain_end_) {
			throw FormatError(WhereIn(Name(), position) + ": a block header past the container's chain");
		}
		file_.ReadAt(position, header, BLOCK_HEADER);
		bytes_read_ += BLOCK_HEADER;
	}
	Block block;
	if (!Plausible(h, position, position, block)) {
		throw FormatError(WhereIn(Name(), position) + ": no block header where the chain leads");
	}
	if (h[1] != BLOCK_OUTPUT / 512) {
		throw OrderBroken(WhereIn(Name(), position) + ": a block of a data pass whose output is " +
						  std::to_string(h[1] * 512u) + " bytes, not 64 KB (one extent group, E13)");
	}
	return block;
}

void ContainerPassMedium::DecodeGroup(uint64_t position, std::vector<uint8_t> &out) const {
	std::vector<uint8_t> none;
	auto block = BlockAt(none, 0, position);
	auto payload = ReadSpan(position + BLOCK_HEADER, block.payload);
	if (payload.size() != block.payload) {
		throw FormatError(WhereIn(Name(), position) + ": a block's payload past the container's chain");
	}
	out.resize(BLOCK_OUTPUT);
	if (block.kind == KIND_STORED) {
		std::memcpy(out.data(), payload.data(), BLOCK_OUTPUT);
	} else if (!decoder_) {
		throw FormatError(WhereIn(Name(), position) + ": a compressed block, and no decoder for this container");
	} else {
		decoder_(payload.data(), payload.size(), out.data(), BLOCK_OUTPUT);
	}
}

bool ContainerPassMedium::GroupOf(const std::vector<uint8_t> &bytes, uint64_t base, uint64_t position,
								  PassSample &sample) const {
	auto block = BlockAt(bytes, base, position);
	std::vector<uint8_t> payload;
	const uint8_t *data = nullptr;
	const uint64_t at = position + BLOCK_HEADER;
	if (at >= base && at - base + block.payload <= bytes.size()) {
		data = bytes.data() + (at - base);
	} else {
		payload = ReadSpan(at, block.payload);
		if (payload.size() != block.payload) {
			throw FormatError(WhereIn(Name(), position) + ": a block's payload past the container's chain");
		}
		data = payload.data();
	}
	static thread_local std::vector<uint8_t> group;
	group.resize(BLOCK_OUTPUT);
	if (block.kind == KIND_STORED) {
		std::memcpy(group.data(), data, BLOCK_OUTPUT);
	} else if (!decoder_) {
		throw FormatError(WhereIn(Name(), position) + ": a compressed block, and no decoder for this container");
	} else {
		decoder_(data, block.payload, group.data(), BLOCK_OUTPUT);
	}
	std::vector<GroupHeader> header;
	// The units' "offsets" in errors are the block's file offset: their data-set offset is unknown.
	PassHeaderScan::ScanGroups(Name(), position, group.data(), 1, header);
	sample.position = position;
	sample.next = position + BLOCK_HEADER + block.payload;
	if (!header[0].found) {
		return false;
	}
	sample.extent = header[0].extent;
	return true;
}

// A probe or a window that meets what is not the chain E13 describes (no four chained headers, a
// header where none can be, a group whose units are not pages of one extent) gives the pass up to
// its header scan rather than failing the open: OrderBroken, not a bare FormatError.
template <class F>
static auto GiveUpOnFormat(F f) -> decltype(f()) {
	try {
		return f();
	} catch (OrderBroken &) {
		throw;
	} catch (FormatError &e) {
		throw OrderBroken(e.what());
	}
}

bool ContainerPassMedium::Probe(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const {
	return GiveUpOnFormat([&]() { return ProbeChecked(at, limit, sample, resumed); });
}

void ContainerPassMedium::Window(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const {
	GiveUpOnFormat([&]() { WindowChecked(from, limit, out); });
}

bool ContainerPassMedium::ProbeChecked(uint64_t at, uint64_t limit, PassSample &sample, uint64_t &resumed) const {
	limit = std::min(limit, end_);
	resumed = limit;
	if (at >= limit) {
		return false;
	}
	// Resync: the first offset whose header and the three its chain leads to are plausible; the
	// fourth is the block. Read once, past it by a block, so the decode needs no second read.
	auto bytes = ReadSpan(at, PROBE_BYTES);
	uint64_t found = 0;
	bool synced = false;
	for (uint64_t y = at; !synced && y + BLOCK_HEADER <= at + bytes.size(); y++) {
		uint64_t z = y;
		uint64_t last = y;
		int ok = 0;
		Block block;
		while (ok < RESYNC_HEADERS && z + BLOCK_HEADER <= at + bytes.size() && Plausible(bytes.data(), at, z, block)) {
			last = z;
			z += BLOCK_HEADER + block.payload;
			ok++;
		}
		if (ok == RESYNC_HEADERS) {
			found = last;
			synced = true;
		}
	}
	if (!synced) {
		// No four chained headers in the bytes read: past the pass's last blocks (the probe started
		// within four blocks of the chain's end), or not a chain.
		if (at + bytes.size() >= chain_end_) {
			return false;
		}
		throw FormatError(WhereIn(Name(), at) + ": no block found within " + std::to_string(bytes.size()) +
						  " bytes of the container's chain");
	}
	resumed = std::min(found, limit);
	for (uint64_t position = found; position < limit;) {
		if (GroupOf(bytes, at, position, sample)) {
			return true;
		}
		position = sample.next;
	}
	return false;
}

void ContainerPassMedium::WindowChecked(uint64_t from, uint64_t limit, std::vector<PassSample> &out) const {
	limit = std::min(limit, end_);
	if (from >= limit) {
		return;
	}
	// One read: the blocks that start before `limit`, the last one whole.
	auto bytes = ReadSpan(from, limit - from + BLOCK_HEADER + BLOCK_OUTPUT);
	for (uint64_t position = from; position < limit;) {
		PassSample sample;
		if (GroupOf(bytes, from, position, sample)) {
			out.push_back(sample);
		}
		position = sample.next;
	}
}

//===--------------------------------------------------------------------===//
// PassSearch
//===--------------------------------------------------------------------===//

PassSearch::PassSearch(std::unique_ptr<PassMedium> medium, Strategy strategy)
	: medium_(std::move(medium)), strategy_(strategy) {
	if (!medium_) {
		throw FormatError("backup: a pass search over no pass");
	}
}

size_t PassSearch::SampleCount() const {
	std::lock_guard<std::mutex> guard(lock_);
	return by_position_.size();
}

size_t PassSearch::MemoryBytes() const {
	std::lock_guard<std::mutex> guard(lock_);
	// A std::map node: the value and about four pointers of tree bookkeeping.
	const size_t node = 4 * sizeof(void *);
	return by_position_.size() * (sizeof(PassSample) + sizeof(uint64_t) + node) +
		   by_extent_.size() * (sizeof(ExtentId) + sizeof(uint64_t) + node) +
		   empty_.size() * (2 * sizeof(uint64_t) + node);
}

uint64_t PassSearch::Reads() const {
	std::lock_guard<std::mutex> guard(lock_);
	return reads_;
}

bool PassSearch::Bounds(const ExtentId &extent, uint64_t &from, uint64_t &to, uint64_t &position) const {
	from = medium_->Begin();
	to = medium_->End();
	auto above = by_extent_.upper_bound(extent);
	if (above != by_extent_.end()) {
		to = above->second;
	}
	if (above != by_extent_.begin()) {
		auto at_or_below = std::prev(above);
		const auto &sample = by_position_.at(at_or_below->second);
		if (at_or_below->first == extent) {
			position = sample.position;
			return true;
		}
		from = sample.next;
	}
	return false;
}

void PassSearch::Keep(const PassSample &sample) {
	std::lock_guard<std::mutex> guard(lock_);
	auto same = by_position_.find(sample.position);
	if (same != by_position_.end()) {
		if (!(same->second.extent == sample.extent)) {
			throw OrderBroken(WhereIn(medium_->Name(), sample.position) +
							  ": a group of the pass read twice naming two extents");
		}
		same->second.next = std::max(same->second.next, sample.next);
		return;
	}
	auto after = by_position_.upper_bound(sample.position);
	const bool below_next = after == by_position_.end() || sample.extent < after->second.extent;
	const bool above_previous = after == by_position_.begin() || std::prev(after)->second.extent < sample.extent;
	if (!below_next || !above_previous || by_extent_.count(sample.extent)) {
		throw OrderBroken(WhereIn(medium_->Name(), sample.position) + ": extent " + std::to_string(sample.extent.file) +
						  ":" + std::to_string(sample.extent.extent) +
						  " out of order in its data pass, which a search cannot use");
	}
	if (by_position_.size() >= MAX_SAMPLES) {
		return;
	}
	by_position_.emplace(sample.position, sample);
	by_extent_.emplace(sample.extent, sample.position);
}

void PassSearch::KeepEmpty(uint64_t from, uint64_t to) {
	if (from >= to) {
		return;
	}
	std::lock_guard<std::mutex> guard(lock_);
	// Merge with every span that overlaps or touches [from, to).
	auto it = empty_.upper_bound(from);
	if (it != empty_.begin() && std::prev(it)->second >= from) {
		--it;
	}
	while (it != empty_.end() && it->first <= to) {
		from = std::min(from, it->first);
		to = std::max(to, it->second);
		it = empty_.erase(it);
	}
	if (empty_.size() < MAX_SAMPLES) {
		empty_.emplace(from, to);
	}
}

void PassSearch::SkipEmptyLocked(uint64_t &from, uint64_t &to) const {
	// A span holding `from` moves it to the span's end; one holding the position just before `to`
	// moves `to` to the span's start. Merged spans do not touch, so one step each suffices.
	auto at_from = empty_.upper_bound(from);
	if (at_from != empty_.begin() && std::prev(at_from)->second > from) {
		from = std::prev(at_from)->second;
	}
	if (from < to) {
		auto at_to = empty_.lower_bound(to);
		if (at_to != empty_.begin() && std::prev(at_to)->second >= to && std::prev(at_to)->first < to) {
			to = std::max(from, std::prev(at_to)->first);
		}
	}
}

bool PassSearch::Find(const ExtentId &extent, uint64_t &position) {
	uint64_t from = 0;
	uint64_t to = 0;
	{
		std::lock_guard<std::mutex> guard(lock_);
		if (Bounds(extent, from, to, position)) {
			return true;
		}
	}
	bool window_rest = false;
	// Galloping until a probe lands past the target; from then on the interval brackets it.
	bool galloping = strategy_ == Strategy::Gallop;
	uint64_t step = 1;
	for (;;) {
		{
			std::lock_guard<std::mutex> guard(lock_);
			SkipEmptyLocked(from, to);
		}
		if (from >= to) {
			return false;
		}
		if (window_rest || to - from <= medium_->WindowSpan()) {
			std::vector<PassSample> window;
			medium_->Window(from, to, window);
			{
				std::lock_guard<std::mutex> guard(lock_);
				reads_++;
			}
			// The window read every group of [from, to): the groups between those that name an extent
			// are empty, so each sample's `next` (and that of the sample ending at `from`) skips them,
			// and a later search for an extent in such a gap reads nothing.
			for (size_t i = 0; i < window.size(); i++) {
				window[i].next = i + 1 < window.size() ? window[i + 1].position : to;
			}
			bool found = false;
			for (const auto &sample : window) {
				Keep(sample);
				if (sample.extent == extent) {
					position = sample.position;
					found = true;
				}
			}
			KeepEmpty(from, window.empty() ? to : window.front().position);
			return found;
		}
		uint64_t mid = from + (to - from) / 2;
		if (galloping) {
			mid = from + std::min(step, (to - from) / 2);
			step = step > UINT64_MAX / 2 ? step : step * 2;
		}
		PassSample sample;
		uint64_t resumed = 0;
		const bool got = medium_->Probe(mid, to, sample, resumed);
		{
			std::lock_guard<std::mutex> guard(lock_);
			reads_++;
		}
		if (!got && resumed >= to) {
			// The probe settled past the interval (fewer than four blocks start in [mid, to)): read
			// what is left at once.
			window_rest = true;
			continue;
		}
		if (!got) {
			galloping = false;
			// Nothing from where the probe settled (`mid` in a file; the block a resync found, past
			// the blocks it passed over, which stay in the interval) to `to` names an extent.
			KeepEmpty(resumed, to);
			to = resumed;
			continue;
		}
		// The groups between where the probe settled and the one it found name no extent.
		KeepEmpty(resumed, sample.position);
		Keep(sample);
		if (sample.extent == extent) {
			position = sample.position;
			return true;
		}
		if (sample.extent < extent) {
			from = sample.next;
		} else {
			galloping = false;
			to = resumed;
		}
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

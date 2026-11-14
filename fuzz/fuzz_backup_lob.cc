// Fuzz harness: values stored off row (spec 085 PR 3): ParseOffRowPointer, WalkLob in both of its
// modes over up to 16 pages, and the `_mssql_lob` map (AppendLobMap / ParseLobMap).
//
// The input: u8 options, u8 n, n bytes of an in-row pointer (any of the three forms: its length and
// first byte decide), then up to 16 pages of 8 KB (a short last page padded with zeros). Options bit 7
// reads the rest of the input as a map instead.
//
// Options: bits 0-1 the walk's bound (64 bytes, 8,040, 64 KiB, 1 MiB); bit 2 the rowset has no LOB_DATA
// unit, bit 3 no ROW_OVERFLOW_DATA unit (else both are the first page's allocation unit), bit 4 the
// ROW_OVERFLOW_DATA unit is the second page's (so a pointer naming a page of the other unit is
// reachable). The pointer is walked twice: assembled and map-only. Checked beyond "no crash":
//   - an assembled walk hands over exactly the value's bytes: the nodes that hold them (DATA,
//     SMALL_ROOT) cover [0, length) in order, back to back, and each one's bytes are its record's;
//   - the nodes are a tree: a root record (depth 0) covers the value, and the children of the root
//     and of every INTERNAL / LARGE_ROOT node tile their parent's bytes in link order (a leaf has none);
//   - a map-only walk reads less (the leaves are its parent's claims) and hands over no bytes, so
//     where the assembled walk succeeds the map-only walk succeeds too, with the same value and nodes;
//   - the value's map entry survives AppendLobMap / ParseLobMap unchanged;
//   - a map (bit 7) that ParseLobMap takes is written back by AppendLobMap byte for byte.
// Every malformed input must end in a FormatError; any sanitizer report, trap, signal, hang or other
// exception is a real bug. Seeds (fuzz/gen_backup_seeds.cc): an in-row root over an INTERNAL node, a
// root whose last leaf holds more than claimed (a truncating .WRITE), a row-overflow pointer, root
// pointers to a LARGE_ROOT, a SMALL_ROOT and a NULL root, and a map. Not reachable here: MAX_NODES (16
// pages hold fewer fragments) and a pointer past 65,535 bytes (n is a byte).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_lob.hpp"
#include "backup/backup_lob_map.hpp"
#include "backup/backup_page.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;
static const size_t MAX_PAGES = 16;
static const uint64_t BOUNDS[] = {64, 8040, 64 * 1024, 1024 * 1024};
// LobNode::fragment_type of the nodes that hold a value's bytes (backup_lob.hpp).
static const uint8_t SMALL_ROOT = 0, INTERNAL = 2, DATA = 3, LARGE_ROOT = 5;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

namespace {

//! The bytes and the nodes of one walk.
class Collect : public LobSink {
public:
	std::string bytes;
	std::vector<LobNode> nodes;
	void Data(const uint8_t *data, size_t size) override {
		bytes.append(reinterpret_cast<const char *>(data), size);
	}
	void Node(const LobNode &node) override {
		nodes.push_back(node);
	}
};

bool SameNode(const LobNode &a, const LobNode &b) {
	return a.page.page == b.page.page && a.page.file == b.page.file && a.slot == b.slot && a.offset == b.offset &&
		   a.length == b.length && a.depth == b.depth && a.fragment_type == b.fragment_type;
}

bool SameEntry(const LobMapEntry &a, const LobMapEntry &b) {
	if (a.column_id != b.column_id || a.form != b.form || a.is_null != b.is_null || a.pointer != b.pointer ||
		a.length != b.length || a.nodes.size() != b.nodes.size()) {
		return false;
	}
	for (size_t i = 0; i < a.nodes.size(); i++) {
		if (!SameNode(a.nodes[i], b.nodes[i])) {
			return false;
		}
	}
	return true;
}

void FuzzMap(const uint8_t *data, size_t size) {
	std::vector<LobMapEntry> entries;
	try {
		entries = ParseLobMap(data, size, "the fuzz row");
	} catch (FormatError &) {
		return;
	}
	std::string again;
	AppendLobMap(entries, "the fuzz row", again);  // whatever ParseLobMap takes, AppendLobMap can write
	Require(again.size() == size && std::memcmp(again.data(), data, size) == 0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 2) {
		return 0;
	}
	const uint8_t options = data[0];
	if (options & 0x80) {
		FuzzMap(data + 1, size - 1);
		return 0;
	}
	const size_t pointer_size = std::min<size_t>(data[1], size - 2);
	const uint8_t *pointer_bytes = data + 2;
	const uint8_t *body = data + 2 + pointer_size;
	const size_t body_size = size - 2 - pointer_size;
	const size_t count = std::min(MAX_PAGES, (body_size + PAGE_SIZE - 1) / PAGE_SIZE);
	std::vector<Page> pages(count);
	std::map<std::pair<uint16_t, uint32_t>, const Page *> by_id;
	for (size_t i = 0; i < count; i++) {
		pages[i].bytes.assign(PAGE_SIZE, 0);
		const size_t take = std::min<size_t>(PAGE_SIZE, body_size - i * PAGE_SIZE);
		std::memcpy(pages[i].bytes.data(), body + i * PAGE_SIZE, take);
		pages[i].header = ReadPageHeader(pages[i].bytes.data());
		by_id.emplace(std::make_pair(pages[i].header.page_id.file, pages[i].header.page_id.page), &pages[i]);
	}
	const LobPageFetch fetch = [&](const PagePointer &at, Page &out) {
		auto it = by_id.find(std::make_pair(at.file, at.page));
		if (it == by_id.end()) {
			throw FormatError("backup: no page " + at.ToString());
		}
		out = *it->second;
	};

	OffRowPointer pointer;
	try {
		pointer = ParseOffRowPointer(pointer_bytes, pointer_size, "the fuzz value");
	} catch (FormatError &) {
		return 0;
	}
	const uint64_t unit = pages.empty() ? 1 : pages[0].header.AllocationUnit();
	LobUnits units;
	units.lob = options & 4 ? 0 : unit;
	units.row_overflow = options & 8 ? 0 : (options & 16) && pages.size() > 1 ? pages[1].header.AllocationUnit() : unit;
	const uint64_t bound = BOUNDS[options & 3];

	Collect assembled;
	LobValue value;
	bool assembled_ok = true;
	try {
		value = WalkLob(fetch, pointer, units, LobWalk::Assemble, bound, "the fuzz value", assembled);
	} catch (FormatError &) {
		assembled_ok = false;
	}
	Collect mapped;
	LobValue map_value;
	bool mapped_ok = true;
	try {
		map_value = WalkLob(fetch, pointer, units, LobWalk::MapOnly, bound, "the fuzz value", mapped);
	} catch (FormatError &) {
		mapped_ok = false;
	}
	g_sink += assembled.bytes.size() + mapped.nodes.size();
	if (!assembled_ok) {
		return 0;
	}

	Require(value.length <= bound && assembled.bytes.size() == value.length);
	uint64_t covered = 0;
	for (const LobNode &n : assembled.nodes) {
		if (n.fragment_type == DATA || n.fragment_type == SMALL_ROOT) {
			Require(n.offset == covered);
			covered += n.length;
		}
	}
	Require(covered == value.length);
	// Each data node's bytes are its record's: DATA after its 14-byte header, a SMALL_ROOT after 20.
	for (const LobNode &n : assembled.nodes) {
		if (n.fragment_type != DATA && n.fragment_type != SMALL_ROOT) {
			continue;
		}
		const Page &page = *by_id.at(std::make_pair(n.page.file, n.page.page));
		const size_t slot_at = PAGE_SIZE - 2 - 2 * static_cast<size_t>(n.slot);
		const size_t from = static_cast<size_t>(page.bytes[slot_at] | (page.bytes[slot_at + 1] << 8)) +
							(n.fragment_type == DATA ? 14 : 20);
		Require(from <= PAGE_SIZE && n.length <= PAGE_SIZE - from);
		Require(std::memcmp(page.bytes.data() + from, assembled.bytes.data() + n.offset, n.length) == 0);
	}
	// The tree: frame d is the node at depth d whose children are being read (frame 0 the in-row pointer
	// for an in-row root and row-overflow, the root record for a root pointer).
	struct Frame {
		uint64_t cursor, end;
		bool inner;
	};
	std::vector<Frame> frames;
	if (pointer.form != OffRowForm::RootPointer) {
		frames.push_back(Frame{0, value.length, true});
	}
	auto close = [&](size_t depth) {
		while (frames.size() > depth) {
			Require(!frames.back().inner || frames.back().cursor == frames.back().end);
			frames.pop_back();
		}
	};
	for (const LobNode &n : assembled.nodes) {
		const bool inner = n.fragment_type == INTERNAL || n.fragment_type == LARGE_ROOT;
		if (n.depth == 0) {
			Require(pointer.form == OffRowForm::RootPointer && frames.empty() && n.offset == 0 &&
					n.length == value.length);
			frames.push_back(Frame{0, n.length, inner});
			continue;
		}
		close(n.depth);
		Require(frames.size() == n.depth && frames.back().inner && n.offset == frames.back().cursor);
		frames.back().cursor += n.length;
		Require(frames.back().cursor <= frames.back().end);
		frames.push_back(Frame{n.offset, n.offset + n.length, inner});
	}
	close(0);
	Require(mapped.bytes.empty());
	Require(mapped_ok && map_value.is_null == value.is_null && map_value.length == value.length);
	Require(mapped.nodes.size() == assembled.nodes.size());
	for (size_t i = 0; i < mapped.nodes.size(); i++) {
		Require(SameNode(mapped.nodes[i], assembled.nodes[i]));
	}

	LobMapEntry entry;
	entry.column_id = 1;
	entry.form = pointer.form;
	entry.is_null = value.is_null;
	entry.pointer.assign(reinterpret_cast<const char *>(pointer_bytes), pointer_size);
	entry.length = static_cast<uint32_t>(value.length);
	entry.nodes = assembled.nodes;
	std::string map;
	AppendLobMap({entry}, "the fuzz row", map);
	const std::vector<LobMapEntry> back =
		ParseLobMap(reinterpret_cast<const uint8_t *>(map.data()), map.size(), "the fuzz row");
	Require(back.size() == 1 && SameEntry(back[0], entry));
	return 0;
}

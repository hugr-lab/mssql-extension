#include "backup/backup_lob.hpp"

#include "backup/backup_record.hpp"

#include <set>
#include <tuple>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr size_t ROOT_HEADER = 12;
constexpr size_t ROOT_LINK = 12;
constexpr uint16_t FRAGMENT_SMALL_ROOT = 0;
constexpr uint16_t FRAGMENT_INTERNAL = 2;
constexpr uint16_t FRAGMENT_DATA = 3;
constexpr uint16_t FRAGMENT_LARGE_ROOT = 5;
constexpr size_t MAX_DEPTH = 16;
// A child never claims more than its parent and every node holds at least one byte, so the value's
// bound bounds the bytes and the nodes; this bounds the page reads of a tree of one-byte fragments.
constexpr size_t MAX_NODES = 1u << 20;

struct Reader {
	Reader(const LobPageFetch &fetch_p, uint64_t lob_unit_p, const std::string &what_p)
		: fetch(fetch_p), lob_unit(lob_unit_p), what(what_p) {}

	const LobPageFetch &fetch;
	uint64_t lob_unit;
	const std::string &what;
	std::string out;
	size_t nodes = 0;
	std::set<std::tuple<uint16_t, uint32_t, uint16_t>> seen;  // a tree references each fragment once

	[[noreturn]] void Fail(const PagePointer &at, uint16_t slot, const std::string &why) const {
		throw FormatError("backup: the LOB fragment at " + at.ToString() + " slot " + std::to_string(slot) + " of " +
						  what + " " + why);
	}

	void Node(const PagePointer &at, uint16_t slot, uint64_t expected, size_t depth) {
		if (depth > MAX_DEPTH) {
			Fail(at, slot, "lies deeper than " + std::to_string(MAX_DEPTH) + " levels");
		}
		if (++nodes > MAX_NODES) {
			Fail(at, slot, "is one of more than " + std::to_string(MAX_NODES) + " fragments");
		}
		if (!seen.insert(std::make_tuple(at.file, at.page, slot)).second) {
			Fail(at, slot, "is referenced twice");
		}
		Page page;
		fetch(at, page);
		if (page.bytes.size() != PAGE_SIZE) {
			Fail(at, slot, "is on a page of " + std::to_string(page.bytes.size()) + " bytes");
		}
		if (page.header.type != static_cast<uint8_t>(PageType::TextMix) &&
			page.header.type != static_cast<uint8_t>(PageType::TextTree)) {
			Fail(at, slot, "is on a page of type " + std::to_string(page.header.type) + ", not a text page");
		}
		if (page.header.AllocationUnit() != lob_unit) {
			Fail(at, slot,
				 "is on a page of allocation unit " + std::to_string(page.header.AllocationUnit()) +
					 ", not the value's LOB unit " + std::to_string(lob_unit));
		}
		if (static_cast<size_t>(page.header.slot_count) * 2 + PAGE_HEADER_SIZE > PAGE_SIZE) {
			Fail(at, slot, "is on a page of " + std::to_string(page.header.slot_count) + " slots, more than it holds");
		}
		if (slot >= page.header.slot_count) {
			Fail(at, slot, "names a slot past the page's " + std::to_string(page.header.slot_count));
		}
		const size_t slot_at = PAGE_SIZE - 2 - 2 * static_cast<size_t>(slot);
		if (static_cast<uint16_t>(page.bytes[slot_at] | (page.bytes[slot_at + 1] << 8)) < PAGE_HEADER_SIZE) {
			Fail(at, slot, "names a freed slot");
		}
		const RecordView record = ReadSlot(page.bytes.data(), PAGE_SIZE, page.header.slot_count, slot, what.c_str());
		if (record.Type() != 4) {
			Fail(at, slot, "is a record of type " + std::to_string(record.Type()) + ", not a LOB fragment");
		}
		const ByteReader r(record.data, record.limit, what.c_str());
		const uint16_t length = r.U16(2);
		if (length < 14 || length > record.limit) {
			Fail(at, slot, "has a length of " + std::to_string(length));
		}
		const ByteReader body(record.data, length, what.c_str());
		const uint16_t type = body.U16(12);
		switch (type) {
		case FRAGMENT_DATA:
			Leaf(at, slot, body.Bytes(14, length - 14), length - 14u, expected);
			return;
		case FRAGMENT_SMALL_ROOT: {
			const uint16_t size = body.U16(14);
			Leaf(at, slot, body.Bytes(20, size), size, expected);
			return;
		}
		case FRAGMENT_INTERNAL:
		case FRAGMENT_LARGE_ROOT: {
			const uint16_t count = body.U16(16);
			const bool wide = type == FRAGMENT_INTERNAL;
			const size_t first = wide ? 20 : 24, step = wide ? 16 : 12;
			body.Require(first, step * count);
			uint64_t previous = 0;
			for (uint16_t i = 0; i < count; i++) {
				const size_t link = first + step * i;
				const uint64_t cumulative = wide ? body.U64(link) : body.U32(link);
				const size_t pointer = link + (wide ? 8 : 4);
				Child(at, slot, cumulative, previous, expected, ReadPagePointer(body, pointer), body.U16(pointer + 6),
					  depth);
			}
			if (previous != expected) {
				Fail(
					at, slot,
					"holds " + std::to_string(previous) + " bytes where its parent claims " + std::to_string(expected));
			}
			return;
		}
		default:
			Fail(at, slot, "is of fragment type " + std::to_string(type) + ", which the reader does not know");
		}
	}

	void Child(const PagePointer &at, uint16_t slot, uint64_t cumulative, uint64_t &previous, uint64_t expected,
			   const PagePointer &child, uint16_t child_slot, size_t depth) {
		if (cumulative <= previous) {
			Fail(at, slot, "has links whose sizes do not ascend");
		}
		if (cumulative > expected) {
			Fail(at, slot,
				 "has a link to " + std::to_string(cumulative) + " bytes where its parent claims " +
					 std::to_string(expected));
		}
		Node(child, child_slot, cumulative - previous, depth + 1);
		previous = cumulative;
	}

	void Leaf(const PagePointer &at, uint16_t slot, const uint8_t *data, size_t size, uint64_t expected) {
		if (size != expected) {
			Fail(at, slot,
				 "holds " + std::to_string(size) + " bytes where its parent claims " + std::to_string(expected));
		}
		out.append(reinterpret_cast<const char *>(data), size);
	}
};

}  // namespace

std::string ReadLobValue(const LobPageFetch &fetch, const std::string &pointer, uint64_t lob_unit,
						 const std::string &what, uint64_t max_bytes) {
	const ByteReader root(reinterpret_cast<const uint8_t *>(pointer.data()), pointer.size(), what.c_str());
	const uint8_t kind = root.U8(0);
	if (kind != 4 && kind != 5) {
		throw FormatError("backup: " + what + " is an off-row value of kind " + std::to_string(kind) +
						  (kind == 2 ? " (a row-overflow or text pointer)" : std::string()) +
						  ", which the catalog reader does not read");
	}
	if (pointer.size() < ROOT_HEADER + ROOT_LINK || (pointer.size() - ROOT_HEADER) % ROOT_LINK != 0) {
		throw FormatError("backup: " + what + "'s in-row LOB root is " + std::to_string(pointer.size()) +
						  " bytes, not a header and whole links");
	}
	const size_t links = (pointer.size() - ROOT_HEADER) / ROOT_LINK;
	// Every link checked before any is followed: ascending, so the last is the total, and that total
	// within max_bytes -- a node's own links are then bounded by what its parent claims (Child), so
	// no subtree is read past the bound (a first link of 0xFFFFFFFF before a last of 1 is refused
	// here, not after its subtree was read; review of #549).
	uint64_t ascending = 0;
	for (size_t i = 0; i < links; i++) {
		const uint64_t cumulative = root.U32(ROOT_HEADER + ROOT_LINK * i);
		if (cumulative <= ascending) {
			throw FormatError("backup: " + what + "'s in-row LOB root has links whose sizes do not ascend");
		}
		ascending = cumulative;
	}
	const uint64_t total = ascending;
	if (total > max_bytes) {
		throw FormatError("backup: " + what + " is a value of " + std::to_string(total) + " bytes, more than the " +
						  std::to_string(max_bytes) + " the reader takes");
	}
	Reader reader(fetch, lob_unit, what);
	reader.out.reserve(static_cast<size_t>(total));
	uint64_t previous = 0;
	for (size_t i = 0; i < links; i++) {
		const size_t link = ROOT_HEADER + ROOT_LINK * i;
		const uint64_t cumulative = root.U32(link);
		if (cumulative <= previous) {
			throw FormatError("backup: " + what + "'s in-row LOB root has links whose sizes do not ascend");
		}
		reader.Node(ReadPagePointer(root, link + 4), root.U16(link + 10), cumulative - previous, 1);
		previous = cumulative;
	}
	return reader.out;
}

std::string ReadLobValue(const PageStore &store, const std::string &pointer, uint64_t lob_unit, const std::string &what,
						 uint64_t max_bytes) {
	const std::string name = "a LOB fragment of " + what;
	return ReadLobValue([&](const PagePointer &at, Page &out) { store.ReadLive(at.file, at.page, out, name.c_str()); },
						pointer, lob_unit, what, max_bytes);
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

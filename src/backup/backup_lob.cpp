#include "backup/backup_lob.hpp"

#include "backup/backup_record.hpp"

#include <set>
#include <tuple>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr size_t ROOT_HEADER = 12;
constexpr size_t ROOT_LINK = 12;
constexpr size_t ROW_OVERFLOW_SIZE = 24;
constexpr size_t ROOT_POINTER_SIZE = 16;
constexpr uint16_t FRAGMENT_SMALL_ROOT = 0;
constexpr uint16_t FRAGMENT_INTERNAL = 2;
constexpr uint16_t FRAGMENT_DATA = 3;
constexpr uint16_t FRAGMENT_LARGE_ROOT = 5;
constexpr uint16_t FRAGMENT_NULL = 8;
constexpr size_t MAX_DEPTH = 16;
// A child never claims more than its parent and every node holds at least one byte, so the value's
// bound bounds the bytes and the nodes; this bounds the page reads of a tree of one-byte fragments.
constexpr size_t MAX_NODES = 1u << 20;
// Fragments looked up linearly before a set is built (Reader::FirstVisit).
constexpr size_t SEEN_LINEAR = 32;

//! What a node's parent says it is: a root record (behind a 16-byte root pointer), a DATA fragment
//! (a level-0 parent, row-overflow), or an INTERNAL node one level below its parent's.
struct Expect {
	enum Kind : uint8_t { Root, Leaf, Inner } kind;
	uint16_t level;
	//! A DATA fragment named by its parent's last link: it may hold more than the link claims (a
	//! truncating .WRITE leaves the last leaf's tail stale, lob.sql on 2022), its first claimed bytes the
	//! value's. Never for row-overflow: its pointer's length is exact.
	bool last;
};

struct Reader {
	Reader(const LobPageFetch &fetch_p, LobWalk mode_p, const std::string &what_p, LobSink &sink_p)
		: fetch(fetch_p), mode(mode_p), what(what_p), sink(sink_p) {}

	const LobPageFetch &fetch;
	LobWalk mode;
	const std::string &what;
	LobSink &sink;
	uint64_t unit = 0;
	size_t nodes = 0;
	bool is_null = false;
	// A tree references each fragment once. Most values are a handful of fragments, so the first ones are
	// looked up in a vector and a set is built only past it: no allocation per row for a row-overflow
	// value or a small root (review of PR 3 S1).
	typedef std::tuple<uint16_t, uint32_t, uint16_t> FragmentId;
	std::vector<FragmentId> seen_few;
	std::set<FragmentId> seen_many;

	bool FirstVisit(const PagePointer &at, uint16_t slot) {
		const FragmentId id = std::make_tuple(at.file, at.page, slot);
		if (seen_many.empty()) {
			for (const auto &f : seen_few) {
				if (f == id) {
					return false;
				}
			}
			if (seen_few.size() < SEEN_LINEAR) {
				seen_few.push_back(id);
				return true;
			}
			seen_many.insert(seen_few.begin(), seen_few.end());
		}
		return seen_many.insert(id).second;
	}

	[[noreturn]] void Fail(const PagePointer &at, uint16_t slot, const std::string &why) const {
		throw FormatError("backup: the LOB fragment at " + at.ToString() + " slot " + std::to_string(slot) + " of " +
						  what + " " + why);
	}

	void Count(const PagePointer &at, uint16_t slot, size_t depth) {
		if (depth > MAX_DEPTH) {
			Fail(at, slot, "lies deeper than " + std::to_string(MAX_DEPTH) + " levels");
		}
		if (++nodes > MAX_NODES) {
			Fail(at, slot, "is one of more than " + std::to_string(MAX_NODES) + " fragments");
		}
		if (!FirstVisit(at, slot)) {
			Fail(at, slot, "is referenced twice");
		}
	}

	//! The fragment record at (at, slot), checked: a text page of the form's unit, a slot in use, a
	//! record of type 4 whose length fits it. Its bytes are `page`'s, which must outlive the result.
	ByteReader Fragment(const PagePointer &at, uint16_t slot, Page &page) const {
		fetch(at, page);
		if (page.bytes.size() != PAGE_SIZE) {
			Fail(at, slot, "is on a page of " + std::to_string(page.bytes.size()) + " bytes");
		}
		if (page.header.type != static_cast<uint8_t>(PageType::TextMix) &&
			page.header.type != static_cast<uint8_t>(PageType::TextTree)) {
			Fail(at, slot, "is on a page of type " + std::to_string(page.header.type) + ", not a text page");
		}
		if (page.header.AllocationUnit() != unit) {
			Fail(at, slot,
				 "is on a page of allocation unit " + std::to_string(page.header.AllocationUnit()) +
					 ", not the value's unit " + std::to_string(unit));
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
		return ByteReader(record.data, length, what.c_str());
	}

	//! A leaf a map-only walk does not read: its place and length are its parent's link.
	void Unread(const PagePointer &at, uint16_t slot, uint64_t offset, uint64_t length, size_t depth) {
		Count(at, slot, depth);
		sink.Node(LobNode{at, slot, offset, length, static_cast<uint8_t>(depth), FRAGMENT_DATA});
	}

	//! The node at (at, slot), covering `expected` bytes from `offset` (a root record: whatever it
	//! holds, within `bound`). Returns the bytes it covers.
	uint64_t Node(const PagePointer &at, uint16_t slot, uint64_t offset, uint64_t expected, uint64_t bound,
				  size_t depth, Expect expect) {
		Count(at, slot, depth);
		Page page;
		const ByteReader body = Fragment(at, slot, page);
		const uint16_t type = body.U16(12);
		if (expect.kind == Expect::Leaf && type != FRAGMENT_DATA) {
			Fail(at, slot, "is of fragment type " + std::to_string(type) + " where its parent's level 0 names data");
		}
		if (expect.kind == Expect::Inner && type != FRAGMENT_INTERNAL) {
			Fail(at, slot,
				 "is of fragment type " + std::to_string(type) + " where its parent's level names an internal node");
		}
		if (expect.kind == Expect::Root) {
			// A root record is a SMALL_ROOT, a LARGE_ROOT or a NULL one (E21); a SMALL_ROOT's size is the
			// value's, within the bound.
			if (type != FRAGMENT_SMALL_ROOT && type != FRAGMENT_LARGE_ROOT && type != FRAGMENT_NULL) {
				Fail(at, slot, "is a root record of fragment type " + std::to_string(type));
			}
			if (type == FRAGMENT_SMALL_ROOT && body.U16(14) > bound) {
				throw FormatError("backup: " + what + " is a value of " + std::to_string(body.U16(14)) +
								  " bytes, more than the " + std::to_string(bound) + " the reader takes");
			}
		}
		switch (type) {
		case FRAGMENT_DATA:
			return Leaf(at, slot, body, 14, body.Size() - 14u, offset, expected, depth, type, expect);
		case FRAGMENT_SMALL_ROOT:
			return Leaf(at, slot, body, 20, body.U16(14), offset, expected, depth, type, expect);
		case FRAGMENT_NULL:
			sink.Node(LobNode{at, slot, offset, 0, static_cast<uint8_t>(depth), static_cast<uint8_t>(type)});
			is_null = true;
			return 0;
		case FRAGMENT_INTERNAL:
		case FRAGMENT_LARGE_ROOT: {
			const uint16_t count = body.U16(16);
			const uint16_t level = body.U16(18);
			if (expect.kind == Expect::Inner && level != expect.level) {
				Fail(at, slot,
					 "is at level " + std::to_string(level) + " where its parent names level " +
						 std::to_string(expect.level));
			}
			const bool wide = type == FRAGMENT_INTERNAL;
			const size_t first = wide ? 20 : 24, step = wide ? 16 : 12;
			body.Require(first, step * count);
			if (expect.kind == Expect::Root) {
				// A root record: what its links add up to is the value's length, checked against the bound
				// before any child is read.
				expected =
					count ? (wide ? body.U64(first + step * (count - 1u)) : body.U32(first + step * (count - 1u))) : 0;
				if (expected > bound) {
					throw FormatError("backup: " + what + " is a value of " + std::to_string(expected) +
									  " bytes, more than the " + std::to_string(bound) + " the reader takes");
				}
			}
			sink.Node(LobNode{at, slot, offset, expected, static_cast<uint8_t>(depth), static_cast<uint8_t>(type)});
			const Expect child =
				level == 0 ? Expect{Expect::Leaf, 0, false} : Expect{Expect::Inner, uint16_t(level - 1u), false};
			uint64_t previous = 0;
			for (uint16_t i = 0; i < count; i++) {
				const size_t link = first + step * i;
				const uint64_t cumulative = wide ? body.U64(link) : body.U32(link);
				const size_t pointer = link + (wide ? 8 : 4);
				if (cumulative <= previous) {
					Fail(at, slot, "has links whose sizes do not ascend");
				}
				if (cumulative > expected) {
					Fail(at, slot,
						 "has a link to " + std::to_string(cumulative) + " bytes where its parent claims " +
							 std::to_string(expected));
				}
				Expect named = child;
				named.last = i + 1u == count;
				Child(ReadPagePointer(body, pointer), body.U16(pointer + 6), offset + previous, cumulative - previous,
					  depth + 1, named);
				previous = cumulative;
			}
			if (previous != expected) {
				Fail(
					at, slot,
					"holds " + std::to_string(previous) + " bytes where its parent claims " + std::to_string(expected));
			}
			return expected;
		}
		default:
			Fail(at, slot, "is of fragment type " + std::to_string(type) + ", which the reader does not know");
		}
	}

	void Child(const PagePointer &at, uint16_t slot, uint64_t offset, uint64_t length, size_t depth, Expect expect) {
		if (expect.kind == Expect::Leaf && mode == LobWalk::MapOnly) {
			Unread(at, slot, offset, length, depth);
			return;
		}
		Node(at, slot, offset, length, length, depth, expect);
	}

	uint64_t Leaf(const PagePointer &at, uint16_t slot, const ByteReader &body, size_t from, size_t size,
				  uint64_t offset, uint64_t expected, size_t depth, uint16_t type, Expect expect) {
		if (expect.kind == Expect::Root) {
			expected = size;  // a SMALL_ROOT root record: its size is the value's
		}
		// A DATA fragment may hold more than its parent claims: a truncating .WRITE rewrites the links, not
		// the last leaf, whose tail is then stale (measured, lob.sql on 2022: 8,040 bytes held, 5,880
		// claimed). The value is its first `expected` bytes; fewer than claimed is a broken tree.
		if (size < expected || (size != expected && !(type == FRAGMENT_DATA && expect.last))) {
			Fail(at, slot,
				 "holds " + std::to_string(size) + " bytes where its parent claims " + std::to_string(expected));
		}
		const uint8_t *data = body.Bytes(from, static_cast<size_t>(expected));
		sink.Node(LobNode{at, slot, offset, expected, static_cast<uint8_t>(depth), static_cast<uint8_t>(type)});
		if (mode == LobWalk::Assemble) {
			sink.Data(data, static_cast<size_t>(expected));
		}
		return expected;
	}
};

class StringSink : public LobSink {
public:
	std::string out;
	void Data(const uint8_t *data, size_t size) override {
		out.append(reinterpret_cast<const char *>(data), size);
	}
};

}  // namespace

OffRowPointer ParseOffRowPointer(const uint8_t *bytes, size_t size, const std::string &what) {
	if (size > 0xFFFF) {
		throw FormatError("backup: " + what + " is a complex column of " + std::to_string(size) + " bytes");
	}
	OffRowPointer p;
	p.bytes = bytes;
	p.size = static_cast<uint16_t>(size);
	const ByteReader r(bytes, size, what.c_str());
	if (size == ROOT_POINTER_SIZE) {
		// Its first byte is the blob id's low byte, not a kind (E21): the length alone decides.
		p.form = OffRowForm::RootPointer;
		return p;
	}
	const uint8_t kind = r.U8(0);
	if (kind == 2 && size == ROW_OVERFLOW_SIZE) {
		p.form = OffRowForm::RowOverflow;
		p.length = r.U32(12);
		return p;
	}
	if (kind == 4 || kind == 5) {
		if (size < ROOT_HEADER + ROOT_LINK || (size - ROOT_HEADER) % ROOT_LINK != 0) {
			throw FormatError("backup: " + what + "'s in-row LOB root is " + std::to_string(size) +
							  " bytes, not a header and whole links");
		}
		// Every link checked before any is followed: ascending, so the last is the total, which the walk
		// checks against its bound before reading a node (review of #549).
		uint64_t ascending = 0;
		for (size_t i = 0; i < (size - ROOT_HEADER) / ROOT_LINK; i++) {
			const uint64_t cumulative = r.U32(ROOT_HEADER + ROOT_LINK * i);
			if (cumulative <= ascending) {
				throw FormatError("backup: " + what + "'s in-row LOB root has links whose sizes do not ascend");
			}
			ascending = cumulative;
		}
		p.form = OffRowForm::InRowRoot;
		p.length = ascending;
		return p;
	}
	throw FormatError("backup: " + what + " is an off-row value of " + std::to_string(size) + " bytes and kind " +
					  std::to_string(kind) + ", which is none of the forms the reader knows");
}

LobValue WalkLob(const LobPageFetch &fetch, const OffRowPointer &pointer, const LobUnits &units, LobWalk mode,
				 uint64_t max_bytes, const std::string &what, LobSink &sink) {
	Reader reader(fetch, mode, what, sink);
	const ByteReader r(pointer.bytes, pointer.size, what.c_str());
	LobValue value;
	const bool overflow = pointer.form == OffRowForm::RowOverflow;
	reader.unit = overflow ? units.row_overflow : units.lob;
	if (reader.unit == 0) {
		throw FormatError("backup: " + what + " is stored off row, but its rowset has no " +
						  (overflow ? "ROW_OVERFLOW_DATA" : "LOB_DATA") + " unit");
	}
	if (pointer.form != OffRowForm::RootPointer && pointer.length > max_bytes) {
		throw FormatError("backup: " + what + " is a value of " + std::to_string(pointer.length) +
						  " bytes, more than the " + std::to_string(max_bytes) + " the reader takes");
	}
	switch (pointer.form) {
	case OffRowForm::RowOverflow:
		reader.Child(ReadPagePointer(r, 16), r.U16(22), 0, pointer.length, 1, Expect{Expect::Leaf, 0, false});
		value.length = pointer.length;
		break;
	case OffRowForm::RootPointer:
		value.length =
			reader.Node(ReadPagePointer(r, 8), r.U16(14), 0, 0, max_bytes, 0, Expect{Expect::Root, 0, false});
		value.is_null = reader.is_null;
		break;
	case OffRowForm::InRowRoot: {
		const uint16_t level = r.U8(2);
		const Expect child =
			level == 0 ? Expect{Expect::Leaf, 0, false} : Expect{Expect::Inner, uint16_t(level - 1u), false};
		const size_t links = (pointer.size - ROOT_HEADER) / ROOT_LINK;
		uint64_t previous = 0;
		for (size_t i = 0; i < links; i++) {
			const size_t link = ROOT_HEADER + ROOT_LINK * i;
			const uint64_t cumulative = r.U32(link);
			// Checked here again, not trusted from ParseOffRowPointer: the walk is bounded by what it reads
			// itself, whoever built the pointer (review of PR 3 S1).
			if (cumulative <= previous || cumulative > pointer.length) {
				throw FormatError("backup: " + what + "'s in-row LOB root has links that do not ascend to its " +
								  std::to_string(pointer.length) + " bytes");
			}
			Expect named = child;
			named.last = i + 1 == links;
			reader.Child(ReadPagePointer(r, link + 4), r.U16(link + 10), previous, cumulative - previous, 1, named);
			previous = cumulative;
		}
		if (previous != pointer.length) {
			throw FormatError("backup: " + what + "'s in-row LOB root has links that do not ascend to its " +
							  std::to_string(pointer.length) + " bytes");
		}
		value.length = pointer.length;
		break;
	}
	}
	return value;
}

std::string ReadLobValue(const LobPageFetch &fetch, const std::string &pointer, uint64_t lob_unit,
						 const std::string &what, uint64_t max_bytes) {
	const OffRowPointer parsed =
		ParseOffRowPointer(reinterpret_cast<const uint8_t *>(pointer.data()), pointer.size(), what);
	if (parsed.form != OffRowForm::InRowRoot) {
		throw FormatError("backup: " + what + " is an off-row value " +
						  (parsed.form == OffRowForm::RowOverflow ? std::string("of kind 2 (a row-overflow pointer)")
																  : std::string("behind a 16-byte root pointer")) +
						  ", which the catalog reader does not read");
	}
	StringSink sink;
	sink.out.reserve(static_cast<size_t>(std::min<uint64_t>(parsed.length, max_bytes)));
	LobUnits units;
	units.lob = lob_unit;
	WalkLob(fetch, parsed, units, LobWalk::Assemble, max_bytes, what, sink);
	return std::move(sink.out);
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

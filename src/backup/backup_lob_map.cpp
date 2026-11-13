#include "backup/backup_lob_map.hpp"

#include "backup/backup_bytes.hpp"

#include <limits>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

void Put(std::string &out, uint64_t v, size_t bytes) {
	for (size_t i = 0; i < bytes; i++) {
		out.push_back(static_cast<char>(v >> (8 * i)));
	}
}

[[noreturn]] void Refuse(const std::string &what, const std::string &why) {
	throw FormatError("backup: " + what + "'s off-row map " + why);
}

constexpr size_t NODE_BYTES = 18;
constexpr size_t ENTRY_HEADER_BYTES = 4 + 1 + 1 + 2;

}  // namespace

void AppendLobMap(const std::vector<LobMapEntry> &entries, const std::string &what, std::string &out) {
	if (entries.empty() || entries.size() > 0xFFFF) {
		Refuse(what, "has " + std::to_string(entries.size()) + " entries");
	}
	const uint32_t max32 = std::numeric_limits<uint32_t>::max();
	Put(out, LOB_MAP_FORMAT, 1);
	Put(out, entries.size(), 2);
	for (size_t i = 0; i < entries.size(); i++) {
		const LobMapEntry &e = entries[i];
		if (i > 0 && e.column_id <= entries[i - 1].column_id) {
			Refuse(what, "has entries not in ascending column order");
		}
		if (e.pointer.size() > 0xFFFF || e.nodes.size() > max32) {
			Refuse(what, "has an entry the format cannot hold");
		}
		Put(out, e.column_id, 4);
		Put(out, static_cast<uint8_t>(e.form), 1);
		Put(out, e.is_null ? LOB_MAP_NULL_ROOT : 0, 1);
		Put(out, e.pointer.size(), 2);
		out.append(e.pointer);
		Put(out, e.length, 4);
		Put(out, e.nodes.size(), 4);
		for (const LobNode &n : e.nodes) {
			if (n.offset > max32 || n.length > max32) {
				Refuse(what, "has a node past 4 GiB");
			}
			Put(out, n.page.page, 4);
			Put(out, n.page.file, 2);
			Put(out, n.slot, 2);
			Put(out, n.offset, 4);
			Put(out, n.length, 4);
			Put(out, n.depth, 1);
			Put(out, n.fragment_type, 1);
		}
	}
}

std::vector<LobMapEntry> ParseLobMap(const uint8_t *data, size_t size, const std::string &what) {
	const ByteReader r(data, size, what.c_str());
	if (r.U8(0) != LOB_MAP_FORMAT) {
		Refuse(what, "is of format " + std::to_string(r.U8(0)) + ", not " + std::to_string(LOB_MAP_FORMAT));
	}
	const uint16_t count = r.U16(1);
	if (count == 0) {
		Refuse(what, "has no entries");
	}
	std::vector<LobMapEntry> entries;
	size_t at = 3;
	for (uint16_t i = 0; i < count; i++) {
		r.Require(at, ENTRY_HEADER_BYTES);
		LobMapEntry e;
		e.column_id = r.U32(at);
		const uint8_t form = r.U8(at + 4);
		if (form < static_cast<uint8_t>(OffRowForm::RowOverflow) ||
			form > static_cast<uint8_t>(OffRowForm::RootPointer)) {
			Refuse(what, "has an entry of form " + std::to_string(form));
		}
		e.form = static_cast<OffRowForm>(form);
		const uint8_t flags = r.U8(at + 5);
		if (flags & ~LOB_MAP_NULL_ROOT) {
			Refuse(what, "has an entry with flags " + std::to_string(flags));
		}
		e.is_null = (flags & LOB_MAP_NULL_ROOT) != 0;
		const uint16_t pointer_length = r.U16(at + 6);
		at += ENTRY_HEADER_BYTES;
		e.pointer.assign(reinterpret_cast<const char *>(r.Bytes(at, pointer_length)), pointer_length);
		at += pointer_length;
		e.length = r.U32(at);
		const uint32_t nodes = r.U32(at + 4);
		at += 8;
		// The count is checked against the bytes left before anything is reserved for it.
		if (nodes > (size - at) / NODE_BYTES) {
			Refuse(what, "claims " + std::to_string(nodes) + " nodes past its bytes");
		}
		e.nodes.reserve(nodes);
		for (uint32_t k = 0; k < nodes; k++) {
			LobNode n;
			n.page.page = r.U32(at);
			n.page.file = r.U16(at + 4);
			n.slot = r.U16(at + 6);
			n.offset = r.U32(at + 8);
			n.length = r.U32(at + 12);
			n.depth = r.U8(at + 16);
			n.fragment_type = r.U8(at + 17);
			e.nodes.push_back(n);
			at += NODE_BYTES;
		}
		if (!entries.empty() && e.column_id <= entries.back().column_id) {
			Refuse(what, "has entries not in ascending column order");
		}
		entries.push_back(std::move(e));
	}
	if (at != size) {
		Refuse(what, "has " + std::to_string(size - at) + " bytes past its entries");
	}
	return entries;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

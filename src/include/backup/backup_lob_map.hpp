//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_lob_map.hpp
//
// Spec 085 PR 3 (task 20, DQ-02 / DQ-05 / DQ-46): the plain `_mssql_lob` service column, a BLOB per row
// mapping every value of the row stored off row to its in-row pointer and the fragments of its tree,
// so that spec 089 can splice a value a differential changed in part. Opaque: written and read only by
// this code, versioned by its first byte. NULL (not an empty map) for a row with no off-row value.
//
//   u8  format                     0x01
//   u16 entry count                one per off-row value of the row, ascending column id
//   entry:
//     u32 column id                the catalog's (syscolpars.colid), not the rowset's
//     u8  form                     OffRowForm: 1 row-overflow, 2 in-row root, 3 root pointer
//     u8  flags                    bit 0: a NULL root (the value is NULL; E21)
//     u16 pointer length, then the in-row pointer's bytes exactly as stored (089: root identity)
//     u32 value length             bytes
//     u32 node count, then per node, depth first in link order (LobNode):
//       u32 page, u16 file, u16 slot
//       u32 offset, u32 length     the bytes of the value the node covers
//       u8  depth                  0 a root pointer's root record; 1 and below
//       u8  fragment type          0 SMALL_ROOT, 2 INTERNAL, 3 DATA, 5 LARGE_ROOT, 8 NULL
//
// 18 bytes a node; a value up to 2^31 - 1 bytes (offsets and lengths fit u32). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_lob.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

constexpr uint8_t LOB_MAP_FORMAT = 0x01;
constexpr uint8_t LOB_MAP_NULL_ROOT = 0x01;

struct LobMapEntry {
	uint32_t column_id = 0;
	OffRowForm form = OffRowForm::InRowRoot;
	bool is_null = false;
	std::string pointer;  // the in-row bytes
	uint32_t length = 0;
	std::vector<LobNode> nodes;
};

//! A LobSink keeping the nodes of one value (for its map entry); the bytes go nowhere.
class LobNodeList : public LobSink {
public:
	std::vector<LobNode> nodes;
	void Data(const uint8_t *data, size_t size) override {
		(void)data;
		(void)size;
	}
	void Node(const LobNode &node) override {
		nodes.push_back(node);
	}
};

//! Appends the map of `entries` (ascending column id, at least one) to `out`. A FormatError naming
//! `what` for an entry the format cannot hold: more than 65,535 entries or pointer bytes, a value or a
//! node past u32, entries not ascending.
void AppendLobMap(const std::vector<LobMapEntry> &entries, const std::string &what, std::string &out);

//! The entries of a map `AppendLobMap` wrote; a FormatError naming `what` for any other bytes: another
//! format, counts past the bytes, a form or flag the format does not have, entries not ascending,
//! trailing bytes.
std::vector<LobMapEntry> ParseLobMap(const uint8_t *data, size_t size, const std::string &what);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

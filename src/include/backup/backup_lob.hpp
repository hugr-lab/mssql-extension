//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_lob.hpp
//
// Spec 085: a value stored off row, read from its tree of fragment records (spec 085 § Rules, LOBs;
// E21). A complex variable column holds one of three forms: 16 bytes is always the root pointer, and
// at 24 bytes the first byte tells row-overflow (2) from an in-row root of one link (4):
//   - 24 bytes of kind 2, row-overflow: the value's length at +12 and the RID of one DATA fragment at
//     +16, on the rowset's ROW_OVERFLOW_DATA unit;
//   - 12 + 12n bytes of kind 4 (or 5), an in-row root: byte 2 its level, then n 12-byte links (u32
//     cumulative size, u32 page, u16 file, u16 slot), on the LOB_DATA unit;
//   - 16 bytes, a pointer to a root record (u64 blob id, u32 page, u16 file, u16 slot), on the LOB_DATA
//     unit: text / ntext / image with text in row off, and MAX columns written under `large value types
//     out of row`.
// Fragment records (record type 4) sit on text pages (types 3 and 4): DATA (3) holds bytes from 14 to
// the record's length; SMALL_ROOT (0) a u16 size at 14 and its bytes from 20; INTERNAL (2) 16-byte links
// from 20 (u64 cumulative size) and LARGE_ROOT (5) 12-byte links from 24, their count a u16 at 16 and
// their level a u16 at 18 (0: the links name DATA fragments); NULL (8), a root only: a text / image
// value updated to NULL keeps its pointer and its null bit clear (E21; ntext presumably alike).
// Every node's bytes must equal what its parent claims (a child may never claim more than its parent),
// but for the DATA fragment of a node's last link, which may hold more: its first claimed bytes are the
// value's (a truncating .WRITE leaves the last leaf's tail stale; never for row-overflow),
// a node's level agree with its children, every fragment be referenced once, every page name the
// form's unit, and the value stay within the caller's bound. DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_page.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! Hands page `at` over into `out` (its bytes and header): PageStore::ReadLive's, or a test's.
typedef std::function<void(const PagePointer &at, Page &out)> LobPageFetch;

//! The in-row form of an off-row value (E21).
enum class OffRowForm : uint8_t {
	RowOverflow = 1,  // 24 bytes, kind 2
	InRowRoot = 2,	  // 12 + 12n bytes, kind 4 or 5
	RootPointer = 3,  // 16 bytes
};

//! A complex variable column's bytes, classified. `bytes` points into the record, which must stay
//! valid, and unchanged, while the value is walked.
struct OffRowPointer {
	OffRowForm form = OffRowForm::InRowRoot;
	const uint8_t *bytes = nullptr;
	uint16_t size = 0;
	//! The value's length where the pointer says it (row-overflow, an in-row root's last link); a root
	//! pointer's is its root record's.
	uint64_t length = 0;
};

//! The form of a complex column's `size` bytes; a FormatError naming `what` for any other shape.
OffRowPointer ParseOffRowPointer(const uint8_t *bytes, size_t size, const std::string &what);

//! The rowset's units a pointer may name: LOB_DATA for an in-row root and a root pointer,
//! ROW_OVERFLOW_DATA for row-overflow (E21: strict). 0 = the rowset has none.
struct LobUnits {
	uint64_t lob = 0;
	uint64_t row_overflow = 0;
};

//! A fragment of a value's tree, as the walk reaches it.
struct LobNode {
	PagePointer page;
	uint16_t slot = 0;
	uint64_t offset = 0;  // the first byte of the value it covers
	uint64_t length = 0;  // the bytes it covers
	uint8_t depth = 0;	  // 0 a root pointer's root record; 1 and below, the nodes under a root
	//! 0 SMALL_ROOT, 2 INTERNAL, 3 DATA, 5 LARGE_ROOT, 8 NULL. A leaf a map-only walk does not read is 3,
	//! as its parent's level says it is: its type, unit and length are its parent's claims, unverified
	//! until it is read.
	uint8_t fragment_type = 0;
};

//! Where a walk puts what it finds. The walk hands bytes and nodes over as it reaches them, before
//! it has checked what follows: when it throws, what the sink holds of the value is partial and to
//! be discarded.
class LobSink {
public:
	virtual ~LobSink() = default;
	//! The value's bytes, in order (assembling walks only).
	virtual void Data(const uint8_t *data, size_t size) = 0;
	//! Every node, depth first in link order, before its children.
	virtual void Node(const LobNode &node) {
		(void)node;
	}
};

enum class LobWalk : uint8_t {
	Assemble,  // every node read, the bytes handed to the sink
	MapOnly,   // the leaves are not read: their place and length come from their parent's link
};

struct LobValue {
	bool is_null = false;  // a NULL root (type 8)
	uint64_t length = 0;
};

//! Walks the value behind `pointer` over pages fetched through `fetch`, at most `max_bytes` long. A
//! FormatError naming `what` past the bound, for a child claiming more bytes than its parent (checked
//! before it is read), a level its children contradict, a fragment referenced twice, a slot past the
//! page's slot array, a page of another unit than the form's, a NULL fragment below a root, or a node
//! whose bytes are not the ones its parent claims (a leaf's only when it is read).
LobValue WalkLob(const LobPageFetch &fetch, const OffRowPointer &pointer, const LobUnits &units, LobWalk mode,
				 uint64_t max_bytes, const std::string &what, LobSink &sink);

//! The value whose in-row root is `pointer` (the catalog's own values: a view's definition past 8000
//! bytes), its fragments on pages of allocation unit `lob_unit`; at most `max_bytes`. Any other form
//! is refused by name.
std::string ReadLobValue(const LobPageFetch &fetch, const std::string &pointer, uint64_t lob_unit,
						 const std::string &what, uint64_t max_bytes);
//! ReadLobValue over `store`'s live pages.
std::string ReadLobValue(const PageStore &store, const std::string &pointer, uint64_t lob_unit, const std::string &what,
						 uint64_t max_bytes);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb

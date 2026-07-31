#pragma once

// The physical structure of a table, as SQL Server reports it, plus the mapping
// from the raw sys.indexes.type text a metadata query returns.
//
// Deliberately free of DuckDB and TDS includes: this is a value type and a pure
// function, and keeping it self-contained is what lets test/cpp/test_index_kind.cpp
// compile with -I src/include and nothing else — no submodule, no linking.

#include <cstdint>
#include <string>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Physical structure of a table, as sys.indexes.type reports it for the base
// structure (index_id 0 or 1). Values are SQL Server's own, so the mapping is
// checkable against sys.indexes without a translation table.
//===----------------------------------------------------------------------===//

enum class MSSQLIndexKind : uint8_t {
	HEAP = 0,					//!< no clustered index; bulk load wants TABLOCK
	CLUSTERED = 1,				//!< clustered rowstore; TABLOCK serialises loaders
	CLUSTERED_COLUMNSTORE = 5,	//!< clustered columnstore; wants TABLOCK again
};

// The enumerators ARE sys.indexes.type. Renumbering them silently redefines what
// the catalog queries report, so the identity is pinned here rather than trusted.
static_assert(static_cast<uint8_t>(MSSQLIndexKind::HEAP) == 0, "MSSQLIndexKind must mirror sys.indexes.type");
static_assert(static_cast<uint8_t>(MSSQLIndexKind::CLUSTERED) == 1, "MSSQLIndexKind must mirror sys.indexes.type");
static_assert(static_cast<uint8_t>(MSSQLIndexKind::CLUSTERED_COLUMNSTORE) == 5,
			  "MSSQLIndexKind must mirror sys.indexes.type");

//! Map the raw sys.indexes.type text from a metadata query onto MSSQLIndexKind.
//!
//! Only the base structure reaches here (the queries filter index_id IN (0, 1)),
//! so 0/1/5 are exhaustive. Anything else means SQL Server grew a structure this
//! build has not been taught about: report HEAP, which is the conservative answer
//! for the write path — it is the only kind whose TABLOCK decision is safe when
//! the structure is unknown. A non-numeric value means the query shape and the
//! parser disagree about which column this is, which is the same case, and it
//! must not throw out of a metadata row callback.
inline MSSQLIndexKind MSSQLIndexKindFromSysIndexesType(const std::string &raw) {
	int type_value = 0;
	try {
		type_value = std::stoi(raw);
	} catch (...) {
		return MSSQLIndexKind::HEAP;
	}
	switch (type_value) {
	case 1:
		return MSSQLIndexKind::CLUSTERED;
	case 5:
		return MSSQLIndexKind::CLUSTERED_COLUMNSTORE;
	default:
		return MSSQLIndexKind::HEAP;
	}
}

}  // namespace duckdb

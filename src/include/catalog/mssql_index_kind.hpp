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

//===----------------------------------------------------------------------===//
// TABLOCK, decided by the target's shape (spec 057 step 1)
//
// Lives here, next to the enum, because the decision IS the enum plus one line —
// and because it then unit-tests with -I src/include and nothing else, the same
// property test/cpp/test_index_kind.cpp already relies on.
//===----------------------------------------------------------------------===//

//! What mssql_copy_tablock says. Tri-state so that "the user did not choose" is
//! representable — the previous BOOLEAN could not express it, which is why both
//! auto-TABLOCK branches were dead code (see the setting's registration).
enum class MSSQLTablockChoice : uint8_t { AUTO, ON, OFF };

//! Parse the setting / COPY option. Anything unrecognised is AUTO: this value
//! reaches us from a SET the user may have written by hand, and defaulting to
//! the measured-correct policy beats guessing ON or OFF.
inline MSSQLTablockChoice MSSQLParseTablockChoice(const std::string &raw) {
	if (raw == "true" || raw == "TRUE" || raw == "True" || raw == "1" || raw == "on" || raw == "ON") {
		return MSSQLTablockChoice::ON;
	}
	if (raw == "false" || raw == "FALSE" || raw == "False" || raw == "0" || raw == "off" || raw == "OFF") {
		return MSSQLTablockChoice::OFF;
	}
	return MSSQLTablockChoice::AUTO;
}

//! Should a bulk load into a target of this shape take a table lock?
//!
//! Measured (spec 057, "Revised TABLOCK policy"; emulated server, so the
//! magnitudes are upper bounds and the directions are what hold):
//!
//!   heap, 4 sessions        1.70 s with, 11.97 s without   -> ON
//!   heap, 1 session         11.37 s with, 14.42 s without  -> ON
//!   clustered rowstore, 4   2.11 s/M with, 1.20 s/M without-> OFF
//!   clustered rowstore, 1   2.185 s vs 2.166 s             -> neutral, so OFF
//!                                                             costs nothing
//!
//! Two locks, two opposite behaviours: concurrent bulk loaders on a heap take
//! mutually compatible BU locks, so the hint lets them run together; against a
//! clustered rowstore index the same hint serialises them. A clustered
//! COLUMNSTORE reports index_id = 1 like a rowstore index but behaves like a
//! heap here, which is why this takes the kind and not a bool.
//!
//! Single-session clustered load is neutral, so the degree of parallelism is
//! deliberately NOT an input — the shape alone decides, and no conditional is
//! needed.
//!
//! This replaces the issue-#45 policy of enabling it for newly created tables.
//! Newness is the wrong input: a fresh table is a heap only until an index is
//! created on it, which is exactly what CTAS-then-index does.
inline bool MSSQLTablockForShape(MSSQLIndexKind shape) {
	return shape != MSSQLIndexKind::CLUSTERED;
}

//! The whole decision: an explicit choice wins, otherwise the shape decides.
inline bool MSSQLResolveTablock(MSSQLTablockChoice choice, MSSQLIndexKind shape) {
	switch (choice) {
	case MSSQLTablockChoice::ON:
		return true;
	case MSSQLTablockChoice::OFF:
		return false;
	default:
		return MSSQLTablockForShape(shape);
	}
}

}  // namespace duckdb

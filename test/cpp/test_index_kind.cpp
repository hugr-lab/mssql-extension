// test/cpp/test_index_kind.cpp
//
// Unit tests for MSSQLIndexKind and the sys.indexes.type mapping (spec 049, #85).
//
// No SQL Server, no linking, no DuckDB submodule: catalog/mssql_index_kind.hpp
// is deliberately self-contained, so -I src/include is the whole build recipe.
//
// Why this exists. The catalog records the target's physical shape, but nothing
// consumes it until the write path lands, so without this test the mapping could
// be renumbered or inverted and every other test would still pass. The case that
// matters most is 5: a clustered COLUMNSTORE index reports index_id 1 in
// sys.partitions exactly as a rowstore one does, and the right TABLOCK answer
// for it is the opposite. That divergence is the entire reason the field is an
// enum rather than a has_clustered_index bool — so it is pinned here.
//
// Run:
//   ./build/test/test_index_kind

#include <cassert>
#include <iostream>
#include <string>

#include "catalog/mssql_index_kind.hpp"

using namespace duckdb;

static int g_failures = 0;

static const char *KindName(MSSQLIndexKind kind) {
	switch (kind) {
	case MSSQLIndexKind::HEAP:
		return "HEAP";
	case MSSQLIndexKind::CLUSTERED:
		return "CLUSTERED";
	case MSSQLIndexKind::CLUSTERED_COLUMNSTORE:
		return "CLUSTERED_COLUMNSTORE";
	default:
		return "<unknown>";
	}
}

static void CheckMaps(const char *raw, MSSQLIndexKind expected, const char *why) {
	const MSSQLIndexKind actual = MSSQLIndexKindFromSysIndexesType(raw);
	if (actual != expected) {
		std::cerr << "FAIL: \"" << raw << "\" -> " << KindName(actual) << ", expected " << KindName(expected) << " ("
				  << why << ")\n";
		++g_failures;
	} else {
		std::cout << "ok: \"" << raw << "\" -> " << KindName(expected) << " (" << why << ")\n";
	}
}

// The enumerators are SQL Server's own sys.indexes.type values. The header
// static_asserts this at compile time; asserting again here makes the failure
// name the contract rather than a line number in a header.
static void TestEnumeratorsAreSysIndexesTypeValues() {
	std::cout << "\n-- enumerators mirror sys.indexes.type --\n";
	struct {
		MSSQLIndexKind kind;
		uint8_t value;
	} const expected[] = {
		{MSSQLIndexKind::HEAP, 0},
		{MSSQLIndexKind::CLUSTERED, 1},
		{MSSQLIndexKind::CLUSTERED_COLUMNSTORE, 5},
	};
	for (const auto &e : expected) {
		if (static_cast<uint8_t>(e.kind) != e.value) {
			std::cerr << "FAIL: " << KindName(e.kind) << " is " << (int)static_cast<uint8_t>(e.kind) << ", sys.indexes"
					  << " reports " << (int)e.value << "\n";
			++g_failures;
		} else {
			std::cout << "ok: " << KindName(e.kind) << " == " << (int)e.value << "\n";
		}
	}
}

static void TestKnownStructures() {
	std::cout << "\n-- the three structures the queries can return --\n";
	CheckMaps("0", MSSQLIndexKind::HEAP, "index_id 0, no clustered index");
	CheckMaps("1", MSSQLIndexKind::CLUSTERED, "clustered rowstore");
	CheckMaps("5", MSSQLIndexKind::CLUSTERED_COLUMNSTORE, "clustered columnstore — index_id 1 like a rowstore one");
}

// Everything else means SQL Server grew a structure this build was not taught
// about. HEAP is the conservative answer: it is the only kind whose TABLOCK
// decision is safe when the structure is unknown.
static void TestUnknownStructuresDegradeToHeap() {
	std::cout << "\n-- unknown structures degrade to HEAP --\n";
	CheckMaps("2", MSSQLIndexKind::HEAP, "nonclustered — cannot reach here, index_id filter excludes it");
	CheckMaps("3", MSSQLIndexKind::HEAP, "XML index");
	CheckMaps("4", MSSQLIndexKind::HEAP, "spatial index");
	CheckMaps("6", MSSQLIndexKind::HEAP, "nonclustered columnstore");
	CheckMaps("7", MSSQLIndexKind::HEAP, "nonclustered hash");
	CheckMaps("99", MSSQLIndexKind::HEAP, "a structure that does not exist yet");
	CheckMaps("-1", MSSQLIndexKind::HEAP, "negative type");
}

// A non-numeric value means the query shape and the parser disagree about which
// column this is — the same "unknown structure" case, and it must not throw out
// of a metadata row callback.
static void TestMalformedInputIsHeapAndDoesNotThrow() {
	std::cout << "\n-- malformed input is HEAP, never an exception --\n";
	CheckMaps("", MSSQLIndexKind::HEAP, "empty — ISNULL should prevent it, but NULL handling is not this layer's");
	CheckMaps("NULL", MSSQLIndexKind::HEAP, "the literal text NULL");
	CheckMaps("abc", MSSQLIndexKind::HEAP, "not a number at all");
	CheckMaps("   ", MSSQLIndexKind::HEAP, "whitespace only");
	CheckMaps("99999999999999999999", MSSQLIndexKind::HEAP, "out of int range — std::stoi throws");
	// Documenting std::stoi's actual behaviour rather than assuming it: a
	// leading number wins and trailing junk is ignored. Harmless here, since
	// the column is an integer whenever the SELECT list is the expected one.
	CheckMaps(" 1", MSSQLIndexKind::CLUSTERED, "leading whitespace is skipped by std::stoi");
	CheckMaps("1x", MSSQLIndexKind::CLUSTERED, "trailing junk is ignored by std::stoi");
}

int main() {
	std::cout << "== MSSQLIndexKind unit tests (spec 049, #85) ==\n";
	TestEnumeratorsAreSysIndexesTypeValues();
	TestKnownStructures();
	TestUnknownStructuresDegradeToHeap();
	TestMalformedInputIsHeapAndDoesNotThrow();
	if (g_failures == 0) {
		std::cout << "\nAll MSSQLIndexKind tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " MSSQLIndexKind test(s) failed.\n";
	return 1;
}

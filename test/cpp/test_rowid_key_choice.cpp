// test/cpp/test_rowid_key_choice.cpp
//
// Unit tests for ChooseRowIdKey (spec 077 W1): which index becomes a table's
// rowid, and why each one that does not was rejected.
//
// No SQL Server, no linking, no DuckDB submodule: the header is self-contained,
// so -I src/include is the whole build recipe. The rule is pure over the
// candidate list, which is the point — the tie-break is asserted here so it
// cannot drift, and every rejection reason is pinned by text, because the W5b
// refusal quotes them to the user.
//
// Run:
//   make test-rowid-key-choice

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "catalog/mssql_rowid_key_choice.hpp"

using namespace duckdb::mssql;

static int g_failures = 0;

#define CHECK(cond, what)                                                                              \
	do {                                                                                               \
		if (!(cond)) {                                                                                 \
			std::cerr << "  FAIL: " << what << " (" << #cond << ") at line " << __LINE__ << std::endl; \
			g_failures++;                                                                              \
		}                                                                                              \
	} while (0)

static RowIdKeyColumn Col(const std::string &name, const std::string &type, int16_t max_length, bool nullable = false,
						  bool identity = false, bool cast_required = false, uint8_t scale = 0) {
	RowIdKeyColumn c;
	c.name = name;
	c.type_name = type;
	c.max_length = max_length;
	c.is_nullable = nullable;
	c.is_identity = identity;
	c.cast_required = cast_required;
	c.scale = scale;
	return c;
}

static RowIdKeyCandidate Idx(int32_t id, const std::string &name, std::vector<RowIdKeyColumn> cols, bool pk = false) {
	RowIdKeyCandidate c;
	c.index_id = id;
	c.index_name = name;
	c.is_primary_key = pk;
	c.is_unique = true;
	c.columns = std::move(cols);
	return c;
}

static bool Contains(const std::string &hay, const std::string &needle) {
	return hay.find(needle) != std::string::npos;
}

int main() {
	std::cout << "=== ChooseRowIdKey (spec 077 W1) ===" << std::endl;

	// --- step 1: a usable primary key wins, whatever else is there
	{
		auto pk = Idx(1, "PK_t", {Col("id", "int", 4)}, true);
		auto ux = Idx(2, "UX_t_code", {Col("code", "bigint", 8, false, true)});
		auto r = ChooseRowIdKey({ux, pk});	// listed after the unique one on purpose
		CHECK(r.source == RowIdKeySource::PRIMARY_KEY, "usable PK chosen");
		CHECK(r.index_name == "PK_t", "PK by name");
		CHECK(r.rejections.empty(), "nothing rejected");
	}

	// --- an unusable primary key falls through to a usable unique index,
	// and is listed as rejected with its reason (the step-1 change)
	{
		auto pk = Idx(1, "PK_dt", {Col("k", "datetime", 8)}, true);
		auto ux = Idx(2, "UX_id", {Col("id", "bigint", 8)});
		auto r = ChooseRowIdKey({pk, ux});
		CHECK(r.source == RowIdKeySource::UNIQUE_INDEX, "datetime PK falls through");
		CHECK(r.index_name == "UX_id", "the unique index is chosen");
		CHECK(r.rejections.size() == 1, "the PK is reported");
		CHECK(r.rejections[0].is_primary_key, "…as the primary key");
		CHECK(Contains(r.rejections[0].reason, "#358"), "…pointing at #358");
		CHECK(Contains(DescribeRejections(r.rejections), "primary key 'PK_dt' was rejected because"),
			  "describe names it");
	}
	{
		auto pk = Idx(1, "PK_v", {Col("v", "sql_variant", 8016, false, false, true)}, true);
		auto r = ChooseRowIdKey({pk});
		CHECK(!r.Found(), "sql_variant PK alone: nothing usable");
		CHECK(r.rejections.size() == 1 && Contains(r.rejections[0].reason, "#354"), "lossy read names #354");
	}
	{
		// measured (#350 review): smalldatetime has no fraction and matches its
		// datetime2 literal, datetime2(7) reads as TIMESTAMP_NS — both usable;
		// time(7) / datetimeoffset(7) lose their 100 ns digit on read — refused,
		// while scale 6 of either round-trips
		CHECK(ChooseRowIdKey({Idx(1, "PK_sdt", {Col("k", "smalldatetime", 4)}, true)}).Found(),
			  "smalldatetime is usable");
		CHECK(ChooseRowIdKey({Idx(1, "PK_dt27", {Col("k", "datetime2", 8, false, false, false, 7)}, true)}).Found(),
			  "datetime2(7) is usable");
		CHECK(ChooseRowIdKey({Idx(1, "PK_t6", {Col("k", "time", 5, false, false, false, 6)}, true)}).Found(),
			  "time(6) is usable");
		CHECK(
			ChooseRowIdKey({Idx(1, "PK_dto6", {Col("k", "datetimeoffset", 10, false, false, false, 6)}, true)}).Found(),
			"datetimeoffset(6) is usable");
		auto t7 = ChooseRowIdKey({Idx(1, "PK_t7", {Col("k", "time", 5, false, false, false, 7)}, true)});
		CHECK(!t7.Found() && Contains(t7.rejections[0].reason, "time(7)") && Contains(t7.rejections[0].reason, "#358"),
			  "time(7) is refused by name");
		auto dto7 = ChooseRowIdKey({Idx(1, "PK_dto7", {Col("k", "datetimeoffset", 10, false, false, false, 7)}, true)});
		CHECK(!dto7.Found() && Contains(dto7.rejections[0].reason, "datetimeoffset(7)"),
			  "datetimeoffset(7) is refused");
		CHECK(dto7.rejections[0].unmatchable, "…as unmatchable");
	}

	// --- each structural rejection, by text
	{
		auto c = Idx(2, "IX_notunique", {Col("a", "int", 4)});
		c.is_unique = false;
		CHECK(Contains(ChooseRowIdKey({c}).rejections[0].reason, "not unique"), "non-unique");
	}
	{
		auto c = Idx(2, "UX_filtered", {Col("a", "int", 4)});
		c.has_filter = true;
		CHECK(Contains(ChooseRowIdKey({c}).rejections[0].reason, "filtered"), "filtered");
	}
	{
		auto c = Idx(2, "UX_disabled", {Col("a", "int", 4)});
		c.is_disabled = true;
		CHECK(Contains(ChooseRowIdKey({c}).rejections[0].reason, "disabled"), "disabled");
	}
	{
		auto c = Idx(2, "UX_hypo", {Col("a", "int", 4)});
		c.is_hypothetical = true;
		CHECK(Contains(ChooseRowIdKey({c}).rejections[0].reason, "hypothetical"), "hypothetical");
	}
	{
		auto c = Idx(2, "UX_nullable", {Col("a", "int", 4), Col("b", "int", 4, true)});
		auto r = ChooseRowIdKey({c});
		CHECK(!r.Found(), "nullable key column: unusable");
		CHECK(Contains(r.rejections[0].reason, "'b' is nullable"), "names the nullable column");
	}

	// --- step 2: a single identity key outranks everything below it, even a
	// narrower key
	{
		auto narrow = Idx(2, "UX_int", {Col("n", "int", 4)});
		auto ident = Idx(3, "UX_bigint_identity", {Col("id", "bigint", 8, false, true)});
		auto r = ChooseRowIdKey({narrow, ident});
		CHECK(r.index_name == "UX_bigint_identity", "identity outranks byte width");
	}
	// …but only a SINGLE-column identity key: a composite key that happens to
	// contain the identity column is just a composite key
	{
		auto composite = Idx(2, "UX_id_plus", {Col("id", "bigint", 8, false, true), Col("x", "int", 4)});
		auto single = Idx(3, "UX_code", {Col("code", "int", 4)});
		auto r = ChooseRowIdKey({composite, single});
		CHECK(r.index_name == "UX_code", "fewest key columns beats a composite with identity in it");
	}

	// --- step 3: fewest key columns
	{
		auto two = Idx(2, "UX_two", {Col("a", "int", 4), Col("b", "int", 4)});
		auto one = Idx(3, "UX_one", {Col("c", "bigint", 8)});
		CHECK(ChooseRowIdKey({two, one}).index_name == "UX_one", "one column beats two, even wider");
	}

	// --- step 4: narrowest declared bytes; MAX is wider than anything
	{
		auto wide = Idx(2, "UX_nvarchar", {Col("s", "nvarchar", 200)});
		auto narrow = Idx(3, "UX_int", {Col("n", "int", 4)});
		CHECK(ChooseRowIdKey({wide, narrow}).index_name == "UX_int", "narrowest wins");
		auto max = Idx(4, "UX_max", {Col("m", "varchar", -1)});
		CHECK(ChooseRowIdKey({max, wide}).index_name == "UX_nvarchar", "MAX loses to bounded");
	}

	// --- step 5: lowest index_id, and it is deterministic across orderings
	{
		auto a = Idx(7, "UX_a", {Col("a", "int", 4)});
		auto b = Idx(3, "UX_b", {Col("b", "int", 4)});
		CHECK(ChooseRowIdKey({a, b}).index_name == "UX_b", "lowest index_id");
		CHECK(ChooseRowIdKey({b, a}).index_name == "UX_b", "…regardless of input order");
	}

	// --- steps 2-5 order, they do not filter to one: duplicate indexes on the
	// identity column both pass step 2 and step 5 separates them
	{
		auto d1 = Idx(5, "UX_id_1", {Col("id", "bigint", 8, false, true)});
		auto d2 = Idx(4, "UX_id_2", {Col("id", "bigint", 8, false, true)});
		CHECK(ChooseRowIdKey({d1, d2}).index_name == "UX_id_2", "duplicates: index_id decides");
	}

	// --- nothing at all
	{
		auto r = ChooseRowIdKey({});
		CHECK(!r.Found() && r.rejections.empty(), "no candidates: not found, nothing to report");
		CHECK(DescribeRejections(r.rejections).empty(), "…and nothing to describe");
	}

	// --- the rejection list is complete even when something was chosen
	{
		auto pk = Idx(1, "PK_dt", {Col("k", "datetime", 8)}, true);
		auto bad = Idx(2, "UX_nullable", {Col("n", "int", 4, true)});
		auto good = Idx(3, "UX_id", {Col("id", "int", 4)});
		auto r = ChooseRowIdKey({pk, bad, good});
		CHECK(r.index_name == "UX_id", "the usable one is chosen");
		CHECK(r.rejections.size() == 2, "both unusable ones are listed");
	}

	if (g_failures) {
		std::cout << "FAIL: " << g_failures << " check(s)" << std::endl;
		return 1;
	}
	std::cout << "PASS: choice rule and every rejection reason pinned" << std::endl;
	return 0;
}

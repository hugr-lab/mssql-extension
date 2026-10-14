//===----------------------------------------------------------------------===//
// Spec 080 D4: the WHEN clauses of a MERGE that T-SQL has a form for
// (mssql::CheckMergeShape). The pushed MERGE and the staged one (PR 4b) both
// go through it, so its answers are the refusals a user of either sees.
//===----------------------------------------------------------------------===//

#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "pushdown/mssql_merge_shape.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

#define ASSERT_TRUE(cond)                                                                    \
	do {                                                                                     \
		if (!(cond)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition was false: " #cond << std::endl;                       \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

namespace {

using Actions = std::map<MergeActionCondition, std::vector<MergeActionType>>;
using Kept = std::map<MergeActionCondition, std::vector<idx_t>>;

const auto MATCHED = MergeActionCondition::WHEN_MATCHED;
const auto BY_TARGET = MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET;
const auto BY_SOURCE = MergeActionCondition::WHEN_NOT_MATCHED_BY_SOURCE;
const auto UPDATE = MergeActionType::MERGE_UPDATE;
const auto DEL = MergeActionType::MERGE_DELETE;
const auto INSERT = MergeActionType::MERGE_INSERT;
const auto NOTHING = MergeActionType::MERGE_DO_NOTHING;
const auto ERROR_ACTION = MergeActionType::MERGE_ERROR;

void ExpectKept(const Actions &actions, const Kept &expected) {
	Kept kept;
	std::string why;
	if (!CheckMergeShape(actions, kept, why)) {
		std::cerr << "  unexpected refusal: " << why << std::endl;
		assert(false);
	}
	ASSERT_TRUE(kept == expected);
}

void ExpectRefused(const Actions &actions, const std::string &expected_why) {
	Kept kept;
	std::string why;
	if (CheckMergeShape(actions, kept, why) || why != expected_why) {
		std::cerr << "  expected refusal: " << expected_why << std::endl;
		std::cerr << "  got:              " << why << std::endl;
		assert(false);
	}
}

}  // namespace

static void TestAcceptedShapes() {
	std::cout << "  TestAcceptedShapes..." << std::endl;
	ExpectKept({{MATCHED, {UPDATE}}}, {{MATCHED, {0}}});
	ExpectKept({{MATCHED, {UPDATE, DEL}}, {BY_TARGET, {INSERT}}, {BY_SOURCE, {DEL, UPDATE}}},
			   {{MATCHED, {0, 1}}, {BY_TARGET, {0}}, {BY_SOURCE, {0, 1}}});
	ExpectKept({{BY_TARGET, {INSERT}}}, {{BY_TARGET, {0}}});
	// a last DO NOTHING is dropped; a kind left with nothing is absent
	ExpectKept({{MATCHED, {UPDATE, NOTHING}}}, {{MATCHED, {0}}});
	ExpectKept({{MATCHED, {NOTHING}}, {BY_TARGET, {INSERT, NOTHING}}}, {{BY_TARGET, {0}}});
	ExpectKept({{MATCHED, {DEL, UPDATE, NOTHING}}}, {{MATCHED, {0, 1}}});
}

static void TestRefusedShapes() {
	std::cout << "  TestRefusedShapes..." << std::endl;
	ExpectRefused({{MATCHED, {NOTHING, UPDATE}}}, "DO NOTHING before another WHEN MATCHED clause");
	ExpectRefused({{BY_SOURCE, {NOTHING, DEL}}}, "DO NOTHING before another WHEN NOT MATCHED BY SOURCE clause");
	ExpectRefused({{MATCHED, {ERROR_ACTION}}}, "an ERROR action in a MERGE");
	ExpectRefused({{BY_TARGET, {ERROR_ACTION}}}, "an ERROR action in a MERGE");
	ExpectRefused({{BY_TARGET, {INSERT, INSERT}}}, "more than one WHEN NOT MATCHED BY TARGET clause");
	ExpectRefused({{BY_TARGET, {UPDATE}}}, "WHEN NOT MATCHED BY TARGET without INSERT");
	ExpectRefused({{MATCHED, {UPDATE, DEL, UPDATE}}}, "more than two WHEN MATCHED clauses");
	ExpectRefused({{MATCHED, {UPDATE, UPDATE}}}, "two WHEN MATCHED clauses of one action");
	ExpectRefused({{BY_SOURCE, {DEL, DEL}}}, "two WHEN NOT MATCHED BY SOURCE clauses of one action");
	ExpectRefused({{MATCHED, {INSERT}}}, "INSERT in WHEN MATCHED");
	ExpectRefused({{BY_SOURCE, {UPDATE, INSERT}}}, "INSERT in WHEN NOT MATCHED BY SOURCE");
	ExpectRefused({{MATCHED, {NOTHING}}}, "a MERGE with no action");
	ExpectRefused({{MATCHED, {NOTHING}}, {BY_SOURCE, {NOTHING}}}, "a MERGE with no action");
}

static void TestWhenSql() {
	std::cout << "  TestWhenSql..." << std::endl;
	ASSERT_TRUE(std::string(MergeWhenSql(MATCHED)) == "WHEN MATCHED");
	ASSERT_TRUE(std::string(MergeWhenSql(BY_TARGET)) == "WHEN NOT MATCHED BY TARGET");
	ASSERT_TRUE(std::string(MergeWhenSql(BY_SOURCE)) == "WHEN NOT MATCHED BY SOURCE");
}

int main() {
	std::cout << "Running MERGE shape unit tests..." << std::endl;
	TestAcceptedShapes();
	TestRefusedShapes();
	TestWhenSql();
	std::cout << "All MERGE shape tests PASSED!" << std::endl;
	return 0;
}

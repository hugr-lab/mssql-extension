// test/cpp/test_filter_encoder.cpp
// Unit tests for FilterEncoder::EncodeSearchCondition / EncodeExpression.
//
// Why this file exists (PR #269 review): every pushdown test before it was a
// sqllogictest that asserted ROWS. Rows cannot distinguish "the predicate ran on
// the server" from "the predicate ran client-side in the spec-069 filter net" —
// both return the same answer, which is the whole point of the net. So the
// encoder could silently stop pushing anything and no test would fail. These
// tests pin the T-SQL STRING instead, which is the thing that actually changes.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "table_scan/filter_encoder.hpp"

using namespace duckdb;
using namespace duckdb::mssql;

//==============================================================================
// Helper macros
//==============================================================================
#define ASSERT_TRUE(cond)                                                                    \
	do {                                                                                     \
		if (!(cond)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Condition was false: " #cond << std::endl;                       \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_SQL(result, expected)                                                                 \
	do {                                                                                             \
		const auto &r_ = (result);                                                                   \
		const std::string e_ = (expected);                                                           \
		if (!r_.supported || r_.sql != e_) {                                                         \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl;         \
			std::cerr << "  expected: " << e_ << std::endl;                                          \
			std::cerr << "  actual:   " << (r_.supported ? r_.sql : "<not supported>") << std::endl; \
			assert(false);                                                                           \
		}                                                                                            \
	} while (0)

#define ASSERT_REFUSED(result)                                                               \
	do {                                                                                     \
		const auto &r_ = (result);                                                           \
		if (r_.supported) {                                                                  \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  expected refusal, got: " << r_.sql << std::endl;                 \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// Test fixture: a 5-column table, projected 1:1.
//
//   0 id    INTEGER
//   1 dt    TIMESTAMP_NS   (how DuckDB models SQL Server DATETIME2)
//   2 flag  BOOLEAN        (a SQL Server BIT)
//   3 name  VARCHAR
//   4 d     DOUBLE
//==============================================================================
namespace {

const idx_t COL_ID = 0;
const idx_t COL_DT = 1;
const idx_t COL_FLAG = 2;
const idx_t COL_NAME = 3;
const idx_t COL_D = 4;

struct Fixture {
	std::vector<column_t> column_ids = {0, 1, 2, 3, 4};
	std::vector<std::string> column_names = {"id", "dt", "flag", "name", "d"};
	std::vector<LogicalType> column_types = {LogicalType::INTEGER, LogicalType::TIMESTAMP_NS, LogicalType::BOOLEAN,
											 LogicalType::VARCHAR, LogicalType::DOUBLE};

	ExpressionEncodeContext Context() const {
		return ExpressionEncodeContext(column_ids, column_names, column_types);
	}
};

unique_ptr<Expression> ColRef(const Fixture &fx, idx_t col) {
	return make_uniq<BoundColumnRefExpression>(fx.column_types[col],
											   ColumnBinding(TableIndex(0), ProjectionIndex(col)));
}

unique_ptr<Expression> Const(Value v) {
	return make_uniq<BoundConstantExpression>(std::move(v));
}

// A bound call to `name` over one argument, as the binder would leave it.
unique_ptr<Expression> Call1(const std::string &name, const LogicalType &arg_type, const LogicalType &return_type,
							 unique_ptr<Expression> arg) {
	ScalarFunction fn(Identifier(name), std::vector<LogicalType>{arg_type}, return_type, nullptr);
	vector<unique_ptr<Expression>> args;
	args.push_back(std::move(arg));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(fn), std::move(args), nullptr);
}

unique_ptr<Expression> Call2(const std::string &name, const LogicalType &lhs_type, const LogicalType &rhs_type,
							 const LogicalType &return_type, unique_ptr<Expression> lhs, unique_ptr<Expression> rhs) {
	ScalarFunction fn(Identifier(name), std::vector<LogicalType>{lhs_type, rhs_type}, return_type, nullptr);
	vector<unique_ptr<Expression>> args;
	args.push_back(std::move(lhs));
	args.push_back(std::move(rhs));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(fn), std::move(args), nullptr, true);
}

}  // namespace

//==============================================================================
// Test: the spec 070 W1 headline — year(dt) pushes through the implicit
// TIMESTAMP_NS -> TIMESTAMP precision cast DuckDB puts over a DATETIME2 column.
//==============================================================================
static void TestDatePartOverPrecisionCast() {
	std::cout << "  TestDatePartOverPrecisionCast..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// year(CAST(dt AS TIMESTAMP)) = 2024
	auto cast = BoundCastExpression::AddDefaultCastToType(ColRef(fx, COL_DT), LogicalType::TIMESTAMP);
	ASSERT_TRUE(BoundCastExpression::IsCast(*cast));
	auto year = Call1("year", LogicalType::TIMESTAMP, LogicalType::BIGINT, std::move(cast));
	auto cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(year), Const(Value::BIGINT(2024)));

	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp, ctx), "(YEAR([dt]) = 2024)");
}

//==============================================================================
// Test: the cast strip is scoped to date-part functions AND to naive-timestamp
// precision changes. A cast that CHANGES THE VALUE must survive (and therefore
// refuse), or the server would answer a different question than DuckDB asked.
//==============================================================================
static void TestValueChangingCastNotStripped() {
	std::cout << "  TestValueChangingCastNotStripped..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// year(CAST(dt AS DATE)) — DATE is not a naive timestamp: not stripped.
	auto date_cast = BoundCastExpression::AddDefaultCastToType(ColRef(fx, COL_DT), LogicalType::DATE);
	auto year_of_date = Call1("year", LogicalType::DATE, LogicalType::BIGINT, std::move(date_cast));
	auto cmp1 = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(year_of_date),
												  Const(Value::BIGINT(2024)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*cmp1, ctx));

	// CAST(dt AS TIMESTAMP) compared directly — not a date-part function, so the
	// precision cast is not stripped either; the comparison falls to the client.
	auto ts_cast = BoundCastExpression::AddDefaultCastToType(ColRef(fx, COL_DT), LogicalType::TIMESTAMP);
	auto cmp2 = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, std::move(ts_cast),
												  Const(Value::BIGINT(0)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*cmp2, ctx));
}

//==============================================================================
// Test: bit coercion. SQL Server has no boolean VALUE type — `WHERE [flag]` is
// error 4145, the predicate is `WHERE [flag] = 1`. EncodeSearchCondition adds
// that at predicate position; EncodeExpression (value position) must not.
//==============================================================================
static void TestBitCoercionAtPredicatePositions() {
	std::cout << "  TestBitCoercionAtPredicatePositions..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// Top-level: flag
	auto bare = ColRef(fx, COL_FLAG);
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*bare, ctx), "([flag] = 1)");
	// Value position keeps the bare form.
	ASSERT_SQL(FilterEncoder::EncodeExpression(*bare, ctx), "[flag]");

	// NOT flag
	auto not_expr = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	not_expr->GetChildrenMutable().push_back(ColRef(fx, COL_FLAG));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*not_expr, ctx), "(NOT ([flag] = 1))");

	// A comparison already IS a search condition — it must not be wrapped again.
	auto cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_ID), Const(Value::INTEGER(7)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp, ctx), "([id] = 7)");
}

//==============================================================================
// Test: a CASE `WHEN` operand is predicate position too (PR #269 review).
// `CASE WHEN [flag] THEN ...` is the same error 4145, and because an encoded
// predicate is ERASED from the DuckDB plan the query fails outright rather than
// degrading to the client filter net.
//==============================================================================
static void TestCaseWhenIsPredicatePosition() {
	std::cout << "  TestCaseWhenIsPredicatePosition..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// CASE WHEN flag THEN id ELSE 0 END > 0
	auto case_expr = make_uniq<BoundCaseExpression>(ColRef(fx, COL_FLAG), ColRef(fx, COL_ID), Const(Value::INTEGER(0)));
	auto cmp = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, std::move(case_expr),
												 Const(Value::INTEGER(0)));

	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp, ctx), "(CASE WHEN ([flag] = 1) THEN [id] ELSE 0 END > 0)");
}

//==============================================================================
// Test: the issue #242 removals stay removed. Each of these has a T-SQL form
// that returns DIFFERENT ROWS than DuckDB does, so "not pushed" is the correct
// answer and a future mapping table edit must not quietly restore it.
//==============================================================================
static void TestDivergingFunctionsNotMapped() {
	std::cout << "  TestDivergingFunctionsNotMapped..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// length(name) = 3 — LEN drops trailing spaces and counts UTF-16 units.
	auto len = Call1("length", LogicalType::VARCHAR, LogicalType::BIGINT, ColRef(fx, COL_NAME));
	auto len_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(len), Const(Value::BIGINT(3)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*len_cmp, ctx));

	// id / 2 = 2 — T-SQL integer division vs DuckDB float division.
	auto div = Call2("/", LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::DOUBLE, ColRef(fx, COL_ID),
					 Const(Value::INTEGER(2)));
	auto div_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(div), Const(Value::INTEGER(2)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*div_cmp, ctx));

	// date_part('year', dt) — the part is a T-SQL keyword, not a string literal.
	auto part = Call2("date_part", LogicalType::VARCHAR, LogicalType::TIMESTAMP, LogicalType::BIGINT,
					  Const(Value("year")), ColRef(fx, COL_DT));
	auto part_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(part), Const(Value::BIGINT(2024)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*part_cmp, ctx));
}

//==============================================================================
// Test: modulo maps for exact numerics but not for float/real, which T-SQL's %
// rejects outright ("Operand data type float is invalid for modulo operator").
// An encoded predicate is erased from the plan, so pushing it would FAIL the
// query rather than fall back (PR #269 review).
//==============================================================================
static void TestModuloOperandTypes() {
	std::cout << "  TestModuloOperandTypes..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// id % 2 = 0 — integers behave identically on both sides.
	auto int_mod = Call2("%", LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER, ColRef(fx, COL_ID),
						 Const(Value::INTEGER(2)));
	auto int_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(int_mod), Const(Value::INTEGER(0)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*int_cmp, ctx), "(([id] % 2) = 0)");

	// d % 2 = 0 on a DOUBLE column — refused.
	auto dbl_mod = Call2("%", LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE, ColRef(fx, COL_D),
						 Const(Value::DOUBLE(2)));
	auto dbl_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(dbl_mod), Const(Value::DOUBLE(0)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*dbl_cmp, ctx));
}

//==============================================================================
// Test: a search condition is illegal in VALUE position — the mirror of the
// CASE WHEN case, and the same bad failure mode (job 1113).
//
// `CASE WHEN flag THEN id > 5 ELSE id < 2 END` encodes to
// `CASE WHEN ([flag] = 1) THEN ([id] > 5) ELSE ([id] < 2) END`, which SQL Server
// rejects with "Incorrect syntax near '>'". Because ComplexFilterPushdown erases
// the expression from the DuckDB plan, the query FAILS rather than degrading to
// the client filter net.
//==============================================================================
static void TestSearchConditionRefusedInValuePosition() {
	std::cout << "  TestSearchConditionRefusedInValuePosition..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// CASE WHEN flag THEN (id > 5) ELSE (id < 2) END  — conditions in THEN/ELSE
	auto then_cond = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_ID),
													   Const(Value::INTEGER(5)));
	auto else_cond = BoundComparisonExpression::Create(ExpressionType::COMPARE_LESSTHAN, ColRef(fx, COL_ID),
													   Const(Value::INTEGER(2)));
	auto case_expr = make_uniq<BoundCaseExpression>(ColRef(fx, COL_FLAG), std::move(then_cond), std::move(else_cond));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*case_expr, ctx));

	// (id > 5) IS NULL — condition as the IS NULL operand
	auto isnull = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	isnull->GetChildrenMutable().push_back(BoundComparisonExpression::Create(
		ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_ID), Const(Value::INTEGER(5))));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*isnull, ctx));

	// COMPARISON OPERAND — the motivating example, and the position that stayed
	// open when the first four were guarded (job 1203). `flag = (id > 5)` encodes
	// to `([flag] = ([id] > 5))`, which SQL Server rejects; the query FAILS,
	// because ComplexFilterPushdown erases the expression from the plan.
	auto inner_cond = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_ID),
														Const(Value::INTEGER(5)));
	auto cmp_operand =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_FLAG), std::move(inner_cond));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*cmp_operand, ctx));

	// FUNCTION ARGUMENT — lower(id > 5).
	auto inner2 = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_ID),
													Const(Value::INTEGER(5)));
	auto fn_arg = Call1("lower", LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(inner2));
	auto fn_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(fn_arg), Const(Value("x")));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*fn_cmp, ctx));

	// The legal shapes still encode — the routing change must not OVER-refuse,
	// which a refusal-only test cannot tell apart from the fix working.
	auto ok_case = make_uniq<BoundCaseExpression>(ColRef(fx, COL_FLAG), ColRef(fx, COL_ID), Const(Value::INTEGER(0)));
	auto ok_cmp = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, std::move(ok_case),
													Const(Value::INTEGER(0)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*ok_cmp, ctx), "(CASE WHEN ([flag] = 1) THEN [id] ELSE 0 END > 0)");

	auto ok_plain =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_ID), Const(Value::INTEGER(7)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*ok_plain, ctx), "([id] = 7)");

	auto ok_fn = Call1("lower", LogicalType::VARCHAR, LogicalType::VARCHAR, ColRef(fx, COL_NAME));
	auto ok_fn_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(ok_fn), Const(Value("x")));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*ok_fn_cmp, ctx), "(LOWER([name]) = N'x')");
}

//==============================================================================
// Test: modulo pushes for exact integers only — T-SQL rejects float, real AND
// money (8117), and money reaches the encoder as DECIMAL, indistinguishable from
// a real decimal (job 1113).
//==============================================================================
static void TestModuloExactIntegersOnly() {
	std::cout << "  TestModuloExactIntegersOnly..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();

	// DECIMAL is refused: a money column arrives as DECIMAL(19,4) and T-SQL's %
	// rejects it, so the encoder cannot tell a pushable decimal from a money one.
	auto dec = Call2("%", LogicalType::DECIMAL(19, 4), LogicalType::DECIMAL(19, 4), LogicalType::DECIMAL(19, 4),
					 Const(Value::DECIMAL(int64_t(100), 19, 4)), Const(Value::DECIMAL(int64_t(20), 19, 4)));
	auto dec_cmp = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(dec),
													 Const(Value::DECIMAL(int64_t(0), 19, 4)));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*dec_cmp, ctx));
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "Running FilterEncoder unit tests..." << std::endl;

	TestDatePartOverPrecisionCast();
	TestValueChangingCastNotStripped();
	TestBitCoercionAtPredicatePositions();
	TestCaseWhenIsPredicatePosition();
	TestDivergingFunctionsNotMapped();
	TestModuloOperandTypes();
	TestSearchConditionRefusedInValuePosition();
	TestModuloExactIntegersOnly();

	std::cout << "All FilterEncoder tests PASSED!" << std::endl;
	return 0;
}

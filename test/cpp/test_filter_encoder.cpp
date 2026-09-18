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

#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "query/mssql_sql_params.hpp"
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

	// BETWEEN input — the third position the previous round asked for, which did
	// not land then (job 1216). EncodeBetweenExpression routes input and both
	// bounds through the helper, and the helper's own doc names this shape.
	auto btw_bad =
		BoundBetweenExpression::Create(BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN,
																		 ColRef(fx, COL_ID), Const(Value::INTEGER(5))),
									   Const(Value::INTEGER(1)), Const(Value::INTEGER(10)), true, true);
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*btw_bad, ctx));

	// and the plain form still encodes
	auto btw_ok = BoundBetweenExpression::Create(ColRef(fx, COL_ID), Const(Value::INTEGER(1)),
												 Const(Value::INTEGER(10)), true, true);
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*btw_ok, ctx), "([id] BETWEEN 1 AND 10)");

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
//==============================================================================
// Spec 076: constants as parameters, declared from the column
//==============================================================================

namespace {

MSSQLColumnInfo Col(const std::string &name, int32_t id, const std::string &sql_type, int16_t max_length,
					uint8_t precision, uint8_t scale, const std::string &collation = "") {
	return MSSQLColumnInfo(name, id, sql_type, max_length, precision, scale, true, collation,
						   "SQL_Latin1_General_CP1_CI_AS");
}

}  // namespace

// Without a sink the text is the literal form it always was; with one, every
// constant is @pN and the declaration comes from the column on the other side
// of the comparison -- the fixture's `name` is varchar(20), `id` is int.
static void TestParameterSinkDeclaresFromColumn() {
	std::cout << "  TestParameterSinkDeclaresFromColumn..." << std::endl;
	Fixture fx;
	std::vector<MSSQLColumnInfo> cols = {Col("id", 1, "int", 4, 10, 0), Col("dt", 2, "datetime2", 8, 27, 7),
										 Col("flag", 3, "bit", 1, 1, 0), Col("name", 4, "varchar", 20, 0, 0),
										 Col("d", 5, "float", 8, 53, 0)};
	auto literal_ctx = fx.Context();
	literal_ctx.mssql_columns = &cols;
	auto lit =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_NAME), Const(Value("ab")));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*lit, literal_ctx), "([name] = N'ab')");

	SqlParamSet params;
	auto ctx = fx.Context();
	ctx.mssql_columns = &cols;
	ctx.params = &params;
	auto cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_NAME), Const(Value("ab")));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp, ctx), "([name] = @p0)");
	ASSERT_TRUE(params.params.size() == 1);
	ASSERT_TRUE(params.params[0].declaration == "varchar(20)");
	ASSERT_TRUE(params.params[0].literal == "N'ab'");

	// the constant on the LEFT, the column on the right: same peer
	auto flipped = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, Const(Value::INTEGER(5)),
													 ColRef(fx, COL_ID));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*flipped, ctx), "(@p1 > [id])");
	ASSERT_TRUE(params.params[1].declaration == "int");

	// a BIGINT constant against the int column widens the declaration
	auto wide = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_ID),
												  Const(Value::BIGINT(3000000000LL)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*wide, ctx), "([id] = @p2)");
	ASSERT_TRUE(params.params[2].declaration == "bigint");

	// a constant with no column peer (inside an expression) is declared from
	// its own type; a NULL stays a literal and registers nothing
	auto year = Call1("year", LogicalType::TIMESTAMP_NS, LogicalType::BIGINT, ColRef(fx, COL_DT));
	auto yc =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(year), Const(Value::BIGINT(2024)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*yc, ctx), "(YEAR([dt]) = @p3)");
	ASSERT_TRUE(params.params[3].declaration == "bigint");
	ASSERT_TRUE(params.params.size() == 4);

	// a refused expression leaves nothing behind: the left constant registers,
	// the right side (a function the encoder has no mapping for) refuses, and
	// the sink is back to its size before the comparison
	{
		auto unmapped = Call1("no_such_function", LogicalType::INTEGER, LogicalType::INTEGER, ColRef(fx, COL_ID));
		auto refused = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, Const(Value::INTEGER(7)),
														 std::move(unmapped));
		const size_t before = params.params.size();
		ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*refused, ctx));
		ASSERT_TRUE(params.params.size() == before);
	}

	ASSERT_TRUE(params.ExecuteSqlBatch("SELECT 1") ==
				"DECLARE @p0 varchar(20) = N'ab', @p1 int = 5, @p2 bigint = 3000000000, @p3 bigint = 2024;\n"
				"EXEC sp_executesql N'SELECT 1', N'@p0 varchar(20), @p1 int, @p2 bigint, @p3 bigint', "
				"@p0 = @p0, @p1 = @p1, @p2 = @p2, @p3 = @p3");
}

//==============================================================================
// Test: IN / NOT IN as operator expressions (#366, #367 review). The combiner
// builds a table filter only for a bare-column IN; NOT IN — which DuckDB binds
// as NOT over COMPARE_IN — and IN over an expression operand reach the
// encoder as BoundOperatorExpressions and used to be refused. Pinned here
// without a server: the SQL text, the declarations, the NULL item, the
// all-or-nothing rule, and the length cap.
//==============================================================================
static unique_ptr<BoundOperatorExpression> InList(ExpressionType type, unique_ptr<Expression> operand,
												  std::vector<Value> items) {
	auto in = make_uniq<BoundOperatorExpression>(type, LogicalType::BOOLEAN);
	in->GetChildrenMutable().push_back(std::move(operand));
	for (auto &v : items) {
		in->GetChildrenMutable().push_back(Const(std::move(v)));
	}
	return in;
}

static void TestInListOperatorExpressions() {
	std::cout << "  TestInListOperatorExpressions..." << std::endl;
	Fixture fx;
	std::vector<MSSQLColumnInfo> cols = {Col("id", 1, "int", 4, 10, 0), Col("dt", 2, "datetime2", 8, 27, 7),
										 Col("flag", 3, "bit", 1, 1, 0), Col("name", 4, "varchar", 20, 0, 0),
										 Col("d", 5, "float", 8, 53, 0)};
	SqlParamSet params;
	auto ctx = fx.Context();
	ctx.mssql_columns = &cols;
	ctx.params = &params;

	// NOT over COMPARE_IN — the shape DuckDB binds `v NOT IN (...)` to — with
	// the list declared from the operand's column: varchar(20), not nvarchar.
	auto not_in = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	not_in->GetChildrenMutable().push_back(
		InList(ExpressionType::COMPARE_IN, ColRef(fx, COL_NAME), {Value("ab"), Value("x")}));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*not_in, ctx), "(NOT ([name] IN (@p0, @p1)))");
	ASSERT_TRUE(params.params.size() == 2);
	ASSERT_TRUE(params.params[0].declaration == "varchar(20)");
	ASSERT_TRUE(params.params[1].declaration == "varchar(20)");

	// COMPARE_NOT_IN rendered directly, should a plan ever carry it.
	auto direct = InList(ExpressionType::COMPARE_NOT_IN, ColRef(fx, COL_ID), {Value::INTEGER(1), Value::INTEGER(2)});
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*direct, ctx), "([id] NOT IN (@p2, @p3))");
	ASSERT_TRUE(params.params[2].declaration == "int");

	// An expression operand: no table filter exists for it, so this path is
	// the only way it reaches the server; the list constants have no column
	// peer and are declared from their own type.
	auto plus = Call2("+", LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER, ColRef(fx, COL_ID),
					  Const(Value::INTEGER(1)));
	auto expr_in = InList(ExpressionType::COMPARE_IN, std::move(plus), {Value::INTEGER(2), Value::INTEGER(3)});
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*expr_in, ctx), "(([id] + @p4) IN (@p5, @p6))");

	// A NULL item stays a literal NULL and registers no parameter.
	const size_t before_null = params.params.size();
	auto with_null =
		InList(ExpressionType::COMPARE_IN, ColRef(fx, COL_NAME), {Value("ab"), Value(LogicalType::VARCHAR)});
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*with_null, ctx), "([name] IN (@p7, NULL))");
	ASSERT_TRUE(params.params.size() == before_null + 1);

	// An item the encoder cannot render refuses the WHOLE list — a partial
	// list is never pushed — and leaves no parameter behind.
	{
		auto bad = InList(ExpressionType::COMPARE_IN, ColRef(fx, COL_ID), {Value::INTEGER(1)});
		bad->GetChildrenMutable().push_back(
			Call1("no_such_function", LogicalType::INTEGER, LogicalType::INTEGER, ColRef(fx, COL_ID)));
		const size_t before = params.params.size();
		ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*bad, ctx));
		ASSERT_TRUE(params.params.size() == before);
	}

	// The length cap: 256 items push, 257 stay client-side.
	{
		std::vector<Value> ok_items, long_items;
		for (int i = 0; i < 256; i++) {
			ok_items.push_back(Value::INTEGER(i));
		}
		long_items = ok_items;
		long_items.push_back(Value::INTEGER(256));
		SqlParamSet cap_params;
		auto cap_ctx = fx.Context();
		cap_ctx.mssql_columns = &cols;
		cap_ctx.params = &cap_params;
		auto ok = InList(ExpressionType::COMPARE_NOT_IN, ColRef(fx, COL_ID), ok_items);
		ASSERT_TRUE(FilterEncoder::EncodeSearchCondition(*ok, cap_ctx).supported);
		ASSERT_TRUE(cap_params.params.size() == 256);
		auto too_long = InList(ExpressionType::COMPARE_NOT_IN, ColRef(fx, COL_ID), long_items);
		ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*too_long, cap_ctx));
		ASSERT_TRUE(cap_params.params.size() == 256);
	}
}

// The declaration table of spec 076 W1: the column's kind, never narrower
// than the constant.
static void TestDeclarationForColumn() {
	std::cout << "  TestDeclarationForColumn..." << std::endl;
	auto decl = [](const MSSQLColumnInfo &c, const Value &v) {
		return FilterEncoder::DeclarationForColumn(c, v, v.type());
	};
	// strings: the column's kind and length, widened to the constant
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0), Value("ab")) == "varchar(20)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0), Value("abcdefghijklmnopqrstuvwxy")) == "varchar(25)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", -1, 0, 0), Value("ab")) == "varchar(max)");
	ASSERT_TRUE(decl(Col("c", 1, "char", 4, 0, 0), Value("ab")) == "varchar(4)");
	ASSERT_TRUE(decl(Col("n", 1, "nvarchar", 20, 0, 0), Value("ab")) == "nvarchar(10)");
	ASSERT_TRUE(decl(Col("n", 1, "nvarchar", 20, 0, 0), Value("\xC3\xBC\xC3\xB1\xC3\xAF")) == "nvarchar(10)");
	ASSERT_TRUE(decl(Col("n", 1, "nvarchar", 4, 0, 0), Value("\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80")) ==
				"nvarchar(6)");
	// a non-ASCII constant goes as nvarchar whatever the column's collation, as
	// the N'...' literal always did: a varchar variable takes the DATABASE's
	// code page, so even a UTF-8 column would see '?' through a varchar one
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0), Value("\xC3\xBC")) == "nvarchar(20)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "Latin1_General_100_BIN2_UTF8"), Value("\xC3\xBC")) ==
				"nvarchar(20)");
	ASSERT_TRUE(decl(Col("t", 1, "text", 16, 0, 0), Value("x")) == "varchar(max)");
	// integers: the wider of column and constant
	ASSERT_TRUE(decl(Col("i", 1, "int", 4, 10, 0), Value::INTEGER(1)) == "int");
	ASSERT_TRUE(decl(Col("i", 1, "int", 4, 10, 0), Value::BIGINT(1)) == "bigint");
	ASSERT_TRUE(decl(Col("b", 1, "bigint", 8, 19, 0), Value::INTEGER(1)) == "bigint");
	ASSERT_TRUE(decl(Col("t", 1, "tinyint", 1, 3, 0), Value::TINYINT(-1)) == "smallint");
	ASSERT_TRUE(decl(Col("f", 1, "bit", 1, 1, 0), Value::BOOLEAN(true)) == "bit");
	ASSERT_TRUE(decl(Col("i", 1, "int", 4, 10, 0), Value::HUGEINT(1)) == "decimal(38,0)");
	// a HUGEINT past decimal(38,0) is refused by name, not sent to the server
	{
		bool named = false;
		try {
			decl(Col("i", 1, "int", 4, 10, 0), Value::HUGEINT(Hugeint::POWERS_OF_TEN[38]));
		} catch (const InvalidInputException &e) {
			named = std::string(e.what()).find("decimal(38,0)") != std::string::npos;
		}
		ASSERT_TRUE(named);
	}
	// every integer rank declares what DeclarationForValue would for the same
	// constant against a bit column (the column never wins there)
	{
		const Value samples[] = {Value::BOOLEAN(true), Value::UTINYINT(1), Value::TINYINT(1),  Value::SMALLINT(1),
								 Value::USMALLINT(1),  Value::INTEGER(1),  Value::UINTEGER(1), Value::BIGINT(1),
								 Value::UBIGINT(1),	   Value::HUGEINT(1),  Value::UHUGEINT(1)};
		for (const auto &v : samples) {
			ASSERT_TRUE(decl(Col("b", 1, "bit", 1, 1, 0), v) == DeclarationForValue("p", v.type(), v));
		}
	}
	// decimals: the wider precision and scale
	// integer digits AND scale are each the wider of the two sides: decimal(9,2)
	// has 7 integer digits, DECIMAL(10,4) has 6 -> 7 + 4 = decimal(11,4)
	ASSERT_TRUE(decl(Col("d", 1, "decimal", 5, 9, 2), Value::DECIMAL(int64_t(15000), 10, 4)) == "decimal(11,4)");
	ASSERT_TRUE(decl(Col("d", 1, "numeric", 5, 9, 2), Value::DECIMAL(int64_t(150), 4, 1)) == "decimal(9,2)");
	// the review's case: decimal(10,8) against DECIMAL(11,1) needs ten integer
	// digits and eight of scale -- decimal(18,8), not decimal(11,8)
	ASSERT_TRUE(decl(Col("d", 1, "decimal", 9, 10, 8), Value::DECIMAL(int64_t(12345678901), 11, 1)) == "decimal(18,8)");
	ASSERT_TRUE(decl(Col("d", 1, "decimal", 17, 38, 10), Value::DECIMAL(int64_t(1), 38, 0)) == "decimal(38,10)");
	ASSERT_TRUE(decl(Col("d", 1, "decimal", 5, 9, 2), Value::INTEGER(2)) == "int");
	ASSERT_TRUE(decl(Col("m", 1, "money", 8, 19, 4), Value::DECIMAL(int64_t(150), 4, 2)) == "money");
	// floats: value-driven (declaring a DOUBLE as real would round it)
	ASSERT_TRUE(decl(Col("r", 1, "real", 4, 24, 0), Value::DOUBLE(1.5)) == "float");
	ASSERT_TRUE(decl(Col("r", 1, "real", 4, 24, 0), Value::FLOAT(1.5f)) == "real");
	// temporals: the column's kind at the wider scale
	ASSERT_TRUE(decl(Col("dt", 1, "date", 3, 10, 0), Value(LogicalType::DATE)) == "date");
	ASSERT_TRUE(decl(Col("ts", 1, "datetime2", 7, 23, 3), Value(LogicalType::TIMESTAMP)) == "datetime2(6)");
	ASSERT_TRUE(decl(Col("ts", 1, "datetime2", 7, 23, 3), Value(LogicalType::TIMESTAMP_MS)) == "datetime2(3)");
	ASSERT_TRUE(decl(Col("ts", 1, "datetime2", 8, 27, 7), Value(LogicalType::TIMESTAMP)) == "datetime2(7)");
	ASSERT_TRUE(decl(Col("tm", 1, "time", 4, 12, 3), Value(LogicalType::TIME)) == "time(6)");
	ASSERT_TRUE(decl(Col("o", 1, "datetimeoffset", 9, 30, 3), Value(LogicalType::TIMESTAMP_TZ)) == "datetimeoffset(6)");
	ASSERT_TRUE(decl(Col("d", 1, "datetime", 8, 23, 3), Value(LogicalType::TIMESTAMP)) == "datetime2(6)");
	// the rest
	ASSERT_TRUE(decl(Col("u", 1, "uniqueidentifier", 16, 0, 0), Value(LogicalType::UUID)) == "uniqueidentifier");
	ASSERT_TRUE(decl(Col("b", 1, "varbinary", 8, 0, 0), Value::BLOB_RAW(std::string("\x01\x02", 2))) == "varbinary(8)");
	ASSERT_TRUE(decl(Col("b", 1, "varbinary", -1, 0, 0), Value::BLOB_RAW(std::string("\x01", 1))) == "varbinary(max)");
	// rowversion, whose sys.types name is `timestamp`. It became a KNOWN type
	// with issue #296 — the scan no longer casts it, because SQL Server refuses
	// the cast (error 529) — so the constant is now a parameter declared from
	// the column, `varbinary(8)`, and the branch in DeclarationForColumn that
	// has always named `timestamp`/`rowversion` is finally reachable.
	ASSERT_TRUE(decl(Col("rv", 1, "timestamp", 8, 0, 0), Value::BLOB_RAW(std::string("\x01", 1))) == "varbinary(8)");
	// no parameter form: stays a literal
	ASSERT_TRUE(decl(Col("x", 1, "xml", -1, 0, 0), Value("<a/>")).empty());
	ASSERT_TRUE(decl(Col("g", 1, "geography", -1, 0, 0), Value("x")).empty());
	ASSERT_TRUE(decl(Col("s", 1, "sql_variant", 8016, 0, 0), Value::INTEGER(1)).empty());
}

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
	TestParameterSinkDeclaresFromColumn();
	TestInListOperatorExpressions();
	TestDeclarationForColumn();

	std::cout << "All FilterEncoder tests PASSED!" << std::endl;
	return 0;
}

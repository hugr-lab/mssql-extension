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

#include "catalog/mssql_code_page.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "pushdown/mssql_expression_vocabulary.hpp"
#include "query/mssql_identifier.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/filter_encoder.hpp"
#include "table_scan/function_mapping.hpp"

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

	// FUNCTION ARGUMENT — year(id > 5). A MAPPED function, so the refusal is
	// the argument's, not the mapping's (lower/upper are no longer mapped,
	// review of #387).
	auto inner2 = BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_ID),
													Const(Value::INTEGER(5)));
	auto fn_arg = Call1("year", LogicalType::TIMESTAMP, LogicalType::BIGINT, std::move(inner2));
	auto fn_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(fn_arg), Const(Value::BIGINT(1)));
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

	// A mapped function over a column still encodes (TestDatePartOverPrecisionCast covers
	// year() in full). lower/upper/trim are refused as functions: the server's
	// case mapping is not DuckDB's -- UPPER('ß') stays 'ß' -- so a pushed
	// comparison lost rows (review of #387).
	auto lower_fn = Call1("lower", LogicalType::VARCHAR, LogicalType::VARCHAR, ColRef(fx, COL_NAME));
	auto lower_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(lower_fn), Const(Value("x")));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*lower_cmp, ctx));
	auto upper_fn = Call1("upper", LogicalType::VARCHAR, LogicalType::VARCHAR, ColRef(fx, COL_NAME));
	auto upper_cmp =
		BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(upper_fn), Const(Value("X")));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*upper_cmp, ctx));
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

// The fixture's database is SQL_Latin1_General_CP1_CI_AS, code page 1252; the
// pages come from the server with the metadata (COLLATIONPROPERTY, #361), so
// the test sets them the way the loaders do.
MSSQLColumnInfo Col(const std::string &name, int32_t id, const std::string &sql_type, int16_t max_length,
					uint8_t precision, uint8_t scale, const std::string &collation = "", int32_t code_page = 1252) {
	MSSQLColumnInfo c(name, id, sql_type, max_length, precision, scale, true, collation,
					  "SQL_Latin1_General_CP1_CI_AS");
	c.code_page = code_page;
	c.database_code_page = 1252;
	return c;
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

// Issue #426: a constant compared with a DATETIME column is the tick that
// decides the comparison, declared datetime -- `=` the tick read as the
// constant, `>` / `<=` the one read at or below it, `>=` / `<` the one at or
// above. `=` between two reads, a bound before 1753 and any column but a
// datetime one keep the declaration from the value (datetime2(6)).
static void TestDatetimeConstantOnTick() {
	std::cout << "  TestDatetimeConstantOnTick..." << std::endl;
	Fixture fx;
	fx.column_types[COL_DT] = LogicalType::TIMESTAMP;  // a datetime column reads as TIMESTAMP
	std::vector<MSSQLColumnInfo> cols = {Col("id", 1, "int", 4, 10, 0), Col("dt", 2, "datetime", 8, 23, 3),
										 Col("flag", 3, "bit", 1, 1, 0), Col("name", 4, "varchar", 20, 0, 0),
										 Col("d", 5, "float", 8, 53, 0)};
	const auto at = [](int32_t us) {
		return Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 1), Time::FromTime(10, 0, 0, us)));
	};
	const auto cmp = [&](ExpressionType type, Value c, bool constant_left = false) {
		return constant_left ? BoundComparisonExpression::Create(type, Const(std::move(c)), ColRef(fx, COL_DT))
							 : BoundComparisonExpression::Create(type, ColRef(fx, COL_DT), Const(std::move(c)));
	};

	SqlParamSet params;
	auto ctx = fx.Context();
	ctx.mssql_columns = &cols;
	ctx.params = &params;
	const auto expect = [&](const unique_ptr<Expression> &expr, const std::string &sql, const std::string &declaration,
							const Value &value) {
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*expr, ctx), sql);
		ASSERT_TRUE(params.params.back().declaration == declaration);
		ASSERT_TRUE(Value::NotDistinctFrom(params.params.back().value, value));
	};
	// .003333 is what the scan reads for the server's .00333...: its own tick.
	expect(cmp(ExpressionType::COMPARE_EQUAL, at(3333)), "([dt] = @p0)", "datetime", at(3333));
	expect(cmp(ExpressionType::COMPARE_NOTEQUAL, at(6666)), "([dt] <> @p1)", "datetime", at(6666));
	// .005 lies between the reads .003333 and .006666.
	expect(cmp(ExpressionType::COMPARE_GREATERTHAN, at(5000)), "([dt] > @p2)", "datetime", at(3333));
	expect(cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, at(5000)), "([dt] <= @p3)", "datetime", at(3333));
	expect(cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, at(5000)), "([dt] >= @p4)", "datetime", at(6666));
	expect(cmp(ExpressionType::COMPARE_LESSTHAN, at(5000)), "([dt] < @p5)", "datetime", at(6666));
	// the constant on the left: `.005 < dt` is `dt > .005`
	expect(cmp(ExpressionType::COMPARE_LESSTHAN, at(5000), true), "(@p6 < [dt])", "datetime", at(3333));
	// `=` between two reads: no read equals it. Sent as a whole millisecond
	// plus one microsecond, which no datetime equals at any compatibility
	// level (below 130 the server converts .003 to .0030000).
	expect(cmp(ExpressionType::COMPARE_EQUAL, at(5000)), "([dt] = @p7)", "datetime2(6)", at(5001));
	// a bound before the datetime range: every value is above it either way
	const auto early = Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(1700, 1, 1), dtime_t(0)));
	expect(cmp(ExpressionType::COMPARE_GREATERTHAN, early), "([dt] > @p8)", "datetime2(6)", early);
	// BETWEEN: each bound as its own comparison
	{
		auto between = BoundBetweenExpression::Create(ColRef(fx, COL_DT), Const(at(3334)), Const(at(9999)), true, true);
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*between, ctx), "([dt] BETWEEN @p9 AND @p10)");
		ASSERT_TRUE(Value::NotDistinctFrom(params.params[9].value, at(6666)));
		ASSERT_TRUE(Value::NotDistinctFrom(params.params[10].value, at(6666)));
	}
	// IN: each item an equality
	{
		vector<unique_ptr<Expression>> children;
		children.push_back(ColRef(fx, COL_DT));
		children.push_back(Const(at(3333)));
		children.push_back(Const(at(5000)));
		auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
		in->GetChildrenMutable() = std::move(children);
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*in, ctx), "([dt] IN (@p11, @p12))");
		ASSERT_TRUE(params.params[11].declaration == "datetime");
		ASSERT_TRUE(params.params[12].declaration == "datetime2(6)");
	}
	// A datetime2 column is compared as it was.
	{
		auto dt2 = cols;
		dt2[COL_DT] = Col("dt", 2, "datetime2", 8, 27, 6);
		auto dt2_ctx = fx.Context();
		dt2_ctx.mssql_columns = &dt2;
		dt2_ctx.params = &params;
		auto e = cmp(ExpressionType::COMPARE_GREATERTHAN, at(5000));
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*e, dt2_ctx), "([dt] > @p13)");
		ASSERT_TRUE(params.params[13].declaration == "datetime2(6)");
		ASSERT_TRUE(Value::NotDistinctFrom(params.params[13].value, at(5000)));
	}

	// `>=` past the last read (9999-12-31 23:59:59.996666): every datetime is
	// below 23:59:59.999999 too.
	{
		const auto late =
			Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(9999, 12, 31), Time::FromTime(23, 59, 59, 996667)));
		const auto end =
			Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(9999, 12, 31), Time::FromTime(23, 59, 59, 999999)));
		expect(cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, late), "([dt] >= @p14)", "datetime2(6)", end);
	}

	// Without a sink: the bound converted to datetime in the text.
	auto literal_ctx = fx.Context();
	literal_ctx.mssql_columns = &cols;
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_GREATERTHAN, at(5000)), literal_ctx),
			   "([dt] > CAST(CAST('2026-01-01T10:00:00.0033330' AS DATETIME2(7)) AS datetime))");
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_EQUAL, at(5000)), literal_ctx),
			   "([dt] = CAST('2026-01-01T10:00:00.0050010' AS DATETIME2(7)))");
}

// Issue #426: a datetime column meets on the server only constants and other
// datetime columns. Against a datetime2 column, under a CASE, or as the IN
// list of a constant, the server would compare its exact 1/300 s value with a
// microsecond one, so the predicate is refused and DuckDB evaluates it.
static void TestDatetimeAgainstOtherValuesRefused() {
	std::cout << "  TestDatetimeAgainstOtherValuesRefused..." << std::endl;
	Fixture fx;
	fx.column_types[COL_DT] = LogicalType::TIMESTAMP;
	fx.column_types[COL_D] = LogicalType::TIMESTAMP;
	std::vector<MSSQLColumnInfo> cols = {Col("id", 1, "int", 4, 10, 0), Col("dt", 2, "datetime", 8, 23, 3),
										 Col("flag", 3, "bit", 1, 1, 0), Col("name", 4, "varchar", 20, 0, 0),
										 Col("d", 5, "datetime2", 8, 26, 6)};
	auto ctx = fx.Context();
	ctx.mssql_columns = &cols;
	const auto c =
		Value::TIMESTAMP(Timestamp::FromDatetime(Date::FromDate(2026, 1, 1), Time::FromTime(10, 0, 0, 3333)));

	// against a datetime2 column, either side
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_DT), ColRef(fx, COL_D)), ctx));
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_D), ColRef(fx, COL_DT)),
		ctx));
	// a constant whose IN list holds the datetime column
	{
		auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
		in->GetChildrenMutable().push_back(Const(c));
		in->GetChildrenMutable().push_back(ColRef(fx, COL_DT));
		ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(*in, ctx));
	}
	// a CASE over the column
	{
		auto when = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_ID),
													  Const(Value::INTEGER(1)));
		auto case_expr = make_uniq<BoundCaseExpression>(std::move(when), ColRef(fx, COL_DT), ColRef(fx, COL_DT));
		ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
			*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(case_expr), Const(c)), ctx));
	}
	// BETWEEN with a datetime2 bound
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundBetweenExpression::Create(ColRef(fx, COL_DT), Const(c), ColRef(fx, COL_D), true, true), ctx));

	// Still pushed: two datetime columns, a date part, a datetime2 column alone.
	auto both = cols;
	both[COL_D] = Col("d", 5, "datetime", 8, 23, 3);
	auto both_ctx = fx.Context();
	both_ctx.mssql_columns = &both;
	ASSERT_SQL(
		FilterEncoder::EncodeSearchCondition(
			*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_DT), ColRef(fx, COL_D)),
			both_ctx),
		"([dt] = [d])");
	auto year = Call1("year", LogicalType::TIMESTAMP, LogicalType::BIGINT, ColRef(fx, COL_DT));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(year),
													  Const(Value::BIGINT(2026))),
				   ctx),
			   "(YEAR([dt]) = 2026)");
	ASSERT_SQL(
		FilterEncoder::EncodeSearchCondition(
			*BoundComparisonExpression::Create(ExpressionType::COMPARE_GREATERTHAN, ColRef(fx, COL_D), Const(c)), ctx),
		"([d] > CAST('2026-01-01T10:00:00.0033330' AS DATETIME2(7)))");
}

// Spec 080 PR 4b: a ColumnResolver answers for every column reference -- two
// relations here, table 1 as [t] and table 2 as [s] -- in place of the scan's
// projection, and its column is the peer a constant is declared from and the
// one #426's datetime rules look at.
namespace {
struct TwoRelationResolver : public ColumnResolver {
	std::vector<MSSQLColumnInfo> target;
	std::vector<MSSQLColumnInfo> source;
	bool Resolve(const BoundColumnRefExpression &expr, std::string &sql,
				 const MSSQLColumnInfo *&column) const override {
		const auto &binding = expr.Binding();
		const bool is_target = binding.table_index.index == 1;
		const auto &cols = is_target ? target : source;
		if (binding.column_index >= cols.size()) {
			return false;
		}
		column = &cols[binding.column_index];
		sql = is_target ? "[t]." + mssql::QuoteIdentifier(column->name)
						: "[s].[c" + std::to_string(binding.column_index) + "]";
		return true;
	}
};

unique_ptr<Expression> RelRef(idx_t table, idx_t col, const LogicalType &type) {
	return make_uniq<BoundColumnRefExpression>(type, ColumnBinding(TableIndex(table), ProjectionIndex(col)));
}
}  // namespace

static void TestColumnResolver() {
	std::cout << "  TestColumnResolver..." << std::endl;
	TwoRelationResolver resolver;
	resolver.target = {Col("id", 1, "int", 4, 10, 0), Col("name", 2, "varchar", 20, 0, 0),
					   Col("dt", 3, "datetime", 8, 23, 3)};
	resolver.source = {Col("c0", 1, "int", 4, 10, 0), Col("c1", 2, "datetime2", 8, 26, 6)};
	// The scan's own fields are empty: nothing may be read through them.
	std::vector<column_t> no_ids;
	std::vector<std::string> no_names;
	std::vector<LogicalType> no_types;
	SqlParamSet params;
	ExpressionEncodeContext ctx(no_ids, no_names, no_types);
	ctx.resolver = &resolver;
	ctx.params = &params;

	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, RelRef(1, 0, LogicalType::INTEGER),
													  RelRef(2, 0, LogicalType::INTEGER)),
				   ctx),
			   "([t].[id] = [s].[c0])");

	// the peer comes from the resolver: varchar(20), not the constant's nvarchar
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, RelRef(1, 1, LogicalType::VARCHAR),
													  Const(Value("ab"))),
				   ctx),
			   "([t].[name] = @p0)");
	ASSERT_TRUE(params.params.size() == 1);
	ASSERT_TRUE(params.params[0].declaration == "varchar(20)");

	// children inherit it: a reference under a function
	auto year = Call1("year", LogicalType::TIMESTAMP, LogicalType::BIGINT, RelRef(1, 2, LogicalType::TIMESTAMP));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, std::move(year),
													  Const(Value::BIGINT(2026))),
				   ctx),
			   "(YEAR([t].[dt]) = @p1)");

	// #426: a datetime column against a datetime2 one is refused, whichever
	// relation each comes from
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, RelRef(1, 2, LogicalType::TIMESTAMP),
										   RelRef(2, 1, LogicalType::TIMESTAMP)),
		ctx));

	// a reference the resolver refuses refuses the expression and leaves no
	// parameter behind: the constant on the left registers one first
	const size_t before = params.params.size();
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, Const(Value::INTEGER(1)),
										   RelRef(2, 7, LogicalType::INTEGER)),
		ctx));
	ASSERT_TRUE(params.params.size() == before);
}

// Spec 080 PR 4b: with widening_casts, an exact numeric widening cast is
// written as CAST(... AS ...); without it, and for any cast that is not one,
// the expression is refused as before.
static unique_ptr<Expression> CastTo(unique_ptr<Expression> child, const LogicalType &type) {
	return BoundCastExpression::AddDefaultCastToType(std::move(child), type);
}

static void TestWideningCasts() {
	std::cout << "  TestWideningCasts..." << std::endl;
	Fixture fx;
	auto ctx = fx.Context();
	auto plus = [&](unique_ptr<Expression> lhs, const LogicalType &type) {
		return Call2("+", type, type, type, std::move(lhs), Const(Value::BIGINT(5).DefaultCastAs(type)));
	};
	// off: refused
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(
		*plus(CastTo(ColRef(fx, COL_ID), LogicalType::BIGINT), LogicalType::BIGINT), ctx));
	ctx.widening_casts = true;
	ASSERT_SQL(FilterEncoder::EncodeExpression(
				   *plus(CastTo(ColRef(fx, COL_ID), LogicalType::BIGINT), LogicalType::BIGINT), ctx),
			   "(CAST([id] AS bigint) + 5)");
	ASSERT_SQL(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::HUGEINT), ctx),
			   "CAST([id] AS decimal(38,0))");
	ASSERT_SQL(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::DECIMAL(12, 2)), ctx),
			   "CAST([id] AS decimal(12,2))");
	// not widening: narrower, a decimal that cannot hold every int, a float
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::SMALLINT), ctx));
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::DECIMAL(9, 0)), ctx));
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::DOUBLE), ctx));
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_D), LogicalType::DECIMAL(38, 2)), ctx));
	// unsigned: into a wider signed integer, or a decimal for UBIGINT
	auto uref = make_uniq<BoundColumnRefExpression>(LogicalType::UINTEGER,
													ColumnBinding(TableIndex(0), ProjectionIndex(COL_ID)));
	ASSERT_SQL(FilterEncoder::EncodeExpression(*CastTo(uref->Copy(), LogicalType::BIGINT), ctx),
			   "CAST([id] AS bigint)");
	ASSERT_SQL(FilterEncoder::EncodeExpression(*CastTo(uref->Copy(), LogicalType::UBIGINT), ctx),
			   "CAST([id] AS decimal(20,0))");
	ASSERT_REFUSED(FilterEncoder::EncodeExpression(*CastTo(uref->Copy(), LogicalType::INTEGER), ctx));
	// children inherit it
	auto child = ctx.child();
	ASSERT_SQL(FilterEncoder::EncodeExpression(*CastTo(ColRef(fx, COL_ID), LogicalType::BIGINT), child),
			   "CAST([id] AS bigint)");
}

// Issue #426: time(7) / datetimeoffset(7) are read with their 100 ns digit
// dropped, so the read equals c for every stored value in [c, c + 1 us): `=`
// is that range, `>` starts at c + 1 us, `<=` ends before it, `>=` / `<` stay.
// At the last microsecond c + 1 us is never sent (an overflow on the server).
static void TestTruncatedReadComparisons() {
	std::cout << "  TestTruncatedReadComparisons..." << std::endl;
	Fixture fx;
	fx.column_types[COL_DT] = LogicalType::TIME;
	fx.column_types[COL_D] = LogicalType::TIMESTAMP_TZ;
	std::vector<MSSQLColumnInfo> cols = {Col("id", 1, "int", 4, 10, 0), Col("dt", 2, "time", 5, 16, 7),
										 Col("flag", 3, "bit", 1, 1, 0), Col("name", 4, "varchar", 20, 0, 0),
										 Col("d", 5, "datetimeoffset", 10, 34, 7)};
	const auto t = [](int32_t h, int32_t m, int32_t sec, int32_t us) {
		return Value::TIME(Time::FromTime(h, m, sec, us));
	};
	const auto cmp = [&](ExpressionType type, idx_t col, Value c, bool constant_left = false) {
		return constant_left ? BoundComparisonExpression::Create(type, Const(std::move(c)), ColRef(fx, col))
							 : BoundComparisonExpression::Create(type, ColRef(fx, col), Const(std::move(c)));
	};

	SqlParamSet params;
	auto ctx = fx.Context();
	ctx.mssql_columns = &cols;
	ctx.params = &params;
	ASSERT_SQL(
		FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_EQUAL, COL_DT, t(10, 0, 0, 123456)), ctx),
		"([dt] >= @p0 AND [dt] < @p1)");
	ASSERT_TRUE(params.params[0].declaration == "time(7)");
	ASSERT_TRUE(Value::NotDistinctFrom(params.params[0].value, t(10, 0, 0, 123456)));
	ASSERT_TRUE(Value::NotDistinctFrom(params.params[1].value, t(10, 0, 0, 123457)));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *cmp(ExpressionType::COMPARE_GREATERTHAN, COL_DT, t(10, 0, 0, 123456)), ctx),
			   "([dt] >= @p2)");
	ASSERT_TRUE(Value::NotDistinctFrom(params.params[2].value, t(10, 0, 0, 123457)));
	// the constant on the left: `c >= dt` is `dt <= c`
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(
				   *cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, COL_DT, t(10, 0, 0, 123456), true), ctx),
			   "([dt] < @p3)");
	ASSERT_TRUE(Value::NotDistinctFrom(params.params[3].value, t(10, 0, 0, 123457)));
	// `<` and `>=` are the stored values' own
	ASSERT_SQL(
		FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_LESSTHAN, COL_DT, t(10, 0, 0, 123456)), ctx),
		"([dt] < @p4)");
	ASSERT_TRUE(Value::NotDistinctFrom(params.params[4].value, t(10, 0, 0, 123456)));
	// the last microsecond: nothing lies past it, and 24:00:00 is never sent
	const auto last = t(23, 59, 59, 999999);
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_GREATERTHAN, COL_DT, last), ctx),
			   "([dt] <> [dt])");
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_LESSTHANOREQUALTO, COL_DT, last), ctx),
			   "([dt] = [dt])");
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_EQUAL, COL_DT, last), ctx),
			   "([dt] >= @p5)");
	// IN: one range per item; BETWEEN: both bounds
	{
		auto in = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_IN, LogicalType::BOOLEAN);
		in->GetChildrenMutable().push_back(ColRef(fx, COL_DT));
		in->GetChildrenMutable().push_back(Const(t(1, 0, 0, 0)));
		in->GetChildrenMutable().push_back(Const(t(2, 0, 0, 0)));
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*in, ctx),
				   "(([dt] >= @p6 AND [dt] < @p7) OR ([dt] >= @p8 AND [dt] < @p9))");
		auto between =
			BoundBetweenExpression::Create(ColRef(fx, COL_DT), Const(t(1, 0, 0, 0)), Const(t(2, 0, 0, 0)), true, true);
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*between, ctx), "(([dt] >= @p10) AND ([dt] < @p11))");
		// a NULL item is three-valued on both sides: `k = NULL`
		auto with_null = make_uniq<BoundOperatorExpression>(ExpressionType::COMPARE_NOT_IN, LogicalType::BOOLEAN);
		with_null->GetChildrenMutable().push_back(ColRef(fx, COL_DT));
		with_null->GetChildrenMutable().push_back(Const(t(1, 0, 0, 0)));
		with_null->GetChildrenMutable().push_back(Const(Value(LogicalType::TIME)));
		ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*with_null, ctx),
				   "(NOT (([dt] >= @p12 AND [dt] < @p13) OR ([dt] = NULL)))");
	}
	// datetimeoffset(7), as a literal: declared datetimeoffset(7) by the column
	auto literal_ctx = fx.Context();
	literal_ctx.mssql_columns = &cols;
	const auto tz = Value::TIMESTAMPTZ(
		timestamp_tz_t(Timestamp::FromDatetime(Date::FromDate(2026, 1, 1), Time::FromTime(10, 0, 0, 123456)).value));
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_EQUAL, COL_D, tz), literal_ctx),
			   "([d] >= CAST('2026-01-01T10:00:00.1234560+00:00' AS DATETIMEOFFSET(7)) AND [d] < "
			   "CAST('2026-01-01T10:00:00.1234570+00:00' AS DATETIMEOFFSET(7)))");
	// against another column of the family: refused, DuckDB evaluates it
	auto both = cols;
	both[COL_D] = Col("d", 5, "time", 5, 16, 7);
	fx.column_types[COL_D] = LogicalType::TIME;
	auto both_ctx = fx.Context();
	both_ctx.mssql_columns = &both;
	ASSERT_REFUSED(FilterEncoder::EncodeSearchCondition(
		*BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, ColRef(fx, COL_DT), ColRef(fx, COL_D)),
		both_ctx));
	// time(6) is read exactly: compared as it always was
	auto exact = cols;
	exact[COL_DT] = Col("dt", 2, "time", 5, 15, 6);
	auto exact_ctx = fx.Context();
	exact_ctx.mssql_columns = &exact;
	ASSERT_SQL(FilterEncoder::EncodeSearchCondition(*cmp(ExpressionType::COMPARE_EQUAL, COL_DT, t(10, 0, 0, 123456)),
													exact_ctx),
			   "([dt] = '10:00:00.1234560')");
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
	// #361: a non-ASCII constant goes as varchar when both the column's and the
	// database's code page (the fixture's database is SQL_Latin1_General_CP1,
	// i.e. 1252) can hold it -- that is what keeps the seek -- and as nvarchar
	// otherwise. ü is in 1252: varchar. ы is not: nvarchar, on a 1252 column
	// and on a UTF-8 column alike, because the varchar PARAMETER takes the
	// database's page (#321's case) whatever the column can hold.
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0), Value("\xC3\xBC")) == "varchar(20)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "Latin1_General_100_BIN2_UTF8", 65001), Value("\xC3\xBC")) ==
				"varchar(20)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0), Value("\xD1\x8B")) == "nvarchar(20)");
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "Latin1_General_100_BIN2_UTF8", 65001), Value("\xD1\x8B")) ==
				"nvarchar(20)");
	// the column's page matters too: ü against a Cyrillic column is nvarchar
	// even though the 1252 database could carry the parameter
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "Cyrillic_General_CI_AS", 1251), Value("\xC3\xBC")) ==
				"nvarchar(20)");
	// a Cyrillic constant on a Cyrillic column of a Cyrillic database: varchar
	{
		MSSQLColumnInfo cyr = Col("v", 1, "varchar", 20, 0, 0, "Cyrillic_General_CI_AS", 1251);
		cyr.database_code_page = 1251;
		ASSERT_TRUE(decl(cyr, Value("\xD1\x8B")) == "varchar(20)");
	}
	// a page the server could not name (NULL -> 0): nvarchar, the old form
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "", 0), Value("\xC3\xBC")) == "nvarchar(20)");
	// text follows the same rule (#361 review): ы on a 1252 page is nvarchar(max)
	ASSERT_TRUE(decl(Col("t", 1, "text", 16, 0, 0), Value("\xD1\x8B")) == "nvarchar(max)");
	ASSERT_TRUE(decl(Col("t", 1, "text", 16, 0, 0), Value("\xC3\xBC")) == "varchar(max)");
	// a double-byte or unknown page cannot be told: nvarchar
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 20, 0, 0, "Japanese_CI_AS", 932), Value("\xC3\xBC")) == "nvarchar(20)");
	// the byte width is still the constant's UTF-8 size when it is the larger
	ASSERT_TRUE(decl(Col("v", 1, "varchar", 2, 0, 0), Value("\xC3\xBC\xC3\xBC")) == "varchar(4)");
	// the page tables
	ASSERT_TRUE(mssql::CodePageCanEncode(1252, "\xE2\x82\xAC"));  // € is 0x80 in 1252
	ASSERT_TRUE(mssql::CodePageCanEncode(1251, "\xE2\x82\xAC"));  // and 0x88 in 1251
	ASSERT_TRUE(!mssql::CodePageCanEncode(1252, "\xC5\x91"));	  // ő: 1250 only
	ASSERT_TRUE(mssql::CodePageCanEncode(1250, "\xC5\x91"));
	ASSERT_TRUE(!mssql::CodePageCanEncode(1252, "\xF0\x9F\x98\x80"));  // above the BMP
	ASSERT_TRUE(mssql::CodePageCanEncode(932, "ascii only"));
	ASSERT_TRUE(mssql::CodePageCanEncode(0, "ascii only"));
	ASSERT_TRUE(!mssql::CodePageCanEncode(850, "\xC3\xBC"));  // an OEM page: no table
	ASSERT_TRUE(!mssql::CodePageCanEncode(932, "\xC3\xBC"));
	ASSERT_TRUE(mssql::CodePageCanEncode(65001, "\xF0\x9F\x98\x80"));
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
	// #392: no case-insensitive LIKE form is pushed (the server's LOWER is not
	// DuckDB's lower); DuckDB has no such names on the 2.0 pin either.
	ASSERT_TRUE(IsLikePatternFunction("contains"));
	ASSERT_TRUE(!IsLikePatternFunction("icontains"));
	ASSERT_TRUE(!IsLikePatternFunction("iprefix"));
	ASSERT_TRUE(!IsLikePatternFunction("isuffix"));

	// Arithmetic operators take numbers only (DuckDB's `date + 1` is a date,
	// T-SQL refuses it: 206); float arithmetic overflows on the server, which
	// has no Inf, where DuckDB says inf -- for + and - as well as *, a narrower
	// value window but the same divergence (roborev 1819 finding 2); a decimal
	// product DuckDB caps at DECIMAL(18) overflows there first (spec 079 C,
	// from PR D's full review).
	{
		std::string why;
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("+", {LogicalType::DATE, LogicalType::INTEGER}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("-", {LogicalType::TIMESTAMP, LogicalType::INTERVAL}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("+", {LogicalType::DOUBLE, LogicalType::DOUBLE}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("-", {LogicalType::DOUBLE, LogicalType::DOUBLE}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("*", {LogicalType::DOUBLE, LogicalType::DOUBLE}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("+", {LogicalType::FLOAT, LogicalType::FLOAT}, why));
		// Exact numbers still push.
		ASSERT_TRUE(ExpressionVocabulary::FunctionFor("+", {LogicalType::INTEGER, LogicalType::INTEGER}, why));
		// The zero-divisor policy is the vocabulary's, so both walkers get it
		// (roborev 1819 finding 3): `x % 0` is NULL in DuckDB with
		// error_on_division_by_zero off, error 8134 on the server.
		ASSERT_TRUE(ExpressionVocabulary::FunctionFor("%", {LogicalType::INTEGER, LogicalType::INTEGER}, why));
		ASSERT_TRUE(!ExpressionVocabulary::FunctionFor("%", {LogicalType::INTEGER, LogicalType::INTEGER}, why,
													   /*division_by_zero_errors=*/false));
		ASSERT_TRUE(
			!ExpressionVocabulary::FunctionFor("*", {LogicalType::DECIMAL(10, 2), LogicalType::DECIMAL(10, 2)}, why));
		ASSERT_TRUE(
			ExpressionVocabulary::FunctionFor("*", {LogicalType::DECIMAL(9, 2), LogicalType::DECIMAL(9, 2)}, why));
		ASSERT_TRUE(ExpressionVocabulary::FunctionFor("*", {LogicalType::INTEGER, LogicalType::INTEGER}, why));
		// DuckDB caps at 18 only while the scale fits: (18,10)^2 is DECIMAL(36,20).
		ASSERT_TRUE(
			ExpressionVocabulary::FunctionFor("*", {LogicalType::DECIMAL(18, 10), LogicalType::DECIMAL(18, 10)}, why));
		// Past 38 digits the server rounds the scale.
		ASSERT_TRUE(
			!ExpressionVocabulary::FunctionFor("*", {LogicalType::DECIMAL(19, 4), LogicalType::DECIMAL(19, 4)}, why));
		ASSERT_TRUE(
			!ExpressionVocabulary::FunctionFor("+", {LogicalType::DECIMAL(38, 10), LogicalType::DECIMAL(38, 10)}, why));
		ASSERT_TRUE(
			ExpressionVocabulary::FunctionFor("+", {LogicalType::DECIMAL(10, 2), LogicalType::DECIMAL(10, 2)}, why));
	}

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
	TestDatetimeConstantOnTick();
	TestDatetimeAgainstOtherValuesRefused();
	TestTruncatedReadComparisons();
	TestInListOperatorExpressions();
	TestDeclarationForColumn();
	TestColumnResolver();
	TestWideningCasts();

	std::cout << "All FilterEncoder tests PASSED!" << std::endl;
	return 0;
}

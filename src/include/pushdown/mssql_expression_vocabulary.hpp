//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// pushdown/mssql_expression_vocabulary.hpp
//
// Spec 079 D1 / D5: the ONE vocabulary of T-SQL expressions this extension
// sends -- what each construct renders as, and what is refused. It knows
// nothing about DuckDB's expression trees: each caller walks its own (the scan
// path's FilterEncoder the bound tree, the remote-pushdown writer the parsed
// one), resolves its own columns (a join qualifies them, a scan does not), and
// keeps its own policy for a refusal -- the scan path pushes the supported
// conjuncts of an AND and leaves the rest to its client-side net, the writer
// vetoes the whole node. What both must agree on lives here: the atoms.
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

struct MSSQLColumnInfo;

namespace mssql {

struct FunctionMapping;
struct SqlParamSet;

//! One rendered expression, or a refusal.
struct ExpressionEncodeResult {
	std::string sql;  // T-SQL fragment (empty if not supported)
	bool supported;	  // True if expression was fully encoded
};

//! A rendered operand, with what the atoms need to know about it.
struct SqlOperand {
	std::string sql;
	//! It renders as a T-SQL search condition (a comparison, a conjunction,
	//! IN / BETWEEN / IS NULL / NOT / LIKE), not as a value.
	bool is_condition = false;
	//! Its DuckDB type; INVALID when the caller does not know it.
	LogicalType type;
};

class ExpressionVocabulary {
public:
	//! Items one IN / NOT IN may carry to the server; a longer list is left to
	//! DuckDB. Per predicate: with parameterisation on, the statement's budget
	//! is SqlParamSet::MAX_PARAMS (constants past it become literals), and with
	//! it off a batch of several capped lists is still bounded by their number.
	static constexpr size_t MAX_IN_ITEMS = 256;

	//===--------------------------------------------------------------------===//
	// Constants
	//===--------------------------------------------------------------------===//
	//! A T-SQL literal for `value` of DuckDB `type`.
	static std::string ValueToSQLLiteral(const Value &value, const LogicalType &type);
	//! The declaration of a parameter compared with `column` (spec 076, #361);
	//! empty when there is none and the constant stays a literal.
	static std::string DeclarationForColumn(const MSSQLColumnInfo &column, const Value &value, const LogicalType &type);
	//! A constant: `@pN` registered in `params` and declared from `peer` (the
	//! column it is compared with) or from the value, or a literal when there
	//! is no sink, no declaration or no budget left.
	static std::string Constant(const Value &value, const LogicalType &type, SqlParamSet *params,
								const MSSQLColumnInfo *peer);

	//===--------------------------------------------------------------------===//
	// Value and condition positions
	//===--------------------------------------------------------------------===//
	//! T-SQL has no boolean value type: a search condition is illegal where a
	//! value is expected (`CASE … THEN [id] > 5`, `LOWER([a] > 1)`, 4145).
	static bool IsValue(const SqlOperand &operand);
	//! A BOOLEAN value is not a T-SQL predicate: `WHERE [b]` errors 4145,
	//! `WHERE ([b] = 1)` is the condition. A condition is returned as is.
	static std::string AsCondition(const SqlOperand &operand);

	//===--------------------------------------------------------------------===//
	// Operators
	//===--------------------------------------------------------------------===//
	//! " = ", " <> ", …; false for a comparison T-SQL does not have.
	static bool ComparisonOperator(ExpressionType type, std::string &out_operator);
	static std::string Comparison(const std::string &op, const std::string &left, const std::string &right);
	static std::string Not(const std::string &condition);
	static std::string IsNull(const std::string &value, bool negated);
	static bool InListFits(size_t items);
	static std::string In(const std::string &operand, const std::vector<std::string> &items, bool negated);
	static std::string Between(const std::string &input, const std::string &lower, const std::string &upper,
							   bool lower_inclusive, bool upper_inclusive);
	//! `whens` are conditions, `thens` and `otherwise` values.
	static std::string Case(const std::vector<std::string> &whens, const std::vector<std::string> &thens,
							const std::string &otherwise);
	//! `a / b` as DuckDB's `/` means it -- floating division, whatever the
	//! operands (5 / 2 = 2.5) -- where T-SQL divides integers as integers:
	//! `CAST(a AS float) / NULLIF(CAST(b AS float), 0)`. A zero divisor is the
	//! one recorded divergence (spec 079 D2): DuckDB gives inf / NaN, SQL
	//! Server's float has no infinity or NaN and a bare `/` raises 8134, so
	//! NULL (for x / 0 and 0 / 0 alike).
	static std::string Divide(const std::string &left, const std::string &right);
	static std::string Coalesce(const std::vector<std::string> &args);
	static std::string NullIf(const std::string &left, const std::string &right);
	//! `CAST(value AS type)`, `type` a T-SQL type name.
	static std::string Cast(const std::string &value, const std::string &type);
	//! Parts already rendered as conditions; one part is returned as is.
	static std::string Conjunction(const std::vector<std::string> &parts, bool is_and);

	//===--------------------------------------------------------------------===//
	// Casts and functions
	//===--------------------------------------------------------------------===//
	//! A cast the server does not need: the MSSQL_VARCHAR / MSSQL_NVARCHAR
	//! labels reach the plain VARCHAR operators through a no-op cast (spec 060).
	static bool IsTransparentCast(const LogicalType &source, const LogicalType &target);
	//! The date-part extractors of the function table.
	static bool IsDatePartFunction(const std::string &name);
	//! A cast between naive-timestamp precisions, which a date part does not
	//! see (spec 070 W1): strippable under a date-part function only.
	static bool IsNaiveTimestampCast(const LogicalType &source, const LogicalType &target);
	//! The mapping for `name` applied to arguments of `arg_types`, or null with
	//! the reason: unmapped, wrong arity, `%` on a non-integer (8117), a date
	//! part of a TIMESTAMP WITH TIME ZONE (the server's offset, not DuckDB's
	//! TimeZone -- review of #387), `+ - *` on a float (the server errors where
	//! DuckDB says inf), a decimal product DuckDB overflows first.
	//!
	//! `division_by_zero_errors` is DuckDB's `error_on_division_by_zero` as the
	//! asking session has it: false answers NULL for `x % 0` there and error
	//! 8134 on the server, so `%` is refused. BOTH walkers pass it -- the gate
	//! is here rather than in one of them because a construct both paths take
	//! must answer the same whichever takes it.
	static const FunctionMapping *FunctionFor(const std::string &name, const std::vector<LogicalType> &arg_types,
											  std::string &why, bool division_by_zero_errors = true);
	static std::string ApplyFunction(const FunctionMapping &mapping, const std::vector<std::string> &args);

	//===--------------------------------------------------------------------===//
	// LIKE
	//===--------------------------------------------------------------------===//
	//! `%`, `_` and `[` taken literally.
	static std::string EscapeLikePattern(const std::string &pattern);
	//! The LIKE pattern text for DuckDB's prefix / suffix / contains over a
	//! constant `needle`; false for another name.
	static bool LikePatternText(const std::string &function_name, const std::string &needle, std::string &out_pattern);
	//! `value LIKE pattern`, evaluated under the value's collation: on a
	//! case-insensitive one it matches case variants, as `=` does (spec 079 D4:
	//! string sets are the server's). No case-insensitive form: the server's
	//! LOWER is not DuckDB's lower (#392).
	static std::string Like(const std::string &value, const std::string &pattern);
	//! A DuckDB LIKE pattern as T-SQL reads it: `%` and `_` mean the same on
	//! both sides and neither has a default escape, but `[` opens a character
	//! class in T-SQL only, so it is taken literally as `[[]`.
	static std::string LikePattern(const std::string &duckdb_pattern);
};

}  // namespace mssql
}  // namespace duckdb

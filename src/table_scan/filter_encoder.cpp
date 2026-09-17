// Filter Encoder Implementation
// Feature: 013-table-scan-filter-refactor
//
// This is the initial minimal implementation for backward compatibility.
// Enhanced expression support (LIKE patterns, functions, CASE, arithmetic)
// will be added in subsequent phases.

#include "table_scan/filter_encoder.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "catalog/mssql_code_page.hpp"
#include "catalog/mssql_column_info.hpp"
#include "codec/literal_format.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/function_mapping.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_FILTER_DEBUG_LOG(level, fmt, ...)                         \
	do {                                                                \
		if (GetDebugLevel() >= level) {                                 \
			fprintf(stderr, "[MSSQL FILTER] " fmt "\n", ##__VA_ARGS__); \
		}                                                               \
	} while (0)

namespace duckdb {
namespace mssql {

namespace {
// A naive (timezone-less) date+time logical type. SQL Server stores all of
// these as DATETIME2; DuckDB models DATETIME2 as TIMESTAMP_NS and reaches a
// coarser-precision overload (e.g. year() takes TIMESTAMP) through an implicit
// cast that only changes sub-second precision. DATE, TIME and TIMESTAMP_TZ are
// deliberately NOT here: a cast to/from one of them CHANGES THE VALUE (drops the
// time, drops the date, shifts by an offset), so it is never a free view.
bool IsNaiveTimestampType(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

// A date-part extraction function: its result is invariant to the sub-second
// precision an IsNaiveTimestampType->IsNaiveTimestampType cast changes, so
// stripping that cast over the column argument and letting SQL Server apply the
// extraction to the column's own DATETIME2 type is exact. This is the ONLY place
// a temporal cast is stripped (spec 070 W1): in a comparison the same cast would
// change which rows match (dt::DATE truncates the time; dt::TIME drops the date),
// so a comparison operand keeps its cast and, being unencodable, falls to the
// client filter net — correct rather than fast.
bool IsDatePartFunction(const std::string &name) {
	// Exactly the date-part extractors that ARE in FunctionMapping — the cast
	// strip only runs after GetFunctionMapping succeeds, so listing a function
	// that does not map would read as if it pushes when it never reaches here.
	// Add a name here only when its mapping is added too (PR #269 review).
	return name == "year" || name == "month" || name == "day" || name == "hour" || name == "minute" || name == "second";
}

// T-SQL's modulo operator accepts only exact INTEGER operands. `[d] % 2` on a
// FLOAT column fails with "Operand data type float is invalid for modulo
// operator" (8117), and because an encoded predicate is ERASED from the DuckDB
// plan the whole query fails instead of falling to the client filter net.
//
// So the gate is a whitelist, not a float blacklist (job 1113). The first
// version excluded FLOAT/DOUBLE and let everything else through on the reasoning
// that "integer and decimal behave identically on both sides" — but the encoder
// sees only the DUCKDB type, and `money` / `smallmoney` reach it as
// DECIMAL(19,4) / DECIMAL(10,4) (mssql_column_info.cpp). T-SQL rejects `money`
// for `%` with the same 8117, so a DECIMAL allowance re-opens the bug for any
// money column, invisibly — the source type is not recoverable here.
//
// Cost of the whitelist: `decimal_col % 2` no longer pushes and runs in the
// client net. That is the project's usual trade — correct by construction over
// fast — and it is the only form available without threading the SQL Server type
// name into the encode context.
bool IsExactIntegerForModulo(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

// Does this expression already encode to a T-SQL search condition (a predicate
// valid after WHERE), as opposed to a bit-valued expression? A BOOLEAN column,
// constant or cast is a VALUE — `WHERE [b]` is a syntax error (SQL Server 4145),
// the predicate is `WHERE [b] = 1`. Comparisons, conjunctions, BETWEEN/IN,
// IS [NOT] NULL, NOT and the LIKE-pattern functions already encode as
// conditions and must not be wrapped again. Used only in predicate position;
// a boolean OPERAND (e.g. bit = bit) is never coerced (issue: bool pushdown).
bool EncodesAsSearchCondition(const Expression &expr) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		return true;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_BETWEEN:
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN:
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL:
	case ExpressionType::OPERATOR_NOT:
	case ExpressionType::CONJUNCTION_AND:
	case ExpressionType::CONJUNCTION_OR:
		return true;
	default:
		break;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		IsLikePatternFunction(expr.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName())) {
		return true;
	}
	return false;
}
}  // namespace

// Every VALUE position goes through here (job 1191). T-SQL has no boolean value
// type, so a search condition is illegal anywhere a value is expected —
// `CASE … THEN [id] > 5`, `([a] > 1) IS NULL`, `[flag] = ([id] > 5)`,
// `[x] BETWEEN ([a] > 1) AND …`, `LOWER([a] > 1)`. Each produces T-SQL the
// server REJECTS, and because ComplexFilterPushdown erases the expression from
// the DuckDB plan the query FAILS rather than degrading to the client net.
//
// The first fix guarded four such positions individually and left comparison
// operands, BETWEEN and function arguments open — the same bug, three doors
// down. One helper, routed everywhere, so the next value position cannot be
// added without it. Predicate positions use EncodeSearchCondition instead.
static ExpressionEncodeResult EncodeValueExpression(const Expression &expr, const ExpressionEncodeContext &ctx) {
	if (EncodesAsSearchCondition(expr)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeValueExpression: refusing a search condition in value position");
		return {"", false};
	}
	return FilterEncoder::EncodeExpression(expr, ctx);
}

//------------------------------------------------------------------------------
// Utility Functions
//------------------------------------------------------------------------------

std::string FilterEncoder::EscapeBracketIdentifier(const std::string &identifier) {
	std::string result;
	result.reserve(identifier.size() + 2);
	for (char c : identifier) {
		result += c;
		if (c == ']') {
			result += ']';	// Double the ] character
		}
	}
	return result;
}

std::string FilterEncoder::EscapeLikePattern(const std::string &pattern) {
	std::string result;
	result.reserve(pattern.size() + 10);
	for (char c : pattern) {
		switch (c) {
		case '%':
			result += "[%]";
			break;
		case '_':
			result += "[_]";
			break;
		case '[':
			result += "[[]";
			break;
		default:
			result += c;
			break;
		}
	}
	return result;
}

bool FilterEncoder::GetComparisonOperator(ExpressionType type, std::string &out_operator) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		out_operator = " = ";
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		out_operator = " <> ";
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		out_operator = " < ";
		return true;
	case ExpressionType::COMPARE_GREATERTHAN:
		out_operator = " > ";
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		out_operator = " <= ";
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		out_operator = " >= ";
		return true;
	default:
		return false;
	}
}

bool FilterEncoder::GetArithmeticOperator(ExpressionType type, std::string &out_operator) {
	// In DuckDB stable, arithmetic operations are handled as BoundFunctionExpression
	// with function names like "+", "-", "*", "/", not as ExpressionType operators.
	// This function is kept for potential future use but returns false for now.
	(void)type;
	(void)out_operator;
	return false;
}

std::string FilterEncoder::ValueToSQLLiteral(const Value &value, const LogicalType &type) {
	// All supported families route through the canonical codec dispatcher
	// (handles NULL + 9-arm family switch internally). Unsupported types
	// throw NotImplementedException; fall back to a string-escaped form so
	// filter pushdown still produces *something* valid for the SQL Server side
	// (filter is then compared as text rather than rejected outright).
	try {
		return codec::FormatSqlLiteral(value, type, codec::LiteralContext::Filter);
	} catch (const NotImplementedException &) {
		if (value.IsNull()) {
			return "NULL";
		}
		return "N'" + codec::string::EscapeSqlSingleQuotes(value.ToString()) + "'";
	}
}

//------------------------------------------------------------------------------
// Spec 076: constants as parameters, declared from the column
//------------------------------------------------------------------------------

namespace {

bool IsAscii(const std::string &text) {
	for (unsigned char c : text) {
		if (c >= 0x80) {
			return false;
		}
	}
	return true;
}

// UTF-16 code units of a UTF-8 string: one per lead byte, two for a 4-byte
// sequence (a surrogate pair). This is nvarchar's unit.
size_t Utf16Units(const std::string &text) {
	size_t units = 0;
	for (unsigned char c : text) {
		if ((c & 0xC0) != 0x80) {
			units += (c >= 0xF0) ? 2 : 1;
		}
	}
	return units;
}

// Width rank of the integer family, so a parameter is never narrower than
// the column OR the constant: bit < tinyint < smallint < int < bigint <
// decimal(20,0) < decimal(38,0). -1 = not an integer type.
int IntegerRankOfColumn(const std::string &sql_type) {
	if (sql_type == "bit") {
		return 0;
	}
	if (sql_type == "tinyint") {
		return 1;
	}
	if (sql_type == "smallint") {
		return 2;
	}
	if (sql_type == "int") {
		return 3;
	}
	if (sql_type == "bigint") {
		return 4;
	}
	return -1;
}

int IntegerRankOfValue(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return 0;
	case LogicalTypeId::UTINYINT:
		return 1;
	case LogicalTypeId::TINYINT:  // signed: SQL Server's tinyint is not
	case LogicalTypeId::SMALLINT:
		return 2;
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		return 3;
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		return 4;
	case LogicalTypeId::UBIGINT:
		return 5;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return 6;
	default:
		return -1;
	}
}

const char *IntegerNameOfRank(int rank) {
	static const char *names[] = {"bit", "tinyint", "smallint", "int", "bigint", "decimal(20,0)", "decimal(38,0)"};
	return names[rank];
}

// Fractional-second digits a DuckDB temporal constant carries.
int TemporalScaleOfValue(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP_SEC:
		return 0;
	case LogicalTypeId::TIMESTAMP_MS:
		return 3;
	case LogicalTypeId::TIMESTAMP_NS:
		return 7;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
		return 6;
	default:
		return -1;
	}
}

// The constant's own declaration (spec 075 W5's table), or empty for a type
// it refuses; the constant then stays a literal.
std::string DeclarationOfValueOrEmpty(const Value &value, const LogicalType &type) {
	try {
		return mssql::DeclarationForValue("p", type, value);
	} catch (const std::exception &) {
		return "";
	}
}

}  // namespace

std::string FilterEncoder::DeclarationForColumn(const MSSQLColumnInfo &column, const Value &value,
												const LogicalType &type) {
	if (column.is_cast_required || column.is_geometry) {
		return "";
	}
	const std::string t = StringUtil::Lower(column.sql_type_name);
	const int col_rank = IntegerRankOfColumn(t);
	if (col_rank >= 0) {
		const int value_rank = IntegerRankOfValue(type);
		if (value_rank < 0) {
			// A DECIMAL or DOUBLE constant against an integer column: DuckDB
			// compared in the constant's type, so declare it that way.
			return DeclarationOfValueOrEmpty(value, type);
		}
		if (value_rank == 6) {
			// HUGEINT / UHUGEINT: decimal(38,0) holds +/-(10^38 - 1) and the
			// constant may not fit. DeclarationForValue applies the shared range
			// check and names the parameter; the server's bare "Arithmetic
			// overflow" is what this replaces (review of #345).
			return mssql::DeclarationForValue("p", type, value);
		}
		return IntegerNameOfRank(col_rank > value_rank ? col_rank : value_rank);
	}
	if (t == "decimal" || t == "numeric") {
		if (type.id() != LogicalTypeId::DECIMAL) {
			return DeclarationOfValueOrEmpty(value, type);
		}
		// Never narrower than either side in BOTH dimensions: the wider scale,
		// and enough precision for the wider INTEGER part on top of it. Taking
		// the two maxima independently lost integer digits -- decimal(10,8)
		// against a DECIMAL(11,1) constant gave decimal(11,8), three digits for
		// a constant that needs ten (review of #345).
		const int col_int = column.precision - column.scale;
		const int val_int = DecimalType::GetWidth(type) - DecimalType::GetScale(type);
		const int scale = column.scale > DecimalType::GetScale(type) ? column.scale : DecimalType::GetScale(type);
		int precision = (col_int > val_int ? col_int : val_int) + scale;
		if (precision > 38) {
			precision = 38;
		}
		return "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
	}
	if (t == "money" || t == "smallmoney") {
		return type.id() == LogicalTypeId::DECIMAL ? t : DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "float" || t == "real") {
		// Value-driven: declaring a DOUBLE constant as real would round it and
		// turn a DuckDB "not equal" into an "equal".
		return DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "text") {
		return type.id() == LogicalTypeId::VARCHAR ? "varchar(max)" : DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "ntext") {
		return type.id() == LogicalTypeId::VARCHAR ? "nvarchar(max)" : DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "char" || t == "varchar" || t == "nchar" || t == "nvarchar" || t == "sysname") {
		if (type.id() != LogicalTypeId::VARCHAR) {
			return DeclarationOfValueOrEmpty(value, type);
		}
		const std::string &text = StringValue::Get(value);
		// varchar keeps the column's kind -- that is what keeps an index on it
		// seekable: on a SQL_ collation an nvarchar parameter puts a
		// CONVERT_IMPLICIT on the column and the seek becomes a scan (#361).
		// A varchar VARIABLE takes the DATABASE's code page and the comparison
		// converts it to the COLUMN's, so a non-ASCII constant may go as
		// varchar only when both pages can hold every character of it;
		// otherwise nvarchar, as the N'...' literal always did -- which is how
		// `@p varchar(max) = N'ы...'` against a UTF-8 column on a 1252 database
		// stopped arriving as '?' (annotated_max_string.test, #321).
		const bool unicode = column.is_unicode || !(mssql::CodePageCanEncode(column.code_page, text) &&
													mssql::CodePageCanEncode(column.database_code_page, text));
		if (unicode) {
			// max_length is bytes; nvarchar counts UTF-16 units.
			size_t k = column.max_length < 0 ? 0 : static_cast<size_t>(column.max_length) / (column.is_unicode ? 2 : 1);
			const size_t units = Utf16Units(text);
			if (units > k) {
				k = units;
			}
			if (column.max_length < 0 || k > 4000) {
				return "nvarchar(max)";
			}
			return "nvarchar(" + std::to_string(k == 0 ? 1 : k) + ")";
		}
		size_t k = column.max_length < 0 ? 0 : static_cast<size_t>(column.max_length);
		if (text.size() > k) {
			k = text.size();
		}
		if (column.max_length < 0 || k > 8000) {
			return "varchar(max)";
		}
		return "varchar(" + std::to_string(k == 0 ? 1 : k) + ")";
	}
	if (t == "date") {
		return type.id() == LogicalTypeId::DATE ? "date" : DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "time" || t == "datetime2" || t == "datetimeoffset") {
		const int value_scale = TemporalScaleOfValue(type);
		if (value_scale < 0) {
			return DeclarationOfValueOrEmpty(value, type);
		}
		const int scale = column.scale > value_scale ? column.scale : value_scale;
		return t + "(" + std::to_string(scale) + ")";
	}
	if (t == "datetime" || t == "smalldatetime") {
		// Value-driven, as the literal form (a CAST to datetime2) always was.
		return DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "uniqueidentifier") {
		return type.id() == LogicalTypeId::UUID ? "uniqueidentifier" : DeclarationOfValueOrEmpty(value, type);
	}
	if (t == "binary" || t == "varbinary" || t == "image" || t == "timestamp" || t == "rowversion") {
		if (type.id() != LogicalTypeId::BLOB) {
			return DeclarationOfValueOrEmpty(value, type);
		}
		size_t k = (t == "image" || column.max_length < 0) ? 0 : static_cast<size_t>(column.max_length);
		const size_t bytes = StringValue::Get(value).size();
		if (bytes > k) {
			k = bytes;
		}
		if (t == "image" || column.max_length < 0 || k > 8000) {
			return "varbinary(max)";
		}
		return "varbinary(" + std::to_string(k == 0 ? 1 : k) + ")";
	}
	return "";
}

std::string FilterEncoder::EncodeConstantValue(const Value &value, const LogicalType &type,
											   const ExpressionEncodeContext &ctx, const MSSQLColumnInfo *peer) {
	std::string literal = ValueToSQLLiteral(value, type);
	if (!ctx.params || value.IsNull() || ctx.params->params.size() >= mssql::SqlParamSet::MAX_PARAMS) {
		return literal;
	}
	std::string declaration = peer ? DeclarationForColumn(*peer, value, type) : DeclarationOfValueOrEmpty(value, type);
	if (declaration.empty()) {
		return literal;
	}
	std::string name = ctx.params->Add(declaration, literal);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstantValue: @%s %s = %s", name.c_str(), declaration.c_str(), literal.c_str());
	return "@" + name;
}

// The SQL Server column an expression IS, when it is a plain reference to
// one: the peer a constant compared with it is declared from.
static const MSSQLColumnInfo *ColumnInfoOf(const Expression &expr_in, const ExpressionEncodeContext &ctx) {
	if (!ctx.mssql_columns) {
		return nullptr;
	}
	// Spec 060 reports MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n) for string columns
	// and DuckDB reaches the plain VARCHAR operators through an implicit no-op
	// cast over the column: the column under it is still the peer.
	const Expression *expr = &expr_in;
	while (expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && BoundCastExpression::IsCast(*expr)) {
		expr = &BoundCastExpression::Child(expr->Cast<BoundFunctionExpression>());
	}
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_REF) {
		// Inside an EXPRESSION_FILTER the combiner replaced the column reference
		// with BoundReference(0): the filter's own column.
		return ctx.filter_column_info;
	}
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	const auto &binding = expr->Cast<BoundColumnRefExpression>().Binding();
	column_t table_col_idx;
	if (ctx.column_ids.empty()) {
		table_col_idx = binding.column_index;
	} else if (binding.column_index < ctx.column_ids.size()) {
		table_col_idx = ctx.column_ids[binding.column_index];
	} else {
		return nullptr;
	}
	if (table_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
		// rowid IS the scalar primary-key column (EncodeColumnRef renders it as
		// such), so a constant compared with it is declared from that column --
		// a varchar key stays varchar (review of #345).
		if (ctx.HasPKInfo() && !ctx.pk_is_composite) {
			for (const auto &col : *ctx.mssql_columns) {
				if (col.name == (*ctx.pk_column_names)[0]) {
					return &col;
				}
			}
		}
		return nullptr;
	}
	if (table_col_idx < ctx.mssql_columns->size()) {
		return &(*ctx.mssql_columns)[table_col_idx];
	}
	return nullptr;
}

static const MSSQLColumnInfo *ColumnInfoByName(const std::string &name, const ExpressionEncodeContext &ctx) {
	if (!ctx.mssql_columns) {
		return nullptr;
	}
	for (const auto &col : *ctx.mssql_columns) {
		if (col.name == name) {
			return &col;
		}
	}
	return nullptr;
}

//------------------------------------------------------------------------------
// Main Encode Function
//------------------------------------------------------------------------------

FilterEncoderResult FilterEncoder::Encode(const TableFilterSet *filters, const std::vector<column_t> &column_ids,
										  const std::vector<std::string> &column_names,
										  const std::vector<LogicalType> &column_types) {
	return Encode(filters, column_ids, column_names, column_types, nullptr, nullptr);
}

FilterEncoderResult FilterEncoder::Encode(const TableFilterSet *filters, const std::vector<column_t> &column_ids,
										  const std::vector<std::string> &column_names,
										  const std::vector<LogicalType> &column_types,
										  const std::vector<MSSQLColumnInfo> *mssql_columns,
										  mssql::SqlParamSet *params) {
	FilterEncoderResult result;
	result.needs_duckdb_filter = false;

	if (!filters || !filters->HasFilters()) {
		MSSQL_FILTER_DEBUG_LOG(1, "Encode: no filters to encode");
		return result;
	}

	MSSQL_FILTER_DEBUG_LOG(1, "Encode: encoding %zu filter(s)", static_cast<size_t>(filters->FilterCount()));

	ExpressionEncodeContext ctx(column_ids, column_names, column_types);
	ctx.mssql_columns = mssql_columns;
	ctx.params = params;
	std::vector<std::string> where_conditions;

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	for (const auto &filter_entry : *filters) {
		idx_t projected_col_idx = filter_entry.GetIndex();

		// Map from projected column index to actual table column index
		idx_t table_col_idx;
		if (column_ids.empty()) {
			// No projection - use filter index directly as table column index
			table_col_idx = projected_col_idx;
		} else if (projected_col_idx >= column_ids.size()) {
			// Cannot map this filter to a column, so it can be neither pushed
			// to the server nor handed to the client-side net (which needs the
			// column type). 2.0 does not re-apply TableFilters behind a
			// filter_pushdown scan, so skipping here would silently return a
			// superset. Fail instead — a filter over a column outside the
			// projection is a malformed plan, not a pushdown limitation.
			throw InternalException("MSSQL scan: filter column %llu outside projection (%llu columns)",
									(unsigned long long)projected_col_idx, (unsigned long long)column_ids.size());
		} else {
			// Map through column_ids to get actual table column index
			table_col_idx = column_ids[projected_col_idx];
		}

		// Skip virtual/special columns (rowid): not pushable as SQL, but the
		// filter still binds to an output column, so the client net can run it.
		if (table_col_idx >= VIRTUAL_COL_START) {
			MSSQL_FILTER_DEBUG_LOG(2, "  skipping virtual column_id=%llu", (unsigned long long)table_col_idx);
			result.needs_duckdb_filter = true;
			result.unhandled.emplace_back(projected_col_idx, &filter_entry.Filter());
			continue;
		}

		if (table_col_idx >= column_names.size()) {
			// Same reasoning as the projection guard above: unmappable means
			// undroppable, or the scan returns rows the filter excluded.
			throw InternalException("MSSQL scan: filter maps to table column %llu, table has %llu",
									(unsigned long long)table_col_idx, (unsigned long long)column_names.size());
		}

		const std::string &col_name = column_names[table_col_idx];
		const LogicalType &col_type = column_types[table_col_idx];
		std::string escaped_col = "[" + EscapeBracketIdentifier(col_name) + "]";
		ctx.filter_column_info =
			(mssql_columns && table_col_idx < mssql_columns->size()) ? &(*mssql_columns)[table_col_idx] : nullptr;

		MSSQL_FILTER_DEBUG_LOG(2, "  encoding filter for column: projected_idx=%llu -> table_idx=%llu -> %s",
							   (unsigned long long)projected_col_idx, (unsigned long long)table_col_idx,
							   col_name.c_str());

		auto encode_result = EncodeFilter(filter_entry.Filter(), escaped_col, col_type, ctx);

		if (encode_result.supported && !encode_result.sql.empty()) {
			where_conditions.push_back(encode_result.sql);
			MSSQL_FILTER_DEBUG_LOG(2, "    encoded: %s", encode_result.sql.c_str());
		}

		if (!encode_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(2, "    filter not fully supported, will need client-side re-filter");
			result.needs_duckdb_filter = true;
			result.unhandled.emplace_back(projected_col_idx, &filter_entry.Filter());
		}
	}

	// Combine all conditions with AND
	if (!where_conditions.empty()) {
		for (idx_t i = 0; i < where_conditions.size(); i++) {
			if (i > 0) {
				result.where_clause += " AND ";
			}
			result.where_clause += where_conditions[i];
		}
		MSSQL_FILTER_DEBUG_LOG(1, "Encode: generated WHERE clause: %s", result.where_clause.c_str());
	}

	MSSQL_FILTER_DEBUG_LOG(1, "Encode: needs_duckdb_filter=%s", result.needs_duckdb_filter ? "true" : "false");
	return result;
}

//------------------------------------------------------------------------------
// TableFilter Encoding
//------------------------------------------------------------------------------

ExpressionEncodeResult FilterEncoder::EncodeFilter(const TableFilter &filter, const std::string &column_name,
												   const LogicalType &column_type, const ExpressionEncodeContext &ctx) {
	switch (filter.filter_type) {
	case TableFilterType::LEGACY_CONSTANT_COMPARISON:
		return EncodeConstantComparison(filter.Cast<LegacyConstantFilter>(), ctx, column_name, column_type);

	case TableFilterType::LEGACY_IS_NULL:
		return EncodeIsNull(column_name);

	case TableFilterType::LEGACY_IS_NOT_NULL:
		return EncodeIsNotNull(column_name);

	case TableFilterType::LEGACY_IN_FILTER:
		return EncodeInFilter(filter.Cast<LegacyInFilter>(), column_name, column_type);

	case TableFilterType::LEGACY_CONJUNCTION_OR:
		return EncodeConjunctionOr(filter.Cast<LegacyConjunctionOrFilter>(), column_name, column_type, ctx);

	case TableFilterType::LEGACY_CONJUNCTION_AND:
		return EncodeConjunctionAnd(filter.Cast<LegacyConjunctionAndFilter>(), column_name, column_type, ctx);

	case TableFilterType::EXPRESSION_FILTER: {
		// The 2.0 filter combiner rewrites a pushed predicate's column reference
		// into BoundReference(0) before wrapping it in an ExpressionFilter, so
		// the expression no longer names its column — the filter's slot does.
		// Carry the escaped name in the context for the BOUND_REF arm.
		ExpressionEncodeContext expr_ctx = ctx;
		expr_ctx.filter_column = &column_name;
		return EncodeExpressionFilter(filter.Cast<ExpressionFilter>(), expr_ctx);
	}

	case TableFilterType::LEGACY_OPTIONAL_FILTER:
	case TableFilterType::LEGACY_STRUCT_EXTRACT:
	case TableFilterType::LEGACY_DYNAMIC_FILTER:
	default:
		// These filter types cannot be pushed down to SQL Server
		MSSQL_FILTER_DEBUG_LOG(1, "Filter type %d cannot be pushed down", (int)filter.filter_type);
		return {"", false};
	}
}

ExpressionEncodeResult FilterEncoder::EncodeConstantComparison(const LegacyConstantFilter &filter,
															   const ExpressionEncodeContext &ctx,
															   const std::string &column_name,
															   const LogicalType &column_type) {
	std::string op;
	if (!GetComparisonOperator(filter.comparison_type, op)) {
		return {"", false};
	}

	std::string sql = column_name + op + EncodeConstantValue(filter.constant, column_type, ctx, ctx.filter_column_info);
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNull(const std::string &column_name) {
	return {column_name + " IS NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeIsNotNull(const std::string &column_name) {
	return {column_name + " IS NOT NULL", true};
}

ExpressionEncodeResult FilterEncoder::EncodeInFilter(const LegacyInFilter &filter, const std::string &column_name,
													 const LogicalType &column_type) {
	std::string sql = column_name + " IN (";
	for (idx_t i = 0; i < filter.values.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += ValueToSQLLiteral(filter.values[i], column_type);
	}
	sql += ")";
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionAnd(const LegacyConjunctionAndFilter &filter,
														   const std::string &column_name,
														   const LogicalType &column_type,
														   const ExpressionEncodeContext &ctx) {
	if (filter.child_filters.empty()) {
		return {"", false};
	}

	std::vector<std::string> conditions;
	bool all_supported = true;

	for (const auto &child : filter.child_filters) {
		auto result = EncodeFilter(*child, column_name, column_type, ctx);
		if (result.supported && !result.sql.empty()) {
			conditions.push_back(result.sql);
		}
		if (!result.supported) {
			all_supported = false;
		}
	}

	if (conditions.empty()) {
		return {"", false};
	}

	if (conditions.size() == 1) {
		return {conditions[0], all_supported};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " AND ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return {sql, all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionOr(const LegacyConjunctionOrFilter &filter,
														  const std::string &column_name,
														  const LogicalType &column_type,
														  const ExpressionEncodeContext &ctx) {
	if (filter.child_filters.empty()) {
		return {"", false};
	}

	// OR is all-or-nothing: if any child is unsupported, skip entire OR
	std::vector<std::string> conditions;
	for (const auto &child : filter.child_filters) {
		auto result = EncodeFilter(*child, column_name, column_type, ctx);
		if (!result.supported || result.sql.empty()) {
			// Cannot push OR if any child is unsupported
			MSSQL_FILTER_DEBUG_LOG(2, "  OR child not supported, skipping entire OR");
			return {"", false};
		}
		conditions.push_back(result.sql);
	}

	if (conditions.size() == 1) {
		return {conditions[0], true};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += " OR ";
		}
		sql += conditions[i];
	}
	sql += ")";
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeExpressionFilter(const ExpressionFilter &filter,
															 const ExpressionEncodeContext &ctx) {
	// Expression filters contain arbitrary expressions. This is predicate
	// position (the expression IS the WHERE clause), so a bare bit value must be
	// coerced to `= 1`.
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpressionFilter: encoding expression type %d",
						   (int)filter.expr->GetExpressionType());
	return EncodeSearchCondition(*filter.expr, ctx);
}

ExpressionEncodeResult FilterEncoder::EncodeSearchCondition(const Expression &expr,
															const ExpressionEncodeContext &ctx) {
	auto result = EncodeExpression(expr, ctx);
	// A BOOLEAN value (bit column / constant / cast) is not a T-SQL predicate:
	// `WHERE [b]` errors 4145, `WHERE [b] = 1` is the condition. Expressions that
	// already encode AS a condition (comparison/conjunction/BETWEEN/IN/IS NULL/
	// NOT/LIKE) are left alone. Boolean operands elsewhere (bit = bit) go through
	// EncodeExpression, never here, so they keep their bare form.
	if (result.supported && !result.sql.empty() && expr.GetReturnType().id() == LogicalTypeId::BOOLEAN &&
		!EncodesAsSearchCondition(expr)) {
		result.sql = "(" + result.sql + " = 1)";
	}
	return result;
}

//------------------------------------------------------------------------------
// Expression Encoding
//------------------------------------------------------------------------------

ExpressionEncodeResult FilterEncoder::EncodeExpression(const Expression &expr, const ExpressionEncodeContext &ctx) {
	// Spec 076: constants register themselves in the sink as they are met, so
	// an expression refused halfway (a comparison whose other side does not
	// encode) would leave its parameters in the DECLARE line -- unused, and a
	// different text per value for nothing. Roll them back on refusal.
	const size_t mark = ctx.params ? ctx.params->params.size() : 0;
	auto result = EncodeExpressionImpl(expr, ctx);
	if (!result.supported && ctx.params && ctx.params->params.size() > mark) {
		ctx.params->params.resize(mark);
	}
	return result;
}

ExpressionEncodeResult FilterEncoder::EncodeExpressionImpl(const Expression &expr, const ExpressionEncodeContext &ctx) {
	// Check recursion depth
	if (ctx.at_max_depth()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: max depth reached");
		return {"", false};
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeExpression: type=%d class=%d", (int)expr.GetExpressionType(),
						   (int)expr.GetExpressionClass());

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return EncodeColumnRef(expr.Cast<BoundColumnRefExpression>(), ctx);

	case ExpressionClass::BOUND_REF: {
		// Only inside an EXPRESSION_FILTER, where the combiner replaced the
		// filter column's reference with BoundReference(0) — the combiner
		// rejects multi-column expressions before pushing, so index 0 is the
		// filter's own column and anything else is unencodable.
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ctx.filter_column && ref.Index() == 0) {
			return {*ctx.filter_column, true};
		}
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: BOUND_REF outside a filter context, not pushed");
		return {"", false};
	}

	case ExpressionClass::BOUND_CONSTANT:
		return EncodeConstant(expr.Cast<BoundConstantExpression>(), ctx);

	case ExpressionClass::BOUND_FUNCTION: {
		auto &func_expr = expr.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsCast(expr)) {
			// Spec 060: the catalog reports MSSQL_VARCHAR(n) / MSSQL_NVARCHAR(n)
			// for string columns, and DuckDB reaches a plain VARCHAR overload
			// through the implicit no-op cast registered with those types. On the
			// server there is nothing to cast — the column already IS that string
			// — so encode straight through it. Without this arm the cast reads as
			// an unsupported expression, the filter stops being pushed, and the
			// whole column is fetched and filtered on the client instead.
			//
			// VARCHAR to VARCHAR only. A real conversion (INTEGER to VARCHAR) has
			// formatting semantics SQL Server need not reproduce, and stays
			// unpushed.
			const auto &target_type = BoundCastExpression::TargetType(func_expr);
			const auto &cast_child = BoundCastExpression::Child(func_expr);
			const auto target_id = target_type.id();
			const auto source_id = cast_child.GetReturnType().id();
			if (target_id == LogicalTypeId::VARCHAR && source_id == LogicalTypeId::VARCHAR) {
				return EncodeExpression(cast_child, ctx);
			}
			// A temporal cast is NOT stripped here. It is a free view only when the
			// surrounding operation is precision-insensitive (a date-part
			// extraction), and only for a naive-timestamp precision change — never
			// in a comparison, where dt::DATE / dt::TIME / dt::TIMESTAMPTZ change
			// which rows match. That narrow, provably-exact strip lives in
			// EncodeFunctionExpression for the date-part functions (spec 070 W1);
			// every other temporal cast falls through to the client filter net.
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: cast %s -> %s not pushed",
								   cast_child.GetReturnType().ToString().c_str(), target_type.ToString().c_str());
			return {"", false};
		}
		// Comparisons and BETWEEN are BoundFunctionExpressions too (their
		// helper structs only accept that class), dispatched by expression
		// type, not class.
		if (BoundComparisonExpression::IsComparison(expr)) {
			return EncodeComparisonExpression(func_expr, ctx);
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			return EncodeBetweenExpression(func_expr, ctx);
		}
		return EncodeFunctionExpression(func_expr, ctx);
	}

	case ExpressionClass::BOUND_CONJUNCTION:
		return EncodeConjunctionExpression(expr.Cast<BoundConjunctionExpression>(), ctx);

	case ExpressionClass::BOUND_OPERATOR:
		return EncodeOperatorExpression(expr.Cast<BoundOperatorExpression>(), ctx);

	case ExpressionClass::BOUND_CASE:
		return EncodeCaseExpression(expr.Cast<BoundCaseExpression>(), ctx);

	default:
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeExpression: unsupported expression class %d", (int)expr.GetExpressionClass());
		return {"", false};
	}
}

ExpressionEncodeResult FilterEncoder::EncodeFunctionExpression(const BoundFunctionExpression &expr,
															   const ExpressionEncodeContext &ctx) {
	const std::string &func_name = expr.Function().GetName().GetIdentifierName();
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: function=%s, args=%zu", func_name.c_str(),
						   expr.GetChildren().size());

	// Check for LIKE pattern functions (prefix, suffix, contains, iprefix, isuffix, icontains)
	if (IsLikePatternFunction(func_name)) {
		if (expr.GetChildren().size() >= 2) {
			return EncodeLikePattern(func_name, *expr.GetChildren()[0], *expr.GetChildren()[1], ctx);
		}
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: LIKE pattern function %s needs 2 args, got %zu",
							   func_name.c_str(), expr.GetChildren().size());
		return {"", false};
	}

	// Check for supported functions in the mapping table
	const FunctionMapping *mapping = GetFunctionMapping(func_name);
	if (!mapping) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: function %s not supported", func_name.c_str());
		return {"", false};
	}

	// Modulo: push only for exact integer operands. T-SQL's `%` rejects float,
	// real AND money (8117), and money is indistinguishable from decimal here.
	if (func_name == "%") {
		for (const auto &child : expr.GetChildren()) {
			if (!IsExactIntegerForModulo(child->GetReturnType().id())) {
				MSSQL_FILTER_DEBUG_LOG(
					1, "EncodeFunctionExpression: %% on %s not pushed (T-SQL modulo takes exact integers only)",
					child->GetReturnType().ToString().c_str());
				return {"", false};
			}
		}
	}

	// Validate argument count
	if (mapping->expected_args != static_cast<int>(expr.GetChildren().size())) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: %s expects %d args, got %zu", func_name.c_str(),
							   mapping->expected_args, expr.GetChildren().size());
		return {"", false};
	}

	// Encode all arguments
	auto child_ctx = ctx.child();
	const bool datepart = IsDatePartFunction(func_name);
	std::vector<std::string> encoded_args;
	for (const auto &child : expr.GetChildren()) {
		const Expression *arg = child.get();
		// Spec 070 W1: strip the implicit precision cast DuckDB puts over a
		// DATETIME2 column when a date-part function takes it. DATETIME2 is
		// TIMESTAMP_NS to DuckDB; year()/hour()/... take TIMESTAMP, so an
		// implicit TIMESTAMP_NS->TIMESTAMP cast wraps the column. SQL Server's
		// YEAR([col]) applies to the column's own DATETIME2 and the extraction is
		// invariant to the sub-second precision the cast changed, so encoding the
		// cast's child directly is exact. Restricted to naive-timestamp->
		// naive-timestamp: a DATE/TIME/TZ cast here changes the value and must not
		// be stripped.
		if (datepart && BoundCastExpression::IsCast(*arg)) {
			const auto &cast_fn = arg->Cast<BoundFunctionExpression>();
			const auto &cast_child = BoundCastExpression::Child(cast_fn);
			if (IsNaiveTimestampType(BoundCastExpression::TargetType(cast_fn).id()) &&
				IsNaiveTimestampType(cast_child.GetReturnType().id())) {
				arg = &cast_child;
			}
		}
		auto result = EncodeValueExpression(*arg, child_ctx);
		if (!result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeFunctionExpression: argument encoding failed for %s", func_name.c_str());
			return {"", false};
		}
		encoded_args.push_back(result.sql);
	}

	// Apply the template
	std::string sql = mapping->sql_template;
	for (size_t i = 0; i < encoded_args.size(); i++) {
		std::string placeholder = "{" + std::to_string(i) + "}";
		size_t pos = 0;
		while ((pos = sql.find(placeholder, pos)) != std::string::npos) {
			sql.replace(pos, placeholder.length(), encoded_args[i]);
			pos += encoded_args[i].length();
		}
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeFunctionExpression: encoded %s -> %s", func_name.c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeComparisonExpression(const BoundFunctionExpression &expr,
																 const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: type=%d", (int)expr.GetExpressionType());

	const auto &left = BoundComparisonExpression::Left(expr);
	const auto &right = BoundComparisonExpression::Right(expr);

	// Check for rowid equality: rowid = value (Spec 001-pk-rowid-semantics)
	if (expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL && ctx.HasPKInfo()) {
		// Check if left is rowid and right is constant
		if (IsRowidColumn(left, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected rowid = value");
			return EncodeRowidEquality(right, ctx);
		}
		// Check if right is rowid and left is constant (value = rowid)
		if (IsRowidColumn(right, ctx)) {
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: detected value = rowid");
			return EncodeRowidEquality(left, ctx);
		}
	}

	// Get the comparison operator
	std::string op;
	if (!GetComparisonOperator(expr.GetExpressionType(), op)) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: unsupported comparison type %d",
							   (int)expr.GetExpressionType());
		return {"", false};
	}

	// Encode left and right sides
	// Spec 076: a constant on one side is declared from the column on the other.
	auto left_ctx = ctx.child();
	left_ctx.constant_peer = ColumnInfoOf(right, ctx);
	auto right_ctx = ctx.child();
	right_ctx.constant_peer = ColumnInfoOf(left, ctx);
	auto left_result = EncodeValueExpression(left, left_ctx);
	if (!left_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: left side encoding failed");
		return {"", false};
	}

	auto right_result = EncodeValueExpression(right, right_ctx);
	if (!right_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeComparisonExpression: right side encoding failed");
		return {"", false};
	}

	std::string sql = "(" + left_result.sql + op + right_result.sql + ")";
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeComparisonExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeOperatorExpression(const BoundOperatorExpression &expr,
															   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeOperatorExpression: type=%d, children=%zu", (int)expr.GetExpressionType(),
						   expr.GetChildren().size());

	// Handle NOT operator
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		// NOT negates a predicate: its operand is itself predicate position, so a
		// bare bit child (`NOT flag`) becomes `NOT ([flag] = 1)`, not `NOT [flag]`.
		auto child_result = EncodeSearchCondition(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(NOT " + child_result.sql + ")", true};
	}

	// Handle IS NULL / IS NOT NULL operators
	if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeValueExpression(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(" + child_result.sql + " IS NULL)", true};
	}

	if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		if (expr.GetChildren().size() != 1) {
			return {"", false};
		}
		auto child_ctx = ctx.child();
		auto child_result = EncodeValueExpression(*expr.GetChildren()[0], child_ctx);
		if (!child_result.supported) {
			return {"", false};
		}
		return {"(" + child_result.sql + " IS NOT NULL)", true};
	}

	// For other operators, we don't support them yet
	// Arithmetic is handled via BoundFunctionExpression in DuckDB stable
	MSSQL_FILTER_DEBUG_LOG(1, "EncodeOperatorExpression: unsupported operator type %d", (int)expr.GetExpressionType());
	return {"", false};
}

ExpressionEncodeResult FilterEncoder::EncodeCaseExpression(const BoundCaseExpression &expr,
														   const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: case_checks=%zu", expr.CaseChecks().size());

	auto child_ctx = ctx.child();
	std::string sql = "CASE";

	// Encode each WHEN ... THEN clause
	for (const auto &check : expr.CaseChecks()) {
		// A CASE `WHEN` operand is predicate position like any other: a bare bit
		// there emits `CASE WHEN [flag] THEN ...`, which SQL Server rejects with
		// error 4145. Coerce it to `([flag] = 1)` (PR #269 review).
		auto when_result = EncodeSearchCondition(*check.when_expr, child_ctx);
		if (!when_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: WHEN clause encoding failed");
			return {"", false};
		}

		// THEN is VALUE position — the mirror of the WHEN case above. T-SQL has no
		// boolean value type, so a search condition is illegal here:
		// `CASE WHEN c THEN [id] > 5 ...` is "Incorrect syntax near '>'". And the
		// failure is the bad kind — ComplexFilterPushdown erases the expression
		// from the plan, so the query FAILS instead of falling to the client net
		// (job 1113).
		auto then_result = EncodeValueExpression(*check.then_expr, child_ctx);
		if (!then_result.supported) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: THEN clause encoding failed");
			return {"", false};
		}

		sql += " WHEN " + when_result.sql + " THEN " + then_result.sql;
	}

	// ELSE is VALUE position too — same refusal as THEN.
	auto else_result = EncodeValueExpression(expr.Else(), child_ctx);
	if (!else_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeCaseExpression: ELSE clause encoding failed");
		return {"", false};
	}
	sql += " ELSE " + else_result.sql + " END";

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeCaseExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeBetweenExpression(const BoundFunctionExpression &expr,
															  const ExpressionEncodeContext &ctx) {
	const bool lower_inclusive = BoundBetweenExpression::LowerInclusive(expr);
	const bool upper_inclusive = BoundBetweenExpression::UpperInclusive(expr);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeBetweenExpression: lower_inclusive=%s, upper_inclusive=%s",
						   lower_inclusive ? "true" : "false", upper_inclusive ? "true" : "false");

	auto child_ctx = ctx.child();

	// Encode the input expression (the column or expression being checked)
	auto input_result = EncodeValueExpression(BoundBetweenExpression::Input(expr), child_ctx);
	if (!input_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: input encoding failed");
		return {"", false};
	}

	// Encode the lower bound
	// Spec 076: the bounds are compared with the input column.
	child_ctx.constant_peer = ColumnInfoOf(BoundBetweenExpression::Input(expr), ctx);
	auto lower_result = EncodeValueExpression(BoundBetweenExpression::LowerBound(expr), child_ctx);
	if (!lower_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: lower bound encoding failed");
		return {"", false};
	}

	// Encode the upper bound
	auto upper_result = EncodeValueExpression(BoundBetweenExpression::UpperBound(expr), child_ctx);
	if (!upper_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeBetweenExpression: upper bound encoding failed");
		return {"", false};
	}

	// Build the SQL: (input >= lower AND input <= upper) or variants based on inclusivity
	// For standard BETWEEN (both inclusive), we can use T-SQL BETWEEN
	if (lower_inclusive && upper_inclusive) {
		std::string sql = "(" + input_result.sql + " BETWEEN " + lower_result.sql + " AND " + upper_result.sql + ")";
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeBetweenExpression: encoded -> %s", sql.c_str());
		return {sql, true};
	}

	// For non-standard bounds, use explicit comparisons
	std::string lower_op = lower_inclusive ? " >= " : " > ";
	std::string upper_op = upper_inclusive ? " <= " : " < ";
	std::string sql = "((" + input_result.sql + lower_op + lower_result.sql + ") AND (" + input_result.sql + upper_op +
					  upper_result.sql + "))";
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeBetweenExpression: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeColumnRef(const BoundColumnRefExpression &expr,
													  const ExpressionEncodeContext &ctx) {
	// Get the column binding - this contains the table index and column index
	const auto &binding = expr.Binding();
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: table_idx=%llu, column_idx=%llu",
						   (unsigned long long)binding.table_index.index, (unsigned long long)binding.column_index);

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	// The column_index from binding refers to the projected column index
	// We need to map it through column_ids to get the actual table column index
	column_t projected_idx = binding.column_index;

	column_t table_col_idx;
	if (ctx.column_ids.empty()) {
		// No projection - use binding index directly
		table_col_idx = projected_idx;
	} else if (projected_idx >= ctx.column_ids.size()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: column index %llu out of range (projection has %zu)",
							   (unsigned long long)projected_idx, ctx.column_ids.size());
		return {"", false};
	} else {
		table_col_idx = ctx.column_ids[projected_idx];
	}

	// Handle rowid virtual column (Spec 001-pk-rowid-semantics)
	// For non-equality expressions like rowid > 100, we can use scalar PK
	if (table_col_idx == COLUMN_IDENTIFIER_ROW_ID) {
		// Only scalar PK can be used in arbitrary expressions
		if (ctx.HasPKInfo() && !ctx.pk_is_composite) {
			std::string sql = "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[0]) + "]";
			MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: rowid (scalar PK) -> %s", sql.c_str());
			return {sql, true};
		}
		// Composite PK rowid can only be used in equality (handled in EncodeComparisonExpression)
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: rowid not supported for non-equality (composite PK or no PK info)");
		return {"", false};
	}

	// Skip other virtual columns
	if (table_col_idx >= VIRTUAL_COL_START) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: virtual column %llu not supported",
							   (unsigned long long)table_col_idx);
		return {"", false};
	}

	if (table_col_idx >= ctx.column_names.size()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeColumnRef: table column index %llu out of range (table has %zu)",
							   (unsigned long long)table_col_idx, ctx.column_names.size());
		return {"", false};
	}

	const std::string &col_name = ctx.column_names[table_col_idx];
	std::string sql = "[" + EscapeBracketIdentifier(col_name) + "]";
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeColumnRef: encoded -> %s", sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConstant(const BoundConstantExpression &expr,
													 const ExpressionEncodeContext &ctx) {
	std::string sql = EncodeConstantValue(expr.GetValue(), expr.GetReturnType(), ctx, ctx.constant_peer);
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConstant: value=%s, type=%s -> %s", expr.GetValue().ToString().c_str(),
						   expr.GetReturnType().ToString().c_str(), sql.c_str());
	return {sql, true};
}

ExpressionEncodeResult FilterEncoder::EncodeConjunctionExpression(const BoundConjunctionExpression &expr,
																  const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: type=%d, children=%zu", (int)expr.GetExpressionType(),
						   expr.GetChildren().size());

	if (expr.GetChildren().empty()) {
		return {"", false};
	}

	bool is_and = (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
	std::string conj_op = is_and ? " AND " : " OR ";

	auto child_ctx = ctx.child();
	std::vector<std::string> conditions;
	bool all_supported = true;

	for (const auto &child : expr.GetChildren()) {
		// Each conjunct is predicate position — a bare bit child needs `= 1`.
		auto result = EncodeSearchCondition(*child, child_ctx);
		if (is_and) {
			// AND: partial pushdown allowed - skip unsupported children
			if (result.supported && !result.sql.empty()) {
				conditions.push_back(result.sql);
			}
			if (!result.supported) {
				all_supported = false;
			}
		} else {
			// OR: all-or-nothing - if any child is unsupported, reject entire OR
			if (!result.supported || result.sql.empty()) {
				MSSQL_FILTER_DEBUG_LOG(1, "EncodeConjunctionExpression: OR child not supported, rejecting entire OR");
				return {"", false};
			}
			conditions.push_back(result.sql);
		}
	}

	if (conditions.empty()) {
		return {"", false};
	}

	if (conditions.size() == 1) {
		return {conditions[0], all_supported};
	}

	std::string sql = "(";
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			sql += conj_op;
		}
		sql += conditions[i];
	}
	sql += ")";

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeConjunctionExpression: encoded -> %s", sql.c_str());
	return {sql, all_supported};
}

ExpressionEncodeResult FilterEncoder::EncodeLikePattern(const std::string &function_name, const Expression &column_expr,
														const Expression &pattern_expr,
														const ExpressionEncodeContext &ctx) {
	MSSQL_FILTER_DEBUG_LOG(2, "EncodeLikePattern: function=%s", function_name.c_str());

	// Encode the column expression
	auto child_ctx = ctx.child();
	// Value position like every other (job 1203) — this was the one site the
	// helper did not cover, which contradicted its own stated invariant. No live
	// bug today (reaching it with a condition needs a BOOLEAN->VARCHAR cast, which
	// the cast arm refuses), but it is the same "three doors down" shape.
	auto column_result = EncodeValueExpression(column_expr, child_ctx);
	if (!column_result.supported) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: column encoding failed");
		return {"", false};
	}

	// The pattern must be a constant for us to encode it properly
	// (We need to escape LIKE special characters in the pattern)
	if (pattern_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: pattern is not a constant, cannot push down");
		return {"", false};
	}

	const auto &pattern_const = pattern_expr.Cast<BoundConstantExpression>();
	if (pattern_const.GetValue().IsNull()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: pattern is NULL");
		return {"", false};
	}

	std::string pattern_str = pattern_const.GetValue().ToString();
	std::string escaped_pattern = EscapeLikePattern(pattern_str);

	// Convert function name to lowercase for comparison
	std::string lower_func = function_name;
	std::transform(lower_func.begin(), lower_func.end(), lower_func.begin(),
				   [](unsigned char c) { return std::tolower(c); });

	// Check if this is a case-insensitive LIKE function
	bool case_insensitive = IsCaseInsensitiveLikeFunction(function_name);

	// Build the LIKE pattern based on function type
	std::string pattern_text;
	if (lower_func == "prefix" || lower_func == "iprefix") {
		pattern_text = escaped_pattern + "%";
	} else if (lower_func == "suffix" || lower_func == "isuffix") {
		pattern_text = "%" + escaped_pattern;
	} else if (lower_func == "contains" || lower_func == "icontains") {
		pattern_text = "%" + escaped_pattern + "%";
	} else {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeLikePattern: unknown LIKE pattern function %s", function_name.c_str());
		return {"", false};
	}
	// Spec 076: the pattern is compared with the column, so it is declared from
	// it (a varchar column keeps a seekable prefix pattern); without a sink this
	// is the N'...' literal it always was.
	std::string like_pattern =
		EncodeConstantValue(Value(pattern_text), LogicalType::VARCHAR, child_ctx, ColumnInfoOf(column_expr, ctx));

	// Build the T-SQL expression
	std::string sql;
	if (case_insensitive) {
		// ILIKE: apply LOWER() to both column and pattern
		sql = "(LOWER(" + column_result.sql + ") LIKE LOWER(" + like_pattern + "))";
	} else {
		// Case-sensitive LIKE
		sql = "(" + column_result.sql + " LIKE " + like_pattern + ")";
	}

	MSSQL_FILTER_DEBUG_LOG(2, "EncodeLikePattern: encoded -> %s", sql.c_str());
	return {sql, true};
}

//------------------------------------------------------------------------------
// Rowid Filter Pushdown Helpers (Spec 001-pk-rowid-semantics)
//------------------------------------------------------------------------------

bool FilterEncoder::IsRowidColumn(const Expression &expr, const ExpressionEncodeContext &ctx) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &col_ref = expr.Cast<BoundColumnRefExpression>();
	idx_t projected_idx = col_ref.Binding().column_index;

	if (ctx.column_ids.empty()) {
		// No projection - check if the index is COLUMN_IDENTIFIER_ROW_ID
		return projected_idx == COLUMN_IDENTIFIER_ROW_ID;
	}
	if (projected_idx >= ctx.column_ids.size()) {
		return false;
	}
	return ctx.column_ids[projected_idx] == COLUMN_IDENTIFIER_ROW_ID;
}

ExpressionEncodeResult FilterEncoder::EncodeRowidEquality(const Expression &value_expr,
														  const ExpressionEncodeContext &ctx) {
	if (!ctx.HasPKInfo()) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: no PK info available");
		return {"", false};
	}

	// Value must be a constant
	if (value_expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: value is not a constant");
		return {"", false};
	}
	auto &const_expr = value_expr.Cast<BoundConstantExpression>();

	if (ctx.pk_is_composite) {
		// Composite PK: rowid = {'col1': val1, 'col2': val2}
		// Extract struct children and build AND conditions
		if (const_expr.GetValue().type().id() != LogicalTypeId::STRUCT) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: composite PK expects STRUCT value, got %s",
								   const_expr.GetValue().type().ToString().c_str());
			return {"", false};
		}
		auto &children = StructValue::GetChildren(const_expr.GetValue());
		if (children.size() != ctx.pk_column_names->size()) {
			MSSQL_FILTER_DEBUG_LOG(1, "EncodeRowidEquality: STRUCT has %zu children, expected %zu", children.size(),
								   ctx.pk_column_names->size());
			return {"", false};
		}

		std::string sql = "(";
		for (idx_t i = 0; i < children.size(); i++) {
			if (i > 0) {
				sql += " AND ";
			}
			sql += "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[i]) + "]";
			sql += " = ";
			sql += EncodeConstantValue(children[i], (*ctx.pk_column_types)[i], ctx,
									   ColumnInfoByName((*ctx.pk_column_names)[i], ctx));
		}
		sql += ")";
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: composite PK -> %s", sql.c_str());
		return {sql, true};
	} else {
		// Scalar PK: rowid = value
		std::string sql = "[" + EscapeBracketIdentifier((*ctx.pk_column_names)[0]) + "]";
		sql += " = ";
		sql += EncodeConstantValue(const_expr.GetValue(), (*ctx.pk_column_types)[0], ctx,
								   ColumnInfoByName((*ctx.pk_column_names)[0], ctx));
		MSSQL_FILTER_DEBUG_LOG(2, "EncodeRowidEquality: scalar PK -> %s", sql.c_str());
		return {sql, true};
	}
}

}  // namespace mssql
}  // namespace duckdb

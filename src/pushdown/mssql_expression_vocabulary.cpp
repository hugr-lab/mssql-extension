// pushdown/mssql_expression_vocabulary.cpp -- see the header. The rules here
// were the scan path's FilterEncoder's; each keeps the reason it was written
// with, since both the scan and the remote-pushdown writer now depend on it.

#include "pushdown/mssql_expression_vocabulary.hpp"

#include "catalog/mssql_code_page.hpp"
#include "catalog/mssql_column_info.hpp"
#include "codec/literal_format.hpp"
#include "codec/string_codec.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/function_mapping.hpp"

#include <algorithm>
#include <cctype>

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
bool IsDatePartName(const std::string &name) {
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

std::string ExpressionVocabulary::DeclarationForColumn(const MSSQLColumnInfo &column, const Value &value,
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
		if (type.id() != LogicalTypeId::VARCHAR) {
			return DeclarationOfValueOrEmpty(value, type);
		}
		// The same two-page rule as varchar below: a text column cannot carry a
		// UTF-8 collation, so a constant the database's page cannot hold went
		// as '?' in a varchar(max) parameter and `LIKE 'ы%'` matched '?…' rows.
		const std::string &text = StringValue::Get(value);
		const bool fits = mssql::CodePageCanEncode(column.code_page, text) &&
						  mssql::CodePageCanEncode(column.database_code_page, text);
		return fits ? "varchar(max)" : "nvarchar(max)";
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

std::string ExpressionVocabulary::Constant(const Value &value, const LogicalType &type, SqlParamSet *params,
										   const MSSQLColumnInfo *peer) {
	std::string literal = ValueToSQLLiteral(value, type);
	if (!params || value.IsNull() || params->params.size() >= SqlParamSet::MAX_PARAMS) {
		return literal;
	}
	std::string declaration = peer ? DeclarationForColumn(*peer, value, type) : DeclarationOfValueOrEmpty(value, type);
	if (declaration.empty()) {
		return literal;
	}
	return "@" + params->Add(declaration, literal);
}

std::string ExpressionVocabulary::ValueToSQLLiteral(const Value &value, const LogicalType &type) {
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

std::string ExpressionVocabulary::EscapeLikePattern(const std::string &pattern) {
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

bool ExpressionVocabulary::ComparisonOperator(ExpressionType type, std::string &out_operator) {
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

bool ExpressionVocabulary::IsValue(const SqlOperand &operand) {
	return !operand.is_condition;
}

std::string ExpressionVocabulary::AsCondition(const SqlOperand &operand) {
	if (!operand.is_condition && !operand.sql.empty() && operand.type.id() == LogicalTypeId::BOOLEAN) {
		return "(" + operand.sql + " = 1)";
	}
	return operand.sql;
}

std::string ExpressionVocabulary::Comparison(const std::string &op, const std::string &left, const std::string &right) {
	return "(" + left + op + right + ")";
}

std::string ExpressionVocabulary::Not(const std::string &condition) {
	return "(NOT " + condition + ")";
}

std::string ExpressionVocabulary::IsNull(const std::string &value, bool negated) {
	return "(" + value + (negated ? " IS NOT NULL)" : " IS NULL)");
}

bool ExpressionVocabulary::InListFits(size_t items) {
	return items <= MAX_IN_ITEMS;
}

std::string ExpressionVocabulary::In(const std::string &operand, const std::vector<std::string> &items, bool negated) {
	std::string sql = "(" + operand + (negated ? " NOT IN (" : " IN (");
	for (size_t i = 0; i < items.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += items[i];
	}
	return sql + "))";
}

std::string ExpressionVocabulary::Between(const std::string &input, const std::string &lower, const std::string &upper,
										  bool lower_inclusive, bool upper_inclusive) {
	// Both bounds inclusive is T-SQL's BETWEEN; otherwise explicit comparisons.
	if (lower_inclusive && upper_inclusive) {
		return "(" + input + " BETWEEN " + lower + " AND " + upper + ")";
	}
	const std::string lower_op = lower_inclusive ? " >= " : " > ";
	const std::string upper_op = upper_inclusive ? " <= " : " < ";
	return "((" + input + lower_op + lower + ") AND (" + input + upper_op + upper + "))";
}

std::string ExpressionVocabulary::Case(const std::vector<std::string> &whens, const std::vector<std::string> &thens,
									   const std::string &otherwise) {
	std::string sql = "CASE";
	for (size_t i = 0; i < whens.size(); i++) {
		sql += " WHEN " + whens[i] + " THEN " + thens[i];
	}
	return sql + " ELSE " + otherwise + " END";
}

std::string ExpressionVocabulary::Divide(const std::string &left, const std::string &right) {
	return "(CAST(" + left + " AS float) / NULLIF(CAST(" + right + " AS float), 0))";
}

std::string ExpressionVocabulary::Coalesce(const std::vector<std::string> &args) {
	std::string sql = "COALESCE(";
	for (size_t i = 0; i < args.size(); i++) {
		sql += (i ? ", " : "") + args[i];
	}
	return sql + ")";
}

std::string ExpressionVocabulary::NullIf(const std::string &left, const std::string &right) {
	return "NULLIF(" + left + ", " + right + ")";
}

std::string ExpressionVocabulary::Cast(const std::string &value, const std::string &type) {
	return "CAST(" + value + " AS " + type + ")";
}

std::string ExpressionVocabulary::Conjunction(const std::vector<std::string> &parts, bool is_and) {
	if (parts.size() == 1) {
		return parts[0];
	}
	std::string sql = "(";
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0) {
			sql += is_and ? " AND " : " OR ";
		}
		sql += parts[i];
	}
	return sql + ")";
}

// VARCHAR to VARCHAR only. A real conversion (INTEGER to VARCHAR) has
// formatting semantics SQL Server need not reproduce, and stays unpushed. A
// temporal cast is never transparent: it is a free view only under a date-part
// extraction (IsNaiveTimestampCast), never in a comparison, where dt::DATE /
// dt::TIME / dt::TIMESTAMPTZ change which rows match.
bool ExpressionVocabulary::IsTransparentCast(const LogicalType &source, const LogicalType &target) {
	return target.id() == LogicalTypeId::VARCHAR && source.id() == LogicalTypeId::VARCHAR;
}

bool ExpressionVocabulary::IsDatePartFunction(const std::string &name) {
	return IsDatePartName(name);
}

bool ExpressionVocabulary::IsNaiveTimestampCast(const LogicalType &source, const LogicalType &target) {
	return IsNaiveTimestampType(target.id()) && IsNaiveTimestampType(source.id());
}

const FunctionMapping *ExpressionVocabulary::FunctionFor(const std::string &name,
														 const std::vector<LogicalType> &arg_types, std::string &why) {
	const FunctionMapping *mapping = GetFunctionMapping(name);
	if (!mapping) {
		why = "function " + name + " not supported";
		return nullptr;
	}
	// Modulo: push only for exact integer operands. T-SQL's `%` rejects float,
	// real AND money (8117), and money is indistinguishable from decimal here.
	if (name == "%") {
		for (const auto &type : arg_types) {
			if (!IsExactIntegerForModulo(type.id())) {
				why = "% on " + type.ToString() + " not pushed (T-SQL modulo takes exact integers only)";
				return nullptr;
			}
		}
	}
	if (name == "+" || name == "-" || name == "*") {
		for (const auto &type : arg_types) {
			// Numbers only: DuckDB's `date + 1` is a date, T-SQL's `+` refuses
			// a date and an int (206) -- the bound tree reaches the scan's
			// encoder with the DATE operand, the parsed one the writer with it.
			if (!type.IsNumeric()) {
				why = name + " on " + type.ToString() + " not pushed (T-SQL's operator is numeric)";
				return nullptr;
			}
		}
		if (name == "*" && arg_types.size() == 2) {
			for (const auto &type : arg_types) {
				// A double product overflows near 1e154: error 8115 on the server
				// -- for every row WHERE passes, TOP's discards included --
				// where DuckDB gives inf (spec 079 PR D full review).
				if (type.id() == LogicalTypeId::DOUBLE || type.id() == LogicalTypeId::FLOAT) {
					why = "* on " + type.ToString() + " not pushed (overflows on the server where DuckDB says inf)";
					return nullptr;
				}
			}
			// DuckDB types DECIMAL(w1,s1) * DECIMAL(w2,s2) as DECIMAL(w1 + w2,
			// s1 + s2), but CAPS it at 18 when both factors fit 18 and the
			// scale does too -- then raising an overflow where the server holds
			// the value (99999999.99 squared in decimal(10,2)).
			if (arg_types[0].id() == LogicalTypeId::DECIMAL && arg_types[1].id() == LogicalTypeId::DECIMAL) {
				const auto w1 = DecimalType::GetWidth(arg_types[0]);
				const auto w2 = DecimalType::GetWidth(arg_types[1]);
				const auto s1 = DecimalType::GetScale(arg_types[0]);
				const auto s2 = DecimalType::GetScale(arg_types[1]);
				if (w1 <= 18 && w2 <= 18 && w1 + w2 > 18 && s1 + s2 < 18) {
					why = "a decimal product DuckDB types as DECIMAL(18) not pushed (it overflows there first)";
					return nullptr;
				}
			}
		}
		// Past 38 digits the server reduces the result's SCALE, rounding the
		// value, where DuckDB keeps it exact: decimal(19,4) * decimal(19,4) is
		// decimal(38,7) there, DECIMAL(38,8) here (measured, PR C); the server's
		// precision is p1 + p2 + 1 for *, max(s) + max(p - s) + 1 for + and -.
		if (arg_types.size() == 2 && arg_types[0].id() == LogicalTypeId::DECIMAL &&
			arg_types[1].id() == LogicalTypeId::DECIMAL) {
			const auto w1 = DecimalType::GetWidth(arg_types[0]);
			const auto w2 = DecimalType::GetWidth(arg_types[1]);
			const auto s1 = DecimalType::GetScale(arg_types[0]);
			const auto s2 = DecimalType::GetScale(arg_types[1]);
			const auto server_precision = name == "*" ? w1 + w2 + 1 : std::max(s1, s2) + std::max(w1 - s1, w2 - s2) + 1;
			if (server_precision > 38) {
				why = name + " of " + arg_types[0].ToString() + " and " + arg_types[1].ToString() +
					  " not pushed (past 38 digits the server rounds the scale)";
				return nullptr;
			}
		}
	}
	if (mapping->expected_args != static_cast<int>(arg_types.size())) {
		why = name + " expects " + std::to_string(mapping->expected_args) + " args, got " +
			  std::to_string(arg_types.size());
		return nullptr;
	}
	// A date part of a datetimeoffset is taken in the value's own offset by the
	// server and in the session TimeZone by DuckDB (measured: HOUR of 12:00
	// +05:00 is 12 there, 7 in UTC here), so a pushed `hour(dto) = 12` would
	// match other rows than DuckDB's (review of #387).
	if (IsDatePartName(name)) {
		for (const auto &type : arg_types) {
			if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
				why = name + " of a TIMESTAMP WITH TIME ZONE not pushed";
				return nullptr;
			}
		}
	}
	return mapping;
}

std::string ExpressionVocabulary::ApplyFunction(const FunctionMapping &mapping, const std::vector<std::string> &args) {
	std::string sql = mapping.sql_template;
	for (size_t i = 0; i < args.size(); i++) {
		std::string placeholder = "{" + std::to_string(i) + "}";
		size_t pos = 0;
		while ((pos = sql.find(placeholder, pos)) != std::string::npos) {
			sql.replace(pos, placeholder.length(), args[i]);
			pos += args[i].length();
		}
	}
	return sql;
}

bool ExpressionVocabulary::LikePatternText(const std::string &function_name, const std::string &needle,
										   std::string &out_pattern) {
	std::string lower_func = function_name;
	std::transform(lower_func.begin(), lower_func.end(), lower_func.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	const std::string escaped = EscapeLikePattern(needle);
	if (lower_func == "prefix") {
		out_pattern = escaped + "%";
	} else if (lower_func == "suffix") {
		out_pattern = "%" + escaped;
	} else if (lower_func == "contains") {
		out_pattern = "%" + escaped + "%";
	} else {
		return false;
	}
	return true;
}

std::string ExpressionVocabulary::LikePattern(const std::string &duckdb_pattern) {
	std::string pattern;
	pattern.reserve(duckdb_pattern.size());
	for (char c : duckdb_pattern) {
		if (c == '[') {
			pattern += "[[]";
		} else {
			pattern += c;
		}
	}
	return pattern;
}

std::string ExpressionVocabulary::Like(const std::string &value, const std::string &pattern) {
	return "(" + value + " LIKE " + pattern + ")";
}

}  // namespace mssql
}  // namespace duckdb

#include "query/mssql_sql_params.hpp"

#include <cctype>
#include <map>

#include "codec/integer_codec.hpp"
#include "codec/literal_format.hpp"
#include "codec/target_string_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"

namespace duckdb {
namespace mssql {

std::string NVarcharLiteral(const std::string &text) {
	std::string out;
	out.reserve(text.size() + 4);
	out += "N'";
	for (char c : text) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += '\'';
	return out;
}

std::string BuildExecuteSqlBatch(const std::string &statement, const std::string &declarations,
								 const std::vector<SqlParamAssignment> &assignments) {
	std::string batch = "EXEC sp_executesql " + NVarcharLiteral(statement);
	if (declarations.empty()) {
		return batch;
	}
	batch += ", " + NVarcharLiteral(declarations);
	for (const auto &a : assignments) {
		batch += ", @" + a.name + " = " + a.literal;
	}
	return batch;
}

//===----------------------------------------------------------------------===//
// SqlParamSet
//===----------------------------------------------------------------------===//

std::string SqlParamSet::Declarations() const {
	std::string out;
	for (const auto &p : params) {
		if (!out.empty()) {
			out += ", ";
		}
		out += "@" + p.name + " " + p.declaration;
	}
	return out;
}

std::string SqlParamSet::DeclareBlock() const {
	if (params.empty()) {
		return "";
	}
	std::string out = "DECLARE ";
	for (size_t i = 0; i < params.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += "@" + params[i].name + " " + params[i].declaration + " = " + params[i].literal;
	}
	out += ";\n";
	return out;
}

std::string SqlParamSet::ExecuteSqlBatch(const std::string &statement) const {
	std::string batch = DeclareBlock() + "EXEC sp_executesql " + NVarcharLiteral(statement);
	if (params.empty()) {
		return batch;
	}
	batch += ", " + NVarcharLiteral(Declarations());
	for (const auto &p : params) {
		batch += ", @" + p.name + " = @" + p.name;
	}
	return batch;
}

std::string SqlParamSet::ExecuteByHandleBatch(int32_t handle) const {
	std::string batch = DeclareBlock() + "EXEC sp_execute " + std::to_string(handle);
	for (const auto &p : params) {
		batch += ", @" + p.name;
	}
	return batch;
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

static std::string StringDeclaration(const LogicalType &type, const Value &value) {
	// A spec 060 annotation says exactly what the column is; a variable cannot
	// carry a COLLATE clause, so only the kind and the length travel -- the
	// server converts the value to the column's collation where it is compared.
	codec::TargetStringType spec;
	if (codec::TryGetTargetStringType(type, spec)) {
		std::string decl = spec.unicode ? "nvarchar(" : "varchar(";
		decl += codec::IsMaxLength(spec.length) ? std::string("max") : std::to_string(spec.length);
		return decl + ")";
	}
	// A plain VARCHAR: the two buckets Microsoft.Data.SqlClient uses, so a call
	// site has at most two declaration texts and therefore two plans.
	if (!value.IsNull() && StringValue::Get(value).size() > static_cast<size_t>(codec::MAX_NVARCHAR_LENGTH)) {
		return "nvarchar(max)";
	}
	return "nvarchar(4000)";
}

// Thin dispatch onto codec::integer's exported range rule -- the same one the
// BCP encoder enforces (#177). Kept as a one-liner rather than inlined twice so
// the HUGEINT and UHUGEINT arms cannot drift apart.
static bool ValueFitsDecimal38(const LogicalType &type, const Value &value) {
	if (value.IsNull()) {
		return true;
	}
	if (type.id() == LogicalTypeId::UHUGEINT) {
		return codec::integer::UhugeintFitsDecimal38(UhugeIntValue::Get(value));
	}
	return codec::integer::HugeintFitsDecimal38(HugeIntValue::Get(value));
}

std::string DeclarationForValue(const std::string &name, const LogicalType &type, const Value &value) {
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		throw InvalidInputException(
			"parameter '%s' is NULL of no type; cast it to the column's type, e.g. NULL::INTEGER", name);
	case LogicalTypeId::BOOLEAN:
		return "bit";
	case LogicalTypeId::UTINYINT:
		return "tinyint";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
		return "smallint";
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::INTEGER:
		return "int";
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::BIGINT:
		return "bigint";
	case LogicalTypeId::UBIGINT:
		return "decimal(20,0)";
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		// decimal(38,0) holds +/-(10^38 - 1) and HUGEINT reaches ~1.7e38, so the
		// widest values render as literals the server cannot convert --
		// "Arithmetic overflow error converting numeric to data type numeric",
		// which names neither the parameter nor the cause. Same rule the BCP
		// encoder has enforced since #177, shared from codec::integer rather than
		// re-derived. (UHUGEINT cannot actually reach here today: FormatSqlLiteral
		// refuses it a few lines below. Checked anyway so the two stay symmetric
		// if that arm is ever implemented.)
		if (!ValueFitsDecimal38(type, value)) {
			throw InvalidInputException(
				"parameter '%s' does not fit T-SQL decimal(38,0), the widest exact numeric SQL Server has; "
				"cast it to VARCHAR and convert server-side",
				name);
		}
		return "decimal(38,0)";
	case LogicalTypeId::FLOAT:
		return "real";
	case LogicalTypeId::DOUBLE:
		return "float";
	case LogicalTypeId::DECIMAL:
		return "decimal(" + std::to_string(DecimalType::GetWidth(type)) + "," +
			   std::to_string(DecimalType::GetScale(type)) + ")";
	case LogicalTypeId::DATE:
		return "date";
	case LogicalTypeId::TIME:
		return "time(6)";
	case LogicalTypeId::TIMESTAMP:
		return "datetime2(6)";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "datetime2(0)";
	case LogicalTypeId::TIMESTAMP_MS:
		return "datetime2(3)";
	case LogicalTypeId::TIMESTAMP_NS:
		return "datetime2(7)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "datetimeoffset(6)";
	case LogicalTypeId::BLOB:
		return "varbinary(max)";
	case LogicalTypeId::UUID:
		return "uniqueidentifier";
	case LogicalTypeId::VARCHAR:
		return StringDeclaration(type, value);
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
		throw InvalidInputException(
			"parameter '%s' is a %s; a table-valued parameter needs RPC, which this extension does not speak -- "
			"pass a scalar, or unnest on the DuckDB side",
			name, type.ToString());
	default:
		throw InvalidInputException("parameter '%s' has type %s, which has no SQL Server parameter type; cast it", name,
									type.ToString());
	}
}

//===----------------------------------------------------------------------===//
// BuildSqlParams
//===----------------------------------------------------------------------===//

static std::string Trimmed(const std::string &text) {
	size_t a = text.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) {
		return "";
	}
	size_t b = text.find_last_not_of(" \t\r\n");
	return text.substr(a, b - a + 1);
}

static bool IsSqlIdentifier(const std::string &name) {
	if (name.empty()) {
		return false;
	}
	if (!(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
		return false;
	}
	for (char c : name) {
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
			return false;
		}
	}
	return true;
}

// "@ts datetime, @c varchar(8), @d decimal(10,2)" -> {ts: "datetime", ...},
// splitting on the commas that are not inside parentheses.
static std::map<std::string, std::string> ParseDeclarations(const std::string &text) {
	std::map<std::string, std::string> out;
	std::string item;
	int depth = 0;
	auto flush = [&]() {
		std::string t = Trimmed(item);
		item.clear();
		if (t.empty()) {
			return;
		}
		if (t[0] != '@') {
			throw InvalidInputException("declaration '%s' must start with '@name'", t);
		}
		size_t sp = t.find_first_of(" \t");
		if (sp == std::string::npos) {
			throw InvalidInputException("declaration '%s' names a parameter without a type", t);
		}
		std::string name = t.substr(1, sp - 1);
		std::string decl = Trimmed(t.substr(sp + 1));
		if (!IsSqlIdentifier(name) || decl.empty()) {
			throw InvalidInputException("declaration '%s' is not '@name <type>'", t);
		}
		std::string key = StringUtil::Lower(name);
		if (out.count(key)) {
			throw InvalidInputException("declaration list names '@%s' twice", name);
		}
		out[key] = decl;
	};
	for (char c : text) {
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		}
		if (c == ',' && depth == 0) {
			flush();
		} else {
			item += c;
		}
	}
	flush();
	return out;
}

SqlParamSet BuildSqlParams(const Value &params, const std::string &declarations_override) {
	if (params.type().id() != LogicalTypeId::STRUCT) {
		throw InvalidInputException("parameters must be a STRUCT of name -> value, e.g. {'a': 1, 'b': 'x'}; got %s",
									params.type().ToString());
	}
	if (params.IsNull()) {
		throw InvalidInputException("parameters must be a STRUCT of name -> value, not NULL");
	}
	const auto &child_types = StructType::GetChildTypes(params.type());
	const auto &children = StructValue::GetChildren(params);
	std::map<std::string, std::string> declared;
	const bool has_override = !Trimmed(declarations_override).empty();
	if (has_override) {
		declared = ParseDeclarations(declarations_override);
	}

	SqlParamSet set;
	std::map<std::string, std::string> seen;  // lower-cased -> as written
	for (size_t i = 0; i < child_types.size(); i++) {
		const std::string &name = child_types[i].first.GetIdentifierName();
		const LogicalType &type = child_types[i].second;
		const Value &value = children[i];
		if (!IsSqlIdentifier(name)) {
			throw InvalidInputException(
				"parameter name '%s' is not a T-SQL identifier (letters, digits and '_', not starting with a digit)",
				name);
		}
		// T-SQL variable names are case-insensitive, so two keys differing only in
		// case DECLARE the same variable twice. A STRUCT built by the parser is
		// already safe -- Identifier compares case-insensitively and both
		// struct-literal sites use identifier_set_t, so {'a':1,'A':2} is refused
		// at bind with "Duplicate struct entry name". This is the backstop for a
		// STRUCT that did not come from there: a direct Value::STRUCT, an
		// inferred-schema file read, another extension. Without it the batch
		// reaches the server and returns "The variable name '@A' has already been
		// declared", which does not say which key to change.
		std::string key = StringUtil::Lower(name);
		auto dup = seen.find(key);
		if (dup != seen.end()) {
			throw InvalidInputException(
				"parameter names '%s' and '%s' differ only in case; T-SQL variable names are "
				"case-insensitive, so both declare @%s",
				dup->second, name, key);
		}
		seen[key] = name;
		SqlParam p;
		p.name = name;
		if (has_override) {
			auto it = declared.find(key);
			if (it == declared.end()) {
				throw InvalidInputException("parameter '%s' has a value but no declaration in '%s'", name,
											declarations_override);
			}
			// Naming the type does not make the value fit it: a 39-digit HUGEINT
			// under an explicit `@p decimal(38,0)` used to skip the check below
			// and come back as the server's bare "Arithmetic overflow", which is
			// the error this guard exists to replace.
			if ((type.id() == LogicalTypeId::HUGEINT || type.id() == LogicalTypeId::UHUGEINT) &&
				!ValueFitsDecimal38(type, value)) {
				throw InvalidInputException(
					"parameter '%s' does not fit T-SQL decimal(38,0), the widest exact numeric SQL Server has; "
					"cast it to VARCHAR and convert server-side",
					name);
			}
			p.declaration = it->second;
		} else {
			p.declaration = DeclarationForValue(name, type, value);
		}
		p.literal = codec::FormatSqlLiteral(value, type, codec::LiteralContext::InsertValues);
		set.params.push_back(std::move(p));
	}
	if (has_override) {
		for (const auto &d : declared) {
			if (!seen.count(d.first)) {
				throw InvalidInputException("declaration '@%s %s' has no value in the parameter STRUCT", d.first,
											d.second);
			}
		}
	}
	return set;
}

}  // namespace mssql
}  // namespace duckdb

#include "query/mssql_sql_params.hpp"

#include <cctype>
#include <map>

#include "codec/integer_codec.hpp"
#include "codec/literal_format.hpp"
#include "codec/sql_server_type_text.hpp"
#include "codec/target_string_type.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"
#include "tds/encoding/utf16.hpp"
#include "tds/tds_rpc.hpp"

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

std::string SqlParamSet::Add(const std::string &declaration, const std::string &literal) {
	SqlParam p;
	p.name = "p" + std::to_string(params.size());
	p.declaration = declaration;
	p.literal = literal;
	params.push_back(std::move(p));
	return params.back().name;
}

std::string SqlParamSet::Add(const std::string &declaration, const std::string &literal, const Value &value) {
	auto name = Add(declaration, literal);
	params.back().value = value;
	params.back().has_value = true;
	return name;
}

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
			"parameter '%s' is a %s; table-valued parameters are not supported -- "
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
	// 128 is SQL Server's identifier limit (error 103 past it); an RPC
	// parameter name has to fit a B_VARCHAR as well (spec 083).
	if (name.empty() || name.size() > 128) {
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
				"parameter name '%s' is not a T-SQL identifier (letters, digits and '_', not starting with a digit, "
				"at most 128 characters)",
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
		p.value = value;
		p.has_value = true;
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

//===----------------------------------------------------------------------===//
// Spec 083: sp_executesql over RPC
//===----------------------------------------------------------------------===//
//
// The frame (procedure id, parameter names and flags) is tds::RpcRequestBuilder's;
// each parameter's TYPE_INFO and value are the codecs' (BCPWriter::WriteTypeInfo,
// BCPRowEncoder), the same bytes a bulk load writes for a column of that type.

namespace {

//! A declaration's type as FromServerColumn takes it: the name and
//! sys.columns' max_length (bytes, -1 for MAX), precision and scale.
struct DeclaredType {
	std::string name;
	int16_t max_length = 0;
	uint8_t precision = 0;
	uint8_t scale = 0;
};

//! False when the declaration is not one this encoder knows -- a CLR type
//! (geometry, geography, hierarchyid), sql_variant, an alias type
//! (`dbo.Name`), a synonym spelling, modifiers out of range: the caller then
//! sends the batch form, which the server parses as it always has.
bool ParseDeclaredType(const std::string &declaration, DeclaredType &result) {
	codec::SqlServerTypeText text;
	if (!codec::ParseSqlServerTypeText(declaration, text)) {
		return false;
	}
	const auto &args = text.args;
	bool ok = true;
	auto number = [&](size_t i, int fallback, int max) -> int {
		if (i >= args.size()) {
			return fallback;
		}
		const auto &arg = args[i];
		if (arg.empty() || arg.size() > 5 || arg.find_first_not_of("0123456789") != std::string::npos) {
			ok = false;
			return fallback;
		}
		const int v = std::atoi(arg.c_str());
		if (v > max) {
			ok = false;
		}
		return v;
	};
	const auto &n = text.base;
	result.name = n;
	if (n == "decimal" || n == "numeric") {
		result.precision = static_cast<uint8_t>(number(0, 18, 38));
		result.scale = static_cast<uint8_t>(number(1, 0, 38));
		ok = ok && result.precision >= 1 && result.scale <= result.precision && args.size() <= 2;
	} else if (n == "money" || n == "smallmoney") {
		result.name = "decimal";
		result.precision = n == "money" ? 19 : 10;
		result.scale = 4;
		ok = args.empty();
	} else if (n == "datetime" || n == "smalldatetime") {
		// Sent as datetime2(7); the server converts it to the declared type as
		// it converts the literal today (no second rounding).
		result.name = "datetime2";
		result.scale = 7;
		ok = args.empty();
	} else if (n == "datetime2" || n == "time" || n == "datetimeoffset") {
		result.scale = static_cast<uint8_t>(number(0, 7, 7));
		ok = ok && args.size() <= 1;
	} else if (n == "bit" || n == "tinyint" || n == "smallint" || n == "int" || n == "bigint" || n == "real" ||
			   n == "date" || n == "uniqueidentifier") {
		ok = args.empty();
	} else if (n == "float") {
		ok = args.size() <= 1;
	} else if (n == "char" || n == "varchar" || n == "nchar" || n == "nvarchar" || n == "binary" || n == "varbinary") {
		// The value goes as nvarchar / varbinary of its own length; the
		// declared length only matters to the server.
		ok = args.size() <= 1;
	} else if (n == "text" || n == "ntext" || n == "image" || n == "xml" || n == "sysname") {
		ok = args.empty();
	} else {
		return false;
	}
	return ok;
}

bool IsCharFamily(const std::string &name) {
	return name == "char" || name == "varchar" || name == "nchar" || name == "nvarchar" || name == "text" ||
		   name == "ntext" || name == "xml" || name == "sysname";
}

bool IsBinaryFamily(const std::string &name) {
	return name == "binary" || name == "varbinary" || name == "image";
}

//! TYPE_INFO and value of `value` as `col` declares it, appended to the open
//! parameter.
void AppendTypedValue(tds::RpcRequestBuilder &rpc, const BCPColumnMetadata &col, const Value &value) {
	vector<uint8_t> bytes;
	BCPWriter::WriteTypeInfo(bytes, col);
	if (col.duckdb_type.id() == LogicalTypeId::DECIMAL && !value.IsNull()) {
		// The mantissa at the declared scale, read by the value's physical type.
		// BCPRowEncoder::EncodeValue's Value path takes GetValue<hugeint_t>(),
		// the value cast to an integer -- the fraction is lost, and a 38-digit
		// value came out wrong (the bulk path encodes from vectors and never
		// reaches it).
		hugeint_t mantissa;
		switch (col.duckdb_type.InternalType()) {
		case PhysicalType::INT16:
			mantissa = hugeint_t(value.GetValueUnsafe<int16_t>());
			break;
		case PhysicalType::INT32:
			mantissa = hugeint_t(value.GetValueUnsafe<int32_t>());
			break;
		case PhysicalType::INT64:
			mantissa = hugeint_t(value.GetValueUnsafe<int64_t>());
			break;
		default:
			mantissa = value.GetValueUnsafe<hugeint_t>();
			break;
		}
		tds::encoding::BCPRowEncoder::EncodeDecimal(bytes, mantissa, col.precision, col.scale);
	} else {
		tds::encoding::BCPRowEncoder::EncodeValue(bytes, value, col);
	}
	rpc.Append(bytes.data(), bytes.size());
}

//! A string parameter as nvarchar: inline up to 8000 bytes, PLP past that.
//! A char-family declaration takes it too -- the server converts an nvarchar
//! argument to the declared varchar exactly as it converts the N'...' literal
//! of the batch form, so the semantics (and #361's seek) do not change.
void AddNVarcharParam(tds::RpcRequestBuilder &rpc, const std::string &name, const Value &value) {
	// UTF-16 never takes more than twice the UTF-8 bytes, so only a long value
	// is measured exactly (the encoder converts it once more).
	idx_t utf16_bytes = 0;
	if (!value.IsNull()) {
		const auto &utf8 = StringValue::Get(value);
		utf16_bytes = utf8.size() * 2 <= 8000 ? utf8.size() * 2 : tds::encoding::Utf16LEEncode(utf8).size();
	}
	rpc.BeginParam(name);
	AppendTypedValue(
		rpc, BCPColumnMetadata::FromServerColumn(name, "nvarchar", utf16_bytes > 8000 ? -1 : 8000, 0, 0, true, ""),
		value);
}

//! False when this parameter has no RPC encoding -- an unknown declaration, or
//! a value the declared type cannot take: the whole call then goes as the batch
//! form, and the server answers it as it always has.
bool AddTypedParam(tds::RpcRequestBuilder &rpc, const SqlParam &param, bool positional = false) {
	// sp_executesql binds by name; sp_execute takes its values in declaration
	// order, unnamed.
	const auto name = positional ? std::string() : "@" + param.name;
	DeclaredType declared;
	if (!ParseDeclaredType(param.declaration, declared)) {
		return false;
	}
	if (IsCharFamily(declared.name)) {
		AddNVarcharParam(
			rpc, name,
			param.value.IsNull() ? Value(LogicalType::VARCHAR) : param.value.DefaultCastAs(LogicalType::VARCHAR));
		return true;
	}
	if (IsBinaryFamily(declared.name)) {
		Value blob(LogicalType::BLOB);
		if (!param.value.IsNull()) {
			auto cast = param.value.DefaultTryCastAs(LogicalType::BLOB, nullptr, true);
			if (!cast) {
				return false;
			}
			blob = std::move(*cast);
		}
		const idx_t bytes = blob.IsNull() ? 0 : StringValue::Get(blob).size();
		rpc.BeginParam(name);
		AppendTypedValue(
			rpc, BCPColumnMetadata::FromServerColumn(name, "varbinary", bytes > 8000 ? -1 : 8000, 0, 0, true, ""),
			blob);
		return true;
	}
	auto col = BCPColumnMetadata::FromServerColumn(name, declared.name, declared.max_length, declared.precision,
												   declared.scale, true, "");
	if (col.bulk_unsupported || col.duckdb_type.id() == LogicalTypeId::VARCHAR) {
		return false;
	}
	const auto value_id = param.value.type().id();
	const bool timestamp_value = value_id == LogicalTypeId::TIMESTAMP || value_id == LogicalTypeId::TIMESTAMP_SEC ||
								 value_id == LogicalTypeId::TIMESTAMP_MS || value_id == LogicalTypeId::TIMESTAMP_NS;
	Value typed(col.duckdb_type);
	if (col.duckdb_type.id() == LogicalTypeId::TIMESTAMP && timestamp_value) {
		// datetime2 from a TIMESTAMP_* value in its own unit: the encoder rescales
		// from the value's type, where a cast to TIMESTAMP first would drop
		// TIMESTAMP_NS's seventh digit (review of spec 083).
		col.duckdb_type = param.value.type();
		typed = param.value;
	} else if (!param.value.IsNull()) {
		auto cast = param.value.DefaultTryCastAs(col.duckdb_type, nullptr, true);
		if (!cast) {
			return false;
		}
		typed = std::move(*cast);
	}
	rpc.BeginParam(name);
	AppendTypedValue(rpc, col, typed);
	return true;
}

}  // namespace

tds::Request BuildExecuteSqlRequest(const std::string &statement, const std::string &declarations,
									const std::vector<std::pair<std::string, std::string>> &string_values) {
	// Named values need their declarations: the batch form drops them without
	// one, and the two forms must say the same thing.
	D_ASSERT(!declarations.empty() || string_values.empty());
	std::vector<SqlParamAssignment> assignments;
	tds::RpcRequestBuilder rpc(tds::RPC_PROC_SP_EXECUTESQL);
	AddNVarcharParam(rpc, "", Value(statement));
	if (!declarations.empty()) {
		AddNVarcharParam(rpc, "", Value(declarations));
	}
	for (const auto &named : string_values) {
		AddNVarcharParam(rpc, "@" + named.first, Value(named.second));
		assignments.push_back({named.first, NVarcharLiteral(named.second)});
	}
	return tds::Request::Rpc(rpc.Finish(), BuildExecuteSqlBatch(statement, declarations, assignments));
}

tds::Request SqlParamSet::ExecuteSqlRequest(const std::string &statement) const {
	for (const auto &p : params) {
		if (!p.has_value) {
			return tds::Request(ExecuteSqlBatch(statement));
		}
	}
	tds::RpcRequestBuilder rpc(tds::RPC_PROC_SP_EXECUTESQL);
	AddNVarcharParam(rpc, "", Value(statement));
	if (!params.empty()) {
		AddNVarcharParam(rpc, "", Value(Declarations()));
		for (const auto &p : params) {
			if (!AddTypedParam(rpc, p)) {
				return tds::Request(ExecuteSqlBatch(statement));
			}
		}
	}
	return tds::Request::Rpc(rpc.Finish(), ExecuteSqlBatch(statement));
}

tds::Request SqlParamSet::ExecuteByHandleRequest(int32_t handle) const {
	for (const auto &p : params) {
		if (!p.has_value) {
			return tds::Request(ExecuteByHandleBatch(handle));
		}
	}
	tds::RpcRequestBuilder rpc(tds::RPC_PROC_SP_EXECUTE);
	// @handle int, then the values in declaration order.
	rpc.BeginParam("");
	AppendTypedValue(rpc, BCPColumnMetadata::FromServerColumn("", "int", 4, 0, 0, true, ""), Value::INTEGER(handle));
	for (const auto &p : params) {
		if (!AddTypedParam(rpc, p, true)) {
			return tds::Request(ExecuteByHandleBatch(handle));
		}
	}
	return tds::Request::Rpc(rpc.Finish(), ExecuteByHandleBatch(handle));
}

}  // namespace mssql
}  // namespace duckdb

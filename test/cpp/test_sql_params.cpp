// test/cpp/test_sql_params.cpp
// Unit tests for query/mssql_sql_params (spec 075): the text that carries a
// name to the metadata queries (W4) and a caller's parameters to
// mssql_scan_params / mssql_exec_params (W5). No SQL Server needed.
//
// Covers:
//   - NVarcharLiteral doubles every quote and nothing else.
//   - BuildExecuteSqlBatch: no declarations -> bare sp_executesql; with them
//     the assignments follow in order.
//   - DeclarationForValue: spec 075 W5's table, one line per type; the
//     nvarchar(4000) / nvarchar(max) bucket; the three refusals name the fix.
//   - BuildSqlParams: derived declarations, the DECLARE block, both batch
//     forms, the override list (parenthesised commas, case-insensitive
//     names) and its errors; a non-STRUCT and a key that is not an
//     identifier.
//   - ExecuteByHandleRequest (spec 083): sp_execute as an RPC call, its bytes
//     and parameter order; a CLR declaration keeps the batch form.
//
// Build & run:
//   make test-cpp-run

#include "codec/target_string_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "query/mssql_sql_params.hpp"

#include <iostream>
#include <string>
#include <vector>

using duckdb::Hugeint;
using duckdb::hugeint_t;
using duckdb::Identifier;
using duckdb::InvalidInputException;
using duckdb::LogicalType;
using duckdb::NumericLimits;
using duckdb::Value;
using duckdb::mssql::BuildExecuteSqlBatch;
using duckdb::mssql::BuildSqlParams;
using duckdb::mssql::DeclarationForValue;
using duckdb::mssql::NVarcharLiteral;
using duckdb::mssql::SqlParamAssignment;
using duckdb::mssql::SqlParamSet;

namespace {

int failures = 0;

#define CHECK_EQ(actual, expected)                                                                       \
	do {                                                                                                 \
		const auto &_a = (actual);                                                                       \
		const auto &_e = (expected);                                                                     \
		if (!(_a == _e)) {                                                                               \
			++failures;                                                                                  \
			std::cerr << "FAIL [" << __LINE__ << "] " #actual " == " #expected << "\n  actual:   " << _a \
					  << "\n  expected: " << _e << "\n";                                                 \
		}                                                                                                \
	} while (0)

// The message must carry `needle`: an error that does not name the fix is a
// worse error than none.
#define CHECK_THROWS_WITH(expr, needle)                                                                            \
	do {                                                                                                           \
		bool _threw = false;                                                                                       \
		try {                                                                                                      \
			(void)(expr);                                                                                          \
		} catch (const InvalidInputException &e) {                                                                 \
			_threw = true;                                                                                         \
			std::string _msg = e.what();                                                                           \
			if (_msg.find(needle) == std::string::npos) {                                                          \
				++failures;                                                                                        \
				std::cerr << "FAIL [" << __LINE__ << "] " #expr " threw without '" << (needle) << "':\n  " << _msg \
						  << "\n";                                                                                 \
			}                                                                                                      \
		}                                                                                                          \
		if (!_threw) {                                                                                             \
			++failures;                                                                                            \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr " did not throw\n";                                    \
		}                                                                                                          \
	} while (0)

Value Struct(duckdb::child_list_t<Value> children) {
	return Value::STRUCT(std::move(children));
}

void TestNVarcharLiteral() {
	CHECK_EQ(NVarcharLiteral("dbo"), std::string("N'dbo'"));
	CHECK_EQ(NVarcharLiteral("it's"), std::string("N'it''s'"));
	CHECK_EQ(NVarcharLiteral("[a]]b"), std::string("N'[a]]b'"));
	CHECK_EQ(NVarcharLiteral(""), std::string("N''"));
}

void TestBuildExecuteSqlBatch() {
	CHECK_EQ(BuildExecuteSqlBatch("SELECT 1", "", {}), std::string("EXEC sp_executesql N'SELECT 1'"));
	std::vector<SqlParamAssignment> names{{"s", NVarcharLiteral("dbo")}, {"t", NVarcharLiteral("it's")}};
	CHECK_EQ(
		BuildExecuteSqlBatch("SELECT OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t))", "@s sysname, @t sysname", names),
		std::string("EXEC sp_executesql N'SELECT OBJECT_ID(QUOTENAME(@s) + N''.'' + QUOTENAME(@t))', "
					"N'@s sysname, @t sysname', @s = N'dbo', @t = N'it''s'"));
}

void TestDeclarationForValue() {
	CHECK_EQ(DeclarationForValue("p", LogicalType::BOOLEAN, Value::BOOLEAN(true)), std::string("bit"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::UTINYINT, Value::UTINYINT(1)), std::string("tinyint"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TINYINT, Value::TINYINT(1)), std::string("smallint"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::SMALLINT, Value::SMALLINT(1)), std::string("smallint"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::USMALLINT, Value::USMALLINT(1)), std::string("int"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::INTEGER, Value::INTEGER(1)), std::string("int"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::UINTEGER, Value::UINTEGER(1)), std::string("bigint"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::BIGINT, Value::BIGINT(1)), std::string("bigint"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::UBIGINT, Value::UBIGINT(1)), std::string("decimal(20,0)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::HUGEINT, Value::HUGEINT(1)), std::string("decimal(38,0)"));
	// decimal(38,0) holds +/-(10^38 - 1). 38 nines is the widest that fits; the
	// 39-digit neighbours must be refused CLIENT-side, or the server answers with
	// a bare "Arithmetic overflow" naming neither the parameter nor the cause.
	// HUGEINT min is the case an abs-then-compare would miss (#177).
	{
		const hugeint_t max38 = Hugeint::POWERS_OF_TEN[38] - 1;
		CHECK_EQ(DeclarationForValue("p", LogicalType::HUGEINT, Value::HUGEINT(max38)), std::string("decimal(38,0)"));
		CHECK_EQ(DeclarationForValue("p", LogicalType::HUGEINT, Value::HUGEINT(Hugeint::Negate(max38))),
				 std::string("decimal(38,0)"));
		CHECK_THROWS_WITH(DeclarationForValue("p", LogicalType::HUGEINT, Value::HUGEINT(Hugeint::POWERS_OF_TEN[38])),
						  "does not fit T-SQL decimal(38,0)");
		CHECK_THROWS_WITH(
			DeclarationForValue("p", LogicalType::HUGEINT, Value::HUGEINT(NumericLimits<hugeint_t>::Minimum())),
			"does not fit T-SQL decimal(38,0)");
	}
	CHECK_EQ(DeclarationForValue("p", LogicalType::FLOAT, Value::FLOAT(1.5f)), std::string("real"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::DOUBLE, Value::DOUBLE(1.5)), std::string("float"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::DECIMAL(10, 2), Value::DECIMAL(int64_t(1234), 10, 2)),
			 std::string("decimal(10,2)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::DATE, Value(LogicalType::DATE)), std::string("date"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIME, Value(LogicalType::TIME)), std::string("time(6)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIMESTAMP, Value(LogicalType::TIMESTAMP)),
			 std::string("datetime2(6)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIMESTAMP_S, Value(LogicalType::TIMESTAMP_S)),
			 std::string("datetime2(0)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIMESTAMP_MS, Value(LogicalType::TIMESTAMP_MS)),
			 std::string("datetime2(3)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIMESTAMP_NS, Value(LogicalType::TIMESTAMP_NS)),
			 std::string("datetime2(7)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::TIMESTAMP_TZ, Value(LogicalType::TIMESTAMP_TZ)),
			 std::string("datetimeoffset(6)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::BLOB, Value::BLOB_RAW(std::string("\x01", 1))),
			 std::string("varbinary(max)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::UUID, Value(LogicalType::UUID)), std::string("uniqueidentifier"));

	// The VARCHAR bucket: SqlClient's two sizes, so a call site has two plans at most.
	CHECK_EQ(DeclarationForValue("p", LogicalType::VARCHAR, Value("x")), std::string("nvarchar(4000)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::VARCHAR, Value(std::string(4000, 'x'))),
			 std::string("nvarchar(4000)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::VARCHAR, Value(std::string(4001, 'x'))),
			 std::string("nvarchar(max)"));
	CHECK_EQ(DeclarationForValue("p", LogicalType::VARCHAR, Value(LogicalType::VARCHAR)),
			 std::string("nvarchar(4000)"));

	// The refusals name the parameter and the fix.
	CHECK_THROWS_WITH(DeclarationForValue("p", LogicalType::SQLNULL, Value()), "parameter 'p' is NULL of no type");
	CHECK_THROWS_WITH(DeclarationForValue("p", LogicalType::LIST(LogicalType::INTEGER),
										  Value::LIST(LogicalType::INTEGER, {Value::INTEGER(1)})),
					  "table-valued parameters are not supported");
	CHECK_THROWS_WITH(DeclarationForValue("p", LogicalType::INTERVAL, Value(LogicalType::INTERVAL)),
					  "no SQL Server parameter type");
}

void TestBuildSqlParamsDerived() {
	auto set = BuildSqlParams(Struct({{Identifier("a"), Value::INTEGER(1)}, {Identifier("b"), Value("it's")}}), "");
	CHECK_EQ(set.params.size(), size_t(2));
	CHECK_EQ(set.Declarations(), std::string("@a int, @b nvarchar(4000)"));
	CHECK_EQ(set.DeclareBlock(), std::string("DECLARE @a int = 1, @b nvarchar(4000) = N'it''s';\n"));
	CHECK_EQ(set.ExecuteSqlBatch("SELECT @a, @b"),
			 std::string("DECLARE @a int = 1, @b nvarchar(4000) = N'it''s';\n"
						 "EXEC sp_executesql N'SELECT @a, @b', N'@a int, @b nvarchar(4000)', @a = @a, @b = @b"));
	CHECK_EQ(set.ExecuteByHandleBatch(7),
			 std::string("DECLARE @a int = 1, @b nvarchar(4000) = N'it''s';\nEXEC sp_execute 7, @a, @b"));

	// A typed NULL is a NULL of that type.
	auto null_set = BuildSqlParams(Struct({{Identifier("n"), Value(LogicalType::BIGINT)}}), "");
	CHECK_EQ(null_set.DeclareBlock(), std::string("DECLARE @n bigint = NULL;\n"));

	// No parameters at all: the batch is the bare sp_executesql.
	SqlParamSet empty;
	CHECK_EQ(empty.Declarations(), std::string(""));
	CHECK_EQ(empty.DeclareBlock(), std::string(""));
	CHECK_EQ(empty.ExecuteSqlBatch("SELECT 1"), std::string("EXEC sp_executesql N'SELECT 1'"));
	CHECK_EQ(empty.ExecuteByHandleBatch(3), std::string("EXEC sp_execute 3"));
}

void TestBuildSqlParamsOverride() {
	// Commas inside parentheses do not split; names match case-insensitively.
	auto set = BuildSqlParams(Struct({{Identifier("d"), Value::DECIMAL(int64_t(150), 10, 2)},
									  {Identifier("Ts"), Value(LogicalType::TIMESTAMP)}}),
							  " @d decimal(10,2) , @ts datetime ");
	CHECK_EQ(set.params[0].declaration, std::string("decimal(10,2)"));
	CHECK_EQ(set.params[1].declaration, std::string("datetime"));
	CHECK_EQ(set.Declarations(), std::string("@d decimal(10,2), @Ts datetime"));

	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value::INTEGER(1)}}), "@q int"),
					  "parameter 'p' has a value but no declaration in '@q int'");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value::INTEGER(1)}}), "@p int, @q int"),
					  "declaration '@q int' has no value in the parameter STRUCT");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value::INTEGER(1)}}), "p int"),
					  "must start with '@name'");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value::INTEGER(1)}}), "@p"),
					  "names a parameter without a type");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value::INTEGER(1)}}), "@p int, @p int"),
					  "names '@p' twice");
}

void TestBuildSqlParamsCaseCollision() {
	// Value::STRUCT bypasses struct_pack's identifier_set_t dedupe, so this is the
	// one construction where two keys differing only in case actually reach
	// BuildSqlParams. Both DECLARE @a, which the server reports as "The variable
	// name '@A' has already been declared" -- an error that does not say which key
	// to change.
	CHECK_THROWS_WITH(
		BuildSqlParams(Struct({{Identifier("a"), Value::INTEGER(1)}, {Identifier("A"), Value::INTEGER(2)}}), ""),
		"differ only in case");
	// Naming the type does not make the value fit it: the declarations override
	// used to skip the range check entirely.
	CHECK_THROWS_WITH(
		BuildSqlParams(Struct({{Identifier("p"), Value::HUGEINT(Hugeint::POWERS_OF_TEN[38])}}), "@p decimal(38,0)"),
		"does not fit T-SQL decimal(38,0)");
}

void TestBuildSqlParamsRefusals() {
	CHECK_THROWS_WITH(BuildSqlParams(Value::INTEGER(5), ""), "parameters must be a STRUCT of name -> value");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("my p"), Value::INTEGER(1)}}), ""),
					  "parameter name 'my p' is not a T-SQL identifier");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("1p"), Value::INTEGER(1)}}), ""),
					  "parameter name '1p' is not a T-SQL identifier");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value()}}), ""), "parameter 'p' is NULL of no type");
}

void TestExecuteByHandleRequest() {
	// sp_execute over RPC: ProcID 12, no flags, then the handle and the values
	// as unnamed parameters, in the STRUCT's order (sp_prepare's), whatever
	// order the declarations list was written in.
	auto set = BuildSqlParams(Struct({{Identifier("a"), Value::INTEGER(42)}, {Identifier("b"), Value("x")}}),
							  "@b nvarchar(10), @a int");
	auto request = set.ExecuteByHandleRequest(7);
	CHECK_EQ(request.IsRpc(), true);
	const std::vector<uint8_t> prefix = {0xFF, 0xFF, 0x0C, 0x00, 0x00, 0x00,  // ProcIDSwitch, sp_execute, OptionFlags
										 0x00, 0x00, 0x26, 0x04, 0x04, 0x07, 0x00, 0x00, 0x00,	// @handle int = 7
										 0x00, 0x00, 0x26, 0x04, 0x04, 0x2A, 0x00, 0x00, 0x00,	// @a int = 42
										 0x00, 0x00, 0xE7};										// @b nvarchar ...
	CHECK_EQ(request.rpc_body.size() > prefix.size(), true);
	CHECK_EQ(std::vector<uint8_t>(request.rpc_body.begin(), request.rpc_body.begin() + prefix.size()) == prefix, true);
	// The batch text rides along for logs and errors.
	CHECK_EQ(request.sql, set.ExecuteByHandleBatch(7));

	// A declaration the encoder does not know (a CLR type) keeps the batch form.
	auto geometry = BuildSqlParams(Struct({{Identifier("g"), Value("POINT (1 2)")}}), "@g geometry");
	auto fallback = geometry.ExecuteByHandleRequest(7);
	CHECK_EQ(fallback.IsRpc(), false);
	CHECK_EQ(fallback.sql, geometry.ExecuteByHandleBatch(7));
}

void TestExecuteSqlRequestForm() {
	// A value goes as an RPC parameter only where DuckDB's conversion of it
	// cannot differ from the server's; otherwise the whole call keeps the batch
	// form and the server converts the typed literal, as before spec 083
	// (review of #414).
	auto IsRpc = [](const Value &v, const std::string &declarations) {
		return BuildSqlParams(Struct({{Identifier("p"), v}}), declarations).ExecuteSqlRequest("SELECT @p").IsRpc();
	};
	// Of the declared family, or converted with nothing lost.
	CHECK_EQ(IsRpc(Value("abc"), "@p varchar(5)"), true);
	CHECK_EQ(IsRpc(Value::BLOB("abc"), "@p varbinary(10)"), true);
	CHECK_EQ(IsRpc(Value::INTEGER(42), "@p bigint"), true);
	CHECK_EQ(IsRpc(Value::BOOLEAN(true), "@p int"), true);
	CHECK_EQ(IsRpc(Value("42"), "@p int"), true);
	CHECK_EQ(IsRpc(Value::DECIMAL(int16_t(125), 3, 1), "@p decimal(10,2)"), true);
	CHECK_EQ(IsRpc(Value::DATE(duckdb::date_t(0)), "@p datetime2(6)"), true);
	CHECK_EQ(IsRpc(Value::DOUBLE(0.5), "@p float"), true);
	CHECK_EQ(IsRpc(Value(LogicalType::BOOLEAN), "@p varchar(5)"), true);  // a NULL is a NULL
	CHECK_EQ(IsRpc(Value(LogicalType::VARCHAR), "@p varbinary(10)"), true);
	// A string declaration and a value of another type: `true` is `1` there.
	CHECK_EQ(IsRpc(Value::BOOLEAN(true), "@p varchar(5)"), false);
	CHECK_EQ(IsRpc(Value::TIMESTAMP(duckdb::timestamp_t(0)), "@p varchar(30)"), false);
	CHECK_EQ(IsRpc(Value::BLOB("abc"), "@p varchar(5)"), false);
	// A binary declaration and a string: error 257 there (no implicit conversion).
	CHECK_EQ(IsRpc(Value("abc"), "@p varbinary(10)"), false);
	// A lossy conversion: 12.5 into an int is 12 there, 13 by DuckDB's cast.
	CHECK_EQ(IsRpc(Value::DECIMAL(int16_t(125), 3, 1), "@p int"), false);
	CHECK_EQ(IsRpc(Value::DOUBLE(2.5), "@p int"), false);
	CHECK_EQ(IsRpc(Value("12.5"), "@p int"), false);
	// A floating-point value into another type: decimal text there, the binary
	// value here (0.1f as a float: 0.100000001 there, 0.10000000149011612 here).
	CHECK_EQ(IsRpc(Value::FLOAT(0.1f), "@p float"), false);
	CHECK_EQ(IsRpc(Value::DOUBLE(0.1), "@p decimal(38,20)"), false);

	// The native string types (spec 060) are VARCHARs: they take the string
	// path, with their own declaration (no list) or a written one, and convert
	// into another type by the same rule.
	duckdb::mssql::codec::TargetStringType varchar_spec;
	varchar_spec.unicode = false;
	varchar_spec.length = 10;
	duckdb::mssql::codec::TargetStringType nvarchar_spec;
	nvarchar_spec.unicode = true;
	nvarchar_spec.length = duckdb::mssql::codec::MAX_LENGTH;
	const auto mssql_varchar = duckdb::mssql::codec::MakeTargetStringType(varchar_spec);
	const auto mssql_nvarchar = duckdb::mssql::codec::MakeTargetStringType(nvarchar_spec);
	const auto Native = [](const char *text, const LogicalType &type) { return Value(text).DefaultCastAs(type); };
	CHECK_EQ(IsRpc(Native("abc", mssql_varchar), ""), true);
	CHECK_EQ(IsRpc(Native("abc", mssql_nvarchar), ""), true);
	CHECK_EQ(IsRpc(Native("abc", mssql_varchar), "@p varchar(10)"), true);
	CHECK_EQ(IsRpc(Native("abc", mssql_nvarchar), "@p nvarchar(max)"), true);
	CHECK_EQ(IsRpc(Native("42", mssql_varchar), "@p int"), true);
	CHECK_EQ(IsRpc(Native("12.5", mssql_nvarchar), "@p int"), false);
	CHECK_EQ(IsRpc(Native("abc", mssql_varchar), "@p varbinary(10)"), false);
	CHECK_EQ(IsRpc(Value(mssql_varchar), "@p varchar(10)"), true);
}

}  // namespace

int main() {
	TestNVarcharLiteral();
	TestBuildExecuteSqlBatch();
	TestDeclarationForValue();
	TestBuildSqlParamsDerived();
	TestBuildSqlParamsOverride();
	TestBuildSqlParamsRefusals();
	TestBuildSqlParamsCaseCollision();
	TestExecuteByHandleRequest();
	TestExecuteSqlRequestForm();
	if (failures) {
		std::cerr << failures << " failure(s)\n";
		return 1;
	}
	std::cout << "test_sql_params: all checks passed\n";
	return 0;
}

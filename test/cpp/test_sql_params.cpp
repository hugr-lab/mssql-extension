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
//
// Build & run:
//   make test-cpp-run

#include "duckdb/common/exception.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "query/mssql_sql_params.hpp"

#include <iostream>
#include <string>

using duckdb::Identifier;
using duckdb::InvalidInputException;
using duckdb::LogicalType;
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
					  "table-valued parameter needs RPC");
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

void TestBuildSqlParamsRefusals() {
	CHECK_THROWS_WITH(BuildSqlParams(Value::INTEGER(5), ""), "parameters must be a STRUCT of name -> value");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("my p"), Value::INTEGER(1)}}), ""),
					  "parameter name 'my p' is not a T-SQL identifier");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("1p"), Value::INTEGER(1)}}), ""),
					  "parameter name '1p' is not a T-SQL identifier");
	CHECK_THROWS_WITH(BuildSqlParams(Struct({{Identifier("p"), Value()}}), ""), "parameter 'p' is NULL of no type");
}

}  // namespace

int main() {
	TestNVarcharLiteral();
	TestBuildExecuteSqlBatch();
	TestDeclarationForValue();
	TestBuildSqlParamsDerived();
	TestBuildSqlParamsOverride();
	TestBuildSqlParamsRefusals();
	if (failures) {
		std::cerr << failures << " failure(s)\n";
		return 1;
	}
	std::cout << "test_sql_params: all checks passed\n";
	return 0;
}

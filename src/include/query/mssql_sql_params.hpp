//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// query/mssql_sql_params.hpp
//
// Parameters for T-SQL sent as SQL_BATCH text (spec 075). `sp_executesql`
// gives one plan per statement text, cached by the SERVER and reused by every
// session -- unlike an ad-hoc text, which compiles per distinct text and, under
// `optimize for ad hoc workloads`, twice. Used by the catalog's metadata queries
// (W4: a table name never reaches the server inside the query text) and by
// mssql_exec_params / mssql_scan_params (W5).
//
// A value reaches the server as a T-SQL literal, but `EXEC proc @p = <expr>`
// accepts constants and variables only, and the codec's literal for a
// timestamp is a CAST. So the W5 form declares variables first:
//
//   DECLARE @a int = 1, @ts datetime2(6) = CAST('...' AS DATETIME2(7));
//   EXEC sp_executesql N'<statement>', N'@a int, @ts datetime2(6)', @a = @a, @ts = @ts
//
// The inner statement's text and declarations are what the plan is keyed on;
// the DECLARE line differs per call and is a trivial batch.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "tds/tds_request.hpp"

namespace duckdb {
namespace mssql {

//! `N'...'` with every quote doubled -- the only way a value enters a batch.
std::string NVarcharLiteral(const std::string &text);

//! One `@name = <literal>` assignment for the W4 form; `literal` is already
//! T-SQL text (NVarcharLiteral for a name).
struct SqlParamAssignment {
	std::string name;	  // without the leading '@'
	std::string literal;  // T-SQL constant text
};

//! `EXEC sp_executesql N'<statement>', N'<declarations>', @a = ..., @b = ...`
//! The assignments must be constants (the W4 names are). `declarations` may
//! be empty: then the batch is `EXEC sp_executesql N'<statement>'`.
std::string BuildExecuteSqlBatch(const std::string &statement, const std::string &declarations,
								 const std::vector<SqlParamAssignment> &assignments);

//! Spec 083: the same call as an RPC request (sp_executesql, ProcID 10), for
//! the W4 queries whose parameters are all strings (names -- sysname,
//! nvarchar(776)): each goes as an nvarchar parameter. Its description is
//! BuildExecuteSqlBatch's text.
tds::Request BuildExecuteSqlRequest(const std::string &statement, const std::string &declarations,
									const std::vector<std::pair<std::string, std::string>> &string_values);

//! One caller parameter (W5): its name, its T-SQL declaration type and the
//! literal (or expression) that initialises it.
struct SqlParam {
	std::string name;		  // without the leading '@'
	std::string declaration;  // e.g. "int", "nvarchar(4000)", "datetime2(6)"
	std::string literal;	  // T-SQL constant or expression, "NULL" for a typed NULL
	//! Spec 083: the value itself, which an RPC request sends as a typed
	//! parameter. `has_value` false for a parameter known by its literal only.
	Value value;
	bool has_value = false;
};

struct SqlParamSet {
	std::vector<SqlParam> params;

	//! sp_executesql takes at most 2100 parameters; a filter set past this
	//! keeps its remaining constants as literals rather than fail.
	static constexpr size_t MAX_PARAMS = 2000;

	//! Register a parameter and return its name (without the '@'): "p<N>",
	//! numbered by position so a set copied from the bind data and extended
	//! at init stays collision-free.
	std::string Add(const std::string &declaration, const std::string &literal);
	//! Spec 083: the same with the value, so the parameter can travel over RPC.
	std::string Add(const std::string &declaration, const std::string &literal, const Value &value);

	//! "@a int, @b nvarchar(4000)" -- sp_executesql's / sp_prepare's second
	//! argument; empty when there are no parameters.
	std::string Declarations() const;
	//! "DECLARE @a int = 1, @b nvarchar(4000) = N'x';" -- empty when there are
	//! no parameters.
	std::string DeclareBlock() const;
	//! DECLARE ...; EXEC sp_executesql N'<statement>'[, N'<decl>', @a = @a, ...]
	std::string ExecuteSqlBatch(const std::string &statement) const;
	//! Spec 083: the same call as an RPC request (sp_executesql, ProcID 10),
	//! every parameter encoded from its declared type; its description is
	//! ExecuteSqlBatch's text. The batch form when a parameter has no value (a
	//! literal-only parameter).
	tds::Request ExecuteSqlRequest(const std::string &statement) const;
	//! DECLARE ...; EXEC sp_execute <handle>[, @a, @b]
	std::string ExecuteByHandleBatch(int32_t handle) const;
	//! Spec 083: the same call as an RPC request (sp_execute, ProcID 12): the
	//! handle and the values as positional typed parameters, so no outer batch
	//! is compiled per execution. The batch form when a value has no encoding.
	tds::Request ExecuteByHandleRequest(int32_t handle) const;
};

//! The SQL Server declaration for a DuckDB value of `type` (spec 075 W5's
//! table). Throws InvalidInputException, naming the parameter and the fix, for
//! a bare NULL (no type), a nested type (table-valued parameters need RPC) or
//! a type with no SQL Server counterpart.
std::string DeclarationForValue(const std::string &name, const LogicalType &type, const Value &value);

//! Build the parameter set from a STRUCT value: keys are the names, children
//! the values. `declarations_override`, when non-empty, is the caller's own
//! declaration list (`"@ts datetime, @c varchar(8)"`); every key must appear in
//! it and every declared name must have a key. Throws InvalidInputException
//! naming what is wrong.
SqlParamSet BuildSqlParams(const Value &params, const std::string &declarations_override);

}  // namespace mssql
}  // namespace duckdb

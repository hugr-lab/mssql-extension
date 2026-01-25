//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mssql_delete_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "delete/mssql_delete_target.hpp"
#include "dml/mssql_dml_batch.hpp"

namespace duckdb {

//! MSSQLDeleteStatement generates DELETE SQL using the VALUES join pattern
//! For efficient batched deletion that works with SQL Server's parameter limits
class MSSQLDeleteStatement {
public:
	//! Constructor
	//! @param target The target table metadata
	explicit MSSQLDeleteStatement(const MSSQLDeleteTarget &target);

	//! Build a DELETE statement for a batch of rows
	//! Uses the VALUES join pattern for efficient batched deletion:
	//!   DELETE t FROM [schema].[table] AS t
	//!   JOIN (VALUES (@p1), (@p2), ...) AS v([pk1])
	//!   ON t.[pk1] = v.[pk1]
	//!
	//! @param pk_values Vector of PK value vectors (one inner vector per row)
	//! @return MSSQLDMLBatch containing the SQL and parameters
	MSSQLDMLBatch Build(const vector<vector<Value>> &pk_values) const;

	//! Build a DELETE statement for a single row
	//! @param pk_value PK values for the row (one value for scalar PK, multiple for composite)
	//! @return MSSQLDMLBatch containing the SQL and parameters
	MSSQLDMLBatch BuildSingle(const vector<Value> &pk_value) const;

	//! Get the number of parameters per row (equals PK column count)
	idx_t GetParametersPerRow() const {
		return target_.GetParamsPerRow();
	}

private:
	//! Reference to the target table metadata
	const MSSQLDeleteTarget &target_;

	//! Generate the base DELETE ... JOIN clause
	string GenerateDeleteClause() const;

	//! Generate the ON clause for PK matching
	string GenerateOnClause() const;

	//! Escape a SQL Server identifier with square brackets
	static string EscapeIdentifier(const string &identifier);
};

}  // namespace duckdb

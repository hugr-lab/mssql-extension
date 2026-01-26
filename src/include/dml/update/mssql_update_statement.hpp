#pragma once

#include <string>
#include <vector>
#include "dml/mssql_dml_batch.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLUpdateStatement - SQL generator for UPDATE operations
//
// Generates parameterized UPDATE statements using VALUES join pattern:
//
// UPDATE t
// SET t.[col1] = v.[col1], t.[col2] = v.[col2]
// FROM [schema].[table] AS t
// JOIN (VALUES
//   (@p1, @p2, @p3),
//   (@p4, @p5, @p6)
// ) AS v([pk1], [col1], [col2])
// ON t.[pk1] = v.[pk1]
//===----------------------------------------------------------------------===//

class MSSQLUpdateStatement {
public:
	//===----------------------------------------------------------------------===//
	// Construction
	//===----------------------------------------------------------------------===//

	explicit MSSQLUpdateStatement(const MSSQLUpdateTarget &target);

	//===----------------------------------------------------------------------===//
	// SQL Generation
	//===----------------------------------------------------------------------===//

	// Build UPDATE SQL with parameters for a batch of rows
	// @param pk_values Vector of PK values per row [row][pk_col]
	// @param update_values Vector of update values per row [row][update_col]
	// @param batch_number Sequential batch number for error reporting
	// @return Complete batch ready for execution
	MSSQLDMLBatch Build(const vector<vector<Value>> &pk_values, const vector<vector<Value>> &update_values,
						idx_t batch_number);

private:
	const MSSQLUpdateTarget &target_;

	// Generate SET clause: SET t.[col1] = v.[col1], t.[col2] = v.[col2]
	string GenerateSetClause() const;

	// Generate VALUES column list: [pk1], [pk2], [col1], [col2]
	string GenerateValuesColumnList() const;

	// Generate ON clause: ON t.[pk1] = v.[pk1] AND t.[pk2] = v.[pk2]
	string GenerateOnClause() const;

	// Escape identifier for T-SQL: name → [name]
	static string EscapeIdentifier(const string &name);
};

}  // namespace duckdb

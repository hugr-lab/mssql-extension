#pragma once

#include <string>
#include <vector>
#include "dml/insert/mssql_insert_target.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertStatement - Generates SQL INSERT statements
//
// This class generates T-SQL INSERT statements with multi-row VALUES clauses.
// It handles identifier escaping, column lists, and optional OUTPUT clauses.
//
// SQL Template (without OUTPUT):
//   INSERT INTO [schema].[table] ([col1], [col2])
//   VALUES
//     (lit1, lit2),
//     (lit3, lit4);
//
// SQL Template (with OUTPUT):
//   INSERT INTO [schema].[table] ([col1], [col2])
//   OUTPUT INSERTED.[col1], INSERTED.[col2], INSERTED.[id]
//   VALUES
//     (lit1, lit2),
//     (lit3, lit4);
//===----------------------------------------------------------------------===//

class MSSQLInsertStatement {
public:
	// Constructor
	// @param target Insert target metadata
	// @param include_output Whether to include OUTPUT clause for RETURNING
	MSSQLInsertStatement(const MSSQLInsertTarget &target, bool include_output);

	//===----------------------------------------------------------------------===//
	// SQL Generation
	//===----------------------------------------------------------------------===//

	// Build complete INSERT statement from row literals
	// @param row_literals 2D vector: rows x columns of serialized literals
	// @return Complete T-SQL INSERT statement
	string Build(const vector<vector<string>> &row_literals) const;

	// Get escaped column list for INSERT INTO clause
	// Returns: "[col1], [col2], [col3]"
	string GetColumnList() const;

	// Get OUTPUT clause for RETURNING support
	// Returns: "OUTPUT INSERTED.[col1], INSERTED.[col2]" or empty if not enabled
	string GetOutputClause() const;

	// Get fully qualified table name
	// Returns: "[schema].[table]"
	string GetTableName() const;

	//===----------------------------------------------------------------------===//
	// Configuration
	//===----------------------------------------------------------------------===//

	// Check if OUTPUT clause is enabled
	bool HasOutput() const {
		return include_output_;
	}

private:
	const MSSQLInsertTarget &target_;
	bool include_output_;

	// Cached strings for efficiency
	mutable string cached_column_list_;
	mutable string cached_output_clause_;
	mutable string cached_table_name_;
	mutable bool cache_initialized_ = false;

	// Initialize cached strings
	void InitializeCache() const;
};

}  // namespace duckdb

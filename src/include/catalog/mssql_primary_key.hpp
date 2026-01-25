#pragma once

#include <string>
#include <vector>
#include "duckdb/common/types.hpp"
#include "tds/tds_connection_pool.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// PKColumnInfo - Single PK column metadata
//===----------------------------------------------------------------------===//

struct PKColumnInfo {
	string name;              // Column name
	int32_t column_id;        // SQL Server column_id (1-based)
	int32_t key_ordinal;      // Position in PK (1-based, from sys.index_columns)
	LogicalType duckdb_type;  // Mapped DuckDB type
	string collation_name;    // For string columns (may affect DML predicates)

	// Default constructor
	PKColumnInfo() : column_id(0), key_ordinal(0), duckdb_type(LogicalType::INTEGER) {}

	// Construct from discovery query result
	static PKColumnInfo FromMetadata(
		const string& name,
		int32_t column_id,
		int32_t key_ordinal,
		const string& type_name,
		int16_t max_length,
		uint8_t precision,
		uint8_t scale,
		const string& collation_name,
		const string& database_collation
	);
};

//===----------------------------------------------------------------------===//
// PrimaryKeyInfo - Complete PK metadata for a table
//===----------------------------------------------------------------------===//

struct PrimaryKeyInfo {
	// Loading state
	bool loaded = false;      // Has PK discovery been attempted?

	// PK existence
	bool exists = false;      // Does table have a PK?

	// PK structure (only valid if exists == true)
	vector<PKColumnInfo> columns;  // Ordered by key_ordinal

	// Computed rowid type
	LogicalType rowid_type;   // Scalar (single col) or STRUCT (composite)

	// Default constructor
	PrimaryKeyInfo() : loaded(false), exists(false), rowid_type(LogicalType::SQLNULL) {}

	// Predicates
	bool IsScalar() const { return exists && columns.size() == 1; }
	bool IsComposite() const { return exists && columns.size() > 1; }

	// Get column names for SELECT clause
	vector<string> GetColumnNames() const;

	// Build rowid type from columns
	void ComputeRowIdType();

	// Factory method - discovers PK from SQL Server
	static PrimaryKeyInfo Discover(
		tds::TdsConnection& connection,
		const string& schema_name,
		const string& table_name,
		const string& database_collation
	);
};

}  // namespace mssql
}  // namespace duckdb

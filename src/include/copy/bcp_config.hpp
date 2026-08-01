#pragma once

#include "catalog/mssql_table_options.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

class ClientContext;

//===----------------------------------------------------------------------===//
// Default Values for COPY/BCP Settings
//===----------------------------------------------------------------------===//

// Default flush threshold (rows before sending to SQL Server and committing)
// This controls memory usage on both DuckDB and SQL Server sides
// 0 = no intermediate flushes (send all at end) - WARNING: high memory usage
// Default 100K rows keeps buffer around 10-50MB depending on row size
constexpr idx_t MSSQL_DEFAULT_COPY_FLUSH_ROWS = 100000;	 // 100K rows

namespace mssql {

//===----------------------------------------------------------------------===//
// BCPCopyConfig - Configuration for COPY TO operations via BulkLoadBCP
//
// Loaded from DuckDB settings at runtime via LoadBCPCopyConfig().
// Controls batching behavior and table creation options.
//===----------------------------------------------------------------------===//

struct BCPCopyConfig {
	// Create table if it doesn't exist
	bool create_table = true;

	// Drop and recreate table if it exists
	bool overwrite = false;

	// Empty an EXISTING table before loading, keeping its definition — indexes,
	// permissions, the columnstore index someone built on it. Separately named
	// from `replace` on purpose: both destroy rows the statement does not name,
	// but replace also discards the table's shape, and the two are not
	// interchangeable for a target that was set up deliberately (spec 060 D7).
	bool truncate = false;

	// Rows before flushing to SQL Server (0 = flush only at end)
	// This controls memory usage on SQL Server - data is buffered until flush
	// Recommended: 100K-1M rows depending on row size
	idx_t flush_rows = MSSQL_DEFAULT_COPY_FLUSH_ROWS;

	// Use TABLOCK hint for INSERT BULK (enables table-level locking)
	// Improves performance by 15-30% for bulk loads by:
	// - Reducing lock overhead (table lock vs row locks)
	// - Enabling minimal logging in simple/bulk-logged recovery
	// - Allowing more parallel server-side processing
	// WARNING: Blocks other readers/writers during COPY
	// Default changed to false in Spec 027 for safer multi-user behavior
	bool tablock = false;

	// True if user explicitly set tablock option (vs using default from settings)
	// Used to determine if auto-TABLOCK should be applied for new tables
	bool tablock_explicit = false;

	// True if creating a brand-new table (table didn't exist or overwrite dropped it)
	// Used for auto-TABLOCK: new tables have no concurrent readers, so TABLOCK is safe
	bool is_new_table = false;

	// From mssql_utf8_collation — the collation to give a varchar column this COPY
	// creates, when the column's own MSSQL_VARCHAR(n) annotation names none.
	// Without it the column takes the database code page and SQL Server replaces
	// everything outside it with '?' on insert, silently (issue #225). Empty is the
	// documented way to inherit the database default, which is correct on Fabric.
	string utf8_collation;

	// The collation actually applied, resolved once per statement in
	// ValidateTarget: empty unless some column asked for MSSQL_VARCHAR(n) without
	// naming one, and the database default is not already UTF-8.
	string varchar_collation;

	// From mssql_ctas_text_type — what an unannotated DuckDB VARCHAR becomes.
	// The SAME setting drives CTAS, so the two table-creating paths cannot
	// disagree about it (spec 060 D7).
	bool text_type_varchar = false;

	// From mssql_default_string_length, or the per-statement string_length option.
	// 0 means MAX, which is what a plain VARCHAR has always meant.
	int32_t default_string_length = 0;

	// Shape of a table this COPY creates — mssql_default_table_kind, or the
	// per-statement table_kind option (spec 060 D8). Ignored when the target
	// already exists: COPY does not restructure someone else's table.
	MSSQLTableOptions table_options;

	// Check if data should be flushed to SQL Server
	// Returns true when accumulated rows reach flush_rows threshold
	bool ShouldFlushToServer(idx_t accumulated_rows) const {
		return flush_rows > 0 && accumulated_rows >= flush_rows;
	}
};

//===----------------------------------------------------------------------===//
// Configuration Loading
//===----------------------------------------------------------------------===//

// Load BCP COPY configuration from DuckDB settings
// @param context The client context to read settings from
// @return Validated BCPCopyConfig
BCPCopyConfig LoadBCPCopyConfig(ClientContext &context);

}  // namespace mssql
}  // namespace duckdb

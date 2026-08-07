#pragma once

#include "catalog/mssql_index_kind.hpp"
#include "catalog/mssql_table_options.hpp"
#include "copy/load_policy.hpp"
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
//! Rows per bulk-load batch — the boundary the SERVER sees between DONE tokens,
//! not a client buffer size.
//!
//! 102 400 is SQL Server's own threshold: a batch at least this large is written
//! straight into a COMPRESSED columnstore rowgroup, and anything smaller goes to
//! the delta store and stays there until something rebuilds it. A delta rowgroup
//! only closes on its own at 1 048 576 rows, so a smaller load never compresses
//! at all.
//!
//! Measured (spec 057, 1M rows x 4 columns into a clustered columnstore): at the
//! previous 100 000 default `sys.column_store_row_groups` reported a single OPEN
//! group holding all 1M rows and 53 MB on disk; at 102 400, nine COMPRESSED
//! groups and 7 MB. A 2400-row difference, 7.6x on disk — so
//! `table_kind = 'columnstore'` silently did not compress, which is the entire
//! reason a user asks for it.
//!
//! One constant rather than a columnstore special case: batch size measured
//! nearly flat on a heap (25k 0.93 s / 100k 0.78 s / 500k 0.75 s), so the extra
//! 2400 rows cost nothing there, and a target-dependent default would be a rule
//! to explain for no gain. Set the option lower and the consequence is yours.
constexpr idx_t MSSQL_DEFAULT_COPY_FLUSH_ROWS = 102400;

//! Parallel bulk-load writers per COPY. 0 = derive from DuckDB's thread count.
//!
//! The bound this attacks is SQL Server's ingest rate, not the client and not
//! the wire: `send()` blocks because the receive window stays full while the
//! server lays rows down. Nothing done on one connection moves that — the server
//! parallelises across SESSIONS, so more of them is the only lever.
//!
//! Capped independently of DuckDB's threads because each writer is a pooled
//! connection holding an open bulk-load transaction; a 64-thread machine asking
//! for 64 concurrent BU locks is a different decision from asking for 64
//! threads, and `mssql_connection_limit` is a per-context ceiling shared with
//! everything else.
//!
//! The cap itself — MSSQL_MAX_COPY_PARALLEL_WRITERS — lives in
//! copy/load_policy.hpp with the function that applies it, so the policy header
//! stays self-contained and unit-testable. Included above; still visible here.
constexpr idx_t MSSQL_DEFAULT_COPY_PARALLEL_WRITERS = 0;

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
	// Resolved at load time by MSSQLResolveTablock(tablock_choice, target_shape);
	// read by the INSERT BULK builder. Not a user input on its own.
	bool tablock = false;

	// What the user asked for: mssql_copy_tablock, or the per-statement `tablock`
	// COPY option. AUTO means "decide from the target's shape" and is the default.
	//
	// This replaces a `tablock_explicit` bool that was ALWAYS true — it was set
	// from TryGetCurrentSetting() succeeding, which it does unconditionally for an
	// option carrying a registered default — so both auto-TABLOCK branches were
	// dead from spec 030 (issue #45) until spec 057 step 1.
	MSSQLTablockChoice tablock_choice = MSSQLTablockChoice::AUTO;

	// The target's physical structure, which is what decides TABLOCK under AUTO.
	// Taken from sys.indexes for an existing table (same round trip as the object
	// check) and from what we are about to create otherwise.
	MSSQLIndexKind target_shape = MSSQLIndexKind::HEAP;

	// True if creating a brand-new table (table didn't exist or overwrite dropped it)
	bool is_new_table = false;

	// Lowercased names of matched columns whose (source, target) pair
	// IsTypeCompatible refused. NOT an error at bind: a constant `NULL AS col`
	// in the source SELECT — the ordinary way to fill an unmatched column —
	// reaches bind as INTEGER (DuckDB types a bare NULL before the extension
	// sees it), indistinguishable from real integer data. So the pair is
	// admitted for the all-NULL case only, and the encoder checks the mask per
	// chunk: entirely NULL → the column takes the missing-column NullOnly path;
	// any value → the same type-mismatch error, at first chunk (spec 064).
	vector<string> null_only_columns;

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

	// Collation to NAME in the INSERT BULK column list for a single-byte column,
	// or empty to keep it on the UTF-16 wire. See CTASConfig for why this is not
	// the same question as varchar_collation.
	string wire_varchar_collation;

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

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace mssql {

//===----------------------------------------------------------------------===//
// CTAS Configuration
//===----------------------------------------------------------------------===//

// Default values for CTAS settings
constexpr bool MSSQL_DEFAULT_CTAS_DROP_ON_FAILURE = false;
constexpr const char *MSSQL_DEFAULT_CTAS_TEXT_TYPE = "NVARCHAR";

// Collation given to a VARCHAR target when the server granted UTF8SUPPORT.
// CI_AS rather than BIN2: this collation is stored in the schema and governs
// every comparison against the column afterwards, so it has to match what a
// user gets from the usual database defaults, not silently make the column
// case- and accent-sensitive.
constexpr const char *MSSQL_DEFAULT_UTF8_COLLATION = "Latin1_General_100_CI_AS_SC_UTF8";

// Text type policy for CTAS DDL generation
enum class CTASTextType { NVARCHAR, VARCHAR };

// Configuration for CTAS execution
struct CTASConfig {
	// From mssql_ctas_text_type setting
	CTASTextType text_type = CTASTextType::NVARCHAR;

	// Collation to attach to a VARCHAR target column, or empty to inherit the
	// database default (issue #225). Only ever set when the server granted
	// UTF8SUPPORT: without it, a VARCHAR column takes the database's code page
	// and everything outside that page is replaced by '?' ON INSERT — silently.
	// Empty is also the right answer when the database default is already a
	// UTF-8 collation, as it is on Fabric: inheriting is cleaner than imposing
	// a Latin1 one.
	string varchar_collation;

	// From mssql_ctas_drop_on_failure setting
	bool drop_on_failure = MSSQL_DEFAULT_CTAS_DROP_ON_FAILURE;

	// Inherited from INSERT settings for batch insert phase (when use_bcp = false)
	idx_t batch_size = 1000;
	idx_t max_rows_per_statement = 1000;
	idx_t max_sql_bytes = 8 * 1024 * 1024;

	//===----------------------------------------------------------------------===//
	// BCP Mode Settings (Spec 027)
	//===----------------------------------------------------------------------===//

	// From mssql_ctas_use_bcp setting - use BCP protocol for data transfer
	// BCP is 2-10x faster than batched INSERT statements
	bool use_bcp = true;

	// From mssql_copy_flush_rows setting - rows before flushing to SQL Server
	// Applies to BCP mode only
	idx_t bcp_flush_rows = 100000;

	// From mssql_copy_tablock setting - use TABLOCK hint for BCP operations
	// Provides 15-30% performance improvement but blocks concurrent reads
	bool bcp_tablock = false;

	// True if user explicitly set mssql_copy_tablock (vs using default)
	// Used to determine if auto-TABLOCK should be applied for new tables
	bool bcp_tablock_explicit = false;

	// True if creating a brand-new table (table didn't exist or OR REPLACE dropped it)
	// Used for auto-TABLOCK: new tables have no concurrent readers, so TABLOCK is safe
	bool is_new_table = false;

	// Load configuration from client context
	static CTASConfig Load(ClientContext &context);

	// Parse text type string to enum
	static CTASTextType ParseTextType(const string &text_type_str);
};

//===----------------------------------------------------------------------===//
// Loading CTAS Configuration
//===----------------------------------------------------------------------===//

CTASConfig LoadCTASConfig(ClientContext &context);

}  // namespace mssql
}  // namespace duckdb

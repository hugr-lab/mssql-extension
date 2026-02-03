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

// Text type policy for CTAS DDL generation
enum class CTASTextType { NVARCHAR, VARCHAR };

// Configuration for CTAS execution
struct CTASConfig {
	// From mssql_ctas_text_type setting
	CTASTextType text_type = CTASTextType::NVARCHAR;

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

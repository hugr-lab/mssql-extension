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

	// Inherited from INSERT settings for batch insert phase
	idx_t batch_size = 1000;
	idx_t max_rows_per_statement = 1000;
	idx_t max_sql_bytes = 8 * 1024 * 1024;

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

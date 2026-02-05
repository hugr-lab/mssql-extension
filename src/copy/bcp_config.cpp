#include "copy/bcp_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace mssql {

BCPCopyConfig LoadBCPCopyConfig(ClientContext &context) {
	BCPCopyConfig config;
	Value val;

	if (context.TryGetCurrentSetting("mssql_copy_flush_rows", val)) {
		config.flush_rows = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_copy_tablock", val)) {
		config.tablock = val.GetValue<bool>();
		// Mark as explicitly set so auto-TABLOCK logic knows user preference
		config.tablock_explicit = true;
	}

	return config;
}

}  // namespace mssql
}  // namespace duckdb

#include "copy/bcp_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace mssql {

void BCPCopyConfig::Validate() const {
	if (batch_rows == 0) {
		throw InvalidInputException("MSSQL: mssql_copy_batch_rows must be greater than 0");
	}
	if (max_batch_bytes < MSSQL_MIN_COPY_BATCH_BYTES) {
		throw InvalidInputException("MSSQL: mssql_copy_max_batch_bytes must be at least 1MB");
	}
}

BCPCopyConfig LoadBCPCopyConfig(ClientContext &context) {
	BCPCopyConfig config;
	Value val;

	if (context.TryGetCurrentSetting("mssql_copy_batch_rows", val)) {
		config.batch_rows = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_copy_max_batch_bytes", val)) {
		config.max_batch_bytes = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_copy_flush_rows", val)) {
		config.flush_rows = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_copy_tablock", val)) {
		config.tablock = val.GetValue<bool>();
	}

	config.Validate();
	return config;
}

}  // namespace mssql
}  // namespace duckdb

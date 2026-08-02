#include "copy/bcp_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>

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

	if (context.TryGetCurrentSetting("mssql_utf8_collation", val)) {
		config.utf8_collation = val.IsNull() ? string() : val.ToString();
	}

	// Spec 060 D7: one setting decides what an unannotated VARCHAR becomes, on
	// both table-creating paths. The name is CTAS's for historical reasons.
	if (context.TryGetCurrentSetting("mssql_ctas_text_type", val)) {
		config.text_type_varchar = !val.IsNull() && StringUtil::Upper(val.ToString()) == "VARCHAR";
	}

	if (context.TryGetCurrentSetting("mssql_default_string_length", val)) {
		const int64_t raw = val.IsNull() ? 0 : val.GetValue<int64_t>();
		config.default_string_length = raw <= 0 ? 0 : static_cast<int32_t>(std::min<int64_t>(raw, INT32_MAX));
	}

	config.table_options = MSSQLTableOptions::FromSettings(context);

	return config;
}

}  // namespace mssql
}  // namespace duckdb

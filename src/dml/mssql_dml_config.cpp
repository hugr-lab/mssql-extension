#include "dml/mssql_dml_config.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLDMLConfig::Validate
//===----------------------------------------------------------------------===//

void MSSQLDMLConfig::Validate() const {
	if (batch_size == 0) {
		throw InvalidInputException("mssql_dml_batch_size must be >= 1");
	}
	if (max_parameters == 0) {
		throw InvalidInputException("mssql_dml_max_parameters must be >= 1");
	}
	// Warn if batch_size might exceed parameter limit with reasonable column count
	// A typical UPDATE with 5 columns + 1 PK = 6 params per row
	// At batch_size 500 that's 3000 params which exceeds 2000
	// But we compute effective batch size at runtime, so this is just validation
}

//===----------------------------------------------------------------------===//
// LoadDMLConfig - Load configuration from DuckDB settings
//===----------------------------------------------------------------------===//

MSSQLDMLConfig LoadDMLConfig(ClientContext &context) {
	MSSQLDMLConfig config;
	Value val;

	if (context.TryGetCurrentSetting("mssql_dml_batch_size", val)) {
		config.batch_size = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_dml_max_parameters", val)) {
		config.max_parameters = static_cast<idx_t>(val.GetValue<int64_t>());
	}

	if (context.TryGetCurrentSetting("mssql_dml_use_prepared", val)) {
		config.use_prepared = val.GetValue<bool>();
	}

	// Validate loaded config
	config.Validate();

	return config;
}

}  // namespace duckdb

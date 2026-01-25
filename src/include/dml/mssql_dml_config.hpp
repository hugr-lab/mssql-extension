#pragma once

#include <algorithm>
#include <cstddef>
#include "duckdb/common/types.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Default Values for DML (UPDATE/DELETE) Settings
//===----------------------------------------------------------------------===//

// Default batch size (rows per DML statement)
// Conservative default to stay well under SQL Server's ~2100 parameter limit
constexpr idx_t MSSQL_DEFAULT_DML_BATCH_SIZE = 500;

// Default maximum parameters per statement
// SQL Server limit is approximately 2100, we use 2000 for safety margin
constexpr idx_t MSSQL_DEFAULT_DML_MAX_PARAMETERS = 2000;

// Default: use prepared statements for DML operations
constexpr bool MSSQL_DEFAULT_DML_USE_PREPARED = true;

//===----------------------------------------------------------------------===//
// MSSQLDMLConfig - Configuration for UPDATE/DELETE operations
//
// Loaded from DuckDB settings at runtime via LoadDMLConfig().
// Controls batching behavior and execution mode.
//===----------------------------------------------------------------------===//

struct MSSQLDMLConfig {
	// Maximum rows per batch (user-specified batch size)
	idx_t batch_size = MSSQL_DEFAULT_DML_BATCH_SIZE;

	// Maximum parameters per SQL statement (SQL Server limit ~2100)
	idx_t max_parameters = MSSQL_DEFAULT_DML_MAX_PARAMETERS;

	// Use prepared statements for execution
	bool use_prepared = MSSQL_DEFAULT_DML_USE_PREPARED;

	//===----------------------------------------------------------------------===//
	// Effective Batch Size Calculation
	//===----------------------------------------------------------------------===//

	// Calculate effective batch size based on parameters per row
	// Ensures we don't exceed SQL Server's parameter limit
	//
	// @param params_per_row Number of parameters per row (pk_cols + update_cols for UPDATE, pk_cols for DELETE)
	// @return Minimum of configured batch_size and (max_parameters / params_per_row)
	idx_t EffectiveBatchSize(idx_t params_per_row) const {
		if (params_per_row == 0) {
			return batch_size;
		}
		idx_t param_limited_batch = max_parameters / params_per_row;
		return std::min(batch_size, param_limited_batch);
	}

	//===----------------------------------------------------------------------===//
	// Validation
	//===----------------------------------------------------------------------===//

	// Validate configuration values
	// @throws InvalidInputException if values are out of range
	void Validate() const;
};

//===----------------------------------------------------------------------===//
// Configuration Loading
//===----------------------------------------------------------------------===//

// Forward declaration of ClientContext
class ClientContext;

// Load DML configuration from DuckDB settings
// @param context The client context to read settings from
// @return Validated MSSQLDMLConfig
MSSQLDMLConfig LoadDMLConfig(ClientContext &context);

}  // namespace duckdb

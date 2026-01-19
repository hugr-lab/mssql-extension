#include "insert/mssql_insert_config.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLInsertConfig::Validate()
//===----------------------------------------------------------------------===//

void MSSQLInsertConfig::Validate() const {
	if (batch_size < 1) {
		throw InvalidInputException("mssql_insert_batch_size must be >= 1, got: %llu",
									static_cast<unsigned long long>(batch_size));
	}

	if (max_rows_per_statement < 1) {
		throw InvalidInputException("mssql_insert_max_rows_per_statement must be >= 1, got: %llu",
									static_cast<unsigned long long>(max_rows_per_statement));
	}

	if (max_sql_bytes < MSSQL_MIN_INSERT_SQL_BYTES) {
		throw InvalidInputException("mssql_insert_max_sql_bytes must be >= %llu, got: %llu",
									static_cast<unsigned long long>(MSSQL_MIN_INSERT_SQL_BYTES),
									static_cast<unsigned long long>(max_sql_bytes));
	}
}

}  // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// dml/mssql_dml_outcome.hpp
//
// The sentence that says what happened to the rows a failed statement had
// already sent (spec 062 W1c / W5, issue #344). One function, so the INSERT,
// UPDATE and DELETE executors and the bulk path say it the same way.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! "rolled back" in autocommit — the statement's own server transaction was
//! rolled back, earlier batches included — or, inside a DuckDB transaction,
//! that they sit in the open transaction until its ROLLBACK.
inline string MSSQLRowsBeforeOutcome(bool transaction_pinned, idx_t rows_before) {
	if (transaction_pinned) {
		return StringUtil::Format(
			"the %llu row(s) from the batches before it are in the open transaction; "
			"ROLLBACK discards them",
			(unsigned long long)rows_before);
	}
	return StringUtil::Format("rolled back, the %llu row(s) from the batches before it included",
							  (unsigned long long)rows_before);
}

}  // namespace duckdb

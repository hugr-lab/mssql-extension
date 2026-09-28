//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// pushdown/mssql_order_term.hpp
//
// One ORDER BY key as SQL Server must see it to return DuckDB's order -- the
// one rule both `MSSQLOptimizer` (bound plans) and the spec 079 writer (parsed
// trees) apply (spec 079 D5: one NULL-order emulation, one order predicate).
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "duckdb/common/enums/order_type.hpp"

namespace duckdb {

struct MSSQLColumnInfo;

namespace mssql {

//! The key expression to sort `column` by, given `fragment` (the column or a
//! mapped function of it). The column's own order when the server's is
//! DuckDB's (`OrdersLikeDuckDB`, #362); its bytes -- `CAST(… AS varbinary(n))`
//! -- for a bare UTF-8 varchar under a LIMIT (`limited`), where the key not
//! being sargable costs N rows rather than the table. False otherwise.
bool OrderKeyFragment(const MSSQLColumnInfo &column, const std::string &fragment, bool is_column, bool limited,
					  std::string &out_fragment);

//! The ORDER BY term for one key, with DuckDB's NULL placement.
//!
//! SQL Server has no NULLS FIRST / LAST: NULL sorts lowest, so ASC puts NULLs
//! first and DESC last, while DuckDB's default is NULLS LAST both ways. A
//! nullable key whose requested placement differs from the server's stops the
//! pushdown -- unless `emulate`, when a leading key sorts NULL where DuckDB
//! wants it: `CASE WHEN [col] IS NULL THEN 1 ELSE 0 END, <key> ASC` for NULLS
//! LAST ascending (spec 079 D2). Emulation is only for TOP N / LIMIT (review
//! of #387): the CASE key is not sargable, so the server sorts every row --
//! worth it when only N rows cross the wire, a plain loss for a full ORDER BY,
//! which DuckDB sorts as the rows stream in. The NULL test is on the COLUMN,
//! not the fragment: a mapped function is null exactly when its argument is,
//! and the function is then not evaluated twice per row. `column_ref` is that
//! column as the statement must spell it (quoted, qualified where an alias
//! could shadow it).
//!
//! False (term untouched) when the key cannot be pushed as asked: a placement
//! that needs emulation it may not use, or a null order that is neither FIRST
//! nor LAST -- the caller resolves the default first, and anything else
//! refuses rather than guess (it used to be read as LAST).
bool OrderTerm(const std::string &fragment, const std::string &column_ref, OrderType order_type,
			   OrderByNullType null_order, bool is_nullable, bool emulate, std::string &out_term);

}  // namespace mssql
}  // namespace duckdb

#include "pushdown/mssql_order_term.hpp"

#include "catalog/mssql_column_info.hpp"
#include "catalog/mssql_rowid_key_choice.hpp"

namespace duckdb {
namespace mssql {

bool OrderKeyReadTruncates(const MSSQLColumnInfo &column) {
	// The datetime2 answer is the catalog's (spec 093 D8, merged as #560 while this was in review):
	// under `datetime2_7_as = 'timestamp'` a datetime2(7) is read truncated too, so it is a
	// last-key-only key here for the same reason time(7) is. `OrdersLikeDuckDB` already refuses
	// datetime2(7) outright, so today this only keeps the two predicates from disagreeing.
	return IsRoundTripExactForKey(column.sql_type_name, column.scale, column.comparison_differs,
								  column.ReadsDatetime2Truncated()) == KeyFidelity::NotRoundTrip;
}

bool OrderKeyFragment(const MSSQLColumnInfo &column, const std::string &fragment, bool is_column, bool limited,
					  bool is_last_key, std::string &out_fragment) {
	// Issue #556: `time(7)` / `datetimeoffset(7)` are read with the 100 ns digit
	// dropped, so two stored values 100 ns apart read as ONE DuckDB value. As the
	// last key that is harmless -- rows DuckDB sees as equal may come back in
	// either order -- but with another key after it the server breaks the tie on
	// the digit DuckDB never saw, and the DuckDB sort this pushdown removed is not
	// there to fix it. A LIMIT makes it a different ROW, not just a different
	// order. `IsGroupable` keeps the same columns out of GROUP BY / DISTINCT /
	// PARTITION BY for this reason.
	if (!is_last_key && OrderKeyReadTruncates(column)) {
		return false;
	}
	if (column.OrdersLikeDuckDB()) {
		out_fragment = fragment;
		return true;
	}
	if (limited && is_column && column.OrdersLikeDuckDBAsBytes()) {
		out_fragment = "CAST(" + fragment + " AS varbinary(" + std::to_string(column.max_length) + "))";
		return true;
	}
	return false;
}

bool OrderTerm(const std::string &fragment, const std::string &column_ref, OrderType order_type,
			   OrderByNullType null_order, bool is_nullable, bool emulate, std::string &out_term) {
	const bool descending = order_type == OrderType::DESCENDING;
	const std::string term = fragment + (descending ? " DESC" : " ASC");
	// A NOT NULL key has no placement to honour, whatever the node says
	// (review of #387: the check below came first and refused ORDER_DEFAULT on
	// such a key).
	if (!is_nullable) {
		out_term = term;
		return true;
	}
	if (null_order != OrderByNullType::NULLS_FIRST && null_order != OrderByNullType::NULLS_LAST) {
		return false;
	}
	const bool nulls_first = null_order == OrderByNullType::NULLS_FIRST;
	const bool server_puts_nulls_first = !descending;
	if (nulls_first == server_puts_nulls_first) {
		out_term = term;
		return true;
	}
	if (!emulate) {
		return false;
	}
	// The leading key sorts ascending: the side that must come first gets 0.
	out_term = "CASE WHEN " + column_ref + " IS NULL THEN " + (nulls_first ? "0 ELSE 1" : "1 ELSE 0") + " END, " + term;
	return true;
}

}  // namespace mssql
}  // namespace duckdb

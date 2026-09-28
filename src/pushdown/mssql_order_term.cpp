#include "pushdown/mssql_order_term.hpp"

#include "catalog/mssql_column_info.hpp"

namespace duckdb {
namespace mssql {

bool OrderKeyFragment(const MSSQLColumnInfo &column, const std::string &fragment, bool is_column, bool limited,
					  std::string &out_fragment) {
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

#pragma once

//===----------------------------------------------------------------------===//
// INTERNAL to src/table_scan: what the catalog scan's files share. The scan is
// table_scan.cpp (bind, init, execute, the rowid, registration),
// table_scan_pushdown.cpp (pushdown_expression, pushdown_complex_filter,
// cardinality) and table_scan_serialize.cpp (the bind data, serialized and
// read back). The public interface is table_scan/table_scan.hpp.
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstdlib>

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class LogicalGet;
class NodeStatistics;

namespace mssql {

//! MSSQL_DEBUG, read once.
inline int ScanDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_SCAN_DEBUG_LOG(level, fmt, ...)                               \
	do {                                                                    \
		if (::duckdb::mssql::ScanDebugLevel() >= level) {                   \
			fprintf(stderr, "[MSSQL TABLE_SCAN] " fmt "\n", ##__VA_ARGS__); \
		}                                                                   \
	} while (0)

//! TableFunction::pushdown_expression, pushdown_complex_filter and cardinality (table_scan_pushdown.cpp).
bool MSSQLPushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr);
void ComplexFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
						   vector<unique_ptr<Expression>> &filters);
unique_ptr<NodeStatistics> MSSQLCatalogScanCardinality(ClientContext &context, const FunctionData *bind_data_p);

//! TableFunction::serialize and deserialize (table_scan_serialize.cpp).
void CatalogScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
						  const BoundTableFunction &function);
unique_ptr<FunctionData> CatalogScanDeserialize(Deserializer &deserializer, BoundTableFunction &function);

}  // namespace mssql
}  // namespace duckdb

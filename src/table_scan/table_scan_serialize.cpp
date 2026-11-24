// The catalog scan's bind data, serialized and read back: what makes two scans' plans differ, and a copied
// plan find its table again.

#include "table_scan/table_scan_internal.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "mssql_functions.hpp"	// MSSQLCatalogScanBindData
#include "query/mssql_sql_params.hpp"

namespace duckdb {
namespace mssql {

//------------------------------------------------------------------------------
// Serialization
//------------------------------------------------------------------------------
// These callbacks are NOT optional bookkeeping — without them the scan produces
// WRONG RESULTS.
//
// DuckDB's common-subplan optimizer (added in v1.5.4) identifies equal subplans
// by SERIALIZING each operator and hashing the bytes. For a LOGICAL_GET,
// FunctionSerializer::Serialize writes the bind data only when the function has
// both callbacks set; otherwise it writes just the function name and signature.
// Everything that identifies which table this scan reads — schema, table, and
// the pushdown clauses we build ourselves — lives in MSSQLCatalogScanBindData,
// so without serialization two scans of DIFFERENT tables with the same output
// shape hash identically. The optimizer then materializes one of them as a CTE
// and feeds it to both consumers: `SELECT min(c) FROM t1 UNION ALL
// SELECT min(c) FROM t2` silently returns t1's data twice.
//
// (MSSQLCatalogScanBindData::Equals is correct but never consulted here — this
// optimizer compares serialized bytes, not FunctionData::Equals.)
//
// Which fields have to be here, and why — verified against the optimizer order
// in duckdb/src/optimizer/optimizer.cpp:
//
//   context/schema/table               load-bearing: the whole bug.
//   complex_filter_where_clause        load-bearing: FILTER_PUSHDOWN (:156) runs
//                                      BEFORE COMMON_SUBPLAN (:253), so two scans
//                                      of the SAME table can reach the optimizer
//                                      already carrying different WHERE clauses,
//                                      and this string is the only place they
//                                      differ (simple TableFilters are already
//                                      serialized by LogicalGet itself).
//   order_by_clause / top_n            NOT load-bearing for the signature: they
//                                      are set by our own optimizer extension,
//                                      which DuckDB runs as OptimizerType::
//                                      EXTENSION (:332) — after COMMON_SUBPLAN.
//                                      Both are still empty when the signature is
//                                      taken. Serialized for round-trip fidelity,
//                                      not for distinctness.
//
// Everything else in the bind data (column lists, PK/rowid metadata) is derived
// from the table and is therefore implied by schema+table.
void CatalogScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
						  const BoundTableFunction &function) {
	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();
	serializer.WriteProperty(100, "context_name", bind_data.context_name);
	serializer.WriteProperty(101, "schema_name", bind_data.schema_name);
	serializer.WriteProperty(102, "table_name", bind_data.table_name);
	serializer.WriteProperty(103, "complex_filter_where_clause", bind_data.complex_filter_where_clause);
	serializer.WriteProperty(104, "order_by_clause", bind_data.order_by_clause);
	serializer.WriteProperty(105, "top_n", bind_data.top_n);
	// Two scans of one catalog that differ ONLY in this flag must not be folded
	// together by the common-subplan optimizer: one holds its connection open and
	// the other does not, which is the whole point of the flag.
	serializer.WriteProperty(106, "requires_materialization", bind_data.requires_materialization);
	// Spec 076: the parameters the complex clause refers to, as two parallel
	// lists (names are positional: p0, p1, ...).
	vector<string> param_declarations;
	vector<string> param_literals;
	vector<Value> param_values;
	for (const auto &p : bind_data.complex_filter_params.params) {
		param_declarations.push_back(p.declaration);
		param_literals.push_back(p.literal);
		// Spec 083: the value, which the RPC request binds; a literal-only
		// parameter (none is made today) round-trips as SQLNULL and keeps the
		// batch form.
		param_values.push_back(p.has_value ? p.value : Value(LogicalType::SQLNULL));
	}
	serializer.WriteProperty(107, "complex_filter_param_declarations", param_declarations);
	serializer.WriteProperty(108, "complex_filter_param_literals", param_literals);
	serializer.WritePropertyWithDefault(109, "complex_filter_param_values", param_values);
	serializer.WritePropertyWithDefault(110, "joins_statement", bind_data.joins_statement, false);
}

// DuckDB round-trips a logical plan to copy it: CTE inlining copies a CTE
// used more than once under a LIMIT (LogicalOperatorDeepCopy, through
// LogicalOperator::Copy). The copy finds this function by name in the
// catalog, hence RegisterCatalogScanFunction, and rebuilds the bind data here
// from the live catalog entry. Validate defensively.
unique_ptr<FunctionData> CatalogScanDeserialize(Deserializer &deserializer, BoundTableFunction &function) {
	auto context_name = deserializer.ReadProperty<string>(100, "context_name");
	auto schema_name = deserializer.ReadProperty<string>(101, "schema_name");
	auto table_name = deserializer.ReadProperty<string>(102, "table_name");
	auto complex_filter_where_clause = deserializer.ReadProperty<string>(103, "complex_filter_where_clause");
	auto order_by_clause = deserializer.ReadProperty<string>(104, "order_by_clause");
	auto top_n = deserializer.ReadProperty<int64_t>(105, "top_n");
	auto requires_materialization = deserializer.ReadProperty<bool>(106, "requires_materialization");
	auto param_declarations = deserializer.ReadProperty<vector<string>>(107, "complex_filter_param_declarations");
	auto param_literals = deserializer.ReadProperty<vector<string>>(108, "complex_filter_param_literals");
	auto param_values = deserializer.ReadPropertyWithDefault<vector<Value>>(109, "complex_filter_param_values");
	auto joins_statement = deserializer.ReadPropertyWithDefault<bool>(110, "joins_statement");

	auto &context = deserializer.Get<ClientContext &>();

	// The alias may since have been detached and re-attached over a different
	// storage type. Cast<MSSQLCatalogScanBindData> below is unchecked in release
	// builds, so confirm the catalog is still ours before trusting the entry.
	auto &catalog = Catalog::GetCatalog(context, Identifier(context_name));
	if (catalog.GetCatalogType() != "mssql") {
		throw SerializationException("MSSQL: catalog \"%s\" is no longer an MSSQL catalog (now \"%s\")", context_name,
									 catalog.GetCatalogType());
	}

	// Rebuild through the catalog entry so the column/PK metadata comes from the
	// live catalog rather than from stale serialized copies.
	auto &entry = Catalog::GetEntry<TableCatalogEntry>(
		context, QualifiedName(Identifier(context_name), Identifier(schema_name), Identifier(table_name)));
	unique_ptr<FunctionData> bind_data;
	entry.GetScanFunction(context, bind_data);
	if (!bind_data) {
		throw SerializationException("MSSQL: could not rebind catalog scan for %s.%s.%s", context_name, schema_name,
									 table_name);
	}

	auto &result = bind_data->Cast<MSSQLCatalogScanBindData>();
	result.requires_materialization = requires_materialization;
	result.joins_statement = joins_statement;
	result.complex_filter_where_clause = complex_filter_where_clause;
	if (param_declarations.size() != param_literals.size()) {
		throw SerializationException("MSSQL: catalog scan parameter lists differ in length");
	}
	for (idx_t i = 0; i < param_declarations.size(); i++) {
		if (i < param_values.size() && param_values[i].type().id() != LogicalTypeId::SQLNULL) {
			result.complex_filter_params.Add(param_declarations[i], param_literals[i], param_values[i]);
		} else {
			result.complex_filter_params.Add(param_declarations[i], param_literals[i]);
		}
	}
	result.order_by_clause = order_by_clause;
	result.top_n = top_n;
	return bind_data;
}

}  // namespace mssql
}  // namespace duckdb

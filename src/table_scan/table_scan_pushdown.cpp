// The catalog scan's planner callbacks: pushdown_expression, pushdown_complex_filter and cardinality.

#include "table_scan/table_scan_internal.hpp"

#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_keyless_key.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/common.hpp"	 // For COLUMN_IDENTIFIER_ROW_ID
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_functions.hpp"	// MSSQLCatalogScanBindData
#include "query/mssql_sql_params.hpp"
#include "table_scan/filter_encoder.hpp"

namespace duckdb {
namespace mssql {

//------------------------------------------------------------------------------
// Complex Filter Pushdown
//------------------------------------------------------------------------------
// Handles expressions that cannot be represented as simple TableFilter objects:
// - Function expressions: year(col) = 2024, month(col) = 6, etc.
// - BETWEEN: col BETWEEN a AND b
// - Complex arithmetic in filters

// Map a LogicalGet's projected column ids to the table column indices the
// filter encoder speaks in. Shared by every pushdown callback so the mapping
// cannot drift between the dry-run (pushdown_expression) and the encode
// (pushdown_complex_filter). `column_ids_out` backs the context's reference and
// must outlive it.
static ExpressionEncodeContext BuildEncodeContext(ClientContext &context, const LogicalGet &get,
												  const MSSQLCatalogScanBindData &bind_data,
												  vector<column_t> &column_ids_out) {
	const auto &get_column_ids = get.GetColumnIds();
	column_ids_out.clear();
	column_ids_out.reserve(get_column_ids.size());
	for (const auto &col_idx : get_column_ids) {
		const auto id = col_idx.GetPrimaryIndex();
		if (IsKeylessKeyColumn(id)) {
			column_ids_out.push_back(ResolveKeylessKeyColumn(id));
		} else {
			column_ids_out.push_back(col_idx.IsVirtualColumn() ? COLUMN_IDENTIFIER_ROW_ID : id);
		}
	}

	ExpressionEncodeContext ctx(column_ids_out, bind_data.all_column_names, bind_data.all_types);
	if (!bind_data.pk_column_names.empty()) {
		ctx.SetPKInfo(&bind_data.pk_column_names, &bind_data.pk_column_types, bind_data.pk_is_composite);
	}
	ctx.mssql_columns = &bind_data.mssql_columns;
	// `error_on_division_by_zero = false` makes `x % 0` NULL in DuckDB and error
	// 8134 on the server, so the vocabulary refuses `%` -- the answer the
	// remote-pushdown writer already gave (roborev 1819 finding 3).
	ctx.division_by_zero_errors = LoadErrorOnDivisionByZero(context);
	return ctx;
}

// pushdown_expression: the 2.0 filter combiner offers a single-column
// expression (year(col) = 2024, col LIKE 'x%', octet_length(v) > 2) and asks
// whether the scan can push it. We answer by DRY-RUNNING the same encoder that
// pushdown_complex_filter uses: accept iff it produces T-SQL. An accepted
// expression is later delivered as an EXPRESSION_FILTER and reaches the server
// in the WHERE clause; if the runtime encode ever refuses what the dry-run
// accepted, the spec-069 client-filter net applies it, so a disagreement
// degrades to client-side filtering, never to wrong rows. The combiner has
// already proven the expression references exactly one column and has NOT yet
// rewritten its column ref to BoundReference, so EncodeColumnRef sees a real
// binding here.
//
// REACHABILITY (PR #269 review): as long as pushdown_complex_filter is also
// registered, this callback is effectively unreachable. DuckDB runs
// pushdown_complex_filter FIRST over every pending filter, with this same
// encoder and this same context, and erases everything it can render;
// TryPushdownGenericExpression then only offers what that pass refused — which
// the dry-run below refuses again by construction.
//
// MEASURED SINCE (roadmap v0.3.0, branch spec/070-w1-measure-shadowing): the
// reason first given for keeping both — that restricting ComplexFilterPushdown
// would lose pushdown for the shapes the combiner itself declines (volatile,
// oversized IN, CanThrow with more than one filter) — DID NOT REPRODUCE. Each
// of those still reached the server. Do not re-derive it.
//
// That is NOT the same as "no pushdown lost" (job 1130). Two classes DO lose and
// any deferral must exclude them:
//   * partially-pushed shapes — GenerateTableScanFilters returns
//     PUSHED_DOWN_PARTIALLY for LIKE-prefix / OR-chains / non-dense IN /
//     temporal-cast, and pushdown_get.cpp then SKIPS
//     TryPushdownGenericExpression. LIKE-prefix pushes comparison bounds the
//     encoder can render (a real relaxation, but it is a string predicate and
//     already excluded below). The other three push an `optional_filter` scalar
//     function the encoder has NO mapping for, so it refuses and the scan pushes
//     NOTHING — a full table stream, not a widened range;
//   * rowid — single-column, so it would defer, but FilterEncoder::Encode
//     refuses virtual columns while EncodeColumnRef (reached only through THIS
//     path) rewrites rowid to the scalar PK. `rowid > 100` would go from
//     `[id] > 100` to a full scan.
//
// The real blocker is semantic, and it splits the work in two — do NOT lift this
// as one blanket change. Stop claiming a predicate here and the combiner
// rewrites it before offering it: prefix() becomes a >=/< RANGE, exact for
// DuckDB's binary comparison and NOT for SQL Server's on any collation whose
// ordering is not binary (e.g. the common _CI_AS install default, where 'ñu'
// sorts between 'n' and 'o', so LIKE N'n%' returns 2 rows and the range
// returns 3). Refusing the range instead pushes nothing at all and the client
// net applies DuckDB semantics — 1 row — breaking the native-server-semantics
// contract just as surely. Pinned by
// test/sql/catalog/like_pushdown_collation.test.
//
//   NON-STRING predicates — safe to defer to pushdown_expression today.
//   Predicates over a COLLATED STRING COLUMN — comparisons as well as patterns —
//     must stay claimed HERE until spec 061 supplies a collation-faithful form.
//     TWO hazards, not one (jobs 1183/1184), which is why the criterion is the
//     column type and not the prefix() shape:
//       1. TryPushdownPrefixFilter rewrites prefix() into a >=/< range under
//          binary ordering — patterns only;
//       2. once ANY string comparison is a VISIBLE filter, the combiner prunes
//          contradictions and derives transitive predicates over its constants
//          under BINARY semantics, on predicates this path used to erase before
//          it ever saw them. `name = 'abc' AND name = 'ABC'` is contradictory
//          under binary and satisfiable on _CI_AS, so it can be pruned to empty.
//     `name = 'abc'` encodes identically either way; making it visible is what
//     is not free. They stay planner-invisible until 061 — the accepted cost.
bool MSSQLPushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr) {
	if (!get.bind_data) {
		return false;
	}
	auto &bind_data = get.bind_data->Cast<MSSQLCatalogScanBindData>();
	vector<column_t> column_ids;
	ExpressionEncodeContext ctx = BuildEncodeContext(context, get, bind_data, column_ids);
	// Predicate position — the same coercion (bare bit -> `= 1`) the runtime
	// filter build applies, so the dry-run accepts exactly what will encode.
	auto result = FilterEncoder::EncodeSearchCondition(expr, ctx);
	const bool accepted = result.supported && !result.sql.empty();
	MSSQL_SCAN_DEBUG_LOG(1, "PushdownExpression: type=%d class=%d -> %s", (int)expr.GetExpressionType(),
						 (int)expr.GetExpressionClass(), accepted ? "PUSHED" : "kept above scan");
	return accepted;
}

// cardinality: what the optimizer thinks this scan returns. WITHOUT this
// callback DuckDB has no number for a table-function scan and every MSSQL scan
// plans as ~1 row — a 200000-row table and a 50-row table are indistinguishable,
// so join order, build-side choice and every downstream estimate around them are
// decided by a coin flip. `MSSQLTableEntry::GetStorageInfo` has computed the real
// count since spec 023 and even says so in its comment ("Table-level cardinality
// is provided via GetStorageInfo") — but a TableFunction scan reads
// `TableFunction::cardinality`, not TableStorageInfo, so that work never reached
// the planner (roadmap v0.3.0 "step 0").
//
// Deliberately CHEAP and NON-THROWING. This runs inside the optimizer, once per
// scan per plan, so unlike GetStorageInfo it does NOT acquire a connection to
// query the DMV: it reads the count the catalog already loaded with the table
// metadata, falling back to the statistics cache. A query must not pay a network
// round trip to be planned.
//
// Catalog count FIRST, deliberately (job 1124). The statistics cache used to be
// preferred on the grounds that `PreloadRowCount` refreshes it after a bulk load
// — it does not: its only callers are metadata loads
// (mssql_preload_catalog.cpp, mssql_table_set.cpp), both feeding it the same
// approx_row_count the catalog entry already carries. No COPY/BCP path touches
// it. And until job 1124 that cache was invalidated by nothing, so a stale count
// outlived an explicit mssql_invalidate_cache(). The catalog count is refreshed
// by the metadata reload, which is the thing a user can actually trigger.
//
// Returning nullptr means "no estimate" — the pre-existing behaviour — and is the
// answer whenever the count is 0, because 0 CANNOT BE DISTINGUISHED FROM UNKNOWN
// here. A VIEW has no `sys.partitions` rows and reports 0 while returning
// millions; claiming 0 for it would make the planner pick it as a build side and
// is far worse than claiming nothing. An genuinely empty table therefore also
// plans as unknown, which costs an estimate nobody had before this callback.
unique_ptr<NodeStatistics> MSSQLCatalogScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	if (!bind_data_p) {
		return nullptr;
	}
	// mssql_enable_statistics gates this: it is the one optimizer-facing consumer
	// of the row counts the statistics layer collects, and an escape hatch if an
	// estimate ever plans worse than no estimate did.
	if (!LoadStatisticsEnabled(context)) {
		return nullptr;
	}
	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();
	if (!bind_data.table_entry) {
		return nullptr;
	}
	// static_cast, not dynamic_cast: the codebase avoids RTTI across the extension
	// boundary (see mssql_storage.cpp:189 and azure_secret_reader.cpp:42, both of
	// which say so), and a dynamic_cast here would reintroduce it on the planning
	// path — job 1124. The catalog type is checked first, which is the same
	// discriminator mssql_optimizer.cpp:66 and mssql_diagnostic.cpp:288 use.
	//
	// const_cast: GetMSSQLCatalog()/GetApproxRowCount() are non-const accessors on
	// the entry, and the bind data holds it as a const pointer. Nothing below
	// mutates the entry.
	auto *entry_base = const_cast<TableCatalogEntry *>(bind_data.table_entry.get());
	if (entry_base->ParentCatalog().GetCatalogType() != "mssql") {
		return nullptr;
	}
	auto *table_entry = static_cast<MSSQLTableEntry *>(entry_base);

	idx_t row_count = 0;
	try {
		// The catalog's own count first — it is what a metadata refresh updates.
		// The statistics cache only as a fallback, for the DMV-sourced count when
		// the catalog has none.
		row_count = table_entry->GetApproxRowCount();
		if (row_count == 0) {
			auto &stats_provider = table_entry->GetMSSQLCatalog().GetStatisticsProvider();
			// Apply the documented TTL at the POINT OF USE (job 1193) — but PASS it
			// in rather than calling SetCacheTTL (job 1203). The provider is one
			// object per CATALOG while the setting is per SESSION, so mutating it
			// from the planner let one session's `SET ... = 3600` hand every other
			// session a staleness window it never asked for.
			stats_provider.TryGetCachedRowCount(bind_data.schema_name, bind_data.table_name,
												LoadStatisticsCacheTTL(context), row_count);
		}
	} catch (...) {
		// Planning must not fail because an estimate was unavailable.
		return nullptr;
	}

	if (row_count == 0) {
		MSSQL_SCAN_DEBUG_LOG(1, "Cardinality: %s.%s -> unknown (count 0)", bind_data.schema_name.c_str(),
							 bind_data.table_name.c_str());
		return nullptr;
	}

	// Filter selectivity. DuckDB normally does this itself:
	// `RelationStatisticsHelper::ExtractGetStats` walks `get.table_filters` and
	// either derives a real selectivity from column statistics or, failing that,
	// applies DEFAULT_SELECTIVITY.
	//
	// It cannot do it for us, because OUR pushdown erases the filters before it
	// looks. `ComplexFilterPushdown` encodes each expression into
	// `complex_filter_where_clause` — a STRING — and removes it from the plan, so
	// `get.table_filters.HasFilters()` is false and the estimator applies NOTHING.
	// Verified against a live server: `WHERE id < 100` over 200000 rows, inside a
	// join so the join-order optimizer runs, still estimated 200000 rows, and
	// MSSQL_DEBUG shows "1 expressions handled, 0 remaining for DuckDB".
	//
	// So this mirrors DuckDB's own fallback rather than inventing a number — the
	// same 0.2 constant, applied for the same reason.
	//
	// DELETE THIS PER HALF, not all at once. If single-column expressions reached
	// `pushdown_expression` they would become EXPRESSION_FILTERs in
	// `get.table_filters`, DuckDB would apply selectivity itself, and doing it
	// here too would DOUBLE-COUNT. But the split above means the two halves land
	// at different times: when the NON-STRING half lands, this must stop applying
	// to those predicates while still covering string ones (which remain in
	// `complex_filter_where_clause`); only when the string half lands after 061
	// does the whole block go. Keying it on `complex_filter_where_clause` being
	// non-empty already does the right thing — that string holds exactly what
	// ComplexFilterPushdown still claims — so the likely edit is deletion in one
	// step at the END, not a flip in the middle.
	if (!bind_data.complex_filter_where_clause.empty()) {
		const idx_t filtered =
			MaxValue<idx_t>(static_cast<idx_t>(row_count * RelationStatisticsHelper::DEFAULT_SELECTIVITY), 1);
		MSSQL_SCAN_DEBUG_LOG(1, "Cardinality: %s.%s -> %llu rows (%llu * selectivity, WHERE %s)",
							 bind_data.schema_name.c_str(), bind_data.table_name.c_str(), (unsigned long long)filtered,
							 (unsigned long long)row_count, bind_data.complex_filter_where_clause.c_str());
		row_count = filtered;
	}

	// TOP N is a hard bound — the server cannot return more — so it is reported as
	// max_cardinality. NOTE it is reported but effectively NOT CONSUMED today:
	// max_cardinality from a table function is read only by StatisticsPropagator,
	// which runs BEFORE the EXTENSION stage where MSSQLOptimizer sets top_n, and
	// LogicalGet::EstimateCardinality discards max_cardinality entirely. So only
	// the MinValue estimate clamp below can take effect, and only if top_n is
	// somehow already set. Kept because it costs nothing and is correct the day
	// something does consume it — but do not describe it as a delivered bound.
	//
	// The clamp below is therefore correct but currently unreachable, and the
	// reason is the STAGE ORDER stated above — not anything about a particular
	// fixture (job 1197). Optimizer extensions run after RunBuiltInOptimizers, so
	// the get's estimate is already taken by the time MSSQLOptimizer could set
	// top_n.
	//
	// An earlier version of this comment said `ORDER BY id LIMIT 10` "keeps a Top
	// N above the scan, with mssql_order_pushdown true as well as false" and
	// presented that as a general rule. It is not: that measurement used
	// CardScanBig, which COPY creates with every column NULLABLE, and DuckDB's
	// default NULLS LAST for ASC then failed the NULL-placement check — so
	// TryPushTopN bailed before touching top_n. Against a NOT NULL column the TopN
	// WOULD fold and top_n WOULD be set -- and since spec 079 so does a nullable
	// one, its placement emulated with a leading CASE key. The conclusion
	// survives; the reason given for it did not. That is why
	// scan_cardinality.test carries no EXPLAIN assertion here.
	if (bind_data.top_n > 0) {
		const idx_t top_n = static_cast<idx_t>(bind_data.top_n);
		MSSQL_SCAN_DEBUG_LOG(1, "Cardinality: %s.%s -> capped at TOP %llu", bind_data.schema_name.c_str(),
							 bind_data.table_name.c_str(), (unsigned long long)top_n);
		return make_uniq<NodeStatistics>(MinValue<idx_t>(row_count, top_n), top_n);
	}

	MSSQL_SCAN_DEBUG_LOG(1, "Cardinality: %s.%s -> %llu rows", bind_data.schema_name.c_str(),
						 bind_data.table_name.c_str(), (unsigned long long)row_count);
	return make_uniq<NodeStatistics>(row_count);
}

void ComplexFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
						   vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();

	MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: processing %zu expression(s)", filters.size());

	vector<column_t> column_ids;
	ExpressionEncodeContext ctx = BuildEncodeContext(context, get, bind_data, column_ids);
	// Spec 076: the constants become parameters, kept on the bind data beside
	// the clause that refers to them. DuckDB calls this more than once on one
	// scan (a pushdown pass per optimizer that pushes filters again: a
	// materialized CTE's consumers, filter statistics), each call with the
	// filters left: what an earlier call stored stays, and this call's
	// parameters are numbered after those (issue #623: the clause was replaced,
	// its conditions gone from the plan and the server alike).
	mssql::SqlParamSet params = bind_data.complex_filter_params;
	if (LoadScanParameterizeFilters(context)) {
		ctx.params = &params;
	}

	std::vector<std::string> encoded_conditions;
	std::vector<idx_t> expressions_to_remove;

	for (idx_t i = 0; i < filters.size(); i++) {
		auto &filter = filters[i];
		MSSQL_SCAN_DEBUG_LOG(2, "  filter[%llu]: type=%d class=%d", (unsigned long long)i,
							 (int)filter->GetExpressionType(), (int)filter->GetExpressionClass());

		// Try to encode this expression (predicate position — bare bit -> `= 1`)
		auto result = FilterEncoder::EncodeSearchCondition(*filter, ctx);

		if (result.supported && !result.sql.empty()) {
			MSSQL_SCAN_DEBUG_LOG(1, "  filter[%llu]: encoded -> %s", (unsigned long long)i, result.sql.c_str());
			encoded_conditions.push_back(result.sql);
			expressions_to_remove.push_back(i);
		} else {
			MSSQL_SCAN_DEBUG_LOG(1, "  filter[%llu]: not supported, will be applied by DuckDB", (unsigned long long)i);
		}
	}

	// Remove the expressions we handled (in reverse order to keep indices valid)
	for (auto it = expressions_to_remove.rbegin(); it != expressions_to_remove.rend(); ++it) {
		filters.erase(filters.begin() + *it);
	}

	// Build the WHERE clause from encoded conditions
	if (!encoded_conditions.empty()) {
		std::string where_clause = bind_data.complex_filter_where_clause;
		for (const auto &condition : encoded_conditions) {
			if (!where_clause.empty()) {
				where_clause += " AND ";
			}
			where_clause += condition;
		}
		bind_data.complex_filter_where_clause = where_clause;
		bind_data.complex_filter_params = std::move(params);
		MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: stored WHERE clause: %s (%llu parameter(s))",
							 where_clause.c_str(), (unsigned long long)bind_data.complex_filter_params.params.size());
	}

	MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: %zu expressions handled, %zu remaining for DuckDB",
						 expressions_to_remove.size(), filters.size());
}

}  // namespace mssql
}  // namespace duckdb

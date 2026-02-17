// MSSQL Optimizer Extension - ORDER BY Pushdown
// Feature: 039-order-pushdown
//
// Detects ORDER BY / TOP N patterns above MSSQL catalog scans
// and pushes them down to SQL Server via ORDER BY / SELECT TOP N.
//
// DuckDB's built-in optimizers run before this extension optimizer.
// By the time we see the plan, DuckDB may have inserted LOGICAL_PROJECTION
// nodes between ORDER BY / TOP N and the LOGICAL_GET (mssql_catalog_scan).
// We must look through these projections to find the underlying scan.

#include "table_scan/mssql_optimizer.hpp"
#include <cstdlib>
#include <unordered_set>
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "mssql_functions.hpp"
#include "mssql_storage.hpp"
#include "table_scan/filter_encoder.hpp"
#include "table_scan/function_mapping.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetOptimizerDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("MSSQL_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define MSSQL_OPT_DEBUG(lvl, fmt, ...)                                     \
	do {                                                                   \
		if (GetOptimizerDebugLevel() >= lvl) {                             \
			fprintf(stderr, "[MSSQL OPTIMIZER] " fmt "\n", ##__VA_ARGS__); \
		}                                                                  \
	} while (0)

namespace duckdb {

//------------------------------------------------------------------------------
// Helper: Check if pushdown is enabled for a given MSSQL scan
//------------------------------------------------------------------------------
static bool IsOrderPushdownEnabled(ClientContext &context, const MSSQLCatalogScanBindData &bind_data) {
	// Check global setting first
	Value val;
	if (context.TryGetCurrentSetting("mssql_order_pushdown", val)) {
		bool enabled = val.GetValue<bool>();
		if (enabled) {
			MSSQL_OPT_DEBUG(1, "Order pushdown from global setting: true");
			return true;
		}
	}

	// Check ATTACH option second (true = enable, false = no-op)
	auto &db = *context.db;
	auto &manager = MSSQLContextManager::Get(db);
	auto ctx = manager.GetContext(bind_data.context_name);
	if (ctx && ctx->connection_info) {
		if (ctx->connection_info->order_pushdown > 0) {
			MSSQL_OPT_DEBUG(1, "Order pushdown from ATTACH option: true");
			return true;
		}
	}

	MSSQL_OPT_DEBUG(1, "Order pushdown: disabled (default)");
	return false;
}

//------------------------------------------------------------------------------
// Helper: Find MSSQL LogicalGet through optional Projection nodes
// Returns nullptr if not found or not an MSSQL scan
//------------------------------------------------------------------------------
struct MSSQLScanInfo {
	LogicalGet *get = nullptr;
	LogicalProjection *projection = nullptr;
	unique_ptr<LogicalOperator> *get_owner = nullptr;
};

static MSSQLScanInfo FindMSSQLScan(unique_ptr<LogicalOperator> &node) {
	MSSQLScanInfo info;
	if (node->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = node->Cast<LogicalGet>();
		if (get.function.name == "mssql_catalog_scan") {
			info.get = &get;
			info.get_owner = &node;
		}
		return info;
	}
	if (node->type == LogicalOperatorType::LOGICAL_PROJECTION && node->children.size() == 1) {
		auto &child = node->children[0];
		if (child->type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = child->Cast<LogicalGet>();
			if (get.function.name == "mssql_catalog_scan") {
				info.get = &get;
				info.projection = &node->Cast<LogicalProjection>();
				info.get_owner = &child;
			}
		}
	}
	return info;
}

//------------------------------------------------------------------------------
// Helper: Resolve a column reference expression to a table column index
// Handles both BOUND_REF (positional) and BOUND_COLUMN_REF (table.column binding)
//------------------------------------------------------------------------------
static bool ResolveColumnIndex(const Expression &expr, const LogicalGet &get, idx_t &out_col_ids_index) {
	auto &col_ids = get.GetColumnIds();

	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ref.index >= col_ids.size()) {
			return false;
		}
		out_col_ids_index = ref.index;
		return true;
	}

	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &col_ref = expr.Cast<BoundColumnRefExpression>();
		// Match against the Get's table_index and find column in GetColumnIds
		if (col_ref.binding.table_index != get.table_index) {
			MSSQL_OPT_DEBUG(2, "  BOUND_COLUMN_REF table_index %llu != get.table_index %llu",
							(unsigned long long)col_ref.binding.table_index, (unsigned long long)get.table_index);
			return false;
		}
		// column_index maps to position in GetColumnIds
		if (col_ref.binding.column_index >= col_ids.size()) {
			return false;
		}
		out_col_ids_index = col_ref.binding.column_index;
		return true;
	}

	return false;
}

//------------------------------------------------------------------------------
// Helper: Resolve an ORDER BY expression to a T-SQL ORDER BY fragment
//
// ORDER BY expressions may use BOUND_COLUMN_REF (table.column binding) or
// BOUND_REF (positional reference). When a Projection sits between the
// ordering node and the Get, ORDER BY refs point to projection outputs
// and we must resolve through the Projection's expressions.
//------------------------------------------------------------------------------
static bool ResolveOrderExpression(const Expression &expr, const LogicalGet &get, const LogicalProjection *projection,
								   const MSSQLCatalogScanBindData &bind_data, string &out_fragment,
								   string &out_source_column, idx_t &out_table_col_idx) {
	const Expression *resolve_expr = &expr;
	auto &col_ids = get.GetColumnIds();

	MSSQL_OPT_DEBUG(2, "  ResolveOrderExpression: expr class=%d, has_projection=%s", (int)expr.GetExpressionClass(),
					projection ? "yes" : "no");

	// If there's a projection, try to resolve through it
	if (projection) {
		// BOUND_REF: positional reference to projection output
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto &ref = expr.Cast<BoundReferenceExpression>();
			if (ref.index < projection->expressions.size()) {
				resolve_expr = projection->expressions[ref.index].get();
				MSSQL_OPT_DEBUG(2, "  Resolved BOUND_REF[%llu] through projection -> class=%d",
								(unsigned long long)ref.index, (int)resolve_expr->GetExpressionClass());
			} else {
				return false;
			}
		}
		// BOUND_COLUMN_REF: check if it references the projection's output or the Get directly
		else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &col_ref = expr.Cast<BoundColumnRefExpression>();
			// If binding matches projection's table_index, look through projection
			if (col_ref.binding.table_index == projection->table_index &&
				col_ref.binding.column_index < projection->expressions.size()) {
				resolve_expr = projection->expressions[col_ref.binding.column_index].get();
				MSSQL_OPT_DEBUG(2, "  Resolved BOUND_COLUMN_REF[%llu.%llu] through projection -> class=%d",
								(unsigned long long)col_ref.binding.table_index,
								(unsigned long long)col_ref.binding.column_index,
								(int)resolve_expr->GetExpressionClass());
			}
			// If binding matches Get's table_index, resolve directly against Get (skip projection)
			else if (col_ref.binding.table_index == get.table_index) {
				MSSQL_OPT_DEBUG(2, "  BOUND_COLUMN_REF[%llu.%llu] references Get directly",
								(unsigned long long)col_ref.binding.table_index,
								(unsigned long long)col_ref.binding.column_index);
				// resolve_expr stays as &expr, handled below
			}
		}
	}

	// Case 1: Simple column reference (BOUND_REF or BOUND_COLUMN_REF)
	idx_t col_ids_index;
	if (ResolveColumnIndex(*resolve_expr, get, col_ids_index)) {
		auto col_id = col_ids[col_ids_index];
		idx_t table_col_idx = col_id.IsVirtualColumn() ? idx_t(-1) : col_id.GetPrimaryIndex();
		if (table_col_idx >= bind_data.all_column_names.size()) {
			return false;
		}
		out_source_column = bind_data.all_column_names[table_col_idx];
		out_fragment = "[" + mssql::FilterEncoder::EscapeBracketIdentifier(out_source_column) + "]";
		out_table_col_idx = table_col_idx;
		return true;
	}

	// Case 2: Function expression (e.g., YEAR(col))
	if (resolve_expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func_expr = resolve_expr->Cast<BoundFunctionExpression>();
		auto *mapping = mssql::GetFunctionMapping(func_expr.function.name);
		if (!mapping) {
			MSSQL_OPT_DEBUG(2, "  Unsupported function: %s", func_expr.function.name.c_str());
			return false;
		}

		if (mapping->expected_args != 1 || func_expr.children.size() != 1) {
			MSSQL_OPT_DEBUG(2, "  Function %s: expected 1 arg, got %zu", func_expr.function.name.c_str(),
							func_expr.children.size());
			return false;
		}

		idx_t inner_col_ids_index;
		if (!ResolveColumnIndex(*func_expr.children[0], get, inner_col_ids_index)) {
			MSSQL_OPT_DEBUG(2, "  Function %s: arg is not a resolvable column ref", func_expr.function.name.c_str());
			return false;
		}

		auto col_id = col_ids[inner_col_ids_index];
		idx_t table_col_idx = col_id.IsVirtualColumn() ? idx_t(-1) : col_id.GetPrimaryIndex();
		if (table_col_idx >= bind_data.all_column_names.size()) {
			return false;
		}

		string inner_col = bind_data.all_column_names[table_col_idx];
		string escaped_col = "[" + mssql::FilterEncoder::EscapeBracketIdentifier(inner_col) + "]";
		string tmpl = mapping->sql_template;
		size_t pos = tmpl.find("{0}");
		if (pos != string::npos) {
			tmpl.replace(pos, 3, escaped_col);
		}
		out_fragment = tmpl;
		out_source_column = inner_col;
		out_table_col_idx = table_col_idx;
		return true;
	}

	return false;
}

//------------------------------------------------------------------------------
// Helper: Validate NULL ordering compatibility with SQL Server
//------------------------------------------------------------------------------
static bool IsNullOrderCompatible(OrderType order_type, OrderByNullType null_order, bool is_nullable) {
	// If column is NOT NULL, NULL ordering is irrelevant
	if (!is_nullable) {
		return true;
	}

	// SQL Server defaults:
	// ASC  -> NULLs FIRST
	// DESC -> NULLs LAST
	if (order_type == OrderType::ASCENDING) {
		return null_order == OrderByNullType::NULLS_FIRST;
	} else {
		return null_order == OrderByNullType::NULLS_LAST;
	}
}

//------------------------------------------------------------------------------
// Core: Process ORDER BY nodes and build pushdown clause
//------------------------------------------------------------------------------
static idx_t ProcessOrderByNodes(const vector<BoundOrderByNode> &orders, const LogicalGet &get,
								 const LogicalProjection *projection, MSSQLCatalogScanBindData &bind_data,
								 string &out_order_clause) {
	string order_clause;
	idx_t pushed_count = 0;

	for (idx_t i = 0; i < orders.size(); i++) {
		auto &order = orders[i];

		// Try to build T-SQL fragment for this expression
		string fragment;
		string source_column;
		idx_t table_col_idx;
		if (!ResolveOrderExpression(*order.expression, get, projection, bind_data, fragment, source_column,
									table_col_idx)) {
			MSSQL_OPT_DEBUG(1, "  ORDER BY[%llu]: cannot push (unsupported expression)", (unsigned long long)i);
			break;	// Stop at first non-pushable column (prefix only)
		}

		// Validate NULL ordering
		bool is_nullable = true;
		if (table_col_idx < bind_data.mssql_columns.size()) {
			is_nullable = bind_data.mssql_columns[table_col_idx].is_nullable;
		}

		if (!IsNullOrderCompatible(order.type, order.null_order, is_nullable)) {
			MSSQL_OPT_DEBUG(1, "  ORDER BY[%llu]: NULL order mismatch for nullable column %s", (unsigned long long)i,
							source_column.c_str());
			break;	// Stop at first incompatible column
		}

		// Add direction suffix
		string direction = (order.type == OrderType::DESCENDING) ? " DESC" : " ASC";
		if (!order_clause.empty()) {
			order_clause += ", ";
		}
		order_clause += fragment + direction;
		pushed_count++;

		MSSQL_OPT_DEBUG(1, "  ORDER BY[%llu]: pushed %s%s", (unsigned long long)i, fragment.c_str(), direction.c_str());
	}

	out_order_clause = order_clause;
	return pushed_count;
}

//------------------------------------------------------------------------------
// Helper: Collect Get column_ids positions referenced by an expression tree
//
// Walks the expression tree and adds any BOUND_REF index values to the set.
// These indices correspond to positions in LogicalGet::column_ids.
// Used to determine which Get columns a Projection expression actually needs.
//------------------------------------------------------------------------------
static void CollectGetReferences(const Expression &expr, unordered_set<idx_t> &refs) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF: {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		refs.insert(ref.index);
		return;
	}
	case ExpressionClass::BOUND_COLUMN_REF: {
		// BOUND_COLUMN_REF in a Projection expression may reference Get positions
		// but we can't resolve without table_index matching, so conservatively skip
		return;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		for (auto &child : func.children) {
			CollectGetReferences(*child, refs);
		}
		return;
	}
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		CollectGetReferences(*cast.child, refs);
		return;
	}
	default:
		return;
	}
}

//------------------------------------------------------------------------------
// Helper: Prune trailing ORDER-BY-only columns after full ORDER BY pushdown
//
// When ORDER BY is fully pushed to SQL Server, columns that existed only for
// sorting are no longer needed. The projection_map from LogicalOrder tells us
// which child output positions the parent actually references.
//
// Safety: only prunes trailing positions (beyond max_needed). Non-trailing
// unused positions would shift indices and break parent bindings.
//------------------------------------------------------------------------------
static void TryPruneOrderByOnlyColumns(const vector<idx_t> &projection_map, MSSQLScanInfo &scan_info) {
	// Empty projection_map means everything is referenced (identity pass-through)
	if (projection_map.empty()) {
		MSSQL_OPT_DEBUG(2, "Projection pruning: skipped (empty projection_map = all referenced)");
		return;
	}

	// Build set of needed positions and find the maximum
	unordered_set<idx_t> needed_positions(projection_map.begin(), projection_map.end());
	idx_t max_needed = 0;
	for (auto pos : projection_map) {
		if (pos > max_needed) {
			max_needed = pos;
		}
	}

	// Check that ALL unused positions are trailing (beyond max_needed)
	// If any unused position is non-trailing, abort — truncation would shift indices
	auto &col_ids = scan_info.get->GetColumnIds();
	idx_t child_output_count = scan_info.projection ? scan_info.projection->expressions.size() : col_ids.size();

	for (idx_t i = 0; i < child_output_count; i++) {
		if (needed_positions.find(i) == needed_positions.end() && i <= max_needed) {
			MSSQL_OPT_DEBUG(1, "Projection pruning: ABORTED (non-trailing unused position %llu, max_needed=%llu)",
							(unsigned long long)i, (unsigned long long)max_needed);
			return;
		}
	}

	// All unused positions are trailing — safe to truncate
	idx_t new_size = max_needed + 1;
	idx_t pruned_count = child_output_count - new_size;
	if (pruned_count == 0) {
		MSSQL_OPT_DEBUG(2, "Projection pruning: no trailing columns to prune");
		return;
	}

	if (scan_info.projection) {
		// Case B: ORDER -> Projection -> Get
		// Truncate Projection expressions
		idx_t old_proj_size = scan_info.projection->expressions.size();
		scan_info.projection->expressions.resize(new_size);
		MSSQL_OPT_DEBUG(1, "Projection pruning: truncated Projection expressions %llu -> %llu",
						(unsigned long long)old_proj_size, (unsigned long long)new_size);

		// Now check if any Get column_ids positions are no longer referenced
		// by the remaining Projection expressions
		unordered_set<idx_t> get_refs;
		for (idx_t i = 0; i < scan_info.projection->expressions.size(); i++) {
			CollectGetReferences(*scan_info.projection->expressions[i], get_refs);
		}

		// Find max referenced Get position
		idx_t max_get_ref = 0;
		for (auto ref : get_refs) {
			if (ref > max_get_ref) {
				max_get_ref = ref;
			}
		}

		// Check if unreferenced Get positions are trailing
		auto &mut_col_ids = scan_info.get->GetMutableColumnIds();
		bool can_truncate_get = true;
		for (idx_t i = 0; i < mut_col_ids.size(); i++) {
			if (get_refs.find(i) == get_refs.end() && i <= max_get_ref) {
				can_truncate_get = false;
				break;
			}
		}

		if (can_truncate_get && max_get_ref + 1 < mut_col_ids.size()) {
			idx_t old_get_size = mut_col_ids.size();
			mut_col_ids.resize(max_get_ref + 1);
			MSSQL_OPT_DEBUG(1, "Projection pruning: truncated Get column_ids %llu -> %llu",
							(unsigned long long)old_get_size, (unsigned long long)(max_get_ref + 1));
		}
	} else {
		// Case A: ORDER -> Get (no Projection)
		// Directly truncate Get column_ids
		auto &mut_col_ids = scan_info.get->GetMutableColumnIds();
		idx_t old_size = mut_col_ids.size();
		mut_col_ids.resize(new_size);
		MSSQL_OPT_DEBUG(1, "Projection pruning: truncated Get column_ids %llu -> %llu", (unsigned long long)old_size,
						(unsigned long long)new_size);
	}
}

//------------------------------------------------------------------------------
// Pattern: LogicalOrder -> [Projection ->] LogicalGet (simple ORDER BY)
//------------------------------------------------------------------------------
static void TryPushOrderBy(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
		return;
	}
	if (plan->children.size() != 1) {
		return;
	}

	auto scan_info = FindMSSQLScan(plan->children[0]);
	if (!scan_info.get) {
		return;
	}

	auto &order = plan->Cast<LogicalOrder>();
	auto &bind_data = scan_info.get->bind_data->Cast<MSSQLCatalogScanBindData>();

	if (!IsOrderPushdownEnabled(context, bind_data)) {
		return;
	}

	MSSQL_OPT_DEBUG(1, "Detected LogicalOrder -> %sLogicalGet(mssql_catalog_scan) for %s.%s",
					scan_info.projection ? "Projection -> " : "", bind_data.schema_name.c_str(),
					bind_data.table_name.c_str());

	// Process ORDER BY columns
	string order_clause;
	idx_t pushed = ProcessOrderByNodes(order.orders, *scan_info.get, scan_info.projection, bind_data, order_clause);

	if (pushed == 0) {
		MSSQL_OPT_DEBUG(1, "No columns pushed down");
		return;
	}

	// Store ORDER BY clause in bind_data
	bind_data.order_by_clause = order_clause;
	MSSQL_OPT_DEBUG(1, "ORDER BY clause: %s", order_clause.c_str());

	// Full pushdown: remove LogicalOrder from plan, keep Projection if present
	if (pushed == order.orders.size()) {
		// Capture projection_map BEFORE destroying LogicalOrder
		vector<idx_t> projection_map = order.projection_map;

		MSSQL_OPT_DEBUG(1, "Full ORDER BY pushdown - removing LogicalOrder from plan");
		plan = std::move(plan->children[0]);

		// Re-locate scan_info after plan mutation (plan now points to child)
		auto pruned_scan_info = FindMSSQLScan(plan);
		if (pruned_scan_info.get) {
			TryPruneOrderByOnlyColumns(projection_map, pruned_scan_info);
		}
	} else {
		MSSQL_OPT_DEBUG(1, "Partial ORDER BY pushdown (%llu/%llu columns) - keeping LogicalOrder",
						(unsigned long long)pushed, (unsigned long long)order.orders.size());
	}
}

//------------------------------------------------------------------------------
// Pattern: LogicalLimit -> LogicalOrder -> [Projection ->] LogicalGet
//------------------------------------------------------------------------------
static void TryPushLimitOrderBy(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_LIMIT) {
		return;
	}
	if (plan->children.size() != 1) {
		return;
	}
	auto &limit_child = plan->children[0];
	if (limit_child->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
		return;
	}
	if (limit_child->children.size() != 1) {
		return;
	}

	auto scan_info = FindMSSQLScan(limit_child->children[0]);
	if (!scan_info.get) {
		return;
	}

	auto &limit = plan->Cast<LogicalLimit>();
	auto &order = limit_child->Cast<LogicalOrder>();
	auto &bind_data = scan_info.get->bind_data->Cast<MSSQLCatalogScanBindData>();

	if (!IsOrderPushdownEnabled(context, bind_data)) {
		return;
	}

	// Only push constant LIMIT (no offset, no percentage)
	if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		MSSQL_OPT_DEBUG(1, "LIMIT is not a constant value - skipping TOP N pushdown");
		return;
	}
	if (limit.offset_val.Type() != LimitNodeType::UNSET) {
		MSSQL_OPT_DEBUG(1, "OFFSET present - skipping TOP N pushdown");
		return;
	}

	MSSQL_OPT_DEBUG(1, "Detected LogicalLimit -> LogicalOrder -> %sLogicalGet(mssql_catalog_scan) for %s.%s",
					scan_info.projection ? "Projection -> " : "", bind_data.schema_name.c_str(),
					bind_data.table_name.c_str());

	// Process ORDER BY columns
	string order_clause;
	idx_t pushed = ProcessOrderByNodes(order.orders, *scan_info.get, scan_info.projection, bind_data, order_clause);

	if (pushed == 0) {
		MSSQL_OPT_DEBUG(1, "No columns pushed down");
		return;
	}

	// Store ORDER BY clause
	bind_data.order_by_clause = order_clause;

	// Full pushdown: also push TOP N and remove both LIMIT and ORDER from plan
	if (pushed == order.orders.size()) {
		idx_t limit_val = limit.limit_val.GetConstantValue();
		bind_data.top_n = static_cast<int64_t>(limit_val);
		MSSQL_OPT_DEBUG(1, "Full TOP %llu pushdown with ORDER BY: %s", (unsigned long long)limit_val,
						order_clause.c_str());
		// Replace LIMIT -> ORDER -> [Projection ->] GET with [Projection ->] GET
		plan = std::move(limit_child->children[0]);
	} else {
		MSSQL_OPT_DEBUG(1, "Partial ORDER BY pushdown (%llu/%llu) - keeping LIMIT and ORDER",
						(unsigned long long)pushed, (unsigned long long)order.orders.size());
	}
}

//------------------------------------------------------------------------------
// Pattern: LogicalTopN -> [Projection ->] LogicalGet (merged ORDER BY + LIMIT)
//------------------------------------------------------------------------------
static void TryPushTopN(ClientContext &context, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_TOP_N) {
		return;
	}
	if (plan->children.size() != 1) {
		return;
	}

	auto scan_info = FindMSSQLScan(plan->children[0]);
	if (!scan_info.get) {
		return;
	}

	auto &top_n = plan->Cast<LogicalTopN>();
	auto &bind_data = scan_info.get->bind_data->Cast<MSSQLCatalogScanBindData>();

	if (!IsOrderPushdownEnabled(context, bind_data)) {
		return;
	}

	// Only push when no offset
	if (top_n.offset > 0) {
		MSSQL_OPT_DEBUG(1, "OFFSET present in TopN - skipping pushdown");
		return;
	}

	MSSQL_OPT_DEBUG(1, "Detected LogicalTopN -> %sLogicalGet(mssql_catalog_scan) for %s.%s",
					scan_info.projection ? "Projection -> " : "", bind_data.schema_name.c_str(),
					bind_data.table_name.c_str());

	// Process ORDER BY columns
	string order_clause;
	idx_t pushed = ProcessOrderByNodes(top_n.orders, *scan_info.get, scan_info.projection, bind_data, order_clause);

	if (pushed == 0) {
		MSSQL_OPT_DEBUG(1, "No columns pushed down");
		return;
	}

	// Store ORDER BY clause
	bind_data.order_by_clause = order_clause;

	// Full pushdown: replace TopN with its child (Projection -> GET or just GET)
	if (pushed == top_n.orders.size()) {
		bind_data.top_n = static_cast<int64_t>(top_n.limit);
		MSSQL_OPT_DEBUG(1, "Full TOP %llu pushdown with ORDER BY: %s", (unsigned long long)top_n.limit,
						order_clause.c_str());
		plan = std::move(plan->children[0]);
	} else {
		MSSQL_OPT_DEBUG(1, "Partial ORDER BY pushdown (%llu/%llu) - keeping LogicalTopN", (unsigned long long)pushed,
						(unsigned long long)top_n.orders.size());
	}
}

//------------------------------------------------------------------------------
// Main optimizer entry point
//------------------------------------------------------------------------------
void MSSQLOptimizer::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	// Try each pattern on the current node
	TryPushTopN(input.context, plan);
	TryPushLimitOrderBy(input.context, plan);
	TryPushOrderBy(input.context, plan);

	// Recurse into children
	for (auto &child : plan->children) {
		Optimize(input, child);
	}
}

}  // namespace duckdb

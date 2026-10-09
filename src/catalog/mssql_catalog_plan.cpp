#include "catalog/mssql_catalog.hpp"

#include "catalog/mssql_keyless_key.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "catalog/mssql_transaction.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/bcp_config.hpp"
#include "dml/ctas/mssql_ctas_planner.hpp"
#include "dml/delete/mssql_delete_target.hpp"
#include "dml/delete/mssql_physical_delete.hpp"
#include "dml/insert/mssql_insert_bulk_plan.hpp"
#include "dml/insert/mssql_insert_config.hpp"
#include "dml/insert/mssql_insert_target.hpp"
#include "dml/insert/mssql_physical_insert.hpp"
#include "dml/mssql_dml_config.hpp"
#include "dml/mssql_physical_staged_dml.hpp"
#include "dml/update/mssql_physical_update.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "mssql_functions.hpp"
#include "table_scan/filter_encoder.hpp"

namespace duckdb {

//! Set while DuckDB plans a MERGE's actions through PlanUpdate / PlanDelete /
//! PlanInsert below (planning is single-threaded per statement).
static thread_local bool planning_merge_actions = false;

//! For each INSERT action of the MERGE being planned, in the order DuckDB plans
//! them, which table columns (storage order) the action did NOT name. DuckDB
//! binds a MERGE's INSERT action full width -- an unnamed column carries a copy
//! of its bound default -- and copies every bound default into the synthesized
//! LogicalInsert, so the null-slot tell PlanInsert uses for a plain INSERT does
//! not exist there (review of #423). Filled by PlanMergeInto, consumed by
//! PlanInsert.
static thread_local vector<vector<bool>> merge_insert_unnamed;
static thread_local idx_t merge_insert_next = 0;

//===----------------------------------------------------------------------===//
// Write Operations (all throw - read-only catalog)
//===----------------------------------------------------------------------===//

PhysicalOperator &MSSQLCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
										   optional_ptr<PhysicalOperator> plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("INSERT");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Build MSSQLInsertTarget from table metadata
	MSSQLInsertTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

	// Determine which columns are being inserted
	// If no column map is specified, use all non-identity columns
	vector<idx_t> insert_col_indices;
	if (planning_merge_actions) {
		// A MERGE's INSERT action: the columns it did not name stay out of the
		// list, so the server applies their identity / DEFAULT / computed value,
		// as for a plain INSERT (see merge_insert_unnamed).
		if (merge_insert_next >= merge_insert_unnamed.size()) {
			throw InternalException("MSSQL: MERGE INSERT action planned without its column set");
		}
		const auto &unnamed = merge_insert_unnamed[merge_insert_next++];
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			if (i >= unnamed.size() || !unnamed[i]) {
				insert_col_indices.push_back(i);
			}
		}
	} else if (op.column_index_map.empty()) {
		// 2.0 no longer populates column_index_map: the binder expands the
		// child plan to every physical column and fills the ones the INSERT
		// did not name with their bound default. The tell is the move in
		// ResolveInputProjection — an unnamed column's bound_defaults slot is
		// moved into the projection and left null, a named column's is only
		// dereferenced. Unnamed columns stay OUT of the generated column list
		// so the server applies its own identity/defaults, exactly what the
		// named-map flow produced on 1.5.x. A full-positional INSERT keeps
		// every slot non-null and inserts every column, as before.
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			const bool unnamed = i < op.bound_defaults.size() && !op.bound_defaults[i];
			if (!unnamed) {
				insert_col_indices.push_back(i);
			}
		}
	} else {
		// Specific columns from the INSERT statement
		// The column_index_map maps physical column index -> source index in values
		// IMPORTANT: We must preserve INSERT statement column order, not table column order.
		// Build a list of (source_index, table_col_index) pairs and sort by source index.
		vector<pair<idx_t, idx_t>> col_pairs;
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			PhysicalIndex phys_idx(i);
			if (i < op.column_index_map.size()) {
				auto mapped_index = op.column_index_map[phys_idx];
				if (mapped_index != DConstants::INVALID_INDEX) {
					col_pairs.emplace_back(mapped_index, i);
				}
			}
		}
		// Sort by source index (INSERT statement order)
		std::sort(col_pairs.begin(), col_pairs.end());
		// Extract table column indices in INSERT statement order
		for (auto &pair : col_pairs) {
			insert_col_indices.push_back(pair.second);
		}
	}

	// Build column metadata for insert target
	target.has_identity_column = false;
	target.identity_column_index = 0;

	// Spec 060: must match what the catalog told DuckDB about these columns —
	// the binder resolved RETURNING against those types, and MSSQLPhysicalInsert
	// references the parser's chunk straight into DuckDB's, which requires the
	// types to be equal down to the extension info.
	const bool native_types = MSSQLReportsNativeTypes(*this);

	for (idx_t i = 0; i < mssql_columns.size(); i++) {
		auto &col = mssql_columns[i];
		MSSQLInsertColumn insert_col;
		insert_col.name = col.name;
		insert_col.duckdb_type = native_types ? col.NativeDuckDBType() : col.duckdb_type;
		insert_col.mssql_type = col.sql_type_name;
		insert_col.max_length = col.max_length;
		insert_col.is_identity = col.is_identity;  // spec 062 W4: from sys.columns, via the cache
		if (col.is_identity) {
			target.has_identity_column = true;
			target.identity_column_index = i;
		}
		insert_col.is_nullable = col.is_nullable;
		insert_col.has_default = false;	 // TODO: Query this from sys.columns
		insert_col.collation = col.collation_name;
		insert_col.precision = col.precision;
		insert_col.scale = col.scale;
		target.columns.push_back(std::move(insert_col));
	}

	// The OUTPUT list of an INSERT … RETURNING is built by the same function as
	// the scan's SELECT list, and that function takes this setting.
	target.convert_varchar_max = LoadConvertVarcharMax(context);

	// Spec 077 W2: is the identity column among the columns being inserted?
	// Only for a table — SET IDENTITY_INSERT takes a table, and an explicit
	// identity value through a view is the server's call.
	if (target.has_identity_column && table_entry.GetObjectType() != MSSQLObjectType::VIEW) {
		for (auto idx : insert_col_indices) {
			if (idx == target.identity_column_index) {
				target.identity_in_list = true;
				break;
			}
		}
	}

	// Every column left to the server is `INSERT … DEFAULT VALUES` (or a MERGE
	// action's `INSERT DEFAULT VALUES`), which the statement builder has no
	// form for: it sent an empty column list, refused with error 102.
	if (insert_col_indices.empty()) {
		throw NotImplementedException(
			"MSSQL: INSERT ... DEFAULT VALUES is not supported yet on '%s.%s'; name "
			"at least one column",
			target.schema_name, target.table_name);
	}

	// Set insert column indices
	target.insert_column_indices = std::move(insert_col_indices);

	// Handle RETURNING columns
	if (op.return_chunk) {
		// Map RETURNING columns
		for (idx_t i = 0; i < mssql_columns.size(); i++) {
			target.returning_column_indices.push_back(i);
		}
	}

	// Load insert configuration from settings
	MSSQLInsertConfig config = LoadInsertConfig(context);

	// Spec 062 W1: the bulk path, decided here, its plan settled here. Three
	// things keep an INSERT on the statement path whatever the setting says:
	// RETURNING (no rows come back from INSERT BULK; OUTPUT INSERTED is a
	// statement by construction), an explicit identity column (INSERT BULK
	// keeps the value unconditionally, where a statement lets the server
	// refuse it without IDENTITY_INSERT -- W4 is what makes this decidable),
	// and the setting itself. Whether the rows are FEW enough to stay on the
	// statement path is not known here; the sink decides that at the threshold.
	MSSQLInsertBulkPlan bulk;
	bulk.threshold = config.bcp_threshold;
	if (op.return_chunk) {
		bulk.statement_path_reason = "RETURNING";
	} else if (!config.use_bcp) {
		bulk.statement_path_reason = "mssql_insert_use_bcp = false";
	} else if (table_entry.GetObjectType() == MSSQLObjectType::VIEW) {
		// INSERT BULK into a view is the server's call (updatable single-table
		// views only, INSTEAD OF triggers aside); the statement path is what an
		// INSERT into a view always was (spec 062 self-review).
		bulk.statement_path_reason = "target is a view";
	} else {
		for (auto col_idx : target.insert_column_indices) {
			const auto &col = mssql_columns[col_idx];
			if (col.is_identity) {
				bulk.statement_path_reason = "explicit identity column '" + col.name + "'";
				break;
			}
			// A geometry/geography column arrives as WKB, which the statement
			// path renders as a 0x literal the server converts; on the bulk wire
			// FromServerColumn would declare it nvarchar and send the bytes as
			// text. The same for the types the catalog casts to NVARCHAR(MAX)
			// on read (hierarchyid, sql_variant): the statement path sends a
			// string literal the server converts, the bulk wire would not.
			if (col.is_geometry || col.is_cast_required) {
				bulk.statement_path_reason = "column '" + col.name + "' of type " + col.sql_type_name;
				break;
			}
		}
	}
	if (bulk.statement_path_reason.empty()) {
		bulk.enabled = true;
		if (planning_merge_actions) {
			// A MERGE's INSERT action: its rows stay staged until the action's
			// Finalize and go as statements then (MSSQLDMLConfig::
			// defer_to_finalize). A bulk stream kept open across the sink would
			// hold the pinned connection while the other actions send.
			bulk.threshold = NumericLimits<idx_t>::Maximum();
		}
		bulk.target.catalog_name = target.catalog_name;
		bulk.target.schema_name = target.schema_name;
		bulk.target.table_name = target.table_name;
		bulk.target.DetectTempTable();
		// COLMETADATA from the catalog's own column metadata (W3): the seven
		// fields sys.columns gives, already loaded -- no round trip, and the
		// same mapping COPY's query goes through.
		for (auto col_idx : target.insert_column_indices) {
			const auto &col = mssql_columns[col_idx];
			bulk.columns.push_back(mssql::BCPColumnMetadata::FromServerColumn(col.name, col.sql_type_name,
																			  col.max_length, col.precision, col.scale,
																			  col.is_nullable, col.collation_name));
			// The 2.0 insert child is full-width in table order, so the chunk
			// column of a table column IS its ordinal.
			bulk.column_mapping.push_back(static_cast<int32_t>(col_idx));
		}
		// The mssql_copy_* settings describe the load, not the statement that
		// started it (spec 062 F7): batch size, TABLOCK policy, writer count.
		const auto copy_config = mssql::LoadBCPCopyConfig(context);
		bulk.flush_rows = copy_config.flush_rows;
		bulk.shape = table_entry.GetIndexKind();
		bulk.tablock = MSSQLResolveTablock(copy_config.tablock_choice, bulk.shape);
		// An INSERT checks constraints, fires triggers and keeps its NULLs; a
		// bulk load does none of that unless told (InsertBulkHints).
		bulk.insert_bulk_sql = mssql::BuildInsertBulkSql(bulk.target, bulk.columns, bulk.tablock, bulk.flush_rows,
														 mssql::InsertBulkHints::StatementSemantics());
		Value pw;
		if (context.TryGetCurrentSetting("mssql_copy_parallel_writers", pw)) {
			bulk.configured_writers = pw.GetValue<int64_t>();
		}
	}

	// Determine result types
	vector<LogicalType> result_types;
	if (op.return_chunk) {
		// RETURNING mode - return the inserted columns
		for (auto &col_idx : target.returning_column_indices) {
			result_types.push_back(target.columns[col_idx].duckdb_type);
		}
	} else {
		// Count mode - return BIGINT count
		result_types.push_back(LogicalType::BIGINT);
	}

	// Create the physical operator using planner.Make<T>()
	auto &physical_insert =
		planner.Make<MSSQLPhysicalInsert>(std::move(result_types), op.estimated_cardinality, std::move(target),
										  std::move(config), op.return_chunk, std::move(bulk));

	// Add child operator if present
	if (plan) {
		physical_insert.children.push_back(*plan);
	}

	return physical_insert;
}

PhysicalOperator &MSSQLCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
												  LogicalCreateTable &op, PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("CREATE TABLE AS");

	// Delegate to CTAS planner
	return mssql::CTASPlanner::Plan(context, planner, *this, op, plan);
}

PhysicalOperator &MSSQLCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
											  LogicalMergeInto &op, PhysicalOperator &plan) {
	// Spec 080 D3: a table with no usable key is keyed by every column for an
	// UPDATE / DELETE, but DuckDB's MERGE tells matched from not matched by the
	// first row-id column -- here the value of the table's first column, NULL in
	// a matched row whenever that column is -- so a WHEN NOT MATCHED INSERT or
	// an ERROR action would misfire. MERGE keeps its key requirement.
	auto &merge_target = op.table.Cast<MSSQLTableEntry>();
	if (merge_target.UsesKeylessKey()) {
		const auto schema_name = merge_target.schema.name.GetIdentifierName();
		const auto table_name = merge_target.name.GetIdentifierName();
		throw NotImplementedException(
			"%s MERGE INTO a table without one is not supported: its rows are selected by MERGE's join on the client.",
			merge_target.GetPrimaryKeyInfo(context).RowIdRefusal(schema_name, table_name, "MERGE",
																 GetName().GetIdentifierName()));
	}

	// Which columns each INSERT action did not name, in DuckDB's planning order
	// (the actions map, then each vector -- Catalog::PlanMergeInto's loop). An
	// unnamed column's expression is a copy of its bound default; a named one's
	// refers to the action's projection, never a copy of the default.
	vector<vector<bool>> unnamed_sets;
	for (auto &entry : op.actions) {
		for (auto &action : entry.second) {
			if (action->action_type != MergeActionType::MERGE_INSERT) {
				continue;
			}
			vector<bool> unnamed(op.bound_defaults.size(), false);
			if (!action->column_index_map.empty()) {
				// The deprecated form (a deserialized plan): the map says which
				// physical column each expression fills.
				for (auto &col : op.table.GetColumns().Physical()) {
					const auto storage_idx = col.StorageOid();
					if (storage_idx < unnamed.size()) {
						unnamed[storage_idx] = action->column_index_map[col.Physical()] == DConstants::INVALID_INDEX;
					}
				}
				unnamed_sets.push_back(std::move(unnamed));
				continue;
			}
			idx_t expr_idx = 0;
			for (auto &col : op.table.GetColumns().Physical()) {
				const auto storage_idx = col.StorageOid();
				if (expr_idx < action->expressions.size() && storage_idx < op.bound_defaults.size() &&
					op.bound_defaults[storage_idx]) {
					auto &expr = *action->expressions[expr_idx];
					unnamed[storage_idx] = expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF &&
										   expr.Equals(*op.bound_defaults[storage_idx]);
				}
				expr_idx++;
			}
			unnamed_sets.push_back(std::move(unnamed));
		}
	}
	struct MergePlanningScope {
		bool previous;
		vector<vector<bool>> previous_sets;
		idx_t previous_next;
		MergePlanningScope(vector<vector<bool>> sets)
			: previous(planning_merge_actions),
			  previous_sets(std::move(merge_insert_unnamed)),
			  previous_next(merge_insert_next) {
			planning_merge_actions = true;
			merge_insert_unnamed = std::move(sets);
			merge_insert_next = 0;
		}
		~MergePlanningScope() {
			planning_merge_actions = previous;
			merge_insert_unnamed = std::move(previous_sets);
			merge_insert_next = previous_next;
		}
	} scope(std::move(unnamed_sets));
	return Catalog::PlanMergeInto(context, planner, op, plan);
}

// Spec 080 D3, rung 3: a keyless table's selected rows are matched on the
// server by value, every column, so DuckDB's selection and the server's must
// be the same set. That holds when the plan under the DML is projections over
// this table's own catalog scan, every filter of that scan runs on the server,
// and nothing volatile is computed: the scan's rows are then exactly the rows
// the server's WHERE selects, and a row equal in every column to a selected
// one is selected too. Returns why not, or empty.
static string KeylessPlanRefusal(ClientContext &context, PhysicalOperator &plan, MSSQLTableEntry &table) {
	reference<PhysicalOperator> op = plan;
	while (op.get().type == PhysicalOperatorType::PROJECTION) {
		auto &projection = op.get().Cast<PhysicalProjection>();
		for (auto &expr : projection.select_list) {
			if (expr->IsVolatile()) {
				return "the statement computes a volatile function (random(), gen_random_uuid(), ...), so equal rows "
					   "would not get equal values";
			}
		}
		op = op.get().children[0];
	}
	if (op.get().type == PhysicalOperatorType::EMPTY_RESULT) {
		// The optimizer proved the WHERE selects nothing (`WHERE 1 = 0`).
		return "";
	}
	if (op.get().type != PhysicalOperatorType::TABLE_SCAN) {
		return "its rows are selected on the client (a join, USING / FROM, a subquery, or a condition the scan does "
			   "not send to the server)";
	}
	auto &scan = op.get().Cast<PhysicalTableScan>();
	if (scan.function.GetName().GetIdentifierName() != "mssql_catalog_scan" || !scan.bind_data) {
		return "its rows are not read by the table's own scan";
	}
	auto &bind_data = scan.bind_data->Cast<MSSQLCatalogScanBindData>();
	if (bind_data.table_entry.get() != &table) {
		return "its rows are read from another table";
	}
	if (scan.table_filters && scan.table_filters->HasFilters()) {
		vector<column_t> column_ids;
		for (auto &column : scan.column_ids) {
			column_ids.push_back(mssql::ResolveKeylessKeyColumn(column.GetPrimaryIndex()));
		}
		// The scan's own encode, dry: a filter it would refuse runs in its
		// client-side net (table_scan.cpp), not on the server.
		mssql::SqlParamSet params;
		auto encoded = mssql::FilterEncoder::Encode(scan.table_filters.get(), column_ids, bind_data.all_column_names,
													bind_data.all_types, &bind_data.mssql_columns,
													LoadScanParameterizeFilters(context) ? &params : nullptr,
													LoadErrorOnDivisionByZero(context));
		if (encoded.needs_duckdb_filter || !encoded.unhandled.empty()) {
			return "part of its WHERE runs on the client, where it can compare differently from the server";
		}
	}
	return "";
}

//! Review of #423: a DML's batches go down the connection its feeding scans
//! read from when that connection is shared -- the transaction's pinned one, or
//! a pool of one -- so MSSQLOptimizer materialises those scans
//! (MaterializeSharedConnectionScans, the same test). DuckDB skips the
//! extension's optimizer under `SET enable_optimizer = false`; such a scan
//! still streams when the first batch would go, and RequireIdle refuses it.
//! Then the statement holds its rows until Finalize, after the scan drained,
//! as every DML did before spec 080 PR 1.
static bool FeedsFromStreamingScan(ClientContext &context, MSSQLCatalog &catalog, PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &scan = op.Cast<PhysicalTableScan>();
		if (scan.function.GetName().GetIdentifierName() == "mssql_catalog_scan" && scan.bind_data) {
			auto &bind_data = scan.bind_data->Cast<MSSQLCatalogScanBindData>();
			if (StringUtil::CIEquals(bind_data.context_name, catalog.GetContextName()) &&
				!bind_data.requires_materialization) {
				return true;
			}
		}
	}
	for (auto &child : op.children) {
		if (FeedsFromStreamingScan(context, catalog, child.get())) {
			return true;
		}
	}
	return false;
}

static bool ConnectionShared(ClientContext &context, MSSQLCatalog &catalog) {
	return !context.transaction.IsAutoCommit() || catalog.GetConnectionLimit() <= 1;
}

//! Whether INSERT BULK can carry every column into the stage. A geometry /
//! geography, alias or CLR type goes out under the VARCHAR fallback and fails
//! mid-stream (BCPColumnMetadata::bulk_unsupported); such a statement keeps
//! the VALUES path, which renders those values as literals (#352).
static bool StageCanCarry(const vector<MSSQLColumnInfo> &columns) {
	for (const auto &col : columns) {
		const auto t = StringUtil::Lower(col.sql_type_name);
		if (t == "timestamp" || t == "rowversion") {
			continue;  // staged as binary(8)
		}
		auto meta = mssql::BCPColumnMetadata::FromServerColumn(
			col.name, col.sql_type_name, col.max_length, col.precision, col.scale, col.is_nullable, col.collation_name);
		if (meta.bulk_unsupported) {
			return false;
		}
	}
	return true;
}

//! Spec 080 D3, rungs 1-2: the #stage form a keyed UPDATE / DELETE switches to
//! past mssql_dml_stage_threshold rows (on a platform without the VALUES join,
//! Fabric, the only form), or false where the platform has no stage (Synapse)
//! or a key column cannot be bulk-loaded.
static bool KeyedStagedTarget(ClientContext &context, MSSQLCatalog &catalog, MSSQLTableEntry &table_entry,
							  const mssql::RowIdKeyInfo &pk_info, MSSQLStagedDmlTarget &target) {
	const auto caps = catalog.GetDmlCapabilities();
	if (!caps.stage_bulk) {
		return false;
	}
	target.join_by_subquery = !caps.update_from_join;
	const auto &columns = table_entry.GetMSSQLColumns();
	for (const auto &key : pk_info.columns) {
		bool found = false;
		for (const auto &col : columns) {
			if (col.name == key.name) {
				target.key_columns.push_back(col);
				found = true;
				break;
			}
		}
		if (!found) {
			throw InternalException("MSSQL: rowid key column '%s' is not a column of '%s'", key.name,
									table_entry.name.GetIdentifierName());
		}
	}
	target.key_source = MSSQLStagedKeySource::ROWID;
	if (!StageCanCarry(target.key_columns)) {
		return false;
	}
	target.catalog_name = catalog.GetName().GetIdentifierName();
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
	target.null_safe_operator = caps.null_safe_operator && !LoadTestForceIntersectJoinForm(context);
	target.flush_rows = mssql::LoadBCPCopyConfig(context).flush_rows;
	target.query_timeout_seconds = LoadQueryTimeout(context);
	target.table_entry = &table_entry;
	return true;
}

//! The staged operator is the only way this statement can run (RETURNING, or
//! a platform without the VALUES join) and it cannot: refuse by name. A
//! native MERGE's actions are refused there too -- they defer to Finalize on
//! the VALUES path (spec 080 PR 4 moves MERGE to the server).
static void RefuseUnstageable(const char *verb, MSSQLTableEntry &table_entry, bool can_stage, bool returning) {
	const auto schema = table_entry.schema.name.GetIdentifierName();
	const auto table = table_entry.name.GetIdentifierName();
	if (planning_merge_actions) {
		throw NotImplementedException(
			"MSSQL: MERGE INTO '%s.%s' with an UPDATE or DELETE action is not supported on "
			"Fabric Warehouse yet (spec 080 PR 4)",
			schema, table);
	}
	if (!can_stage) {
		throw NotImplementedException(
			"MSSQL: %s%s on '%s.%s' is not supported: it runs through a #stage table, "
			"and a key or SET column cannot be bulk-loaded into one",
			verb, returning ? " ... RETURNING" : "", schema, table);
	}
}

//! Spec 080 PR 2b: what RETURNING needs on the staged operator. `types` are
//! the logical operator's (LogicalUpdate / LogicalDelete::ResolveTypes): the
//! table's columns, then for a DELETE its virtual columns in
//! GetVirtualColumns()' order -- asked here the same way, so the order agrees.
static void PrepareReturning(ClientContext &context, MSSQLTableEntry &table_entry, MSSQLStagedDmlTarget &target,
							 const vector<LogicalType> &types) {
	target.returning = true;
	target.returning_types = types;
	target.table_columns = table_entry.GetMSSQLColumns();
	for (auto &col : table_entry.GetColumns().Physical()) {
		target.table_types.push_back(col.Type());
	}
	target.convert_varchar_max = LoadConvertVarcharMax(context);
	const idx_t ncols = target.table_types.size();
	if (ncols != target.table_columns.size()) {
		throw InternalException("MSSQL RETURNING: %llu DuckDB columns, %llu SQL Server columns",
								(unsigned long long)ncols, (unsigned long long)target.table_columns.size());
	}
	if (target.kind == MSSQLStagedDmlKind::DELETE_ROWS) {
		for (auto &entry : table_entry.GetVirtualColumns()) {
			if (entry.first == COLUMN_IDENTIFIER_ROW_ID) {
				target.virtual_sources.push_back(-1);
			} else if (mssql::IsKeylessKeyColumn(entry.first)) {
				target.virtual_sources.push_back(static_cast<int64_t>(mssql::ResolveKeylessKeyColumn(entry.first)));
			} else {
				throw InternalException("MSSQL DELETE ... RETURNING: unexpected virtual column %llu",
										(unsigned long long)entry.first);
			}
		}
		const auto *pk_info = table_entry.LoadedPrimaryKeyInfo();
		if (pk_info && pk_info->exists) {
			target.rowid_type = pk_info->rowid_type;
			for (auto &key : pk_info->columns) {
				for (idx_t i = 0; i < target.table_columns.size(); i++) {
					if (target.table_columns[i].name == key.name) {
						target.key_table_index.push_back(i);
					}
				}
			}
		}
	}
	if (types.size() != ncols + target.virtual_sources.size()) {
		throw InternalException("MSSQL RETURNING: %llu columns expected, the table gives %llu",
								(unsigned long long)types.size(),
								(unsigned long long)(ncols + target.virtual_sources.size()));
	}
}

//! RETURNING goes through the stage, so it needs one (spec 080 PR 2b).
static void RequireReturningStage(MSSQLCatalog &catalog, const char *verb, MSSQLTableEntry &table_entry) {
	const auto caps = catalog.GetDmlCapabilities();
	if (!caps.update_from_join || !caps.stage_bulk || !caps.output_into_table) {
		throw NotImplementedException(
			"MSSQL: %s ... RETURNING on '%s.%s' is not supported on %s yet: it runs through a #stage table and "
			"OUTPUT ... INTO, which this platform does not take",
			verb, table_entry.schema.name.GetIdentifierName(), table_entry.name.GetIdentifierName(),
			caps.platform == mssql::DmlPlatform::Fabric ? "Fabric Warehouse" : "this platform");
	}
}

//! A DELETE's row-id expressions say where its key sits in the chunk -- not
//! always last: `WHERE rowid = …` binds the rowid first, and with RETURNING
//! every other column follows it (review of 2b). As DuckCatalog::PlanDelete.
static vector<idx_t> RowIdChunkIndexes(const LogicalDelete &op) {
	vector<idx_t> indexes;
	for (auto &expr : op.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			throw InternalException("MSSQL DELETE: a row-id expression is not a column reference");
		}
		indexes.push_back(expr->Cast<BoundReferenceExpression>().Index());
	}
	return indexes;
}

//! Rung 3: refuse by name, or the staged operator.
static PhysicalOperator &PlanKeylessDml(ClientContext &context, PhysicalPlanGenerator &planner, MSSQLCatalog &catalog,
										MSSQLTableEntry &table_entry, MSSQLStagedDmlTarget target,
										PhysicalOperator &plan, idx_t estimated_cardinality,
										const vector<LogicalType> *returning_types) {
	const char *verb = target.kind == MSSQLStagedDmlKind::UPDATE_ROWS ? "UPDATE" : "DELETE";
	const auto schema_name = table_entry.schema.name.GetIdentifierName();
	const auto table_name = table_entry.name.GetIdentifierName();
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	auto refuse = [&](const string &why) -> PhysicalOperator & {
		throw NotImplementedException(
			"%s It is keyed by all its columns instead, which needs the server to select the same rows as DuckDB, "
			"and here %s. Add a primary key or a unique index to %s.%s.",
			pk_info.RowIdRefusal(schema_name, table_name, verb, catalog.GetName().GetIdentifierName()), why,
			schema_name, table_name);
	};
	if (!table_entry.UsesKeylessKey()) {
		throw NotImplementedException(
			"%s %s", pk_info.RowIdRefusal(schema_name, table_name, verb, catalog.GetName().GetIdentifierName()),
			table_entry.KeylessKeyRefusal());
	}
	// Every column is a key column here, already through IsRoundTripExactForKey;
	// the bulk wire is asked as well, so the two lists cannot drift apart
	// (review of #425).
	if (!StageCanCarry(table_entry.GetMSSQLColumns())) {
		return refuse("a column cannot be bulk-loaded into the stage");
	}
	const auto why = KeylessPlanRefusal(context, plan, table_entry);
	if (!why.empty()) {
		return refuse(why);
	}
	target.catalog_name = catalog.GetName().GetIdentifierName();
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_name;
	target.key_columns = table_entry.GetMSSQLColumns();
	target.null_safe_operator =
		catalog.GetDmlCapabilities().null_safe_operator && !LoadTestForceIntersectJoinForm(context);
	target.join_by_subquery = !catalog.GetDmlCapabilities().update_from_join;
	target.flush_rows = mssql::LoadBCPCopyConfig(context).flush_rows;
	target.query_timeout_seconds = LoadQueryTimeout(context);
	target.table_entry = &table_entry;
	target.hold_until_finalize = ConnectionShared(context, catalog) && FeedsFromStreamingScan(context, catalog, plan);
	vector<LogicalType> result_types{LogicalType::BIGINT};
	if (returning_types) {
		PrepareReturning(context, table_entry, target, *returning_types);
		result_types = *returning_types;
	}
	auto &op = planner.Make<MSSQLPhysicalStagedDml>(std::move(result_types), estimated_cardinality, std::move(target));
	op.children.push_back(plan);
	return op;
}

// Spec 080 (review of #422): Synapse dedicated accepts PRIMARY KEY / UNIQUE
// only as NOT ENFORCED, so a key there can match several rows and an UPDATE /
// DELETE through it can hit rows the statement did not select. The host test
// cannot tell dedicated from serverless, and serverless refuses DML itself, so
// every UPDATE / DELETE through the catalog is refused on the whole domain:
// a behaviour change on a platform without a test environment, chosen over a
// wrong-rows hazard. mssql_exec() still sends what the user writes.
static void RefuseKeyedDmlOnSynapse(const MSSQLCatalog &catalog, const char *verb, MSSQLTableEntry &table_entry) {
	if (!catalog.GetDmlCapabilities().IsSynapse()) {
		return;
	}
	throw NotImplementedException(
		"MSSQL: %s on '%s.%s' is not supported on Azure Synapse: its PRIMARY KEY and UNIQUE constraints are NOT "
		"ENFORCED, so a key can match rows the statement did not select. Use mssql_exec() to run the statement on "
		"the server",
		verb, table_entry.schema.name.GetIdentifierName(), table_entry.name.GetIdentifierName());
}

PhysicalOperator &MSSQLCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("DELETE");
	EnsureServerProperties(context, "DELETE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();
	RefuseKeyedDmlOnSynapse(*this, "DELETE", table_entry);

	// RETURNING returns rows, not the count: the staged operator, whatever the
	// row count, with OUTPUT ... INTO #out (spec 080 PR 2b). Before 2b the
	// count operator was planned under DuckDB's RETURNING projection, which
	// read the BIGINT as the returned columns -- an InternalException.
	if (op.return_chunk) {
		RequireReturningStage(*this, "DELETE", table_entry);
	}

	// Rung 1-2: the rowid key (primary key or a usable unique index); rung 3,
	// a table with none: keyed by every column through #stage (spec 080 D3).
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		MSSQLStagedDmlTarget staged;
		staged.kind = MSSQLStagedDmlKind::DELETE_ROWS;
		staged.key_chunk_index = RowIdChunkIndexes(op);
		return PlanKeylessDml(context, planner, *this, table_entry, std::move(staged), plan, op.estimated_cardinality,
							  op.return_chunk ? &op.types : nullptr);
	}

	// Build MSSQLDeleteTarget from table metadata
	MSSQLDeleteTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
	target.pk_info = pk_info;

	// Load DML configuration from settings
	MSSQLDMLConfig config = LoadDMLConfig(context);
	// Always for a MERGE action, not only where the actions share a connection.
	// In autocommit on a larger pool each action has its own connection and
	// its own server transaction, committed at its Finalize; streaming, an
	// UPDATE action would hold its locks while an INSERT action's bulk load (BU
	// lock under TABLOCK on a heap) waits on them, the INSERT pipeline would
	// stall, DuckDB's exchange would block the MERGE, and the UPDATE action
	// would never reach the Finalize that commits -- a cycle through the
	// client that SQL Server's deadlock detector cannot see. Deferred, every
	// action writes after every sink has finished. The cost is the rows held
	// in memory; spec 080 PR 4 gives the actions one connection instead.
	config.defer_to_finalize =
		planning_merge_actions || (ConnectionShared(context, *this) && FeedsFromStreamingScan(context, *this, plan));

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	MSSQLStagedDmlTarget staged;
	staged.kind = MSSQLStagedDmlKind::DELETE_ROWS;
	staged.key_chunk_index = RowIdChunkIndexes(op);
	const bool can_stage = KeyedStagedTarget(context, *this, table_entry, pk_info, staged);
	// RETURNING, and a platform with no VALUES join (Fabric, D0): the staged
	// operator, whatever the row count.
	const bool always_stage = op.return_chunk || !GetDmlCapabilities().update_from_join;
	if (always_stage) {
		RefuseUnstageable("DELETE", table_entry, can_stage, op.return_chunk);
		staged.hold_until_finalize = ConnectionShared(context, *this) && FeedsFromStreamingScan(context, *this, plan);
		vector<LogicalType> types{LogicalType::BIGINT};
		if (op.return_chunk) {
			PrepareReturning(context, table_entry, staged, op.types);
			types = op.types;
		}
		auto &staged_op = planner.Make<MSSQLPhysicalStagedDml>(types, op.estimated_cardinality, std::move(staged));
		staged_op.children.push_back(plan);
		return staged_op;
	}
	auto &physical_delete =
		planner.Make<MSSQLPhysicalDelete>(std::move(result_types), op.estimated_cardinality, std::move(target), config);
	physical_delete.Cast<MSSQLPhysicalDelete>().SetTableEntry(table_entry);
	if (can_stage) {
		physical_delete.Cast<MSSQLPhysicalDelete>().SetStagedTarget(std::move(staged));
	}

	// Add child operator (provides rowid values)
	physical_delete.children.push_back(plan);

	return physical_delete;
}

PhysicalOperator &MSSQLCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("UPDATE");
	EnsureServerProperties(context, "UPDATE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();
	RefuseKeyedDmlOnSynapse(*this, "UPDATE", table_entry);

	// RETURNING: the staged operator with OUTPUT ... INTO #out (spec 080 PR
	// 2b; see PlanDelete). The OLD image DuckDB captures for its own triggers
	// is not produced.
	if (op.return_chunk) {
		RequireReturningStage(*this, "UPDATE", table_entry);
		if (op.capture_old_rows) {
			throw NotImplementedException(
				"MSSQL: UPDATE on '%s.%s' with a trigger reading the old rows is not "
				"supported",
				table_entry.schema.name.GetIdentifierName(), table_entry.name.GetIdentifierName());
		}
	}

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

	// Rung 1-2: the rowid key (loaded with its metadata, spec 084 D5); rung 3,
	// a table with none: keyed by every column through #stage (spec 080 D3).
	// The stage carries the old values (the match) and the new ones, so a SET
	// of any column is allowed there.
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		MSSQLStagedDmlTarget staged;
		staged.kind = MSSQLStagedDmlKind::UPDATE_ROWS;
		for (idx_t i = 0; i < op.columns.size(); i++) {
			const auto physical_idx = op.columns[i].index;
			if (physical_idx >= mssql_columns.size()) {
				throw InternalException("MSSQL UPDATE: SET column %llu out of range", (unsigned long long)physical_idx);
			}
			staged.set_columns.push_back(mssql_columns[physical_idx]);
			staged.set_chunk_index.push_back(i);
		}
		return PlanKeylessDml(context, planner, *this, table_entry, std::move(staged), plan, op.estimated_cardinality,
							  op.return_chunk ? &op.types : nullptr);
	}

	// Check if any PK column is being updated (reject if so)
	for (auto &pk_col : pk_info.columns) {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			auto physical_idx = op.columns[i].index;
			if (physical_idx < mssql_columns.size() && mssql_columns[physical_idx].name == pk_col.name) {
				throw NotImplementedException(
					"MSSQL: Updating rowid key columns is not supported. Cannot update column '%s'.", pk_col.name);
			}
		}
	}

	// Build MSSQLUpdateTarget from table metadata
	MSSQLUpdateTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
	target.pk_info = pk_info;
	target.table_columns = mssql_columns;

	// Build update column metadata
	// The columns in op.columns are the physical indices of columns being updated
	// The values come after the rowid in the input chunk
	for (idx_t i = 0; i < op.columns.size(); i++) {
		auto physical_idx = op.columns[i].index;
		if (physical_idx >= mssql_columns.size()) {
			throw InternalException("UPDATE column index %llu out of bounds (table has %llu columns)",
									(unsigned long long)physical_idx, (unsigned long long)mssql_columns.size());
		}

		auto &col = mssql_columns[physical_idx];
		MSSQLUpdateColumn update_col;
		update_col.name = col.name;
		update_col.column_index = physical_idx;
		update_col.duckdb_type = col.duckdb_type;
		update_col.mssql_type = col.sql_type_name;
		update_col.collation = col.collation_name;
		update_col.precision = col.precision;
		update_col.scale = col.scale;
		update_col.is_nullable = col.is_nullable;
		// chunk_index: update expressions are at columns 0 to N-1, rowid is at column N (last)
		// See DuckDB bind_update.cpp: BindRowIdColumns appends rowid AFTER update expressions
		update_col.chunk_index = i;

		target.update_columns.push_back(std::move(update_col));
	}

	// Load DML configuration from settings
	MSSQLDMLConfig config = LoadDMLConfig(context);
	// Always for a MERGE action, not only where the actions share a connection.
	// In autocommit on a larger pool each action has its own connection and
	// its own server transaction, committed at its Finalize; streaming, an
	// UPDATE action would hold its locks while an INSERT action's bulk load (BU
	// lock under TABLOCK on a heap) waits on them, the INSERT pipeline would
	// stall, DuckDB's exchange would block the MERGE, and the UPDATE action
	// would never reach the Finalize that commits -- a cycle through the
	// client that SQL Server's deadlock detector cannot see. Deferred, every
	// action writes after every sink has finished. The cost is the rows held
	// in memory; spec 080 PR 4 gives the actions one connection instead.
	config.defer_to_finalize =
		planning_merge_actions || (ConnectionShared(context, *this) && FeedsFromStreamingScan(context, *this, plan));

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	MSSQLStagedDmlTarget staged;
	staged.kind = MSSQLStagedDmlKind::UPDATE_ROWS;
	const bool can_stage = KeyedStagedTarget(context, *this, table_entry, pk_info, staged);
	bool stage_update = can_stage;
	if (stage_update) {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			staged.set_columns.push_back(mssql_columns[op.columns[i].index]);
			staged.set_chunk_index.push_back(i);
		}
		stage_update = StageCanCarry(staged.set_columns);
	}
	const bool always_stage = op.return_chunk || !GetDmlCapabilities().update_from_join;
	if (always_stage) {
		RefuseUnstageable("UPDATE", table_entry, stage_update, op.return_chunk);
		staged.hold_until_finalize = ConnectionShared(context, *this) && FeedsFromStreamingScan(context, *this, plan);
		vector<LogicalType> types{LogicalType::BIGINT};
		if (op.return_chunk) {
			PrepareReturning(context, table_entry, staged, op.types);
			types = op.types;
		}
		auto &staged_op = planner.Make<MSSQLPhysicalStagedDml>(types, op.estimated_cardinality, std::move(staged));
		staged_op.children.push_back(plan);
		return staged_op;
	}
	auto &physical_update =
		planner.Make<MSSQLPhysicalUpdate>(std::move(result_types), op.estimated_cardinality, std::move(target), config);
	if (stage_update) {
		physical_update.Cast<MSSQLPhysicalUpdate>().SetStagedTarget(std::move(staged));
	}

	// Add child operator (provides rowid + new values)
	physical_update.children.push_back(plan);

	return physical_update;
}

unique_ptr<LogicalOperator> MSSQLCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
														  TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE INDEX is not supported");
}

}  // namespace duckdb

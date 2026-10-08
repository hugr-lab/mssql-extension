#include "catalog/mssql_catalog.hpp"

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
#include "dml/update/mssql_physical_update.hpp"
#include "dml/update/mssql_update_target.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {

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
	if (op.column_index_map.empty()) {
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

PhysicalOperator &MSSQLCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("DELETE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Check if table has a primary key (required for DELETE via rowid)
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		throw NotImplementedException(pk_info.RowIdRefusal(table_entry.schema.name.GetIdentifierName(),
														   table_entry.name.GetIdentifierName(), "DELETE",
														   GetName().GetIdentifierName()));
	}

	// Build MSSQLDeleteTarget from table metadata
	MSSQLDeleteTarget target;
	target.catalog_name = context_name_;
	target.schema_name = table_entry.ParentSchema().name.GetIdentifierName();
	target.table_name = table_entry.name.GetIdentifierName();
	target.pk_info = pk_info;

	// Load DML configuration from settings
	MSSQLDMLConfig config = LoadDMLConfig(context);

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	auto &physical_delete =
		planner.Make<MSSQLPhysicalDelete>(std::move(result_types), op.estimated_cardinality, std::move(target), config);

	// Add child operator (provides rowid values)
	physical_delete.children.push_back(plan);

	return physical_delete;
}

PhysicalOperator &MSSQLCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
										   PhysicalOperator &plan) {
	// Check write access first (throws if read-only)
	CheckWriteAccess("UPDATE");

	// Get the target table entry
	auto &table_entry = op.table.Cast<MSSQLTableEntry>();

	// Check if table has a primary key (loaded with its metadata, spec 084 D5)
	const auto &pk_info = table_entry.GetPrimaryKeyInfo(context);
	if (!pk_info.exists) {
		throw NotImplementedException(pk_info.RowIdRefusal(table_entry.schema.name.GetIdentifierName(),
														   table_entry.name.GetIdentifierName(), "UPDATE",
														   GetName().GetIdentifierName()));
	}

	// Get MSSQL column info
	auto &mssql_columns = table_entry.GetMSSQLColumns();

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

	// Result type is BIGINT (row count)
	vector<LogicalType> result_types;
	result_types.push_back(LogicalType::BIGINT);

	// Create the physical operator using planner.Make<T>()
	auto &physical_update =
		planner.Make<MSSQLPhysicalUpdate>(std::move(result_types), op.estimated_cardinality, std::move(target), config);

	// Add child operator (provides rowid + new values)
	physical_update.children.push_back(plan);

	return physical_update;
}

unique_ptr<LogicalOperator> MSSQLCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
														  TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw NotImplementedException("MSSQL catalog is read-only: CREATE INDEX is not supported");
}

}  // namespace duckdb

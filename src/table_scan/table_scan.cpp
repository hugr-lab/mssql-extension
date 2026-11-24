// MSSQL Table Scan Implementation
// Feature: 013-table-scan-filter-refactor
// Feature: 001-pk-rowid-semantics (rowid support)

#include "table_scan/table_scan.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_keyless_key.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "codec/code_page/code_page_codec.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/common.hpp"	 // For COLUMN_IDENTIFIER_ROW_ID
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/table_column.hpp"  // For TableColumn, virtual_column_map_t
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_function_docs.hpp"
#include "mssql_functions.hpp"	// For backward compatibility with MSSQLCatalogScanBindData
#include "query/mssql_identifier.hpp"
#include "query/mssql_query_executor.hpp"
#include "query/mssql_result_stream.hpp"
#include "query/mssql_sql_params.hpp"
#include "table_scan/filter_encoder.hpp"
#include "table_scan/table_scan_bind.hpp"
#include "table_scan/table_scan_internal.hpp"
#include "table_scan/table_scan_state.hpp"

namespace duckdb {
namespace mssql {

// Forward declarations for internal functions
static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output);
static void PopulateRowIdVector(MSSQLScanGlobalState &state, DataChunk &output, idx_t row_count);
static void BindRowIdTargetVectors(MSSQLScanGlobalState &global_state, DataChunk &output);

// Execute the filters the encoder refused to push (see ClientTableFilter) over
// a filled output chunk. Each filter's expression sees its column as
// BoundReference(0), so it runs against a one-column view of the chunk.
// Returns the surviving row count; the chunk is sliced in place.
static idx_t ApplyClientFilters(ClientContext &context, MSSQLScanGlobalState &global_state, DataChunk &output,
								idx_t rows) {
	for (auto &cf : global_state.client_filters) {
		if (rows == 0) {
			break;
		}
		if (cf.out_col >= output.ColumnCount()) {
			throw InternalException("MSSQL scan: client filter column %llu out of range",
									(unsigned long long)cf.out_col);
		}
		DataChunk probe;
		probe.data.emplace_back(output.data[cf.out_col].GetType());
		probe.data[0].Reference(output.data[cf.out_col]);
		probe.SetCardinalityUnsafe(rows);
		SelectionVector sel(rows);
		const idx_t approved = cf.executor->SelectExpression(probe, sel);
		if (approved < rows) {
			output.Slice(sel, approved);
			rows = approved;
		}
	}
	return rows;
}

// The scan's SELECT list, built by the same function as the INSERT path's
// OUTPUT list — see MSSQLColumnInfo::BuildReadExpression. It used to live here
// with three of its four rewrites copied into the OUTPUT builder, and the
// missing one (the legacy LOBs) meant a `text` value failed an
// `INSERT … RETURNING` that never mentioned the column.
static std::string BuildColumnExpression(const MSSQLColumnInfo &col, const std::string &col_name,
										 bool convert_varchar_max, bool client_transcode, bool utf8_on_wire) {
	std::string expr =
		MSSQLColumnInfo::BuildReadExpression(col_name, col.sql_type_name, col.max_length, col.collation_name,
											 convert_varchar_max, client_transcode, "", "", utf8_on_wire);
	// Logged unconditionally at level 2 rather than only when it differs from the
	// bare name: deciding that meant rebuilding the escaped name for every column
	// on the scan path, and re-encoding a spelling BuildReadExpression now owns —
	// so a change there would have silenced the log instead of failing.
	MSSQL_SCAN_DEBUG_LOG(2, "  Column expression: %s (%s) -> %s", col_name.c_str(), col.sql_type_name.c_str(),
						 expr.c_str());
	return expr;
}

//------------------------------------------------------------------------------
// Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> TableScanBind(ClientContext &context, TableFunctionBindInput &input,
											  vector<LogicalType> &return_types, vector<Identifier> &names) {
	// A catalog scan's bind data comes from MSSQLTableEntry::GetScanFunction,
	// and a copied plan's from CatalogScanDeserialize. The function is in the
	// catalog only so a plan copy can find it (RegisterCatalogScanFunction);
	// called by name, there is no table to scan.
	throw BinderException(
		"mssql_catalog_scan is internal: read an attached table by name instead "
		"(SELECT * FROM db.schema.table)");
}

//------------------------------------------------------------------------------
// Init Functions
//------------------------------------------------------------------------------

// Issue #570: the catalog's cached metadata of a table that changed on the server since it was loaded --
// a column dropped or renamed (207, "Invalid column name"), the table dropped or renamed (208, "Invalid
// object name") by another client, a migration, or mssql_exec with mssql_exec_invalidate_cache off. The
// statement was written from the cached columns, so it cannot run; the table's entry is dropped as
// mssql_invalidate_cache(db, schema, table) drops it, and the error says so: run again, the statement
// binds against what the server has now. Nothing is retried here -- the columns are the bind's.
// A view naming a column (or a table) that is gone answers 207 / 208 and then 4413 ("binding errors"):
// the view itself is broken on the server, and dropping the entry would load the same columns again --
// the error says to fix the view there. (A `SELECT *` view that lost a column answers 4502 instead,
// sp_refreshview's case, and goes on as it came: the server's message says it.) In a transaction the error aborts it
// (DuckDB's rule for this exception), so it says to roll back first. Any other error, or one with no numbers, goes on
// as it came. Only this scan's statement: a statement sent whole (remote pushdown, mssql_dml) can read several tables,
// and the server's error does not say which.
static void RefuseStaleTable(ClientContext &context, const MSSQLCatalogScanBindData &bind_data,
							 const std::exception &ex, std::unique_lock<std::mutex> &materialize_lock) {
	ErrorData error(ex);
	auto found = error.ExtraInfo().find(MSSQL_SERVER_ERROR_NUMBERS);
	if (found == error.ExtraInfo().end()) {
		return;
	}
	const auto numbers = StringUtil::Split(found->second, ',');
	if (numbers.empty() || (numbers[0] != "207" && numbers[0] != "208")) {
		return;
	}
	const bool view = std::find(numbers.begin(), numbers.end(), "4413") != numbers.end();
	const string name = bind_data.context_name + "." + bind_data.schema_name + "." + bind_data.table_name;
	if (view) {
		throw InvalidInputException(
			error.ExtraInfo(),
			StringUtil::Format("%s (view %s: it does not compile on the server, a table it reads changed; "
							   "fix the view there, then mssql_invalidate_cache('%s', '%s', '%s'))",
							   error.RawMessage(), name, bind_data.context_name, bind_data.schema_name,
							   bind_data.table_name));
	}
	if (materialize_lock.owns_lock()) {
		materialize_lock.unlock();	// the invalidation takes the cache's locks
	}
	auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name)).Cast<MSSQLCatalog>();
	catalog.InvalidateTableEntry(bind_data.schema_name, bind_data.table_name);
	catalog.NoteTransactionChange(context, bind_data.schema_name, bind_data.table_name);
	const char *what = numbers[0] == "207" ? "its columns changed on the server since they were loaded"
										   : "it no longer exists on the server under that name";
	const char *then = context.transaction.IsAutoCommit() ? "run the statement again"
														  : "roll back the transaction and run the statement again";
	throw InvalidInputException(error.ExtraInfo(),
								StringUtil::Format("%s (table %s: %s; its cached metadata is dropped now, %s)",
												   error.RawMessage(), name, what, then));
}

// Virtual/special column identifiers start at 2^63
static constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

//! How a column is read: the settings and the catalog's answers BuildReadExpression takes.
struct ScanReadOptions {
	bool convert_varchar_max;
	bool client_transcode;
	bool utf8_on_wire;
};

//! What InitGlobal works out of the projection before anything is sent: the SELECT list and, for a
//! rowid, where the key columns are in the statement's result and in the output chunk.
struct ScanProjection {
	//! The projection (column_ids), a hidden key column resolved to its physical column (spec 080 D3,
	//! rung 3): from there on an ordinary (possibly repeated) projected column.
	vector<column_t> column_ids;
	//! Those of them that are table columns: special identifiers (COLUMN_IDENTIFIER_ROW_ID,
	//! COLUMN_IDENTIFIER_EMPTY for COUNT(*), ...) start at 2^63 and are skipped.
	vector<column_t> valid_column_ids;
	bool rowid_requested = false;
	idx_t rowid_output_idx = 0;
	string column_list;
	//! The columns of the SELECT list, in its order: what the stream's COLMETADATA is
	//! checked against (spec 092 D1).
	vector<const MSSQLColumnInfo *> read_columns;
	vector<string> sql_column_names;  // Column names in order of SQL SELECT
	vector<idx_t> pk_result_indices;  // Indices of PK columns in OUTPUT CHUNK (not SQL result)
	vector<idx_t> pk_sql_indices;	  // Indices of PK columns in SQL result
	bool pk_columns_added = false;	  // True if PK columns were added (not in user projection)
};

//! The projection's column ids and whether it asks for the rowid (spec 001-pk-rowid-semantics).
static void ResolveProjection(const TableFunctionInitInput &input, const MSSQLCatalogScanBindData &bind_data,
							  ScanProjection &projection) {
	auto &column_ids = projection.column_ids;
	column_ids.reserve(input.column_ids.size());
	for (const auto column_id : input.column_ids) {
		column_ids.push_back(ResolveKeylessKeyColumn(column_id));
	}
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: projection has %zu columns (table has %zu)", column_ids.size(),
						 bind_data.all_column_names.size());

	// Check for COLUMN_IDENTIFIER_ROW_ID in column_ids
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i] == COLUMN_IDENTIFIER_ROW_ID) {
			projection.rowid_requested = true;
			projection.rowid_output_idx = i;
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: rowid requested at output index %llu", (unsigned long long)i);
			break;
		}
	}

	// If rowid is requested, validate PK availability from bind_data
	if (projection.rowid_requested) {
		if (!bind_data.rowid_requested) {
			// This shouldn't happen - GetScanFunction should have set this
			throw BinderException("MSSQL: rowid requested but PK info not available in bind data");
		}
		if (bind_data.pk_column_names.empty()) {
			throw BinderException("MSSQL: rowid requires a table with a primary key or a usable unique index");
		}
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: PK has %zu columns, composite=%s",
							 bind_data.pk_column_names.size(), bind_data.pk_is_composite ? "true" : "false");
	}
}

static ScanReadOptions LoadReadOptions(ClientContext &context, const MSSQLCatalogScanBindData &bind_data) {
	ScanReadOptions options;
	// Load VARCHAR(MAX) conversion setting (Spec 026)
	options.convert_varchar_max = LoadConvertVarcharMax(context);
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: convert_varchar_max=%s",
						 options.convert_varchar_max ? "true" : "false");
	// Issue #405: a UTF-8 char(n) is read as itself only when the logins were
	// granted UTF8SUPPORT; otherwise -- or before any login answered -- as
	// NCHAR(n), so its padding never reaches DuckDB.
	auto &scan_catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name)).Cast<MSSQLCatalog>();
	options.utf8_on_wire = scan_catalog.UTF8SupportObserved() == MSSQLCatalog::Utf8Support::Granted;
	options.client_transcode = scan_catalog.ClientTranscode();
	return options;
}

//! One column appended to the SELECT list as its read expression: bare, or the server's cast (spec 026,
//! spec 092).
static void AppendReadColumn(const MSSQLColumnInfo &col_info, const string &col_name, const ScanReadOptions &options,
							 ScanProjection &projection) {
	if (!projection.column_list.empty()) {
		projection.column_list += ", ";
	}
	projection.column_list += BuildColumnExpression(col_info, col_name, options.convert_varchar_max,
													options.client_transcode, options.utf8_on_wire);
	projection.read_columns.push_back(&col_info);
}

//! The first position of `name` in `names`. It is always there -- a key column is a column of the table,
//! and of the SELECT list once added -- so one that is not is an error, where it used to read as
//! column 0: another column's read expression, or its values as the key.
static idx_t IndexOfName(const vector<string> &names, const string &name) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i] == name) {
			return i;
		}
	}
	throw InternalException("MSSQL scan: rowid key column \"%s\" is not among the table's columns", name);
}

//! The output chunk's position of table column `name`, UINT64_MAX when the projection has not got it.
static idx_t OutputIndexOfColumn(const vector<column_t> &column_ids, const MSSQLCatalogScanBindData &bind_data,
								 const string &name) {
	// column_ids maps output positions to table column indices
	for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
		if (column_ids[out_col] < VIRTUAL_COL_START && bind_data.all_column_names[column_ids[out_col]] == name) {
			return out_col;
		}
	}
	return UINT64_MAX;	// Sentinel for "not in output"
}

//! The SELECT list of a scan that reads the rowid: the projected columns, then the key columns the
//! projection lacks; and where each key column is, in the statement's result and in the output chunk.
static void BuildRowIdSelectList(const MSSQLCatalogScanBindData &bind_data, const ScanReadOptions &options,
								 ScanProjection &projection) {
	// Build a set of already-projected column names for deduplication
	std::unordered_set<string> projected_columns;
	for (const auto &col_idx : projection.valid_column_ids) {
		projected_columns.insert(bind_data.all_column_names[col_idx]);
	}

	// Start with valid projected columns
	for (const auto &col_idx : projection.valid_column_ids) {
		const string &col_name = bind_data.all_column_names[col_idx];
		AppendReadColumn(bind_data.mssql_columns[col_idx], col_name, options, projection);
		projection.sql_column_names.push_back(col_name);
	}

	// Add PK columns if not already projected
	for (idx_t pk_idx = 0; pk_idx < bind_data.pk_column_names.size(); pk_idx++) {
		const string &pk_col = bind_data.pk_column_names[pk_idx];
		if (projected_columns.find(pk_col) == projected_columns.end()) {
			// Add to SELECT - these are "extra" columns for rowid construction
			const auto &col_info = bind_data.mssql_columns[IndexOfName(bind_data.all_column_names, pk_col)];
			AppendReadColumn(col_info, pk_col, options, projection);
			projection.sql_column_names.push_back(pk_col);
			projection.pk_columns_added = true;
			MSSQL_SCAN_DEBUG_LOG(2, "  added PK column for rowid: %s at SQL index %zu", pk_col.c_str(),
								 projection.sql_column_names.size() - 1);
		}

		// Record the SQL result index of this PK column
		const idx_t sql_result_idx = IndexOfName(projection.sql_column_names, pk_col);
		projection.pk_sql_indices.push_back(sql_result_idx);

		// Find the output chunk index for this PK column (if it's in user projection)
		const idx_t output_idx = OutputIndexOfColumn(projection.column_ids, bind_data, pk_col);
		projection.pk_result_indices.push_back(output_idx);
		MSSQL_SCAN_DEBUG_LOG(2, "  PK column %s: sql_idx=%llu, output_idx=%s", pk_col.c_str(),
							 (unsigned long long)sql_result_idx,
							 output_idx == UINT64_MAX ? "N/A (extra)" : std::to_string(output_idx).c_str());
	}
}

//! The SELECT list (projection pushdown), with the key columns a rowid needs.
static void BuildSelectList(const MSSQLCatalogScanBindData &bind_data, const ScanReadOptions &options,
							ScanProjection &projection) {
	// Filter out special column identifiers and collect valid column indices
	for (const auto &col_idx : projection.column_ids) {
		if (col_idx < VIRTUAL_COL_START && col_idx < bind_data.all_column_names.size()) {
			projection.valid_column_ids.push_back(col_idx);
		} else {
			MSSQL_SCAN_DEBUG_LOG(2, "  skipping special/invalid column_id=%llu", (unsigned long long)col_idx);
		}
	}

	if (projection.rowid_requested) {
		BuildRowIdSelectList(bind_data, options, projection);
	} else if (projection.valid_column_ids.empty()) {
		// No valid columns projected (e.g., COUNT(*))
		// Select only the first column to minimize data transfer while still returning rows
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: no valid columns, selecting first column only for row counting");
		if (!bind_data.all_column_names.empty()) {
			projection.column_list = mssql::QuoteIdentifier(bind_data.all_column_names[0]);
		} else {
			// Fallback to constant if table has no columns (shouldn't happen)
			projection.column_list = "1";
		}
	} else {
		// Build SELECT with only valid projected columns (no rowid)
		for (const auto col_idx : projection.valid_column_ids) {
			AppendReadColumn(bind_data.mssql_columns[col_idx], bind_data.all_column_names[col_idx], options,
							 projection);
		}
	}
}

//! The rowid's state, kept for Execute.
static void StoreRowIdState(const MSSQLCatalogScanBindData &bind_data, const ScanProjection &projection,
							MSSQLScanGlobalState &result) {
	result.rowid_requested = projection.rowid_requested;
	result.rowid_output_idx = projection.rowid_output_idx;
	result.pk_result_indices = projection.pk_result_indices;
	result.pk_is_composite = bind_data.pk_is_composite;
	result.rowid_type = bind_data.rowid_type;
	result.pk_column_types = bind_data.pk_column_types;
	result.pk_columns_added = projection.pk_columns_added;
	result.pk_sql_indices = projection.pk_sql_indices;

	// Track special cases for rowid handling:
	// 1. pk_direct_to_rowid: rowid-only with scalar PK - write PK directly to rowid position
	// 2. composite_pk_direct_to_struct: rowid-only with composite PK - write to STRUCT children
	// 3. pk_columns_added: rowid + other columns, but PK not in user projection - write extra PK columns
	const bool rowid_only = projection.rowid_requested && projection.valid_column_ids.empty();
	result.pk_direct_to_rowid = rowid_only && !bind_data.pk_is_composite;
	result.composite_pk_direct_to_struct = rowid_only && bind_data.pk_is_composite;
	MSSQL_SCAN_DEBUG_LOG(
		1, "TableScanInitGlobal: pk_direct_to_rowid=%s, composite_pk_direct_to_struct=%s, pk_columns_added=%s",
		result.pk_direct_to_rowid ? "true" : "false", result.composite_pk_direct_to_struct ? "true" : "false",
		result.pk_columns_added ? "true" : "false");
}

//! A filter the encoder refused, armed to run over the output chunk. 2.0 does not re-check TableFilters
//! behind a filter_pushdown scan, so every refused filter is executed client-side. The filter's expression
//! references its column as BoundReference(0) -- the same convention the combiner used when it built the
//! filter.
static void ArmClientFilter(ClientContext &context, const MSSQLCatalogScanBindData &bind_data,
							const vector<column_t> &column_ids, idx_t out_col, const TableFilter &filter,
							MSSQLScanGlobalState &result) {
	LogicalType col_type;
	if (out_col < column_ids.size() && column_ids[out_col] == COLUMN_IDENTIFIER_ROW_ID) {
		col_type = bind_data.rowid_type;
	} else if (out_col < column_ids.size() && column_ids[out_col] < bind_data.all_types.size()) {
		col_type = bind_data.all_types[column_ids[out_col]];
	} else {
		// Same failure class as the catch below, same answer: a filter
		// we can neither push nor arm client-side must fail by name,
		// not return a superset of rows (PR #267 review, finding 5).
		throw NotImplementedException(
			"MSSQL scan: cannot apply pushed-down filter at projected column %llu client-side",
			(unsigned long long)out_col);
	}
	try {
		BoundReferenceExpression col_ref(col_type, 0ULL);
		mssql::ClientTableFilter cf;
		cf.out_col = out_col;
		cf.expr = filter.ToExpression(col_ref);
		cf.executor = make_uniq<ExpressionExecutor>(context);
		cf.executor->AddExpression(*cf.expr);
		result.client_filters.push_back(std::move(cf));
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: client-side filter armed on output col %llu",
							 (unsigned long long)out_col);
	} catch (const std::exception &e) {
		// A filter that cannot even build an expression cannot be
		// applied anywhere — fail loudly instead of returning a
		// superset of rows.
		throw NotImplementedException("MSSQL scan: cannot apply pushed-down filter client-side: %s", e.what());
	}
}

//! The WHERE clause, empty for none: the simple filters (FilterEncoder::Encode over the TableFilterSet of
//! filter_pushdown) and the complex ones (stored by pushdown_complex_filter), joined by AND. The filters the
//! encoder refuses are armed client-side; `params` takes the parameters the simple filters add.
static string BuildWhereClause(ClientContext &context, const TableFunctionInitInput &input,
							   const MSSQLCatalogScanBindData &bind_data, const vector<column_t> &column_ids,
							   mssql::SqlParamSet &params, MSSQLScanGlobalState &result) {
	std::vector<std::string> where_conditions;
	bool needs_duckdb_filter = false;
	const bool parameterize = LoadScanParameterizeFilters(context);

	// 1. Encode simple filters (TableFilterSet from filter_pushdown)
	if (input.filters && input.filters->HasFilters()) {
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: simple filter pushdown with %zu filter(s)",
							 static_cast<size_t>(input.filters->FilterCount()));

		auto encode_result = FilterEncoder::Encode(
			input.filters.get(), column_ids, bind_data.all_column_names, bind_data.all_types, &bind_data.mssql_columns,
			parameterize ? &params : nullptr, LoadErrorOnDivisionByZero(context));

		if (!encode_result.where_clause.empty()) {
			where_conditions.push_back(encode_result.where_clause);
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: simple filters encoded: %s",
								 encode_result.where_clause.c_str());
		}

		needs_duckdb_filter = encode_result.needs_duckdb_filter;
		for (auto &uf : encode_result.unhandled) {
			ArmClientFilter(context, bind_data, column_ids, uf.first, *uf.second, result);
		}
	}

	// 2. Add complex filters (from pushdown_complex_filter callback)
	if (!bind_data.complex_filter_where_clause.empty()) {
		where_conditions.push_back(bind_data.complex_filter_where_clause);
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: complex filters added: %s",
							 bind_data.complex_filter_where_clause.c_str());
	}

	// 3. Combine all conditions with AND
	std::string combined_where;
	for (idx_t i = 0; i < where_conditions.size(); i++) {
		if (i > 0) {
			combined_where += " AND ";
		}
		combined_where += where_conditions[i];
	}
	if (!combined_where.empty()) {
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: final WHERE clause: %s", combined_where.c_str());
	}
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: needs_duckdb_filter=%s", needs_duckdb_filter ? "true" : "false");
	return combined_where;
}

//! The statement: SELECT [TOP n] <list> FROM [schema].[table] [WHERE ...] [ORDER BY ...].
static string BuildScanQuery(const MSSQLCatalogScanBindData &bind_data, const string &column_list,
							 const string &where_clause) {
	string full_table_name =
		mssql::QuoteIdentifier(bind_data.schema_name) + "." + mssql::QuoteIdentifier(bind_data.table_name);

	// Build SELECT prefix (with optional TOP N from ORDER BY + LIMIT pushdown, Spec 039)
	string select_prefix = "SELECT ";
	if (bind_data.top_n > 0) {
		select_prefix += "TOP " + std::to_string(bind_data.top_n) + " ";
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: TOP %lld pushdown", (long long)bind_data.top_n);
	}
	string query = select_prefix + column_list + " FROM " + full_table_name;
	if (!where_clause.empty()) {
		query += " WHERE " + where_clause;
	}

	// Append ORDER BY clause from optimizer pushdown (Spec 039)
	if (!bind_data.order_by_clause.empty()) {
		query += " ORDER BY " + bind_data.order_by_clause;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: ORDER BY pushdown: %s", bind_data.order_by_clause.c_str());
	}
	return query;
}

// Issue #559: a keyless table's hidden key columns (spec 080 D3, rung 3) refuse a value read as
// NULL for not fitting, by name: keyed NULL-safely, the row would match another one. Here, in
// the scan, because such a plan has every filter on the server (`PlanKeylessDml`), so every row
// read is a row to write. A rowid's key is checked where the DML takes it instead (its columns
// are NOT NULL: `RefuseNullKeyValue`), after DuckDB's own filters.
static void RefuseOutOfRangeKeys(const TableFunctionInitInput &input, const MSSQLCatalogScanBindData &bind_data,
								 const vector<const MSSQLColumnInfo *> &read_columns, MSSQLResultStream &stream) {
	std::unordered_set<const MSSQLColumnInfo *> key_columns;
	for (const auto column_id : input.column_ids) {
		if (IsKeylessKeyColumn(column_id)) {
			key_columns.insert(&bind_data.mssql_columns[ResolveKeylessKeyColumn(column_id)]);
		}
	}
	if (key_columns.empty()) {
		return;
	}
	std::vector<bool> refuse(read_columns.size(), false);
	for (idx_t i = 0; i < read_columns.size(); i++) {
		refuse[i] = key_columns.count(read_columns[i]) > 0;
	}
	stream.RefuseOutOfRangeIn(std::move(refuse), mssql::QuoteIdentifier(bind_data.schema_name) + "." +
													 mssql::QuoteIdentifier(bind_data.table_name));
}

// A bare read assumed its collation's page. The stream decodes by the wire's,
// so a disagreement reads correctly, but the catalog is stale: say so (spec 092
// D1), or nobody learns why a column is labelled with the wrong collation.
static void CheckWireCodePages(ClientContext &context, const vector<const MSSQLColumnInfo *> &read_columns,
							   bool client_transcode, MSSQLResultStream &stream) {
	const auto &wire_columns = stream.GetColumnMetadata();
	for (idx_t i = 0; i < read_columns.size() && i < wire_columns.size(); i++) {
		const auto *page = MSSQLColumnInfo::ClientTranscodedPage(read_columns[i]->sql_type_name,
																 read_columns[i]->collation_name, client_transcode);
		if (!page) {
			continue;
		}
		const auto undecodable =
			mssql::codec::code_page::UndecodableBareRead(wire_columns[i], read_columns[i]->collation_name);
		if (!undecodable.empty()) {
			throw InvalidInputException(undecodable);
		}
		const auto mismatch =
			mssql::codec::code_page::NamedCodePageMismatch(wire_columns[i], *page, read_columns[i]->collation_name);
		if (!mismatch.empty()) {
			DUCKDB_LOG_WARNING(context, "%s", mismatch);
		}
	}
}

//! The output chunk's positions of the projected table columns, in the statement's order: the rowid's
//! position is skipped (it is populated from the key columns), and so is any other virtual identifier.
//! Example: SELECT id, rowid, name -> column_ids [0, ROWID, 1] -> output [id, rowid, name];
//! SQL returns [id, name] -> mapping [0, 2] (SQL col 0 -> output 0, SQL col 1 -> output 2).
static vector<idx_t> UserColumnOutputs(const vector<column_t> &column_ids) {
	vector<idx_t> output_mapping;
	for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
		if (column_ids[out_col] == COLUMN_IDENTIFIER_ROW_ID) {
			continue;
		}
		if (column_ids[out_col] < VIRTUAL_COL_START) {
			MSSQL_SCAN_DEBUG_LOG(2, "  SQL col %llu -> output col %llu (user col)",
								 (unsigned long long)output_mapping.size(), (unsigned long long)out_col);
			output_mapping.push_back(out_col);
		}
	}
	return output_mapping;
}

//! Which of the statement's columns the stream fills, and into which output columns, by the rowid's case.
static void ConfigureStreamOutput(const MSSQLCatalogScanBindData &bind_data, const ScanProjection &projection,
								  MSSQLScanGlobalState &result) {
	auto &stream = *result.result_stream;
	const auto &column_ids = projection.column_ids;
	const idx_t user_columns = projection.valid_column_ids.size();
	const idx_t rowid_output_idx = projection.rowid_output_idx;
	if (result.pk_direct_to_rowid) {
		// Special case: only rowid requested with scalar PK
		// SQL returns 1 column (PK), we write it directly to rowid output position
		stream.SetColumnsToFill(1);	 // Fill 1 column (the PK)
		vector<idx_t> output_mapping;
		output_mapping.push_back(rowid_output_idx);	 // SQL col 0 -> rowid position
		stream.SetOutputColumnMapping(std::move(output_mapping));
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: pk_direct_to_rowid mode - SQL col 0 -> output %llu",
							 (unsigned long long)rowid_output_idx);
	} else if (result.composite_pk_direct_to_struct) {
		// Special case: only rowid requested with composite PK
		// SQL returns N columns (PK columns), we write them directly to STRUCT children
		// The STRUCT is at rowid_output_idx in the output chunk
		idx_t pk_count = bind_data.pk_column_names.size();
		// Nothing for now: BindRowIdTargetVectors points the stream at the STRUCT's children and sets
		// the count, on the chunk the stream then fills.
		stream.SetColumnsToFill(0);
		// Store pk_result_indices as SQL column indices (0, 1, 2, ...)
		result.pk_result_indices.clear();
		for (idx_t i = 0; i < pk_count; i++) {
			result.pk_result_indices.push_back(i);
		}
		// Mark pk_columns_added as false - Execute uses this to distinguish from mixed case
		result.pk_columns_added = false;
		MSSQL_SCAN_DEBUG_LOG(
			1, "TableScanInitGlobal: composite_pk_direct_to_struct mode - %llu PK columns -> STRUCT at output %llu",
			(unsigned long long)pk_count, (unsigned long long)rowid_output_idx);
	} else if (projection.rowid_requested && projection.pk_columns_added && !bind_data.pk_is_composite) {
		// Special case: rowid + other columns, scalar PK NOT in user projection
		// SQL returns [user_cols..., pk_col]
		// Write user columns to their positions, write PK column directly to rowid position
		idx_t total_sql_cols = user_columns + 1;  // user cols + 1 PK col
		stream.SetColumnsToFill(total_sql_cols);

		// Build mapping: SQL column index -> output chunk index
		vector<idx_t> output_mapping = UserColumnOutputs(column_ids);
		// Map the extra PK column (last SQL column) to rowid position
		MSSQL_SCAN_DEBUG_LOG(2, "  SQL col %llu -> output col %llu (PK to rowid)",
							 (unsigned long long)output_mapping.size(), (unsigned long long)rowid_output_idx);
		output_mapping.push_back(rowid_output_idx);

		stream.SetOutputColumnMapping(std::move(output_mapping));
		// Mark that PK was written directly to rowid
		result.pk_direct_to_rowid = true;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: pk_columns_added scalar mode - %llu cols with PK -> rowid",
							 (unsigned long long)total_sql_cols);
	} else if (projection.rowid_requested && projection.pk_columns_added && bind_data.pk_is_composite) {
		// Special case: rowid + other columns, composite PK NOT in user projection
		// SQL returns [user_cols..., pk_cols...]
		// Write user columns normally, write PK columns to STRUCT children via target_vectors
		// For now, use columns_to_fill for user cols only, handle PK in Execute
		stream.SetColumnsToFill(user_columns);
		stream.SetOutputColumnMapping(UserColumnOutputs(column_ids));
		// Mark for special handling in Execute
		result.composite_pk_direct_to_struct = true;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: pk_columns_added composite mode - user cols + STRUCT handling");
	} else if (projection.rowid_requested) {
		// Rowid with other columns, PK IS in user projection
		// The output chunk has positions for all columns including rowid
		stream.SetColumnsToFill(user_columns);
		stream.SetOutputColumnMapping(UserColumnOutputs(column_ids));
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: rowid with PK in projection - %llu user cols",
							 (unsigned long long)user_columns);
	} else {
		// No rowid - simple case
		stream.SetColumnsToFill(user_columns);
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: set columns_to_fill=%zu", static_cast<size_t>(user_columns));
	}
}

//! The output shape, derived the same way DuckDB derives it for Execute:
//! one slot per column_id, the rowid slot carrying the bind data's rowid
//! type and any other virtual/empty identifier carrying a placeholder that
//! is never written (projected_column_count bounds what FillChunk fills).
static vector<LogicalType> OutputChunkTypes(const MSSQLCatalogScanBindData &bind_data,
											const vector<column_t> &column_ids) {
	vector<LogicalType> chunk_types;
	chunk_types.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i] == COLUMN_IDENTIFIER_ROW_ID) {
			chunk_types.push_back(bind_data.rowid_type);
		} else if (column_ids[i] >= VIRTUAL_COL_START || column_ids[i] >= bind_data.all_types.size()) {
			chunk_types.push_back(LogicalType::BIGINT);
		} else {
			chunk_types.push_back(bind_data.all_types[column_ids[i]]);
		}
	}
	return chunk_types;
}

// Issue #239: drain now, so the connection is Idle before the next scan on this
// catalog initializes.
//
// Only reachable when MSSQLOptimizer set the flag, which it does for a plan
// holding more than one scan of this catalog (or a scan and a sink into it)
// inside an explicit transaction — the case where all of them share the one
// pinned connection — and, since issue #380, in autocommit on a catalog whose
// pool holds a single connection, which they share by taking turns. Streaming
// is untouched everywhere else.
//
// The drain deliberately reuses the normal execution path rather than a second
// row reader: FillChunk applies the projection, the rowid construction and the
// warnings exactly as a streaming scan would, so a materialized scan and a
// streamed one cannot diverge in what they produce.
static void MaterializeScan(ClientContext &context, const MSSQLCatalogScanBindData &bind_data,
							const vector<column_t> &column_ids, MSSQLScanGlobalState &result) {
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: materializing (issue #239 — shared pinned connection)");
	const vector<LogicalType> chunk_types = OutputChunkTypes(bind_data, column_ids);
	// ClientContext, not Allocator::Get(context) (#318, found by @oluies). The
	// Allocator overload builds an IN_MEMORY_ALLOCATOR collection — unaccounted
	// against the buffer manager and unable to spill — so a large materialized
	// scan grew in process memory until it OOMed, while the docs promised the
	// opposite. The ClientContext overload defaults to BUFFER_MANAGER_ALLOCATOR.
	// The chunk below deliberately keeps the plain Allocator: it is one DataChunk,
	// not the accumulating collection.
	result.materialized = make_uniq<ColumnDataCollection>(context, chunk_types);
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), chunk_types);
	// What Execute's first call does for its own output chunk.
	BindRowIdTargetVectors(result, chunk);
	idx_t total = 0;
	for (;;) {
		chunk.Reset();
		idx_t rows = result.result_stream->FillChunk(chunk);
		if (rows == 0) {
			break;
		}
		PopulateRowIdVector(result, chunk, rows);
		// The filters the encoder did not push run here, as in Execute: the
		// collection is served as it stands, and DuckDB does not re-check a
		// filter it handed to the scan (issue #451 -- a materialized scan
		// returned rows such a filter rejects, so an UPDATE in a transaction
		// changed them).
		if (!result.client_filters.empty()) {
			rows = ApplyClientFilters(context, result, chunk, rows);
			if (rows == 0) {
				continue;
			}
		}
		result.materialized->Append(chunk);
		total += rows;
	}
	result.result_stream->SurfaceWarnings(context);
	// Closing the stream is what releases the connection back to Idle. Without
	// it the drain would have read every row and still deadlocked the next scan.
	result.result_stream.reset();
	result.materialized->InitializeScan(result.materialized_scan);
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: materialized %llu row(s), connection released",
						 (unsigned long long)total);
}

static unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: START");

	// Use the existing MSSQLCatalogScanBindData for backward compatibility
	auto &bind_data = input.bind_data->Cast<MSSQLCatalogScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	// Issue #451: the statement writes to this catalog and joins its own DuckDB
	// transaction there. Join before taking any connection -- this scan may be
	// initialised before the sink is (a join's build side, a MERGE's source),
	// and a scan that took a pool connection while the sink joined would read
	// on the wrong one. Whichever comes first pins; the rest find it pinned.
	if (bind_data.joins_statement) {
		ConnectionProvider::JoinStatementTransaction(context, bind_data.context_name);
	}

	// The statement: its SELECT list from the projection, its WHERE from the filters.
	ScanProjection projection;
	ResolveProjection(input, bind_data, projection);
	const ScanReadOptions options = LoadReadOptions(context, bind_data);
	BuildSelectList(bind_data, options, projection);
	StoreRowIdState(bind_data, projection, *result);

	// Spec 076: the plan-time parameters first (the complex clause names them
	// @p0..), then whatever the simple filters add.
	mssql::SqlParamSet params = bind_data.complex_filter_params;
	const string where_clause = BuildWhereClause(context, input, bind_data, projection.column_ids, params, *result);
	string query = BuildScanQuery(bind_data, projection.column_list, where_clause);

	// Spec 083: with parameters, the statement goes as an RPC call of
	// sp_executesql -- one fixed text per shape, the values bound as typed
	// parameters. The batch form (`DECLARE ...; EXEC sp_executesql ...`) made the
	// server parse and compile the outer batch on every scan.
	tds::Request request = params.params.empty() ? tds::Request(query) : params.ExecuteSqlRequest(query);
	query = request.sql;
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: generated query = %s", query.c_str());

	// Execute the query.
	//
	// Issue #239: a materialized scan holds the catalog's materialize mutex from
	// here until after its drain below. R2's drain alone is enough at threads = 1,
	// where DuckDB initializes the two sources in sequence; with threads > 1 the
	// InitGlobals race and the second batch lands mid-stream. The lock makes the
	// pair sequential, which they already are by construction — they share one
	// pinned connection.
	std::unique_lock<std::mutex> materialize_lock;
	if (bind_data.requires_materialization) {
		// Identifier at the DuckDB API boundary, std::string inside (CLAUDE.md).
		auto &catalog = Catalog::GetCatalog(context, Identifier(bind_data.context_name)).Cast<MSSQLCatalog>();
		materialize_lock = std::unique_lock<std::mutex>(catalog.MaterializeMutexFor(context));
	}
	MSSQLQueryExecutor executor(bind_data.context_name);
	try {
		result->result_stream = executor.Execute(context, request);
	} catch (const std::exception &ex) {
		RefuseStaleTable(context, bind_data, ex, materialize_lock);
		throw;
	}

	// Set the number of columns to actually fill in the output chunk
	// When valid_column_ids is empty (e.g., COUNT(*)), we don't fill any columns
	// EXCEPT when pk_direct_to_rowid is true - then we fill the PK directly to rowid position
	result->projected_column_count = projection.valid_column_ids.size();
	if (result->result_stream) {
		try {
			// COLMETADATA is known now, so say what is wrong with the columns before any
			// rows move. Waiting for the drain-end call loses this entirely on a query
			// that stops early -- any LIMIT (issue #224).
			result->result_stream->MarkCatalogScan();
			RefuseOutOfRangeKeys(input, bind_data, projection.read_columns, *result->result_stream);
			CheckWireCodePages(context, projection.read_columns, options.client_transcode, *result->result_stream);
			result->result_stream->SurfaceWarnings(context);
			ConfigureStreamOutput(bind_data, projection, *result);
			if (bind_data.requires_materialization) {
				MaterializeScan(context, bind_data, projection.column_ids, *result);
			}
		} catch (...) {
			// The stream is closed here (ATTENTION, then its drain), while the materialize lock is still
			// held: unwinding releases the lock first -- `result`, which owns the stream, is declared
			// before it -- and another scan on the pinned connection could start while this one still
			// answers (review of #560). That review closed it for a failure in the drain; a refusal
			// before it (a column the wire cannot decode, a warning the logger throws on) left by the
			// same door.
			result->result_stream.reset();
			throw;
		}
	}

	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> TableScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
															  GlobalTableFunctionState *global_state) {
	return make_uniq<TableScanLocalState>();
}

//------------------------------------------------------------------------------
// Execute Function
//------------------------------------------------------------------------------

// A projected key column copied into the rowid slot — a STRUCT child, or the
// scalar rowid itself. The two vectors share a physical type and can differ
// in LOGICAL type: the projected column is reported as MSSQL_VARCHAR(n) /
// MSSQL_NVARCHAR(n) under mssql_catalog_native_types (spec 060), while the
// rowid is typed from the column's plain duckdb_type. A typed Copy asserts on
// that in a debug build (issue #369; release builds copied the same bytes
// and returned the rows). The copy goes through a view of the source in the
// target's type — the same bytes, no cast — which is what the extension-type
// rule asks for: reinterpret, never a no-op cast.
static void CopyKeyColumn(Vector &src, Vector &dst, idx_t row_count) {
	if (src.GetType() == dst.GetType()) {
		VectorOperations::Copy(src, dst, row_count, 0, 0);
		return;
	}
	D_ASSERT(src.GetType().InternalType() == dst.GetType().InternalType());
	Vector view(dst.GetType(), nullptr);
	view.Reinterpret(src);
	VectorOperations::Copy(view, dst, row_count, 0, 0);
}

// Helper function to populate rowid vector from PK columns
static void PopulateRowIdVector(MSSQLScanGlobalState &state, DataChunk &output, idx_t row_count) {
	if (!state.rowid_requested || row_count == 0) {
		return;
	}

	// If pk_direct_to_rowid is true, data was written directly to rowid position
	// by the result stream - no copying needed
	if (state.pk_direct_to_rowid) {
		MSSQL_SCAN_DEBUG_LOG(2, "Execute: pk_direct_to_rowid mode - rowid already populated for %llu rows",
							 (unsigned long long)row_count);
		return;
	}

	// If composite_pk_direct_to_struct is true, some data was written directly to STRUCT children
	// But if pk_columns_added is also true, some PK columns may be in the user projection
	// and need to be copied from output positions to STRUCT children
	if (state.composite_pk_direct_to_struct) {
		auto &rowid_vector = output.data[state.rowid_output_idx];

		// If pk_columns_added, copy PK columns that are in the projection to STRUCT children
		if (state.pk_columns_added && state.pk_is_composite) {
			auto &entries = StructVector::GetEntries(rowid_vector);
			for (idx_t pk_idx = 0; pk_idx < state.pk_result_indices.size(); pk_idx++) {
				idx_t output_idx = state.pk_result_indices[pk_idx];
				if (output_idx != UINT64_MAX) {
					// This PK column is in the projection - copy to STRUCT child
					auto &src_vector = output.data[output_idx];
					auto &dst_vector = entries[pk_idx];
					CopyKeyColumn(src_vector, dst_vector, row_count);
					MSSQL_SCAN_DEBUG_LOG(2, "Execute: copied PK column %llu from output[%llu] to STRUCT child",
										 (unsigned long long)pk_idx, (unsigned long long)output_idx);
				}
			}
		}

		auto &validity = FlatVector::ValidityMutable(rowid_vector);
		validity.SetAllValid(row_count);
		MSSQL_SCAN_DEBUG_LOG(2, "Execute: composite_pk_direct_to_struct mode - STRUCT validity set for %llu rows",
							 (unsigned long long)row_count);
		return;
	}

	auto &rowid_vector = output.data[state.rowid_output_idx];

	if (state.pk_is_composite) {
		// Composite PK: build STRUCT from multiple PK columns
		auto &entries = StructVector::GetEntries(rowid_vector);

		for (idx_t pk_idx = 0; pk_idx < state.pk_result_indices.size(); pk_idx++) {
			idx_t src_col_idx = state.pk_result_indices[pk_idx];
			auto &src_vector = output.data[src_col_idx];
			auto &dst_vector = entries[pk_idx];

			// Copy the PK column data to the struct child
			CopyKeyColumn(src_vector, dst_vector, row_count);
		}

		// Set validity for the struct itself (valid if any child is valid)
		auto &validity = FlatVector::ValidityMutable(rowid_vector);
		validity.SetAllValid(row_count);

		MSSQL_SCAN_DEBUG_LOG(2, "Execute: populated composite rowid with %zu fields for %llu rows",
							 state.pk_result_indices.size(), (unsigned long long)row_count);
	} else {
		// Scalar PK: copy single PK column to rowid
		D_ASSERT(state.pk_result_indices.size() == 1);
		idx_t src_col_idx = state.pk_result_indices[0];
		auto &src_vector = output.data[src_col_idx];

		// Copy the PK column to rowid vector
		CopyKeyColumn(src_vector, rowid_vector, row_count);

		MSSQL_SCAN_DEBUG_LOG(2, "Execute: populated scalar rowid from column %llu for %llu rows",
							 (unsigned long long)src_col_idx, (unsigned long long)row_count);
	}
}

// Composite-key rowid: the stream writes the key columns the projection lacks
// straight into the rowid STRUCT's children of `chunk` (SetTargetVectors).
// Bound to the chunk the stream fills -- the Execute output chunk, or the
// materializing drain's own chunk (issue #451 found the drain filling a chunk
// whose STRUCT children nothing wrote: an UPDATE in a transaction on a
// composite key it did not project crashed copying their garbage). The
// pointers survive DataChunk::Reset: they point into the cache-owned struct
// storage Reset re-references.
static void BindRowIdTargetVectors(MSSQLScanGlobalState &global_state, DataChunk &output) {
	if (!global_state.composite_pk_direct_to_struct || !global_state.result_stream) {
		return;
	}
	auto &rowid_vector = output.data[global_state.rowid_output_idx];
	auto &entries = StructVector::GetEntries(rowid_vector);

	if (global_state.pk_columns_added) {
		// Composite PK with other columns, some PK columns may be in user projection
		// SQL: [user_cols..., added_pk_cols...]
		// - User cols go to their output positions
		// - Added PK cols (not in projection) go to STRUCT children
		// - PK cols already in projection need to be copied to STRUCT children after fill
		vector<Vector *> target_vectors;

		// First, add targets for user columns (based on column_ids order)
		for (idx_t out_col = 0; out_col < output.ColumnCount(); out_col++) {
			// Skip rowid position - PK columns go to STRUCT children
			if (out_col == global_state.rowid_output_idx) {
				continue;
			}
			target_vectors.push_back(&output.data[out_col]);
		}

		// Then add STRUCT children ONLY for PK columns that were ADDED (not in projection)
		// pk_result_indices[i] == UINT64_MAX means the column was added for rowid
		idx_t added_pk_count = 0;
		for (idx_t pk_idx = 0; pk_idx < global_state.pk_result_indices.size(); pk_idx++) {
			if (global_state.pk_result_indices[pk_idx] == UINT64_MAX) {
				// This PK column was added - SQL will write to STRUCT child
				target_vectors.push_back(&entries[pk_idx]);
				added_pk_count++;
			}
			// PK columns already in projection will be copied after FillChunk
		}

		idx_t total_cols = target_vectors.size();
		global_state.result_stream->SetTargetVectors(std::move(target_vectors));
		global_state.result_stream->SetColumnsToFill(total_cols);

		MSSQL_SCAN_DEBUG_LOG(1, "Execute: pk_columns_added composite mode - %llu user cols + %llu added PK cols",
							 (unsigned long long)(total_cols - added_pk_count), (unsigned long long)added_pk_count);
	} else {
		// Composite PK rowid-only: write directly to STRUCT children
		vector<Vector *> target_vectors;
		for (auto &entry : entries) {
			target_vectors.push_back(&entry);
		}
		global_state.result_stream->SetTargetVectors(std::move(target_vectors));
		global_state.result_stream->SetColumnsToFill(entries.size());

		MSSQL_SCAN_DEBUG_LOG(1, "Execute: composite rowid-only - %zu STRUCT children", entries.size());
	}
}

static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();

	// Issue #239: a materialized scan has no stream — InitGlobal drained it and
	// released the connection. Serve from the collection. This has to come first:
	// the rowid/PK plumbing below all keys off result_stream, and the rows in the
	// collection already went through it during the drain.
	if (global_state.materialized) {
		output.Reset();
		global_state.materialized->Scan(global_state.materialized_scan, output);
		if (output.size() == 0) {
			global_state.done = true;
		}
		return;
	}

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_SCAN_DEBUG_LOG(1, "Execute: FIRST CALL - scan started");

		BindRowIdTargetVectors(global_state, output);
	}

	// Check if we're done
	if (global_state.done || !global_state.result_stream) {
		auto scan_end = std::chrono::steady_clock::now();
		auto total_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - global_state.scan_start).count();
		MSSQL_SCAN_DEBUG_LOG(1, "Execute: SCAN COMPLETE - total=%ldms", (long)total_ms);
		output.SetChildCardinality(0);
		return;
	}

	// Check for query cancellation (Ctrl+C)
	if (context.IsInterrupted()) {
		global_state.result_stream->Cancel();
		global_state.done = true;
		output.SetChildCardinality(0);
		return;
	}

	// Fill chunk from result stream
	try {
		// Loop: a chunk the client-side filters reduce to zero rows must NOT
		// reach DuckDB — an empty chunk ends the scan — so fetch the next one.
		// FillChunk itself starts with chunk.Reset(), which is what makes the
		// retry sound: a Slice below rewrites output.data with dictionary
		// buffers, and Reset re-references the flat cache buffers before the
		// next fill. The composite-PK target pointers captured on the first
		// call survive that Reset because they point into the cache-owned
		// struct storage the Reset re-references, not into the slice.
		for (;;) {
			idx_t rows = global_state.result_stream->FillChunk(output);
			if (rows == 0) {
				global_state.done = true;
				// Surface any warnings
				global_state.result_stream->SurfaceWarnings(context);
				break;
			}
			if (global_state.result_stream->HasPendingWarnings()) {
				global_state.result_stream->SurfaceWarnings(context);  // as met: a LIMIT may end the scan here
			}
			if (global_state.rowid_requested) {
				// Populate rowid vector from PK columns
				PopulateRowIdVector(global_state, output, rows);
			}
			if (!global_state.client_filters.empty()) {
				rows = ApplyClientFilters(context, global_state, output, rows);
				if (rows == 0) {
					continue;
				}
			}
			break;
		}
	} catch (const Exception &e) {
		global_state.done = true;
		// The server's own notices about the batch that just failed -- and the ones
		// immediately preceding the failure are the useful ones. Without this they
		// are lost with the stream, which contradicts the rule the mssql_exec path
		// already follows (PR #320 review).
		//
		// Swallowing a throw from here is deliberate: whatever the logger does, it
		// must not replace the exception the caller is waiting for.
		try {
			global_state.result_stream->SurfaceWarnings(context);
		} catch (...) {	 // NOLINT: never mask the original error
		}
		throw;
	}
}

//------------------------------------------------------------------------------
// Virtual Columns (rowid support)
//------------------------------------------------------------------------------

// Callback to expose virtual columns (rowid) for this table
// Called during binding to discover what virtual columns are available
static virtual_column_map_t GetVirtualColumns(ClientContext &context, optional_ptr<FunctionData> bind_data_p) {
	virtual_column_map_t virtual_columns;

	if (!bind_data_p) {
		return virtual_columns;
	}

	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();

	// Only expose rowid if the table has a primary key
	// Views and tables without PK don't support rowid
	if (bind_data.rowid_requested && !bind_data.pk_column_names.empty()) {
		// Expose rowid with the correct type based on PK structure
		virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", bind_data.rowid_type)));
		MSSQL_SCAN_DEBUG_LOG(1, "GetVirtualColumns: exposing rowid with type %s",
							 bind_data.rowid_type.ToString().c_str());
	} else {
		MSSQL_SCAN_DEBUG_LOG(1, "GetVirtualColumns: rowid not available (rowid_requested=%s, pk_columns=%zu)",
							 bind_data.rowid_requested ? "true" : "false", bind_data.pk_column_names.size());
	}
	// Spec 080 D3, rung 3: the same hidden key columns the table entry names in
	// GetRowIdColumns, so the LogicalGet can type the ones the binder adds.
	if (bind_data.keyless_key) {
		// Named as the entry's columns (their DuckDB names, issue #572), not the server's.
		for (idx_t i = 0; i < bind_data.all_column_names.size(); i++) {
			const string &name = i < bind_data.mssql_columns.size() ? bind_data.mssql_columns[i].DuckDBName()
																	: bind_data.all_column_names[i];
			virtual_columns.insert(
				make_pair(KeylessKeyColumn(i), TableColumn(Identifier(name), bind_data.all_types[i])));
		}
	}

	return virtual_columns;
}

//------------------------------------------------------------------------------
// Table entry (for GetTable() support)
//------------------------------------------------------------------------------

// The table a catalog scan reads, for LogicalGet::GetTable(): the binder finds
// the table entry's GetVirtualColumns() (rowid) and an UPDATE / DELETE target
// through it. A callback of its own since DuckDB cb8eeaefed9 (it used to ride
// in BindInfo).
static optional_ptr<TableCatalogEntry> GetTableEntry(optional_ptr<const FunctionData> bind_data_p) {
	if (!bind_data_p) {
		return nullptr;
	}
	return bind_data_p->Cast<MSSQLCatalogScanBindData>().table_entry;
}

//------------------------------------------------------------------------------
// Public Interface
//------------------------------------------------------------------------------

TableFunction GetCatalogScanFunction() {
	// Create table function without arguments - bind_data is set from TableEntry
	TableFunction func("mssql_catalog_scan", {}, TableScanExecute, TableScanBind, TableScanInitGlobal,
					   TableScanInitLocal);

	// Enable projection pushdown - allows DuckDB to tell us which columns are needed
	// The column_ids will be passed to InitGlobal via TableFunctionInitInput
	func.projection_pushdown = true;

	// Enable filter pushdown - allows DuckDB to push WHERE conditions to SQL Server
	// The filters will be passed to InitGlobal via TableFunctionInitInput
	func.filter_pushdown = true;

	// Enable complex filter pushdown - allows us to handle expressions like year(col) = 2024
	// that cannot be represented as simple TableFilter objects
	func.pushdown_complex_filter = ComplexFilterPushdown;

	// Enable single-column expression pushdown (spec 070 W1). The 2.0 combiner
	// asks, per expression, whether the scan can push it into a TableFilter; we
	// accept whatever our encoder can render. Accepted expressions become
	// EXPRESSION_FILTERs (encoded in InitGlobal, netted client-side on refusal).
	func.pushdown_expression = MSSQLPushdownExpression;

	// Report the table's row count to the optimizer. Without this every MSSQL
	// scan plans as ~1 row and join order around it is arbitrary (roadmap step 0).
	func.cardinality = MSSQLCatalogScanCardinality;

	// Enable virtual column discovery - exposes rowid column to DuckDB binder
	// This is called during binding to determine what virtual columns are available
	func.get_virtual_columns = GetVirtualColumns;

	// The table entry for LogicalGet::GetTable() - the binder reads the rowid
	// columns and the UPDATE / DELETE target from it (the scan's own virtual
	// columns come from get_virtual_columns above)
	func.get_table_entry = GetTableEntry;

	// Note: We don't set filter_prune = true because that can cause issues with
	// the DataChunk column count when filter-only columns are excluded

	// Required for correctness, not just for plan persistence — see the comment
	// above CatalogScanSerialize. Both callbacks must be set: DuckDB only
	// serializes bind data when HasSerializationCallbacks() is true.
	func.serialize = CatalogScanSerialize;
	func.deserialize = CatalogScanDeserialize;

	return func;
}

void RegisterCatalogScanFunction(ExtensionLoader &loader) {
	// Without it a plan copy fails: `WITH c AS (SELECT ... FROM db.t) SELECT
	// ... FROM c x, c y LIMIT 2` raised "Table Function with name
	// mssql_catalog_scan does not exist" (CTE inlining copies the CTE, and the
	// copy looks the function up by name).
	RegisterDocumentedFunction(
		loader, TableFunctionSet(GetCatalogScanFunction()),
		{{},
		 "Internal: the scan behind an attached SQL Server table. Registered so DuckDB can copy a plan that holds "
		 "one; read a table by name instead.",
		 {"SELECT * FROM db.dbo.orders"},
		 {"query"}});
}

}  // namespace mssql
}  // namespace duckdb

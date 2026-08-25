// MSSQL Table Scan Implementation
// Feature: 013-table-scan-filter-refactor
// Feature: 001-pk-rowid-semantics (rowid support)

#include "table_scan/table_scan.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "catalog/mssql_catalog.hpp"
#include "catalog/mssql_statistics.hpp"
#include "catalog/mssql_table_entry.hpp"
#include "connection/mssql_settings.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/common.hpp"	 // For COLUMN_IDENTIFIER_ROW_ID
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/table_column.hpp"  // For TableColumn, virtual_column_map_t
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "mssql_functions.hpp"	// For backward compatibility with MSSQLCatalogScanBindData
#include "query/mssql_query_executor.hpp"
#include "table_scan/filter_encoder.hpp"
#include "table_scan/table_scan_bind.hpp"
#include "table_scan/table_scan_state.hpp"

// Debug logging controlled by MSSQL_DEBUG environment variable
static int GetDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define MSSQL_SCAN_DEBUG_LOG(level, fmt, ...)                               \
	do {                                                                    \
		if (GetDebugLevel() >= level) {                                     \
			fprintf(stderr, "[MSSQL TABLE_SCAN] " fmt "\n", ##__VA_ARGS__); \
		}                                                                   \
	} while (0)

namespace duckdb {
namespace mssql {

// Forward declarations for internal functions
static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output);

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

//------------------------------------------------------------------------------
// VARCHAR to NVARCHAR Conversion Helpers (Spec 026)
//------------------------------------------------------------------------------

// The three pre-2005 LOB types. sys.columns reports max_length 16 for all of them — the size of
// the in-row pointer, not of the data, which runs to 2 GB — so any CAST length derived from it
// truncates. All three are "known" types, so is_cast_required is false and nothing else rewrites
// them; without an explicit CAST their wire forms (TEXT 0x23, NTEXT 0x63, IMAGE 0x22) reach a
// codec layer that has no decoder for any of them (issue #197).
enum class LegacyLob : uint8_t { None, Text, NText, Image };

static LegacyLob LegacyLobKind(const string &sql_type_name) {
	string lower_type = sql_type_name;
	std::transform(lower_type.begin(), lower_type.end(), lower_type.begin(),
				   [](unsigned char c) { return std::tolower(c); });
	if (lower_type == "text") {
		return LegacyLob::Text;
	}
	if (lower_type == "ntext") {
		return LegacyLob::NText;
	}
	if (lower_type == "image") {
		return LegacyLob::Image;
	}
	return LegacyLob::None;
}

// Check if column needs NVARCHAR conversion for UTF-8 compatibility
// convert_varchar_max: if true, also convert VARCHAR(MAX) to NVARCHAR(MAX)
static bool NeedsNVarcharConversion(const MSSQLColumnInfo &col, bool convert_varchar_max) {
	// Only CHAR/VARCHAR need conversion (not NCHAR/NVARCHAR/NTEXT)
	if (col.is_unicode) {
		return false;  // Already Unicode
	}
	// Check if it's a text type (CHAR, VARCHAR, TEXT)
	if (!MSSQLColumnInfo::IsTextType(col.sql_type_name)) {
		return false;  // Not a string type
	}
	// Check if UTF-8 collation (safe to pass through)
	if (col.is_utf8) {
		return false;  // UTF-8 is safe
	}
	// VARCHAR(MAX) handling depends on setting
	// When convert_varchar_max is false, skip to preserve TDS buffer capacity (4096 bytes)
	// When true, convert to NVARCHAR(MAX) for UTF-8 compatibility
	//
	// The setting governs *declared*-MAX columns only (max_length == -1), matching its name and
	// documented purpose. A varchar(4001..8000) is not VARCHAR(MAX): GetNVarcharLength promotes it
	// to NVARCHAR(MAX) because no shorter NVARCHAR could hold it, not because the user asked for
	// MAX, so the opt-out does not apply to it.
	//
	// TEXT is never opted out. Unlike varchar, it has no decodable uncast wire form: it is a known
	// type (so is_cast_required is false) and dropping the CAST would put TDS_TYPE_TEXT (0x23) on
	// the wire, which no codec handles — the read would fail outright rather than degrade.
	if (col.max_length == -1 && !convert_varchar_max) {
		return false;  // VARCHAR(MAX) - don't convert when setting is off
	}
	return true;  // Non-UTF8 CHAR/VARCHAR/TEXT needs conversion
}

// Get NVARCHAR length specification for CAST.
// Returns "MAX" for VARCHAR(MAX), for TEXT, and for any CHAR/VARCHAR wider than the 4000-
// character inline NVARCHAR limit — such a column has no valid inline NVARCHAR length, so it
// must go over the wire as PLP. (Mirrors the >4000 PLP fallback on the BCP write path in
// SQLServerTypeMaxLength, src/copy/target_resolver.cpp.)
static std::string GetNVarcharLength(const MSSQLColumnInfo &col) {
	if (col.max_length == -1) {
		return "MAX";  // VARCHAR(MAX) → NVARCHAR(MAX)
	}
	if (LegacyLobKind(col.sql_type_name) != LegacyLob::None) {
		return "MAX";  // TEXT/NTEXT → NVARCHAR(MAX); their max_length of 16 is the pointer size
	}
	if (col.max_length > 4000) {
		return "MAX";  // varchar(4001..8000) → NVARCHAR(MAX); NVARCHAR(4000) would truncate
	}
	return std::to_string(col.max_length);
}

// Build column expression for SELECT, applying NVARCHAR conversion if needed
// Returns either "[column]" or "CAST([column] AS NVARCHAR(n)) AS [column]"
// convert_varchar_max: if true, also convert VARCHAR(MAX) to NVARCHAR(MAX)
static std::string BuildColumnExpression(const MSSQLColumnInfo &col, const std::string &col_name,
										 bool convert_varchar_max) {
	std::string escaped_name = "[" + FilterEncoder::EscapeBracketIdentifier(col_name) + "]";

	// Geometry/geography UDTs: rewrite to .STAsBinary() so the wire delivers OGC WKB
	// (varbinary(max)) instead of MS's proprietary Spatial Type Binary Format. The
	// result lands in a LogicalType::GEOMETRY() vector via the Binary codec (shared
	// string_t storage with BLOB). See codec/binary_codec.cpp + type_family.cpp.
	if (col.is_geometry) {
		MSSQL_SCAN_DEBUG_LOG(2, "  Geometry rewrite: %s (%s) → STAsBinary()", col_name.c_str(),
							 col.sql_type_name.c_str());
		return escaped_name + ".STAsBinary() AS " + escaped_name;
	}

	// NTEXT and IMAGE reach here uncast and unreadable (issue #197). Neither is caught by the
	// NVARCHAR conversion below: ntext is flagged is_unicode, so that path returns early
	// treating it as already safe, and image is not a text type at all. Image also needs a
	// BINARY target, so it cannot share the NVARCHAR rewrite.
	//
	// TEXT needs nothing here — NeedsNVarcharConversion already claims it, and GetNVarcharLength
	// answers MAX.
	const LegacyLob lob_kind = LegacyLobKind(col.sql_type_name);
	if (lob_kind == LegacyLob::NText) {
		MSSQL_SCAN_DEBUG_LOG(2, "  Legacy LOB rewrite: %s (ntext) → NVARCHAR(MAX)", col_name.c_str());
		return "CAST(" + escaped_name + " AS NVARCHAR(MAX)) AS " + escaped_name;
	}
	if (lob_kind == LegacyLob::Image) {
		MSSQL_SCAN_DEBUG_LOG(2, "  Legacy LOB rewrite: %s (image) → VARBINARY(MAX)", col_name.c_str());
		return "CAST(" + escaped_name + " AS VARBINARY(MAX)) AS " + escaped_name;
	}

	// Unsupported SQL Server types (hierarchyid, sql_variant, CLR UDTs, etc.)
	// must be CAST to NVARCHAR(MAX) so SQL Server sends text instead of native wire format
	if (col.is_cast_required) {
		MSSQL_SCAN_DEBUG_LOG(2, "  CAST required: %s (%s) → NVARCHAR(MAX)", col_name.c_str(),
							 col.sql_type_name.c_str());
		return "CAST(" + escaped_name + " AS NVARCHAR(MAX)) AS " + escaped_name;
	}

	if (NeedsNVarcharConversion(col, convert_varchar_max)) {
		std::string nvarchar_len = GetNVarcharLength(col);
		MSSQL_SCAN_DEBUG_LOG(2, "  NVARCHAR conversion: %s (%s, len=%d) → NVARCHAR(%s)", col_name.c_str(),
							 col.sql_type_name.c_str(), col.max_length, nvarchar_len.c_str());
		return "CAST(" + escaped_name + " AS NVARCHAR(" + nvarchar_len + ")) AS " + escaped_name;
	}
	return escaped_name;
}

//------------------------------------------------------------------------------
// Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> TableScanBind(ClientContext &context, TableFunctionBindInput &input,
											  vector<LogicalType> &return_types, vector<Identifier> &names) {
	// This bind function is not used for catalog scans - bind_data is set in GetScanFunction
	// from MSSQLTableEntry
	throw InternalException("TableScanBind should not be called directly");
}

//------------------------------------------------------------------------------
// Init Functions
//------------------------------------------------------------------------------

static unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: START");

	// Use the existing MSSQLCatalogScanBindData for backward compatibility
	auto &bind_data = input.bind_data->Cast<MSSQLCatalogScanBindData>();
	auto result = make_uniq<MSSQLScanGlobalState>();
	result->context_name = bind_data.context_name;

	// Build column list based on projection pushdown (column_ids)
	// column_ids contains the indices of columns needed from the table
	// Note: column_ids may contain special identifiers like COLUMN_IDENTIFIER_ROW_ID or
	// COLUMN_IDENTIFIER_EMPTY for operations like COUNT(*). These are virtual column IDs
	// that start at 2^63, so any value >= that is a special identifier to skip.
	string column_list;
	const auto &column_ids = input.column_ids;

	// Virtual/special column identifiers start at 2^63
	constexpr column_t VIRTUAL_COL_START = UINT64_C(9223372036854775808);

	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: projection has %zu columns (table has %zu)", column_ids.size(),
						 bind_data.all_column_names.size());

	//===----------------------------------------------------------------------===//
	// RowId Detection (Spec 001-pk-rowid-semantics)
	//===----------------------------------------------------------------------===//
	bool rowid_requested = false;
	idx_t rowid_output_idx = 0;

	// Check for COLUMN_IDENTIFIER_ROW_ID in column_ids
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i] == COLUMN_IDENTIFIER_ROW_ID) {
			rowid_requested = true;
			rowid_output_idx = i;
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: rowid requested at output index %llu", (unsigned long long)i);
			break;
		}
	}

	// If rowid is requested, validate PK availability from bind_data
	if (rowid_requested) {
		if (!bind_data.rowid_requested) {
			// This shouldn't happen - GetScanFunction should have set this
			throw BinderException("MSSQL: rowid requested but PK info not available in bind data");
		}
		if (bind_data.pk_column_names.empty()) {
			throw BinderException("MSSQL: rowid requires a primary key");
		}
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: PK has %zu columns, composite=%s",
							 bind_data.pk_column_names.size(), bind_data.pk_is_composite ? "true" : "false");
	}

	// Load VARCHAR(MAX) conversion setting (Spec 026)
	bool convert_varchar_max = LoadConvertVarcharMax(context);
	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: convert_varchar_max=%s", convert_varchar_max ? "true" : "false");

	// Filter out special column identifiers and collect valid column indices
	vector<column_t> valid_column_ids;
	for (const auto &col_idx : column_ids) {
		if (col_idx < VIRTUAL_COL_START && col_idx < bind_data.all_column_names.size()) {
			valid_column_ids.push_back(col_idx);
		} else {
			MSSQL_SCAN_DEBUG_LOG(2, "  skipping special/invalid column_id=%llu", (unsigned long long)col_idx);
		}
	}

	//===----------------------------------------------------------------------===//
	// Build Column List (including PK columns for rowid if needed)
	//===----------------------------------------------------------------------===//

	// Track which columns are in the SQL result and their indices
	vector<string> sql_column_names;  // Column names in order of SQL SELECT
	vector<idx_t> pk_result_indices;  // Indices of PK columns in OUTPUT CHUNK (not SQL result)
	vector<idx_t> pk_sql_indices;	  // Indices of PK columns in SQL result
	bool pk_columns_added = false;	  // True if PK columns were added (not in user projection)

	if (rowid_requested) {
		// Build a set of already-projected column names for deduplication
		std::unordered_set<string> projected_columns;
		for (const auto &col_idx : valid_column_ids) {
			projected_columns.insert(bind_data.all_column_names[col_idx]);
		}

		// Start with valid projected columns
		for (const auto &col_idx : valid_column_ids) {
			const string &col_name = bind_data.all_column_names[col_idx];
			if (!column_list.empty()) {
				column_list += ", ";
			}
			// Use BuildColumnExpression for VARCHAR→NVARCHAR conversion (Spec 026)
			const auto &col_info = bind_data.mssql_columns[col_idx];
			column_list += BuildColumnExpression(col_info, col_name, convert_varchar_max);
			sql_column_names.push_back(col_name);
		}

		// Add PK columns if not already projected
		for (idx_t pk_idx = 0; pk_idx < bind_data.pk_column_names.size(); pk_idx++) {
			const string &pk_col = bind_data.pk_column_names[pk_idx];

			// Check if this PK column is already in the projection
			bool already_projected = projected_columns.find(pk_col) != projected_columns.end();

			if (!already_projected) {
				// Add to SELECT - these are "extra" columns for rowid construction
				if (!column_list.empty()) {
					column_list += ", ";
				}
				// Find column index for VARCHAR→NVARCHAR conversion (Spec 026)
				idx_t pk_col_idx = 0;
				for (idx_t i = 0; i < bind_data.all_column_names.size(); i++) {
					if (bind_data.all_column_names[i] == pk_col) {
						pk_col_idx = i;
						break;
					}
				}
				const auto &col_info = bind_data.mssql_columns[pk_col_idx];
				column_list += BuildColumnExpression(col_info, pk_col, convert_varchar_max);
				sql_column_names.push_back(pk_col);
				pk_columns_added = true;
				MSSQL_SCAN_DEBUG_LOG(2, "  added PK column for rowid: %s at SQL index %zu", pk_col.c_str(),
									 sql_column_names.size() - 1);
			}

			// Record the SQL result index of this PK column
			idx_t sql_result_idx = 0;
			for (idx_t j = 0; j < sql_column_names.size(); j++) {
				if (sql_column_names[j] == pk_col) {
					sql_result_idx = j;
					break;
				}
			}
			pk_sql_indices.push_back(sql_result_idx);

			// Find the output chunk index for this PK column (if it's in user projection)
			// column_ids maps output positions to table column indices
			idx_t output_idx = UINT64_MAX;	// Sentinel for "not in output"
			for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
				if (column_ids[out_col] < VIRTUAL_COL_START &&
					bind_data.all_column_names[column_ids[out_col]] == pk_col) {
					output_idx = out_col;
					break;
				}
			}
			pk_result_indices.push_back(output_idx);
			MSSQL_SCAN_DEBUG_LOG(2, "  PK column %s: sql_idx=%llu, output_idx=%s", pk_col.c_str(),
								 (unsigned long long)sql_result_idx,
								 output_idx == UINT64_MAX ? "N/A (extra)" : std::to_string(output_idx).c_str());
		}
	} else if (valid_column_ids.empty()) {
		// No valid columns projected (e.g., COUNT(*))
		// Select only the first column to minimize data transfer while still returning rows
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: no valid columns, selecting first column only for row counting");
		if (!bind_data.all_column_names.empty()) {
			column_list = "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.all_column_names[0]) + "]";
		} else {
			// Fallback to constant if table has no columns (shouldn't happen)
			column_list = "1";
		}
	} else {
		// Build SELECT with only valid projected columns (no rowid)
		for (idx_t i = 0; i < valid_column_ids.size(); i++) {
			if (i > 0) {
				column_list += ", ";
			}
			column_t col_idx = valid_column_ids[i];
			// Use BuildColumnExpression for VARCHAR→NVARCHAR conversion (Spec 026)
			const auto &col_info = bind_data.mssql_columns[col_idx];
			column_list += BuildColumnExpression(col_info, bind_data.all_column_names[col_idx], convert_varchar_max);
			MSSQL_SCAN_DEBUG_LOG(2, "  column[%llu] = %s", (unsigned long long)i,
								 bind_data.all_column_names[col_idx].c_str());
		}
	}

	// Store rowid state in global state for Execute phase
	result->rowid_requested = rowid_requested;
	result->rowid_output_idx = rowid_output_idx;
	result->pk_result_indices = pk_result_indices;
	result->pk_is_composite = bind_data.pk_is_composite;
	result->rowid_type = bind_data.rowid_type;
	result->pk_column_types = bind_data.pk_column_types;
	result->pk_columns_added = pk_columns_added;
	result->pk_sql_indices = pk_sql_indices;

	// Track special cases for rowid handling:
	// 1. pk_direct_to_rowid: rowid-only with scalar PK - write PK directly to rowid position
	// 2. composite_pk_direct_to_struct: rowid-only with composite PK - write to STRUCT children
	// 3. pk_columns_added: rowid + other columns, but PK not in user projection - write extra PK columns
	result->pk_direct_to_rowid = (rowid_requested && valid_column_ids.empty() && !bind_data.pk_is_composite);
	result->composite_pk_direct_to_struct = (rowid_requested && valid_column_ids.empty() && bind_data.pk_is_composite);
	MSSQL_SCAN_DEBUG_LOG(
		1, "TableScanInitGlobal: pk_direct_to_rowid=%s, composite_pk_direct_to_struct=%s, pk_columns_added=%s",
		result->pk_direct_to_rowid ? "true" : "false", result->composite_pk_direct_to_struct ? "true" : "false",
		result->pk_columns_added ? "true" : "false");

	// Generate the query: SELECT [col1], [col2], ... FROM [schema].[table]
	string full_table_name = "[" + FilterEncoder::EscapeBracketIdentifier(bind_data.schema_name) + "].[" +
							 FilterEncoder::EscapeBracketIdentifier(bind_data.table_name) + "]";

	// Build SELECT prefix (with optional TOP N from ORDER BY + LIMIT pushdown, Spec 039)
	string select_prefix = "SELECT ";
	if (bind_data.top_n > 0) {
		select_prefix += "TOP " + std::to_string(bind_data.top_n) + " ";
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: TOP %lld pushdown", (long long)bind_data.top_n);
	}
	string query = select_prefix + column_list + " FROM " + full_table_name;

	// Build WHERE clause from filter pushdown
	// Combine: simple filters (from FilterEncoder::Encode) + complex filters (from pushdown_complex_filter)
	std::vector<std::string> where_conditions;
	bool needs_duckdb_filter = false;

	// 1. Encode simple filters (TableFilterSet from filter_pushdown)
	if (input.filters && input.filters->HasFilters()) {
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: simple filter pushdown with %zu filter(s)",
							 static_cast<size_t>(input.filters->FilterCount()));

		auto encode_result =
			FilterEncoder::Encode(input.filters.get(), column_ids, bind_data.all_column_names, bind_data.all_types);

		if (!encode_result.where_clause.empty()) {
			where_conditions.push_back(encode_result.where_clause);
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: simple filters encoded: %s",
								 encode_result.where_clause.c_str());
		}

		needs_duckdb_filter = encode_result.needs_duckdb_filter;

		// 2.0 does not re-check TableFilters behind a filter_pushdown scan, so
		// every refused filter is executed client-side over the output chunk.
		// The filter's expression references its column as BoundReference(0) —
		// the same convention the combiner used when it built the filter.
		for (auto &uf : encode_result.unhandled) {
			const idx_t out_col = uf.first;
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
				cf.expr = uf.second->ToExpression(col_ref);
				cf.executor = make_uniq<ExpressionExecutor>(context);
				cf.executor->AddExpression(*cf.expr);
				result->client_filters.push_back(std::move(cf));
				MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: client-side filter armed on output col %llu",
									 (unsigned long long)out_col);
			} catch (const std::exception &e) {
				// A filter that cannot even build an expression cannot be
				// applied anywhere — fail loudly instead of returning a
				// superset of rows.
				throw NotImplementedException("MSSQL scan: cannot apply pushed-down filter client-side: %s", e.what());
			}
		}
	}

	// 2. Add complex filters (from pushdown_complex_filter callback)
	if (!bind_data.complex_filter_where_clause.empty()) {
		where_conditions.push_back(bind_data.complex_filter_where_clause);
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: complex filters added: %s",
							 bind_data.complex_filter_where_clause.c_str());
	}

	// 3. Combine all conditions with AND
	if (!where_conditions.empty()) {
		std::string combined_where;
		for (idx_t i = 0; i < where_conditions.size(); i++) {
			if (i > 0) {
				combined_where += " AND ";
			}
			combined_where += where_conditions[i];
		}
		query += " WHERE " + combined_where;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: final WHERE clause: %s", combined_where.c_str());
	}

	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: needs_duckdb_filter=%s", needs_duckdb_filter ? "true" : "false");

	// Append ORDER BY clause from optimizer pushdown (Spec 039)
	if (!bind_data.order_by_clause.empty()) {
		query += " ORDER BY " + bind_data.order_by_clause;
		MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: ORDER BY pushdown: %s", bind_data.order_by_clause.c_str());
	}

	MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: generated query = %s", query.c_str());

	// Execute the query
	MSSQLQueryExecutor executor(bind_data.context_name);
	result->result_stream = executor.Execute(context, query);

	// Set the number of columns to actually fill in the output chunk
	// When valid_column_ids is empty (e.g., COUNT(*)), we don't fill any columns
	// EXCEPT when pk_direct_to_rowid is true - then we fill the PK directly to rowid position
	result->projected_column_count = valid_column_ids.size();
	if (result->result_stream) {
		if (result->pk_direct_to_rowid) {
			// Special case: only rowid requested with scalar PK
			// SQL returns 1 column (PK), we write it directly to rowid output position
			result->result_stream->SetColumnsToFill(1);	 // Fill 1 column (the PK)
			vector<idx_t> output_mapping;
			output_mapping.push_back(rowid_output_idx);	 // SQL col 0 -> rowid position
			result->result_stream->SetOutputColumnMapping(std::move(output_mapping));
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: pk_direct_to_rowid mode - SQL col 0 -> output %llu",
								 (unsigned long long)rowid_output_idx);
		} else if (result->composite_pk_direct_to_struct) {
			// Special case: only rowid requested with composite PK
			// SQL returns N columns (PK columns), we write them directly to STRUCT children
			// The STRUCT is at rowid_output_idx in the output chunk
			idx_t pk_count = bind_data.pk_column_names.size();
			result->result_stream->SetColumnsToFill(pk_count);	// Fill all PK columns
			// Use special marker (-1) to indicate STRUCT mode - data will be written in PopulateRowIdVector
			// Actually, we need to skip ProcessRow entirely and handle this in PopulateRowIdVector
			// Set columns_to_fill to 0 so ProcessRow doesn't write anything
			result->result_stream->SetColumnsToFill(0);
			// Store pk_result_indices as SQL column indices (0, 1, 2, ...)
			result->pk_result_indices.clear();
			for (idx_t i = 0; i < pk_count; i++) {
				result->pk_result_indices.push_back(i);
			}
			// Mark pk_columns_added as false - Execute uses this to distinguish from mixed case
			result->pk_columns_added = false;
			MSSQL_SCAN_DEBUG_LOG(
				1, "TableScanInitGlobal: composite_pk_direct_to_struct mode - %llu PK columns -> STRUCT at output %llu",
				(unsigned long long)pk_count, (unsigned long long)rowid_output_idx);
		} else if (rowid_requested && pk_columns_added && !bind_data.pk_is_composite) {
			// Special case: rowid + other columns, scalar PK NOT in user projection
			// SQL returns [user_cols..., pk_col]
			// Write user columns to their positions, write PK column directly to rowid position
			idx_t total_sql_cols = valid_column_ids.size() + 1;	 // user cols + 1 PK col
			result->result_stream->SetColumnsToFill(total_sql_cols);

			// Build mapping: SQL column index -> output chunk index
			vector<idx_t> output_mapping;
			idx_t sql_col = 0;
			for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
				if (column_ids[out_col] == COLUMN_IDENTIFIER_ROW_ID) {
					continue;  // Skip rowid - PK column will be written there
				}
				if (column_ids[out_col] < VIRTUAL_COL_START) {
					output_mapping.push_back(out_col);
					MSSQL_SCAN_DEBUG_LOG(2, "  SQL col %llu -> output col %llu (user col)", (unsigned long long)sql_col,
										 (unsigned long long)out_col);
					sql_col++;
				}
			}
			// Map the extra PK column (last SQL column) to rowid position
			output_mapping.push_back(rowid_output_idx);
			MSSQL_SCAN_DEBUG_LOG(2, "  SQL col %llu -> output col %llu (PK to rowid)", (unsigned long long)sql_col,
								 (unsigned long long)rowid_output_idx);

			result->result_stream->SetOutputColumnMapping(std::move(output_mapping));
			// Mark that PK was written directly to rowid
			result->pk_direct_to_rowid = true;
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: pk_columns_added scalar mode - %llu cols with PK -> rowid",
								 (unsigned long long)total_sql_cols);
		} else if (rowid_requested && pk_columns_added && bind_data.pk_is_composite) {
			// Special case: rowid + other columns, composite PK NOT in user projection
			// SQL returns [user_cols..., pk_cols...]
			// Write user columns normally, write PK columns to STRUCT children via target_vectors
			// For now, use columns_to_fill for user cols only, handle PK in Execute
			result->result_stream->SetColumnsToFill(valid_column_ids.size());

			vector<idx_t> output_mapping;
			for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
				if (column_ids[out_col] == COLUMN_IDENTIFIER_ROW_ID) {
					continue;
				}
				if (column_ids[out_col] < VIRTUAL_COL_START) {
					output_mapping.push_back(out_col);
				}
			}
			result->result_stream->SetOutputColumnMapping(std::move(output_mapping));
			// Mark for special handling in Execute
			result->composite_pk_direct_to_struct = true;
			MSSQL_SCAN_DEBUG_LOG(1,
								 "TableScanInitGlobal: pk_columns_added composite mode - user cols + STRUCT handling");
		} else if (rowid_requested) {
			// Rowid with other columns, PK IS in user projection
			// Build mapping: SQL column index -> output chunk index
			result->result_stream->SetColumnsToFill(valid_column_ids.size());

			// The output chunk has positions for all columns including rowid
			// Example: SELECT id, rowid, name -> column_ids [0, ROWID, 1] -> output [id, rowid, name]
			// SQL returns [id, name] -> mapping [0, 2] (SQL col 0->output 0, SQL col 1->output 2)
			// Skip the rowid position in the output
			vector<idx_t> output_mapping;
			idx_t sql_col = 0;
			for (idx_t out_col = 0; out_col < column_ids.size(); out_col++) {
				if (column_ids[out_col] == COLUMN_IDENTIFIER_ROW_ID) {
					// Skip rowid position - it will be populated from PK columns
					continue;
				}
				if (column_ids[out_col] < VIRTUAL_COL_START) {
					output_mapping.push_back(out_col);
					MSSQL_SCAN_DEBUG_LOG(2, "  SQL col %llu -> output col %llu", (unsigned long long)sql_col,
										 (unsigned long long)out_col);
					sql_col++;
				}
			}
			result->result_stream->SetOutputColumnMapping(std::move(output_mapping));
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: rowid with PK in projection - %llu user cols",
								 (unsigned long long)valid_column_ids.size());
		} else {
			// No rowid - simple case
			result->result_stream->SetColumnsToFill(valid_column_ids.size());
			MSSQL_SCAN_DEBUG_LOG(1, "TableScanInitGlobal: set columns_to_fill=%zu", valid_column_ids.size());
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
					VectorOperations::Copy(src_vector, dst_vector, row_count, 0, 0);
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
			VectorOperations::Copy(src_vector, dst_vector, row_count, 0, 0);
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
		VectorOperations::Copy(src_vector, rowid_vector, row_count, 0, 0);

		MSSQL_SCAN_DEBUG_LOG(2, "Execute: populated scalar rowid from column %llu for %llu rows",
							 (unsigned long long)src_col_idx, (unsigned long long)row_count);
	}
}

static void TableScanExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &global_state = data.global_state->Cast<MSSQLScanGlobalState>();

	// Start timing on first call
	if (!global_state.timing_started) {
		global_state.scan_start = std::chrono::steady_clock::now();
		global_state.timing_started = true;
		MSSQL_SCAN_DEBUG_LOG(1, "Execute: FIRST CALL - scan started");

		// Set up target vectors for composite PK cases
		if (global_state.composite_pk_direct_to_struct && global_state.result_stream) {
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

				MSSQL_SCAN_DEBUG_LOG(
					1, "Execute: pk_columns_added composite mode - %llu user cols + %llu added PK cols",
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
		throw;
	}
}

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
static ExpressionEncodeContext BuildEncodeContext(const LogicalGet &get, const MSSQLCatalogScanBindData &bind_data,
												  vector<column_t> &column_ids_out) {
	const auto &get_column_ids = get.GetColumnIds();
	column_ids_out.clear();
	column_ids_out.reserve(get_column_ids.size());
	for (const auto &col_idx : get_column_ids) {
		column_ids_out.push_back(col_idx.IsVirtualColumn() ? COLUMN_IDENTIFIER_ROW_ID : col_idx.GetPrimaryIndex());
	}

	ExpressionEncodeContext ctx(column_ids_out, bind_data.all_column_names, bind_data.all_types);
	if (!bind_data.pk_column_names.empty()) {
		ctx.SetPKInfo(&bind_data.pk_column_names, &bind_data.pk_column_types, bind_data.pk_is_composite);
	}
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
// oversized IN, CanThrow with more than one filter) — DID NOT REPRODUCE. Every
// one of those still reached the server. Do not re-derive it.
//
// The real blocker is semantic. Stop claiming a predicate here and the combiner
// rewrites it before offering it: prefix() becomes a >=/< RANGE, which is exact
// for DuckDB's binary comparison and NOT for SQL Server's collation ('ñu' sorts
// between 'n' and 'o' on the default _CI_AS, so LIKE N'n%' returns 2 rows and
// the range returns 3). Refusing the range instead pushes nothing at all and
// the client net applies DuckDB semantics — 1 row — which breaks the
// native-server-semantics contract just as surely. Pinned by
// test/sql/catalog/like_pushdown_collation.test. Gated on spec 061.
static bool MSSQLPushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr) {
	if (!get.bind_data) {
		return false;
	}
	auto &bind_data = get.bind_data->Cast<MSSQLCatalogScanBindData>();
	vector<column_t> column_ids;
	ExpressionEncodeContext ctx = BuildEncodeContext(get, bind_data, column_ids);
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
// query the DMV: it reads the statistics cache (which `PreloadRowCount` refreshes
// after a bulk load) and falls back to the count the catalog already loaded with
// the table metadata. A query must not pay a network round trip to be planned.
//
// Returning nullptr means "no estimate" — the pre-existing behaviour — and is the
// answer whenever the count is 0, because 0 CANNOT BE DISTINGUISHED FROM UNKNOWN
// here. A VIEW has no `sys.partitions` rows and reports 0 while returning
// millions; claiming 0 for it would make the planner pick it as a build side and
// is far worse than claiming nothing. An genuinely empty table therefore also
// plans as unknown, which costs an estimate nobody had before this callback.
static unique_ptr<NodeStatistics> MSSQLCatalogScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
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
	// const_cast: GetMSSQLCatalog()/GetApproxRowCount() are non-const accessors on
	// the entry, and the bind data holds it as a const pointer. Nothing below
	// mutates the entry.
	auto *table_entry = dynamic_cast<MSSQLTableEntry *>(const_cast<TableCatalogEntry *>(bind_data.table_entry.get()));
	if (!table_entry) {
		return nullptr;
	}

	idx_t row_count = 0;
	try {
		// Fresher than the catalog metadata when a load has happened since, and
		// free — TryGetCachedRowCount never opens a connection.
		auto &stats_provider = table_entry->GetMSSQLCatalog().GetStatisticsProvider();
		if (!stats_provider.TryGetCachedRowCount(bind_data.schema_name, bind_data.table_name, row_count)) {
			row_count = table_entry->GetApproxRowCount();
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
	// DELETE THIS when the filters stay visible. If single-column expressions
	// reached `pushdown_expression` instead (they cannot today — the complex-filter
	// pass runs first and consumes them; see the reachability note above and PR
	// #269), they would become EXPRESSION_FILTERs in `get.table_filters`, DuckDB
	// would apply selectivity itself, and doing it here as well would double-count.
	constexpr double DUCKDB_DEFAULT_SELECTIVITY = 0.2;
	if (!bind_data.complex_filter_where_clause.empty()) {
		const idx_t filtered = MaxValue<idx_t>(static_cast<idx_t>(row_count * DUCKDB_DEFAULT_SELECTIVITY), 1);
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

static void ComplexFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
								  vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();

	MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: processing %zu expression(s)", filters.size());

	vector<column_t> column_ids;
	ExpressionEncodeContext ctx = BuildEncodeContext(get, bind_data, column_ids);

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
		std::string where_clause;
		for (idx_t i = 0; i < encoded_conditions.size(); i++) {
			if (i > 0) {
				where_clause += " AND ";
			}
			where_clause += encoded_conditions[i];
		}
		bind_data.complex_filter_where_clause = where_clause;
		MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: stored WHERE clause: %s", where_clause.c_str());
	}

	MSSQL_SCAN_DEBUG_LOG(1, "ComplexFilterPushdown: %zu expressions handled, %zu remaining for DuckDB",
						 expressions_to_remove.size(), filters.size());
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

	return virtual_columns;
}

//------------------------------------------------------------------------------
// Bind Info (for GetTable() support)
//------------------------------------------------------------------------------

// Callback to return table entry for GetTable() support
// This allows DuckDB to use the table entry's GetVirtualColumns() override
static BindInfo GetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	if (!bind_data_p) {
		return BindInfo(ScanType::EXTERNAL);
	}

	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();

	if (bind_data.table_entry) {
		// Return BindInfo with table entry - this enables GetTable() to work
		// and allows DuckDB to call the table entry's GetVirtualColumns()
		// The BindInfo constructor takes a non-const reference
		auto &table_ref = const_cast<TableCatalogEntry &>(*bind_data.table_entry);
		return BindInfo(table_ref);
	}

	return BindInfo(ScanType::EXTERNAL);
}

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
static void CatalogScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
								 const TableFunction &function) {
	auto &bind_data = bind_data_p->Cast<MSSQLCatalogScanBindData>();
	serializer.WriteProperty(100, "context_name", bind_data.context_name);
	serializer.WriteProperty(101, "schema_name", bind_data.schema_name);
	serializer.WriteProperty(102, "table_name", bind_data.table_name);
	serializer.WriteProperty(103, "complex_filter_where_clause", bind_data.complex_filter_where_clause);
	serializer.WriteProperty(104, "order_by_clause", bind_data.order_by_clause);
	serializer.WriteProperty(105, "top_n", bind_data.top_n);
}

// NOTE: nothing in DuckDB round-trips a logical plan today — the only caller of
// LogicalOperator::Deserialize is the generated serializer itself, and the
// common-subplan optimizer only ever serializes. This exists because
// HasSerializationCallbacks() requires both halves before bind data is written
// at all, so it is currently unexercised by any test. Keep it obvious rather
// than clever, and validate defensively.
static unique_ptr<FunctionData> CatalogScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	auto context_name = deserializer.ReadProperty<string>(100, "context_name");
	auto schema_name = deserializer.ReadProperty<string>(101, "schema_name");
	auto table_name = deserializer.ReadProperty<string>(102, "table_name");
	auto complex_filter_where_clause = deserializer.ReadProperty<string>(103, "complex_filter_where_clause");
	auto order_by_clause = deserializer.ReadProperty<string>(104, "order_by_clause");
	auto top_n = deserializer.ReadProperty<int64_t>(105, "top_n");

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
	result.complex_filter_where_clause = complex_filter_where_clause;
	result.order_by_clause = order_by_clause;
	result.top_n = top_n;
	return bind_data;
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

	// Return table entry for GetTable() support - enables DuckDB to discover
	// virtual columns like rowid from the table entry's GetVirtualColumns()
	func.get_bind_info = GetBindInfo;

	// Note: We don't set filter_prune = true because that can cause issues with
	// the DataChunk column count when filter-only columns are excluded

	// Required for correctness, not just for plan persistence — see the comment
	// above CatalogScanSerialize. Both callbacks must be set: DuckDB only
	// serializes bind data when HasSerializationCallbacks() is true.
	func.serialize = CatalogScanSerialize;
	func.deserialize = CatalogScanDeserialize;

	return func;
}

}  // namespace mssql
}  // namespace duckdb

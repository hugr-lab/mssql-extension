#include "copy/copy_function.hpp"

#include "catalog/mssql_catalog.hpp"
#include "codec/target_string_type.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "mssql_counters.hpp"
#include "query/mssql_simple_query.hpp"
#include "tds/tds_connection.hpp"
#include "tds/tds_types.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Debug Logging
//===----------------------------------------------------------------------===//

static int GetCopyDebugLevel() {
	const char *env = std::getenv("MSSQL_DEBUG");
	if (!env) {
		return 0;
	}
	return std::atoi(env);
}

static void CopyDebugLog(int level, const char *format, ...) {
	if (GetCopyDebugLevel() < level) {
		return;
	}
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[MSSQL COPY] ");
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
	fflush(stderr);	 // Ensure output is flushed before potential crash
	va_end(args);
}

// High-resolution timer for performance analysis
using Clock = std::chrono::high_resolution_clock;
using CopyTimePoint = std::chrono::time_point<Clock>;

static double ElapsedMs(CopyTimePoint start) {
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

// Nanoseconds, deliberately. Spec 055 D0 found the read path accumulating
// per-chunk intervals through duration_cast<microseconds>, which truncated every
// short interval to zero and made the phase it measured report approximately
// nothing. Accumulate ns; divide at print time.
static uint64_t ElapsedNs(CopyTimePoint start) {
	auto end = Clock::now();
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

//===----------------------------------------------------------------------===//
// MSSQL Copy Function Registration
//===----------------------------------------------------------------------===//

// Declare supported COPY options for 'bcp' format
static void BCPListCopyOptions(ClientContext &context, CopyOptionsInput &input) {
	auto &copy_options = input.options;
	// CREATE_TABLE: Create destination table if it doesn't exist (default: true)
	copy_options["create_table"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	// OVERWRITE: Drop and recreate table if it exists (default: false)
	copy_options["replace"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	// FLUSH_ROWS: Number of rows before flushing to SQL Server (default: 100000)
	copy_options["flush_rows"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	// TABLOCK: Use table-level lock for better performance (default: false, from mssql_copy_tablock)
	copy_options["tablock"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	// STRING_LENGTH: length for unannotated VARCHAR columns this COPY creates
	// (default: 0 = MAX, from mssql_default_string_length). Spec 060 D8.
	copy_options["string_length"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	// TABLE_KIND: HEAP (default) or COLUMNSTORE for a table this COPY creates,
	// over mssql_default_table_kind. Spec 060 D8.
	copy_options["table_kind"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	// TRUNCATE: empty an existing target before loading, keeping its definition
	// (default: false). Spec 060 D7.
	copy_options["truncate"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
}

void RegisterMSSQLCopyFunctions(ExtensionLoader &loader) {
	CopyFunction bcp_copy("bcp");

	// Set up the copy options callback
	bcp_copy.copy_options = BCPListCopyOptions;

	// Set up the copy-to callbacks
	bcp_copy.copy_to_bind = mssql::BCPCopyBind;
	bcp_copy.copy_to_initialize_global = mssql::BCPCopyInitGlobal;
	bcp_copy.copy_to_initialize_local = mssql::BCPCopyInitLocal;
	bcp_copy.copy_to_sink = mssql::BCPCopySink;
	bcp_copy.copy_to_combine = mssql::BCPCopyCombine;
	bcp_copy.copy_to_finalize = mssql::BCPCopyFinalize;
	bcp_copy.execution_mode = mssql::BCPCopyExecutionMode;

	// Extension info
	bcp_copy.extension = "mssql";

	loader.RegisterFunction(bcp_copy);

	CopyDebugLog(1, "Registered 'bcp' COPY function");
}

//===----------------------------------------------------------------------===//
// MSSQLCopyGlobalState destructor - last-resort connection release (issue #191)
//===----------------------------------------------------------------------===//

MSSQLCopyGlobalState::~MSSQLCopyGlobalState() {
	// No-op on every path that already released: BCPCopyInitGlobal's error helper and
	// BCPCopyFinalize (both success and error) reset() `connection`. This only fires when the sink
	// threw and copy_to_finalize was never called — see the contract note on the declaration.
	//
	// Shared mid-BCP release protocol (see ReleaseBcpConnectionOnError contract) — worker-thread
	// safe per issue #178 / PR #179.
	mssql::ReleaseBcpConnectionOnError(connection, pool_handle, transaction_pinned, reset_on_release);
}

namespace mssql {

// Forward declarations
static void FlushToServer(MSSQLCopyGlobalState &gdata, const MSSQLCopyBindData &bdata);

//===----------------------------------------------------------------------===//
// BCPCopyBind - Parse target URL and options
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> BCPCopyBind(ClientContext &context, CopyFunctionBindInput &input,
									 const vector<Identifier> &names, const vector<LogicalType> &sql_types) {
	auto bind_data = make_uniq<MSSQLCopyBindData>();

	// Store source schema info
	bind_data->source_types = sql_types;
	bind_data->source_names.reserve(names.size());
	for (auto &n : names) {
		bind_data->source_names.push_back(n.GetIdentifierName());
	}

	// Get the file path (which is actually our target URL or catalog path for MSSQL)
	const string &target_path = input.info.file_path;

	CopyDebugLog(1, "BCPCopyBind: target='%s', columns=%llu", target_path.c_str(), (unsigned long long)names.size());

	// Parse the target - supports two syntaxes:
	// 1. URL syntax: mssql://<catalog>/<schema>/<table>
	// 2. Catalog syntax: <catalog>.<schema>.<table> or <catalog>.<table>
	if (StringUtil::StartsWith(target_path, "mssql://")) {
		// URL syntax
		bind_data->target = TargetResolver::ResolveURL(context, target_path);
	} else {
		// Try catalog syntax: catalog.schema.table or catalog.table or catalog..#temp
		vector<string> parts = StringUtil::Split(target_path, '.');

		if (parts.size() < 2 || parts.size() > 3) {
			throw InvalidInputException(
				"MSSQL COPY: Invalid target format. Use either:\n"
				"  - URL syntax: 'mssql://<catalog>/<schema>/<table>'\n"
				"  - Catalog syntax: <catalog>.<schema>.<table> or <catalog>.<table>\n"
				"  - Temp table: <catalog>..#temp_table (empty schema)\n"
				"Got: %s",
				target_path);
		}

		string catalog_name = parts[0];
		string schema_name;
		string table_name;
		bool allow_empty_schema = false;

		if (parts.size() == 2) {
			// catalog.table - use default schema 'dbo'
			schema_name = "dbo";
			table_name = parts[1];
		} else {
			// catalog.schema.table or catalog..#temp (empty schema)
			schema_name = parts[1];
			table_name = parts[2];
			// If schema is empty, this is the catalog..#temp syntax
			allow_empty_schema = schema_name.empty();
		}

		// Verify catalog exists and is an MSSQL catalog
		try {
			auto &catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
			if (catalog.GetCatalogType() != "mssql") {
				throw InvalidInputException(
					"MSSQL COPY: Catalog '%s' is not an MSSQL catalog (type: %s). "
					"The 'bcp' format can only be used with attached MSSQL databases.",
					catalog_name, catalog.GetCatalogType());
			}
		} catch (CatalogException &e) {
			throw InvalidInputException(
				"MSSQL COPY: Catalog '%s' not found. "
				"Use ATTACH '<connection_string>' AS %s (TYPE mssql) first.",
				catalog_name, catalog_name);
		}

		// Use ResolveCatalog to create the target
		bind_data->target =
			TargetResolver::ResolveCatalog(context, catalog_name, schema_name, table_name, allow_empty_schema);

		CopyDebugLog(1, "BCPCopyBind: resolved catalog syntax: catalog='%s', schema='%s', table='%s', empty_schema=%d",
					 catalog_name.c_str(), schema_name.c_str(), table_name.c_str(), allow_empty_schema ? 1 : 0);
	}

	bind_data->catalog_name = bind_data->target.catalog_name;

	// Load config from settings FIRST as defaults
	bind_data->config = LoadBCPCopyConfig(context);

	// Then let COPY options override
	CopyDebugLog(2, "BCPCopyBind: parsing %llu options", (unsigned long long)input.info.options.size());
	for (auto &option : input.info.options) {
		auto loption = StringUtil::Lower(option.first.GetIdentifierName());
		CopyDebugLog(2, "BCPCopyBind: option '%s' (lower: '%s')", option.first.c_str(), loption.c_str());

		if (loption == "create_table") {
			bind_data->config.create_table = BooleanValue::Get(option.second[0]);
			CopyDebugLog(2, "BCPCopyBind: set create_table=%d", bind_data->config.create_table ? 1 : 0);
		} else if (loption == "replace") {
			bind_data->config.overwrite = BooleanValue::Get(option.second[0]);
			CopyDebugLog(2, "BCPCopyBind: set replace=%d", bind_data->config.overwrite ? 1 : 0);
		} else if (loption == "flush_rows") {
			bind_data->config.flush_rows = static_cast<idx_t>(BigIntValue::Get(option.second[0]));
		} else if (loption == "tablock") {
			// A per-statement `tablock` option is by definition explicit, so it
			// resolves to ON/OFF and never to AUTO — this is the one place the old
			// `tablock_explicit` flag was set correctly.
			bind_data->config.tablock_choice =
				BooleanValue::Get(option.second[0]) ? MSSQLTablockChoice::ON : MSSQLTablockChoice::OFF;
		} else if (loption == "truncate") {
			bind_data->config.truncate = BooleanValue::Get(option.second[0]);
		} else if (loption == "table_kind") {
			bind_data->config.table_options.ApplyOption("table_kind", option.second[0].ToString());
		} else if (loption == "string_length") {
			// Spec 060 D8: per-statement override of mssql_default_string_length.
			const int64_t raw = BigIntValue::Get(option.second[0]);
			if (raw < 0) {
				throw InvalidInputException("MSSQL COPY: string_length must be >= 0 (0 = MAX), got %lld",
											(long long)raw);
			}
			bind_data->config.default_string_length =
				raw == 0 ? 0 : static_cast<int32_t>(std::min<int64_t>(raw, INT32_MAX));
		}
		// Ignore unknown options (may be standard COPY options)
	}

	// Fabric Data Warehouse has no nvarchar type at all, so the setting cannot be
	// honoured there and nvarchar(max) — the default everywhere else — is refused
	// by the server. Verified against a live warehouse.
	{
		auto &target_catalog =
			Catalog::GetCatalog(context, Identifier(bind_data->target.catalog_name)).Cast<MSSQLCatalog>();
		if (target_catalog.RequiresSingleByteText()) {
			bind_data->config.text_type_varchar = true;
		}
	}

	// Spec 060: stamp the session's default target type onto every unannotated
	// VARCHAR here, once, so table creation, the INSERT BULK declaration and the
	// encoder's length guard all read one annotation. A column that already
	// states its own type — from a cast or carried from an attached source by the
	// catalog — is left alone. The collation is filled in later by
	// ValidateTarget, which is where a connection exists to ask the server.
	for (auto &type : bind_data->source_types) {
		type = mssql::codec::ApplyDefaultStringType(type, !bind_data->config.text_type_varchar,
													bind_data->config.default_string_length, string());
	}

	CopyDebugLog(1, "BCPCopyBind: config flush_rows=%llu, create_table=%d, overwrite=%d, tablock=%d",
				 (unsigned long long)bind_data->config.flush_rows, bind_data->config.create_table ? 1 : 0,
				 bind_data->config.overwrite ? 1 : 0, bind_data->config.tablock ? 1 : 0);

	return std::move(bind_data);
}

//===----------------------------------------------------------------------===//
// BCPCopyInitGlobal - Acquire connection, send INSERT BULK, start BCP
//===----------------------------------------------------------------------===//

unique_ptr<GlobalFunctionData> BCPCopyInitGlobal(ClientContext &context, FunctionData &bind_data,
												 const string &file_path) {
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto gstate = make_uniq<MSSQLCopyGlobalState>();

	CopyDebugLog(1, "BCPCopyInitGlobal: starting for %s", bdata.target.GetFullyQualifiedName().c_str());

	// Get the MSSQLCatalog
	auto &catalog = Catalog::GetCatalog(context, Identifier(bdata.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// Check write access
	mssql_catalog.CheckWriteAccess("COPY TO");

	// Acquire a connection from the pool
	// For BCP, we need an exclusive connection that will remain in Executing state
	CopyDebugLog(2, "BCPCopyInitGlobal: acquiring connection from pool");
	gstate->connection = ConnectionProvider::GetConnection(context, mssql_catalog);
	if (!gstate->connection) {
		throw IOException("MSSQL COPY: Failed to acquire connection from pool");
	}
	CopyDebugLog(2, "BCPCopyInitGlobal: connection acquired");

	// Capture the destructor's release targets here, on the client thread — it must not touch a
	// ClientContext itself (issue #178 / PR #179). Covers the sink-throw path, where neither the
	// helper below nor BCPCopyFinalize runs (issue #191).
	gstate->pool_handle = mssql_catalog.GetConnectionPoolHandle();
	gstate->transaction_pinned = ConnectionProvider::IsInTransaction(context, mssql_catalog);
	gstate->reset_on_release = ConnectionProvider::ShouldResetOnRelease(context);

	// Helper to release connection on error
	auto release_connection_on_error = [&]() {
		if (gstate->connection) {
			CopyDebugLog(2, "BCPCopyInitGlobal: releasing connection due to error");
			// Ensure connection is in Idle state before releasing
			if (gstate->connection->GetState() == tds::ConnectionState::Executing) {
				gstate->connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
			}
			bool in_transaction = ConnectionProvider::IsInTransaction(context, mssql_catalog);
			if (in_transaction) {
				ConnectionProvider::ReleaseConnection(context, mssql_catalog, gstate->connection);
			} else {
				mssql_catalog.GetConnectionPool().Release(gstate->connection);
			}
			gstate->connection.reset();
		}
	};

	try {
		// Check connection is in Idle state
		if (gstate->connection->GetState() != tds::ConnectionState::Idle) {
			auto state = gstate->connection->GetState();
			string state_str = tds::ConnectionStateToString(state);

			// Provide specific error messages based on connection state
			if (state == tds::ConnectionState::Executing) {
				throw InvalidInputException(
					"MSSQL COPY: Connection is busy executing another query. "
					"This can happen if you're reading from an MSSQL table (via mssql_scan) "
					"and writing to the same MSSQL database within a transaction. "
					"Either: (1) Read data into a local table first, then COPY to MSSQL, or "
					"(2) Use separate transactions for reading and writing. "
					"Connection state: %s",
					state_str);
			} else {
				throw InvalidInputException(
					"MSSQL COPY: Connection is not ready for BCP operation (state: %s). "
					"The connection may be in an error state or performing another operation.",
					state_str);
			}
		}

		// Check if we're in a DuckDB transaction - warn about potential issues
		bool in_transaction = ConnectionProvider::IsInTransaction(context, mssql_catalog);
		if (in_transaction) {
			CopyDebugLog(1,
						 "BCPCopyInitGlobal: Running COPY within a transaction. "
						 "If COPY fails mid-stream, partial data may be committed. "
						 "For atomic bulk loads, ensure the COPY completes successfully before COMMIT.");
		}

		// Validate target and optionally create table
		TargetResolver::ValidateTarget(context, *gstate->connection, bdata.target, bdata.config, bdata.source_types,
									   bdata.source_names);

		// Point invalidation: invalidate schema's table list if table was created/dropped
		// This ensures the new table schema is visible for subsequent queries
		if (!bdata.target.IsTempTable() && (bdata.config.create_table || bdata.config.overwrite)) {
			mssql_catalog.InvalidateSchemaTableSet(bdata.target.schema_name);
			CopyDebugLog(2, "BCPCopyInitGlobal: schema '%s' table list invalidated after table creation/modification",
						 bdata.target.schema_name.c_str());
		}

		// Generate column metadata for BCP
		// For existing tables (not created or replaced), we MUST use the target table's column types
		// for COLMETADATA. The BCP protocol requires COLMETADATA to match the target table exactly.
		// For newly created tables, we use source types since the table was created from them.
		bool need_target_metadata = !bdata.config.overwrite;
		bool need_column_mapping = false;
		if (need_target_metadata) {
			// Try to get target column metadata - this will work for existing tables
			try {
				gstate->columns = TargetResolver::GetExistingTableColumnMetadata(*gstate->connection, bdata.target);
				CopyDebugLog(1, "BCPCopyInitGlobal: using target table column metadata (%llu columns)",
							 (unsigned long long)gstate->columns.size());

				// Columns whose pair bind admitted for the all-NULL case only
				// (constant `NULL AS col` sources — see BCPCopyConfig): mark the
				// target metadata so PrepareColumnStates verifies the mask per
				// chunk and routes them through the NullOnly path.
				for (const auto &nn : bdata.config.null_only_columns) {
					for (auto &col : gstate->columns) {
						if (StringUtil::Lower(col.name) == nn) {
							col.null_only_source = true;
						}
					}
				}

				// Build column mapping from source to target
				gstate->column_mapping = TargetResolver::BuildColumnMapping(bdata.source_names, gstate->columns);

				// Issue #125: Drop target columns that have no matching source column (by name).
				// These are columns the server fills itself — IDENTITY, DEFAULT-valued, or
				// computed columns the source intentionally omits. INSERT BULK only needs the
				// columns actually being loaded; SQL Server auto-generates the rest. Previously
				// every target column was declared, so an unmapped IDENTITY column had NULL
				// streamed into it and the load failed ("Cannot insert the value NULL into
				// column ..." / "Incorrect syntax near the keyword 'with'").
				{
					vector<BCPColumnMetadata> mapped_columns;
					vector<int32_t> mapped_mapping;
					mapped_columns.reserve(gstate->columns.size());
					mapped_mapping.reserve(gstate->column_mapping.size());
					for (idx_t i = 0; i < gstate->columns.size(); i++) {
						// column_mapping[i] is the source-column index feeding target column i,
						// or -1 when no source column matches it by name. Keep only the mapped
						// (>= 0) target columns; the -1 entries are server-generated (IDENTITY,
						// DEFAULT, computed) and must be left out of INSERT BULK entirely.
						if (gstate->column_mapping[i] >= 0) {
							mapped_columns.push_back(gstate->columns[i]);
							mapped_mapping.push_back(gstate->column_mapping[i]);
						} else {
							CopyDebugLog(1,
										 "BCPCopyInitGlobal: omitting target column '%s' from INSERT BULK "
										 "(no matching source column; server-generated, e.g. IDENTITY/DEFAULT)",
										 gstate->columns[i].name.c_str());
						}
					}
					if (mapped_columns.empty()) {
						throw InvalidInputException(
							"MSSQL COPY: no source columns match target table '%s' by name; "
							"nothing to load. Ensure source column names match the target's columns.",
							bdata.target.GetFullyQualifiedName());
					}
					gstate->columns = std::move(mapped_columns);
					gstate->column_mapping = std::move(mapped_mapping);
				}

				// Check if we need to use column mapping (i.e., not a 1:1 positional match)
				// Mapping is needed if: column counts differ, or any mapping != position
				need_column_mapping = (bdata.source_names.size() != gstate->columns.size());
				if (!need_column_mapping) {
					for (idx_t i = 0; i < gstate->column_mapping.size(); i++) {
						if (gstate->column_mapping[i] != static_cast<int32_t>(i)) {
							need_column_mapping = true;
							break;
						}
					}
				}

				if (need_column_mapping) {
					CopyDebugLog(1, "BCPCopyInitGlobal: using column mapping (source: %llu cols, target: %llu cols)",
								 (unsigned long long)bdata.source_names.size(),
								 (unsigned long long)gstate->columns.size());
				}
			} catch (...) {
				// If we can't get target metadata (e.g., table was just created), use source types
				gstate->columns = TargetResolver::GenerateColumnMetadata(bdata.source_types, bdata.source_names,
																		 bdata.config.wire_varchar_collation,
																		 bdata.config.text_type_varchar);
				CopyDebugLog(1, "BCPCopyInitGlobal: using source column metadata (%llu columns)",
							 (unsigned long long)gstate->columns.size());
			}
		} else {
			// Table was replaced or created, use source types
			gstate->columns = TargetResolver::GenerateColumnMetadata(bdata.source_types, bdata.source_names,
																	 bdata.config.wire_varchar_collation,
																	 bdata.config.text_type_varchar);
			CopyDebugLog(1, "BCPCopyInitGlobal: using source column metadata (table created/replaced)");
		}

		// TABLOCK by the target's shape (spec 057 step 1, replacing issue #45's
		// "new tables" rule). ValidateTarget above set target_shape — from
		// sys.indexes for an existing table, from what we just created otherwise.
		bdata.config.tablock = MSSQLResolveTablock(bdata.config.tablock_choice, bdata.config.target_shape);
		CopyDebugLog(1, "BCPCopyInitGlobal: TABLOCK=%d (choice=%d, target_shape=%d, new_table=%d)",
					 bdata.config.tablock ? 1 : 0, (int)bdata.config.tablock_choice, (int)bdata.config.target_shape,
					 bdata.config.is_new_table ? 1 : 0);

		// Build and execute INSERT BULK statement — one builder for every consumer
		// (spec 063 D4), which is what stops CTAS silently omitting ROWS_PER_BATCH.
		const string insert_bulk =
			BuildInsertBulkSql(bdata.target, gstate->columns, bdata.config.tablock, bdata.config.flush_rows);
		CopyDebugLog(2, "BCPCopyInitGlobal: INSERT BULK SQL: %s", insert_bulk.c_str());

		// Cache the INSERT BULK SQL for re-execution on batch flush
		gstate->insert_bulk_sql = insert_bulk;

		// Execute INSERT BULK to prepare server for bulk load
		auto result = MSSQLSimpleQuery::Execute(*gstate->connection, insert_bulk);
		if (!result.success) {
			throw InvalidInputException("MSSQL COPY: Failed to execute INSERT BULK: %s", result.error_message);
		}

		// Transition connection to Executing state for BCP
		if (!gstate->connection->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
			throw IOException("MSSQL COPY: Failed to transition connection to Executing state");
		}

		// Create BCP writer with optional column mapping
		gstate->writer =
			make_uniq<BCPWriter>(*gstate->connection, bdata.target, gstate->columns, gstate->column_mapping);

		// How many bulk-load sessions this COPY may open, and on whose connection
		// (spec 057 step 7; resolved by one shared function since spec 063 D1,
		// because COPY derived this inline and CTAS derived it again, differently).
		//
		// COPY answers JoinsTransaction: it may be loading into a table that
		// existed before the statement, whose rows nothing else can undo, so
		// inside a transaction the transaction has to own the load — and a second
		// writer would sit outside it and not roll back with the rest. A
		// correctness question, not a tuning one.
		{
			Value pw;
			int64_t configured = 0;
			if (context.TryGetCurrentSetting("mssql_copy_parallel_writers", pw)) {
				configured = pw.GetValue<int64_t>();
			}
			const auto policy = MSSQLResolveLoadPolicy(bdata.target.is_temp_table, gstate->transaction_pinned,
													   MSSQLLoadTransactionRole::JoinsTransaction, configured,
													   static_cast<uint64_t>(context.db->NumberOfThreads()));
			gstate->parallel_writer_limit = static_cast<idx_t>(policy.max_writers);
		}
		CopyDebugLog(1, "BCPCopyInitGlobal: parallel_writer_limit=%llu (pinned=%d, session_temp=%d)",
					 (unsigned long long)gstate->parallel_writer_limit, gstate->transaction_pinned ? 1 : 0,
					 bdata.target.is_temp_table ? 1 : 0);

		// Send COLMETADATA token to start the BCP stream
		gstate->writer->WriteColmetadata();

		CopyDebugLog(1, "BCPCopyInitGlobal: BCP stream started, ready to receive rows");

	} catch (...) {
		// Release connection on any error during initialization
		release_connection_on_error();
		throw;
	}

	return std::move(gstate);
}

//===----------------------------------------------------------------------===//
// BCPCopyInitLocal - Create per-thread buffer
//===----------------------------------------------------------------------===//

unique_ptr<LocalFunctionData> BCPCopyInitLocal(ExecutionContext &context, FunctionData &bind_data) {
	// No local buffering needed - we write directly to BCPWriter
	// This reduces memory usage significantly
	return make_uniq<MSSQLCopyLocalState>();
}

//===----------------------------------------------------------------------===//
// BCPCopySink - Accumulate rows and flush batches
//===----------------------------------------------------------------------===//

void BCPCopySink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
				 LocalFunctionData &lstate, DataChunk &input) {
	const bool counters = mssql::CountersEnabled();
	auto start_sink = counters ? Clock::now() : CopyTimePoint{};
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto &gdata = gstate.Cast<MSSQLCopyGlobalState>();

	if (input.size() == 0) {
		return;
	}

	// Check for interrupt (Ctrl+C) - allows user to cancel long-running COPY
	if (context.client.IsInterrupted()) {
		CopyDebugLog(1, "BCPCopySink: INTERRUPT detected at start");
		throw InterruptException();
	}

	// Check for errors
	if (gdata.has_error.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> error_lock(gdata.error_mutex);
		throw IOException("MSSQL COPY: Previous error occurred: %s", gdata.error_message);
	}

	CopyDebugLog(2, "BCPCopySink: encoding %llu rows...", (unsigned long long)input.size());

	auto &ldata = lstate.Cast<MSSQLCopyLocalState>();
	if (!ldata.init_attempted) {
		ldata.init_attempted = true;
		auto &catalog = Catalog::GetCatalog(context.client, Identifier(bdata.catalog_name));
		auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
		// The POOL, never ConnectionProvider: inside a transaction the provider
		// returns the PINNED connection, and two writers on one connection
		// interleave their ROW tokens into a single bulk load. The policy already
		// caps a pinned load at one writer, but this is what makes it structural.
		ldata.pool = &mssql_catalog.GetConnectionPool();
		ldata.pool_handle = mssql_catalog.GetConnectionPoolHandle();
		// Spec 070 W2: whether this thread may keep asking for its own writer on
		// later chunks. Below the limit of two there is only ever the shared
		// writer, so never ask.
		ldata.may_claim = ldata.pool != nullptr && gdata.parallel_writer_limit > 1;
	}

	// Spec 070 W2: try (or re-try) to claim an own writer while the volume gate
	// permits it. A thread that shares the writer keeps this alive so a large
	// load reaches the full writer count even though every thread arrived early;
	// a small load never opens the gate and stays on one writer, keeping its
	// batches compressible.
	if (ldata.may_claim && !ldata.session.IsOwned()) {
		BulkLoadSessionParams params;
		params.pool = ldata.pool;
		params.pool_handle = ldata.pool_handle;
		params.insert_bulk_sql = &gdata.insert_bulk_sql;
		params.target = &bdata.target;
		params.columns = &gdata.columns;
		params.column_mapping = &gdata.column_mapping;
		params.flush_rows = bdata.config.flush_rows;
		params.collect_timings = counters;
		params.reset_on_release = gdata.reset_on_release;
		// W2 warm-up only for a columnstore target — a heap load has no
		// compression to protect and fans out immediately.
		params.warmup_gate = bdata.config.target_shape == MSSQLIndexKind::CLUSTERED_COLUMNSTORE;
		switch (
			ldata.session.TryStart(params, gdata.parallel_writers_used, gdata.parallel_writer_limit, gdata.rows_sent)) {
		case BulkLoadSession::Claim::Started:
			CopyDebugLog(1, "BCPCopySink: parallel writer started (used=%llu/%llu, rows so far=%llu)",
						 (unsigned long long)gdata.parallel_writers_used.load(),
						 (unsigned long long)gdata.parallel_writer_limit, (unsigned long long)gdata.rows_sent.load());
			break;
		case BulkLoadSession::Claim::GateClosed:
			// The warm-up gate is not open yet — keep may_claim and ask again on a
			// later chunk, once the shared writer has sunk its first batch.
			break;
		case BulkLoadSession::Claim::Unavailable:
			// Cap reached or acquisition failed — no later chunk changes that.
			// Stop asking, so this thread does not re-block Acquire() every chunk.
			ldata.may_claim = false;
			break;
		}
	}

	try {
		// A thread with its own session writes without touching the shared writer
		// at all — no mutex, no shared accumulator. Threads that did not get one
		// share the global writer exactly as before.
		if (ldata.session.IsOwned()) {
			const auto written = ldata.session.Write(input);
			gdata.rows_sent.fetch_add(written.rows_written);
			if (written.flushed) {
				gdata.rows_confirmed.fetch_add(written.rows_confirmed);
				gdata.batches_flushed.fetch_add(1);
			}
			if (counters) {
				gdata.counter_sink_calls.fetch_add(1, std::memory_order_relaxed);
				gdata.counter_sink_ns.fetch_add(ElapsedNs(start_sink), std::memory_order_relaxed);
				gdata.counter_encode_ns.fetch_add(written.encode_ns, std::memory_order_relaxed);
				gdata.counter_flush_ns.fetch_add(written.flush_ns, std::memory_order_relaxed);
			}
			if (context.client.IsInterrupted()) {
				throw InterruptException();
			}
			return;
		}

		// SHARED writer: every thread that did not get its own session appends to
		// this one, so the append and the batch flush must be under the SAME lock.
		//
		// They were not, and it silently lost rows the moment the sink became
		// parallel: WriteRows takes BCPWriter's own internal mutex while the flush
		// took gdata.write_mutex, so a flush could send and clear the accumulator
		// while another thread was still appending to it. Measured at 205376 rows
		// arriving out of 1000000 — no error anywhere, on either side.
		std::unique_lock<std::mutex> shared_lock(gdata.write_mutex);
		auto start_write = counters ? Clock::now() : CopyTimePoint{};
		idx_t rows_written = gdata.writer->WriteRows(input);
		const uint64_t encode_ns = counters ? ElapsedNs(start_write) : 0;
		gdata.rows_sent.fetch_add(rows_written);

		CopyDebugLog(2, "BCPCopySink: encoded %llu rows in %.3f ms, checking flush...",
					 (unsigned long long)rows_written, encode_ns / 1e6);

		// Check for interrupt after encoding
		if (context.client.IsInterrupted()) {
			CopyDebugLog(1, "BCPCopySink: INTERRUPT detected after encoding");
			throw InterruptException();
		}

		// Check if we should flush to SQL Server
		uint64_t flush_ns = 0;
		if (bdata.config.ShouldFlushToServer(gdata.writer->GetRowsInCurrentBatch())) {
			CopyDebugLog(1, "BCPCopySink: triggering server flush (rows_in_batch=%llu, threshold=%llu)...",
						 (unsigned long long)gdata.writer->GetRowsInCurrentBatch(),
						 (unsigned long long)bdata.config.flush_rows);
			auto start_flush = counters ? Clock::now() : CopyTimePoint{};
			// Already held from the append above — the two must not be separable.
			FlushToServer(gdata, bdata);
			flush_ns = counters ? ElapsedNs(start_flush) : 0;
			CopyDebugLog(1, "BCPCopySink: server flush completed in %.2f ms", flush_ns / 1e6);
		}

		// Check for interrupt after flush
		if (context.client.IsInterrupted()) {
			CopyDebugLog(1, "BCPCopySink: INTERRUPT detected after flush");
			throw InterruptException();
		}

		// Accumulate, do NOT log. This was a CopyDebugLog(1, "DONE ...") per chunk,
		// and CopyDebugLog fflush'es: ~245 fprintf+fflush inside the timed path on
		// a 500k-row load, inflating the client CPU it reported ~4x. The summary is
		// printed once in BCPCopyFinalize instead.
		if (counters) {
			gdata.counter_sink_calls.fetch_add(1, std::memory_order_relaxed);
			gdata.counter_sink_ns.fetch_add(ElapsedNs(start_sink), std::memory_order_relaxed);
			gdata.counter_encode_ns.fetch_add(encode_ns, std::memory_order_relaxed);
			gdata.counter_flush_ns.fetch_add(flush_ns, std::memory_order_relaxed);
		}
	} catch (std::exception &e) {
		// Record the error for finalize to handle cleanup
		CopyDebugLog(1, "BCPCopySink: ERROR - %s", e.what());
		{
			// First failure wins: with N writers, one broken load fails them all,
			// and the first message is the one that explains it.
			std::lock_guard<std::mutex> error_lock(gdata.error_mutex);
			if (gdata.error_message.empty()) {
				gdata.error_message = e.what();
			}
		}
		gdata.has_error.store(true, std::memory_order_release);
		throw;
	}
}

//===----------------------------------------------------------------------===//
// FlushToServer - Flush accumulated data to SQL Server
//===----------------------------------------------------------------------===//

static void FlushToServer(MSSQLCopyGlobalState &gdata, const MSSQLCopyBindData &bdata) {
	auto start_total = Clock::now();
	idx_t rows_in_batch = gdata.writer->GetRowsInCurrentBatch();
	if (rows_in_batch == 0) {
		return;
	}

	idx_t total_sent = gdata.rows_sent.load();
	CopyDebugLog(1, "FlushToServer: flushing batch %llu: %llu rows (total: %llu), buffer: %zu MB",
				 (unsigned long long)(gdata.batches_flushed.load() + 1), (unsigned long long)rows_in_batch,
				 (unsigned long long)total_sent, gdata.writer->GetAccumulatorSize() / (1024 * 1024));

	try {
		// Flush the current batch - this sends DONE token and reads response
		auto start_flush = Clock::now();
		CopyDebugLog(1, "FlushToServer: >> Sending data to server...");
		idx_t confirmed = gdata.writer->FlushBatch(rows_in_batch);
		double flush_ms = ElapsedMs(start_flush);
		CopyDebugLog(1, "FlushToServer: >> Server confirmed %llu rows in %.2f ms", (unsigned long long)confirmed,
					 flush_ms);
		gdata.rows_confirmed.fetch_add(confirmed);
		gdata.batches_flushed.fetch_add(1);

		CopyDebugLog(1, "FlushToServer: batch %llu confirmed %llu rows, total confirmed: %llu",
					 (unsigned long long)gdata.batches_flushed.load(), (unsigned long long)confirmed,
					 (unsigned long long)gdata.rows_confirmed.load());

		// Re-execute INSERT BULK to prepare for next batch
		auto start_insert = Clock::now();
		CopyDebugLog(1, "FlushToServer: >> Re-executing INSERT BULK...");
		auto result = MSSQLSimpleQuery::Execute(*gdata.connection, gdata.insert_bulk_sql);
		double insert_ms = ElapsedMs(start_insert);
		CopyDebugLog(1, "FlushToServer: >> INSERT BULK done in %.2f ms", insert_ms);
		if (!result.success) {
			throw InvalidInputException("MSSQL COPY: Failed to re-execute INSERT BULK: %s", result.error_message);
		}

		// Transition connection back to Executing state for BCP
		if (!gdata.connection->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
			throw IOException("MSSQL COPY: Failed to transition connection to Executing state");
		}

		// Reset writer for next batch
		auto start_reset = Clock::now();
		gdata.writer->ResetForNextBatch();
		gdata.writer->WriteColmetadata();
		double reset_ms = ElapsedMs(start_reset);

		double total_ms = ElapsedMs(start_total);
		double rows_per_sec = (total_ms > 0) ? (rows_in_batch * 1000.0 / total_ms) : 0;
		CopyDebugLog(1,
					 "FlushToServer: DONE batch %llu - %llu rows in %.2f ms (flush: %.2f, INSERT BULK: %.2f, reset: "
					 "%.2f) | %.0f rows/s",
					 (unsigned long long)gdata.batches_flushed.load(), (unsigned long long)confirmed, total_ms,
					 flush_ms, insert_ms, reset_ms, rows_per_sec);

	} catch (std::exception &e) {
		{
			// First failure wins: with N writers, one broken load fails them all,
			// and the first message is the one that explains it.
			std::lock_guard<std::mutex> error_lock(gdata.error_mutex);
			if (gdata.error_message.empty()) {
				gdata.error_message = e.what();
			}
		}
		gdata.has_error.store(true, std::memory_order_release);
		throw;
	}
}

//===----------------------------------------------------------------------===//
// BCPCopyCombine - Flush remaining local buffer
//===----------------------------------------------------------------------===//

void BCPCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
					LocalFunctionData &lstate) {
	// The shared writer has no local buffering to flush — rows go straight into
	// it in BCPCopySink. A thread that got its OWN bulk-load session closes it
	// here: DONE, the server's confirmation, connection back to the pool.
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto &gdata = gstate.Cast<MSSQLCopyGlobalState>();
	auto &ldata = lstate.Cast<MSSQLCopyLocalState>();
	if (!ldata.session.IsOwned()) {
		return;
	}
	try {
		const idx_t batches = ldata.session.BatchesFlushed();
		const idx_t confirmed = ldata.session.Finish();
		gdata.rows_confirmed.fetch_add(confirmed);
		// The final batch, which Finish() closed with DONE rather than FlushBatch.
		if (ldata.session.BatchesFlushed() > batches) {
			gdata.batches_flushed.fetch_add(1);
		}
	} catch (std::exception &e) {
		// Recorded AND rethrown, which the two halves need for different reasons:
		// the record is what makes first-failure-wins work across N writers, and
		// the rethrow is what actually fails the statement — Combine is the last
		// place this thread can. The connection is released by the local state's
		// destructor on either path (the #191 contract).
		CopyDebugLog(1, "BCPCopyCombine: parallel writer failed to finish - %s", e.what());
		{
			// First failure wins: with N writers, one broken load fails them all,
			// and the first message is the one that explains it.
			std::lock_guard<std::mutex> error_lock(gdata.error_mutex);
			if (gdata.error_message.empty()) {
				gdata.error_message = e.what();
			}
		}
		gdata.has_error.store(true, std::memory_order_release);
		throw;
	}
}

//===----------------------------------------------------------------------===//
// PrintWriteCounters — the write path's close summary (spec 057 step 0b)
//
// The read path's peer is MSSQLResultStream::PrintDebugCounters. Printed once,
// from the finalize the statement already runs, so nothing is logged from inside
// a timed phase.
//
// What the split does and does not cover, stated because the numbers get quoted:
//
//   encode  BCPRowEncoder + accumulator append. This is the client CPU the
//           columnar work (step 3) attacks.
//   flush   a mid-statement batch boundary, END TO END: packet build, framing,
//           send AND the server's confirmation. It is NOT server time alone —
//           separating those needs a timer inside BCPWriter::FlushBatch, which
//           step 4 adds when it has a reason to. Do not quote it as "waiting for
//           the server".
//   other   everything else in the sink; expected to be ~0, and a useful tripwire
//           if it ever is not.
//
// The FINAL batch is confirmed in BCPCopyFinalize, after the sink has run for the
// last time, so it is NOT in `flush` — with the default flush_rows that is one
// whole batch sitting outside this summary.
//===----------------------------------------------------------------------===//

// Must run before gdata.writer.reset(), which happens on every exit path.
static void SnapshotWriterCounters(MSSQLCopyGlobalState &gdata) {
	if (!gdata.writer) {
		return;
	}
	gdata.counter_build_send_ns = gdata.writer->GetBuildSendNs();
	gdata.counter_server_wait_ns = gdata.writer->GetServerWaitNs();
	gdata.counter_send_calls = gdata.writer->GetSendCalls();
}

static void PrintWriteCounters(MSSQLCopyGlobalState &gdata, idx_t rows) {
	if (!mssql::CountersEnabled()) {
		return;
	}
	if (mssql::CountersConfoundedByLogging()) {
		fprintf(stderr,
				"[MSSQL COUNTERS] NOTE: MSSQL_DEBUG is set, and its logging runs inside the phases timed "
				"below — these numbers include it. For timings use MSSQL_COUNTERS=1 with MSSQL_DEBUG unset.\n");
	}

	const uint64_t build_send_ns = gdata.counter_build_send_ns;
	const uint64_t server_wait_ns = gdata.counter_server_wait_ns;
	const idx_t send_calls = gdata.counter_send_calls;
	const uint64_t sink_ns = gdata.counter_sink_ns.load();
	const uint64_t encode_ns = gdata.counter_encode_ns.load();
	const uint64_t flush_ns = gdata.counter_flush_ns.load();
	const uint64_t other_ns = sink_ns > encode_ns + flush_ns ? sink_ns - encode_ns - flush_ns : 0;
	const idx_t chunks = gdata.counter_sink_calls.load();
	const idx_t cols = gdata.columns.size();

	// The phase totals below are SUMMED ACROSS THREADS, not wall clock: with
	// N writers they can exceed the elapsed time of the statement, and a
	// rise from 1 to N threads does NOT mean it got slower. Said in the
	// output too, because these numbers get quoted.
	fprintf(stderr, "[MSSQL COUNTERS]   (phase totals are summed across threads, not wall clock)\n");
	fprintf(stderr,
			"[MSSQL COUNTERS] copy close: rows=%llu cols=%llu chunks=%llu batches=%llu writers=%llu/%llu | "
			"sink=%lluus (encode=%lluus flush=%lluus other=%lluus)\n",
			(unsigned long long)rows, (unsigned long long)cols, (unsigned long long)chunks,
			(unsigned long long)gdata.batches_flushed.load(), (unsigned long long)gdata.parallel_writers_used.load(),
			(unsigned long long)gdata.parallel_writer_limit, (unsigned long long)(sink_ns / 1000),
			(unsigned long long)(encode_ns / 1000), (unsigned long long)(flush_ns / 1000),
			(unsigned long long)(other_ns / 1000));

	// The decomposition of `flush`, counted in the primitives so the FINAL batch —
	// which BCPCopyFinalize sends through WriteDone + Finalize rather than through
	// FlushBatch — is included. Without this split, build+send and the server's
	// confirmation are one number and steps 3 and 4 cannot be ranked against each
	// other (spec 057, step 1 gate).
	fprintf(stderr, "[MSSQL COUNTERS]   wire: build_send=%lluus over %llu sends | server_wait=%lluus\n",
			(unsigned long long)(build_send_ns / 1000), (unsigned long long)send_calls,
			(unsigned long long)(server_wait_ns / 1000));

	if (rows > 0) {
		const double r = static_cast<double>(rows);
		fprintf(stderr, "[MSSQL COUNTERS]   ns/row: sink=%.1f encode=%.1f flush=%.1f other=%.1f\n", sink_ns / r,
				encode_ns / r, flush_ns / r, other_ns / r);
		fprintf(stderr, "[MSSQL COUNTERS]   ns/row: build_send=%.1f server_wait=%.1f\n", build_send_ns / r,
				server_wait_ns / r);
		if (cols > 0) {
			const double v = r * static_cast<double>(cols);
			fprintf(stderr, "[MSSQL COUNTERS]   ns/value: sink=%.2f encode=%.2f other=%.2f\n", sink_ns / v,
					encode_ns / v, other_ns / v);
		}
	}
	// Encode-path attribution (spec 064). The three shapes are picked PER CHUNK
	// and nothing used to report which, so "did this table reach the fast path?"
	// could not be answered from outside the process.
	{
		auto &pc = mssql::GetEncodePathCounters();
		const uint64_t st = pc.chunks_strided.load(std::memory_order_relaxed);
		const uint64_t cu = pc.chunks_cursor.load(std::memory_order_relaxed);
		const uint64_t rm = pc.chunks_row_major.load(std::memory_order_relaxed);
		if (st + cu + rm > 0) {
			fprintf(stderr, "[MSSQL COUNTERS]   encode path (chunks): strided=%llu cursor=%llu row_major=%llu\n",
					(unsigned long long)st, (unsigned long long)cu, (unsigned long long)rm);
			if (rm > 0) {
				fprintf(stderr,
						"[MSSQL COUNTERS]   row-major cause: unsupported_pair=%llu string_plan=%llu "
						"(last: column %llu, arm %llu)\n",
						(unsigned long long)pc.fallback_unsupported_pair.load(std::memory_order_relaxed),
						(unsigned long long)pc.fallback_string_plan.load(std::memory_order_relaxed),
						(unsigned long long)pc.last_fallback_column.load(std::memory_order_relaxed),
						(unsigned long long)pc.last_fallback_arm.load(std::memory_order_relaxed));
			}
		}
	}
}

//===----------------------------------------------------------------------===//
// BCPCopyFinalize - Send DONE token, read response
//===----------------------------------------------------------------------===//

void BCPCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto &gdata = gstate.Cast<MSSQLCopyGlobalState>();

	// Check for interrupt before starting heavy finalize
	if (context.IsInterrupted()) {
		throw InterruptException();
	}

	CopyDebugLog(1, "BCPCopyFinalize: completing BCP stream");

	// Get catalog early for potential cleanup
	auto &catalog = Catalog::GetCatalog(context, Identifier(bdata.catalog_name));
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();
	bool in_transaction = ConnectionProvider::IsInTransaction(context, mssql_catalog);

	// Helper lambda for cleanup on error
	auto cleanup_on_error = [&](const string &error_msg) {
		CopyDebugLog(1, "BCPCopyFinalize: ERROR - %s", error_msg.c_str());

		// Try to clean up connection state
		if (gdata.connection) {
			gdata.connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);

			// Try to send ATTENTION to cancel any pending operation
			try {
				// Note: In a real implementation, we might send an ATTENTION packet here
				// For now, just transition the state
			} catch (...) {
				// Ignore cleanup errors
			}

			// Release the connection
			if (bdata.target.IsTempTable() && in_transaction) {
				// Keep pinned for transaction cleanup
				ConnectionProvider::ReleaseConnection(context, mssql_catalog, gdata.connection);
			} else {
				mssql_catalog.GetConnectionPool().Release(gdata.connection);
			}
			gdata.connection.reset();
		}

		// Release the writer
		SnapshotWriterCounters(gdata);
		gdata.writer.reset();
	};

	if (gdata.has_error.load(std::memory_order_acquire)) {
		std::unique_lock<std::mutex> error_lock(gdata.error_mutex);
		string error_msg = gdata.error_message;
		error_lock.unlock();
		cleanup_on_error(error_msg);

		if (in_transaction) {
			throw IOException(
				"MSSQL COPY: Error during copy: %s. "
				"You are in a transaction - use ROLLBACK to discard any partial changes, "
				"or COMMIT if you want to keep any rows that were successfully inserted before the error.",
				error_msg);
		} else {
			throw IOException("MSSQL COPY: Error during copy: %s", error_msg);
		}
	}

	idx_t total_rows = gdata.rows_sent.load();
	idx_t rows_in_final_batch = gdata.writer->GetRowsInCurrentBatch();
	idx_t previously_confirmed = gdata.rows_confirmed.load();

	CopyDebugLog(1, "BCPCopyFinalize: total_rows=%llu, previously_confirmed=%llu, rows_in_final_batch=%llu",
				 (unsigned long long)total_rows, (unsigned long long)previously_confirmed,
				 (unsigned long long)rows_in_final_batch);

	try {
		// Send final DONE token and finalize the BCP stream
		// Note: We must ALWAYS send DONE and finalize, even if rows_in_final_batch == 0.
		// After intermediate flushes, we restart BCP with ExecuteBatch + WriteColmetadata,
		// leaving the connection in Executing state. We need DONE to close the stream
		// and transition back to Idle so the connection can be reused.
		if (rows_in_final_batch > 0) {
			CopyDebugLog(1, "BCPCopyFinalize: sending final batch: %llu rows, buffer: %zu MB",
						 (unsigned long long)rows_in_final_batch, gdata.writer->GetAccumulatorSize() / (1024 * 1024));
		} else {
			CopyDebugLog(1, "BCPCopyFinalize: sending empty DONE to close BCP stream");
		}

		// Send DONE token for the final batch (even if 0 rows)
		gdata.writer->WriteDone(rows_in_final_batch);

		CopyDebugLog(1, "BCPCopyFinalize: data sent, waiting for SQL Server to process...");

		// Read server response and get confirmed row count
		idx_t final_batch_confirmed = gdata.writer->Finalize();
		gdata.rows_confirmed.fetch_add(final_batch_confirmed);

		CopyDebugLog(1, "BCPCopyFinalize: final batch confirmed %llu rows", (unsigned long long)final_batch_confirmed);

		idx_t total_confirmed = gdata.rows_confirmed.load();
		idx_t batches = gdata.batches_flushed.load() + (rows_in_final_batch > 0 ? 1 : 0);

		CopyDebugLog(1, "BCPCopyFinalize: server confirmed %llu total rows in %llu batches (sent: %llu)",
					 (unsigned long long)total_confirmed, (unsigned long long)batches, (unsigned long long)total_rows);

		// Verify row counts match (warning only, server is authoritative)
		if (total_confirmed != total_rows) {
			CopyDebugLog(1, "WARNING: Row count mismatch - sent %llu, confirmed %llu", (unsigned long long)total_rows,
						 (unsigned long long)total_confirmed);
		}

	} catch (std::exception &e) {
		string error_msg = e.what();
		cleanup_on_error(error_msg);

		if (in_transaction) {
			throw IOException(
				"MSSQL COPY: Failed to finalize BCP stream: %s. "
				"Some rows may have been inserted before the failure. "
				"Use ROLLBACK to discard partial changes.",
				error_msg);
		} else {
			throw IOException("MSSQL COPY: Failed to finalize BCP stream: %s", error_msg);
		}
	}

	// Release the writer
	SnapshotWriterCounters(gdata);
	gdata.writer.reset();

	// Note: BCPWriter::Finalize() already transitions connection back to Idle state

	// Handle connection release based on transaction state
	if (in_transaction) {
		// In a transaction, keep connection pinned so subsequent operations
		// (queries, COPY, DML) use the same transaction context.
		// ConnectionProvider::ReleaseConnection is a no-op when in a transaction.
		if (bdata.target.IsTempTable()) {
			CopyDebugLog(1, "BCPCopyFinalize: temp table '%s' - connection stays pinned to transaction",
						 bdata.target.table_name.c_str());
		} else {
			CopyDebugLog(1, "BCPCopyFinalize: connection stays pinned to transaction");
		}
		ConnectionProvider::ReleaseConnection(context, mssql_catalog, gdata.connection);
	} else {
		// Not in transaction - release connection back to pool
		if (bdata.target.IsTempTable()) {
			CopyDebugLog(1,
						 "WARNING: COPY to temp table '%s' in auto-commit mode. "
						 "Temp table will be dropped when connection is released. "
						 "Use BEGIN TRANSACTION to keep the temp table accessible.",
						 bdata.target.table_name.c_str());
		}
		mssql_catalog.GetConnectionPool().Release(gdata.connection);
	}

	gdata.connection.reset();

	idx_t final_confirmed = gdata.rows_confirmed.load();
	CopyDebugLog(1, "BCPCopyFinalize: COPY completed successfully, %llu rows transferred",
				 (unsigned long long)final_confirmed);

	PrintWriteCounters(gdata, final_confirmed);
}

//===----------------------------------------------------------------------===//
// BCPCopyExecutionMode - Determine execution mode
//===----------------------------------------------------------------------===//

CopyFunctionExecutionMode BCPCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	// Parallel (spec 057 step 7). This only tells DuckDB it MAY call the sink from
	// several threads; whether those threads actually load in parallel is decided
	// per thread in BCPCopySink, which is where the global state — and therefore
	// the writer limit and the transaction pin — is reachable. A thread that does
	// not get its own session shares the global writer under its mutex, exactly as
	// before, so this is safe even when the limit is 1.
	//
	// `preserve_insertion_order` is not honoured and cannot be: the rows land in a
	// SQL Server heap or index, whose order is the server's to choose. It means
	// something for COPY TO a file and nothing for a table.
	return CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
}

}  // namespace mssql
}  // namespace duckdb

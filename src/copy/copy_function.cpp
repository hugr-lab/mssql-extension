#include "copy/copy_function.hpp"

#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "copy/bcp_writer.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
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
	fflush(stderr);  // Ensure output is flushed before potential crash
	va_end(args);
}

// High-resolution timer for performance analysis
using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;

static double ElapsedMs(TimePoint start) {
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
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
	// TABLOCK: Use table-level lock for better performance (default: true)
	copy_options["tablock"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
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

namespace mssql {

// Forward declarations
static void FlushToServer(MSSQLCopyGlobalState &gdata, const MSSQLCopyBindData &bdata);

//===----------------------------------------------------------------------===//
// BCPCopyBind - Parse target URL and options
//===----------------------------------------------------------------------===//

unique_ptr<FunctionData> BCPCopyBind(ClientContext &context, CopyFunctionBindInput &input, const vector<string> &names,
									 const vector<LogicalType> &sql_types) {
	auto bind_data = make_uniq<MSSQLCopyBindData>();

	// Store source schema info
	bind_data->source_types = sql_types;
	bind_data->source_names = names;

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
		// Try catalog syntax: catalog.schema.table or catalog.table
		vector<string> parts = StringUtil::Split(target_path, '.');

		if (parts.size() < 2 || parts.size() > 3) {
			throw InvalidInputException(
				"MSSQL COPY: Invalid target format. Use either:\n"
				"  - URL syntax: 'mssql://<catalog>/<schema>/<table>'\n"
				"  - Catalog syntax: <catalog>.<schema>.<table> or <catalog>.<table>\n"
				"Got: %s",
				target_path);
		}

		string catalog_name = parts[0];
		string schema_name;
		string table_name;

		if (parts.size() == 2) {
			// catalog.table - use default schema 'dbo'
			schema_name = "dbo";
			table_name = parts[1];
		} else {
			// catalog.schema.table
			schema_name = parts[1];
			table_name = parts[2];
		}

		// Verify catalog exists and is an MSSQL catalog
		try {
			auto &catalog = Catalog::GetCatalog(context, catalog_name);
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
		bind_data->target = TargetResolver::ResolveCatalog(context, catalog_name, schema_name, table_name);

		CopyDebugLog(1, "BCPCopyBind: resolved catalog syntax: catalog='%s', schema='%s', table='%s'",
					 catalog_name.c_str(), schema_name.c_str(), table_name.c_str());
	}

	bind_data->catalog_name = bind_data->target.catalog_name;

	// Load config from settings FIRST as defaults
	bind_data->config = LoadBCPCopyConfig(context);

	// Then let COPY options override
	CopyDebugLog(2, "BCPCopyBind: parsing %llu options", (unsigned long long)input.info.options.size());
	for (auto &option : input.info.options) {
		auto loption = StringUtil::Lower(option.first);
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
			bind_data->config.tablock = BooleanValue::Get(option.second[0]);
		}
		// Ignore unknown options (may be standard COPY options)
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
	auto &catalog = Catalog::GetCatalog(context, bdata.catalog_name);
	auto &mssql_catalog = catalog.Cast<MSSQLCatalog>();

	// Check write access
	mssql_catalog.CheckWriteAccess("COPY TO");

	// Acquire a connection from the pool
	// For BCP, we need an exclusive connection that will remain in Executing state
	gstate->connection = ConnectionProvider::GetConnection(context, mssql_catalog);
	if (!gstate->connection) {
		throw IOException("MSSQL COPY: Failed to acquire connection from pool");
	}

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

	// Invalidate catalog cache if table was created/dropped (non-temp tables only)
	// This ensures the new table schema is visible for subsequent queries
	if (!bdata.target.IsTempTable() && (bdata.config.create_table || bdata.config.overwrite)) {
		mssql_catalog.InvalidateMetadataCache();
		CopyDebugLog(2, "BCPCopyInitGlobal: catalog cache invalidated after table creation/modification");
	}

	// Generate column metadata for BCP
	gstate->columns = TargetResolver::GenerateColumnMetadata(bdata.source_types, bdata.source_names);

	// Build and execute INSERT BULK statement
	// This prepares the server to receive BulkLoad packets
	// For temp tables, use just the table name (not schema-qualified)
	string target_name;
	if (bdata.target.IsTempTable()) {
		target_name = bdata.target.GetBracketedTable();
	} else {
		target_name = bdata.target.GetFullyQualifiedName();
	}
	string insert_bulk = "INSERT BULK " + target_name + " (";
	for (idx_t i = 0; i < gstate->columns.size(); i++) {
		if (i > 0) {
			insert_bulk += ", ";
		}
		insert_bulk += "[" + gstate->columns[i].name + "] ";
		insert_bulk += TargetResolver::GetSQLServerTypeDeclaration(gstate->columns[i].duckdb_type);
	}
	insert_bulk += ")";

	// Add bulk load hints for performance
	// TABLOCK: Table-level lock instead of row locks (15-30% faster, enables minimal logging)
	// ROWS_PER_BATCH: Helps SQL Server optimize batch processing
	if (bdata.config.tablock) {
		insert_bulk += " WITH (TABLOCK";
		if (bdata.config.flush_rows > 0) {
			insert_bulk += ", ROWS_PER_BATCH = " + std::to_string(bdata.config.flush_rows);
		}
		insert_bulk += ")";
	} else if (bdata.config.flush_rows > 0) {
		insert_bulk += " WITH (ROWS_PER_BATCH = " + std::to_string(bdata.config.flush_rows) + ")";
	}

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

	// Create BCP writer
	gstate->writer = make_uniq<BCPWriter>(*gstate->connection, bdata.target, gstate->columns);

	// Send COLMETADATA token to start the BCP stream
	gstate->writer->WriteColmetadata();

	CopyDebugLog(1, "BCPCopyInitGlobal: BCP stream started, ready to receive rows");

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
	auto start_sink = Clock::now();
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto &gdata = gstate.Cast<MSSQLCopyGlobalState>();

	if (input.size() == 0) {
		return;
	}

	// Check for interrupt (Ctrl+C) - allows user to cancel long-running COPY
	if (context.client.interrupted) {
		CopyDebugLog(1, "BCPCopySink: INTERRUPT detected at start");
		throw InterruptException();
	}

	// Check for errors
	if (gdata.has_error) {
		throw IOException("MSSQL COPY: Previous error occurred: %s", gdata.error_message);
	}

	CopyDebugLog(2, "BCPCopySink: encoding %llu rows...", (unsigned long long)input.size());

	// Write directly to the BCPWriter (no local buffering to reduce memory)
	// This is thread-safe because BCPWriter::WriteRows has its own mutex
	auto start_write = Clock::now();
	idx_t rows_written = gdata.writer->WriteRows(input);
	double write_ms = ElapsedMs(start_write);
	gdata.rows_sent.fetch_add(rows_written);

	CopyDebugLog(2, "BCPCopySink: encoded %llu rows in %.2f ms, checking flush...",
				 (unsigned long long)rows_written, write_ms);

	// Check for interrupt after encoding
	if (context.client.interrupted) {
		CopyDebugLog(1, "BCPCopySink: INTERRUPT detected after encoding");
		throw InterruptException();
	}

	// Check if we should flush to SQL Server
	double flush_ms = 0;
	if (bdata.config.ShouldFlushToServer(gdata.writer->GetRowsInCurrentBatch())) {
		CopyDebugLog(1, "BCPCopySink: triggering server flush (rows_in_batch=%llu, threshold=%llu)...",
					 (unsigned long long)gdata.writer->GetRowsInCurrentBatch(),
					 (unsigned long long)bdata.config.flush_rows);
		auto start_flush = Clock::now();
		std::lock_guard<std::mutex> lock(gdata.write_mutex);
		// Double-check after acquiring lock
		if (bdata.config.ShouldFlushToServer(gdata.writer->GetRowsInCurrentBatch())) {
			FlushToServer(gdata, bdata);
		}
		flush_ms = ElapsedMs(start_flush);
		CopyDebugLog(1, "BCPCopySink: server flush completed in %.2f ms", flush_ms);
	}

	// Check for interrupt after flush
	if (context.client.interrupted) {
		CopyDebugLog(1, "BCPCopySink: INTERRUPT detected after flush");
		throw InterruptException();
	}

	double total_ms = ElapsedMs(start_sink);
	if (GetCopyDebugLevel() >= 1) {
		double rows_per_sec = (total_ms > 0) ? (rows_written * 1000.0 / total_ms) : 0;
		CopyDebugLog(1, "BCPCopySink: DONE - %llu rows in %.2f ms (write: %.2f, flush: %.2f) | %.0f rows/s | total sent: %llu",
					 (unsigned long long)rows_written, total_ms, write_ms, flush_ms, rows_per_sec,
					 (unsigned long long)gdata.rows_sent.load());
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
		CopyDebugLog(1, "FlushToServer: >> Server confirmed %llu rows in %.2f ms", (unsigned long long)confirmed, flush_ms);
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
		CopyDebugLog(1, "FlushToServer: DONE batch %llu - %llu rows in %.2f ms (flush: %.2f, INSERT BULK: %.2f, reset: %.2f) | %.0f rows/s",
					 (unsigned long long)gdata.batches_flushed.load(), (unsigned long long)confirmed,
					 total_ms, flush_ms, insert_ms, reset_ms, rows_per_sec);

	} catch (std::exception &e) {
		gdata.has_error = true;
		gdata.error_message = e.what();
		throw;
	}
}

//===----------------------------------------------------------------------===//
// BCPCopyCombine - Flush remaining local buffer
//===----------------------------------------------------------------------===//

void BCPCopyCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
					LocalFunctionData &lstate) {
	// No local buffering - nothing to flush
	// All rows are written directly to BCPWriter in BCPCopySink
}

//===----------------------------------------------------------------------===//
// BCPCopyFinalize - Send DONE token, read response
//===----------------------------------------------------------------------===//

void BCPCopyFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &bdata = bind_data.Cast<MSSQLCopyBindData>();
	auto &gdata = gstate.Cast<MSSQLCopyGlobalState>();

	// Check for interrupt before starting heavy finalize
	if (context.interrupted) {
		throw InterruptException();
	}

	CopyDebugLog(1, "BCPCopyFinalize: completing BCP stream");

	// Get catalog early for potential cleanup
	auto &catalog = Catalog::GetCatalog(context, bdata.catalog_name);
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
		gdata.writer.reset();
	};

	if (gdata.has_error) {
		string error_msg = gdata.error_message;
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
		// Only finalize if there are rows in the current batch
		if (rows_in_final_batch > 0) {
			CopyDebugLog(1, "BCPCopyFinalize: sending final batch: %llu rows, buffer: %zu MB",
						 (unsigned long long)rows_in_final_batch, gdata.writer->GetAccumulatorSize() / (1024 * 1024));

			// Send DONE token for the final batch
			gdata.writer->WriteDone(rows_in_final_batch);

			CopyDebugLog(1, "BCPCopyFinalize: data sent, waiting for SQL Server to process...");

			// Read server response and get confirmed row count
			idx_t final_batch_confirmed = gdata.writer->Finalize();
			gdata.rows_confirmed.fetch_add(final_batch_confirmed);

			CopyDebugLog(1, "BCPCopyFinalize: final batch confirmed %llu rows",
						 (unsigned long long)final_batch_confirmed);
		} else if (previously_confirmed == 0) {
			// No rows at all - still need to send empty completion
			gdata.writer->WriteDone(0);
			gdata.writer->Finalize();
		}

		idx_t total_confirmed = gdata.rows_confirmed.load();
		idx_t batches = gdata.batches_flushed.load() + (rows_in_final_batch > 0 ? 1 : 0);

		CopyDebugLog(1, "BCPCopyFinalize: server confirmed %llu total rows in %llu batches (sent: %llu)",
					 (unsigned long long)total_confirmed, (unsigned long long)batches, (unsigned long long)total_rows);

		// Verify row counts match (warning only, server is authoritative)
		if (total_confirmed != total_rows) {
			CopyDebugLog(1, "WARNING: Row count mismatch - sent %llu, confirmed %llu",
						 (unsigned long long)total_rows, (unsigned long long)total_confirmed);
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
}

//===----------------------------------------------------------------------===//
// BCPCopyExecutionMode - Determine execution mode
//===----------------------------------------------------------------------===//

CopyFunctionExecutionMode BCPCopyExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	// BCP requires sequential writes to maintain packet ordering
	// Even though we use local buffers, the final write to the connection is serialized
	// Use regular mode to ensure proper ordering
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

}  // namespace mssql
}  // namespace duckdb

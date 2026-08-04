#include "dml/ctas/mssql_physical_ctas.hpp"
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLCTASLocalSinkState
//===----------------------------------------------------------------------===//

MSSQLCTASLocalSinkState::~MSSQLCTASLocalSinkState() {
	// The writer goes first: it holds a reference to the connection, and the
	// release protocol closes the socket underneath it.
	writer.reset();
	mssql::ReleaseBcpConnectionOnError(connection, pool_handle, /*transaction_pinned=*/false);
}

//===----------------------------------------------------------------------===//
// MSSQLCTASGlobalSinkState
//===----------------------------------------------------------------------===//

MSSQLCTASGlobalSinkState::MSSQLCTASGlobalSinkState(ClientContext &context, MSSQLCatalog &catalog,
												   const mssql::CTASTarget &target,
												   const vector<mssql::CTASColumnDef> &columns,
												   const mssql::CTASConfig &config) {
	state.Initialize(catalog, target, columns, config);
}

//===----------------------------------------------------------------------===//
// MSSQLPhysicalCreateTableAs Constructor
//===----------------------------------------------------------------------===//

MSSQLPhysicalCreateTableAs::MSSQLPhysicalCreateTableAs(PhysicalPlan &plan, vector<LogicalType> types,
													   idx_t estimated_cardinality, MSSQLCatalog &catalog,
													   mssql::CTASTarget target, vector<mssql::CTASColumnDef> columns,
													   mssql::CTASConfig config)
	: PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	  catalog_(catalog),
	  target_(std::move(target)),
	  columns_(std::move(columns)),
	  config_(std::move(config)) {}

//===----------------------------------------------------------------------===//
// State Management
//===----------------------------------------------------------------------===//

unique_ptr<GlobalSinkState> MSSQLPhysicalCreateTableAs::GetGlobalSinkState(ClientContext &context) const {
	auto gstate = make_uniq<MSSQLCTASGlobalSinkState>(context, catalog_, target_, columns_, config_);

	// Execute DDL phase immediately (CREATE TABLE or DROP + CREATE for OR REPLACE)
	// This is done in GetGlobalSinkState to fail fast before any data is processed
	try {
		// Check if table exists and determine if this is a new table (Issue #45 - auto-TABLOCK)
		bool table_existed = gstate->state.TableExists(context);

		// Handle OR REPLACE: check if table exists and drop if needed
		if (gstate->state.target.or_replace) {
			if (table_existed) {
				gstate->state.ExecuteDrop(context);
				// After drop, this is effectively a new table
				gstate->state.config.is_new_table = true;
			} else {
				// Table didn't exist - definitely new
				gstate->state.config.is_new_table = true;
			}
		} else if (gstate->state.target.if_not_exists) {
			// Handle IF NOT EXISTS: skip if table exists (Issue #44)
			if (table_existed) {
				// Table exists - mark as skipped and return early
				gstate->state.phase = mssql::CTASPhase::SKIPPED;
				gstate->skipped = true;
				gstate->state.LogMetrics();
				return std::move(gstate);
			}
			// Table didn't exist - new table
			gstate->state.config.is_new_table = true;
		} else {
			// Non-OR REPLACE, non-IF NOT EXISTS: fail if table exists (FR-014)
			if (table_existed) {
				throw InvalidInputException(
					"CTAS failed: table '%s' already exists. "
					"Use CREATE OR REPLACE TABLE to overwrite.",
					gstate->state.target.GetQualifiedName());
			}
			// Table didn't exist - new table
			gstate->state.config.is_new_table = true;
		}

		// Validate schema exists (FR-009)
		if (!gstate->state.SchemaExists(context)) {
			throw InvalidInputException("CTAS failed: schema '%s' does not exist in SQL Server.",
										gstate->state.target.schema_name);
		}

		// Execute CREATE TABLE DDL
		gstate->state.ExecuteDDL(context);

		// How many bulk-load sessions this CTAS may open (spec 057 step 7).
		//
		// Only the BCP path: the INSERT path has no session to multiply, and it
		// batches through one executor.
		//
		// An explicit transaction no longer caps this at one. It used to, because
		// the first writer was the transaction's own pinned connection and a second
		// one would have sat outside it. Now none of them is pinned — the load is
		// deliberately outside the transaction either way (see ExecuteBCPInsert) —
		// so writer number two is in exactly the same position as writer number
		// one, and there is nothing left for the cap to protect.
		if (gstate->state.config.use_bcp && gstate->state.bcp_writer) {
			Value pw;
			int64_t configured = 0;
			if (context.TryGetCurrentSetting("mssql_copy_parallel_writers", pw)) {
				configured = pw.GetValue<int64_t>();
			}
			// The same resolver COPY uses (spec 063 D1). CTAS answers OwnsTarget,
			// which is what the paragraph above says in one word: it created the
			// table, so the undo is dropping it and a transaction does not cap the
			// writers.
			const auto policy = MSSQLResolveLoadPolicy(
				gstate->state.bcp_target.is_temp_table, ConnectionProvider::IsInTransaction(context, catalog_),
				MSSQLLoadTransactionRole::OwnsTarget, configured, static_cast<uint64_t>(context.db->NumberOfThreads()));
			gstate->parallel_writer_limit = static_cast<idx_t>(policy.max_writers);
		}

	} catch (...) {
		// DDL phase failed - log and rethrow
		gstate->state.phase = mssql::CTASPhase::FAILED;
		gstate->state.LogMetrics();
		throw;
	}

	return std::move(gstate);
}

unique_ptr<LocalSinkState> MSSQLPhysicalCreateTableAs::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<MSSQLCTASLocalSinkState>();
}

//===----------------------------------------------------------------------===//
// Parallel writers (spec 057 step 7)
//===----------------------------------------------------------------------===//

//! Give this thread its own bulk-load session, or leave it sharing the global
//! one. Tried once, on the thread's first chunk — GetLocalSinkState does not see
//! the global state, and everything needed (the INSERT BULK text, the resolved
//! columns) is settled there by the DDL phase.
//!
//! Failure is NOT an error. A pool at its limit, a server refusing another bulk
//! load: the thread falls back to the shared writer, which is the pre-parallel
//! behaviour and always correct. A CTAS must not fail because it could not go
//! faster.
static void TryStartLocalWriter(MSSQLCatalog &catalog, MSSQLCTASGlobalSinkState &gstate,
								MSSQLCTASLocalSinkState &lstate) {
	if (gstate.parallel_writer_limit <= 1) {
		return;
	}
	// Claim a slot before doing any work, so N threads racing here cannot
	// collectively exceed the limit.
	const idx_t slot = gstate.parallel_writers_used.fetch_add(1);
	if (slot >= gstate.parallel_writer_limit) {
		gstate.parallel_writers_used.fetch_sub(1);
		return;
	}

	std::shared_ptr<tds::TdsConnection> conn;
	try {
		auto &pool = catalog.GetConnectionPool();
		conn = pool.Acquire();
		if (!conn || conn->GetState() != tds::ConnectionState::Idle) {
			throw IOException("no idle connection available for a parallel writer");
		}
		auto result = MSSQLSimpleQuery::Execute(*conn, gstate.state.insert_bulk_sql);
		if (!result.success) {
			throw IOException("INSERT BULK failed on the parallel connection: %s", result.error_message);
		}
		if (!conn->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
			throw IOException("could not transition the parallel connection to Executing");
		}
		lstate.pool_handle = catalog.GetConnectionPoolHandle();
		lstate.connection = conn;
		lstate.writer =
			make_uniq<mssql::BCPWriter>(*lstate.connection, gstate.state.bcp_target, gstate.state.bcp_columns);
		// The stream opens with COLMETADATA; without it the server has no schema
		// for the ROW tokens that follow.
		lstate.writer->WriteColmetadata();
	} catch (std::exception &) {
		if (conn) {
			try {
				catalog.GetConnectionPool().Release(conn);
			} catch (...) {
				// Nothing left to do — the handle is dropped either way.
			}
		}
		lstate.connection.reset();
		lstate.writer.reset();
		gstate.parallel_writers_used.fetch_sub(1);
	}
}

//! Close this thread's own bulk-load session: DONE for whatever is unflushed,
//! then the server's confirmation, then the connection goes back to the pool.
static void FinishLocalWriter(MSSQLCatalog &catalog, MSSQLCTASLocalSinkState &lstate) {
	if (!lstate.writer) {
		return;
	}
	// Always send DONE, even for zero rows: INSERT BULK left the connection in
	// Executing, and only DONE closes the stream so it can be pooled again.
	lstate.writer->WriteDone(lstate.rows_in_batch);
	lstate.rows_confirmed += lstate.writer->Finalize();
	lstate.rows_in_batch = 0;
	lstate.writer.reset();

	if (lstate.connection) {
		lstate.connection->TransitionState(tds::ConnectionState::Executing, tds::ConnectionState::Idle);
		catalog.GetConnectionPool().Release(lstate.connection);
		lstate.connection.reset();
	}
}

//===----------------------------------------------------------------------===//
// Sink Implementation
//===----------------------------------------------------------------------===//

SinkResultType MSSQLPhysicalCreateTableAs::Sink(ExecutionContext &context, DataChunk &chunk,
												OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();
	auto &lstate = input.local_state.Cast<MSSQLCTASLocalSinkState>();

	// Skip if IF NOT EXISTS triggered skip (Issue #44). Read from the flag fixed
	// before the first chunk, never from `state.phase` — a failing thread writes
	// that under the mutex this read does not hold.
	if (gstate.skipped) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Skip empty chunks
	if (chunk.size() == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Another writer has already failed. Whether that ends this one too is the
	// drop_on_failure question and not a separate setting: if the table is about
	// to be dropped, every row still in flight is wasted work AND holds a session
	// open against the table the DROP has to lock. If it is NOT going to be
	// dropped, `false` means "keep what landed" — and a thread that still has rows
	// to send is landing more of exactly what the user asked to keep.
	if (gstate.state.config.drop_on_failure && gstate.load_failed.load(std::memory_order_acquire)) {
		throw IOException(
			"CTAS aborted: another parallel bulk-load writer failed and "
			"mssql_ctas_drop_on_failure is set, so the table is being dropped");
	}

	if (!lstate.init_attempted) {
		lstate.init_attempted = true;
		TryStartLocalWriter(catalog_, gstate, lstate);
	}

	// A thread with its own session writes without touching the global writer at
	// all — no mutex, no shared accumulator. Its batch boundary re-opens its own
	// INSERT BULK, exactly as the global one does.
	if (lstate.writer) {
		try {
			const idx_t written = lstate.writer->WriteRows(chunk);
			lstate.rows_in_batch += written;
			lstate.rows_written += written;
			if (gstate.state.config.bcp_flush_rows > 0 && lstate.rows_in_batch >= gstate.state.config.bcp_flush_rows) {
				lstate.rows_confirmed += lstate.writer->FlushBatch(lstate.rows_in_batch);
				lstate.rows_in_batch = 0;
				auto result = MSSQLSimpleQuery::Execute(*lstate.connection, gstate.state.insert_bulk_sql);
				if (!result.success) {
					throw IOException("CTAS BCP: Failed to re-execute INSERT BULK on a parallel connection: %s",
									  result.error_message);
				}
				if (!lstate.connection->TransitionState(tds::ConnectionState::Idle, tds::ConnectionState::Executing)) {
					throw IOException("CTAS BCP: Failed to transition a parallel connection to Executing");
				}
				lstate.writer->ResetForNextBatch();
				lstate.writer->WriteColmetadata();
			}
		} catch (...) {
			// Close this thread's own stream FIRST, before anything below can issue
			// a DROP. The connection is sitting mid-bulk-load on the very table the
			// DROP has to take a schema lock on, so leaving it open until
			// ~MSSQLCTASLocalSinkState runs would have the cleanup block on this
			// same thread's work.
			lstate.writer.reset();
			mssql::ReleaseBcpConnectionOnError(lstate.connection, lstate.pool_handle,
											   /*transaction_pinned=*/false);

			// Mark the statement failed. The DROP that mssql_ctas_drop_on_failure
			// asks for is done by the FIRST thread to fail and by no other: with N
			// writers, N failures are the normal shape of one broken load (the
			// others' streams die with it), and each would otherwise issue its own
			// DROP against a table the first one already removed.
			const bool cleanup_here = !gstate.load_failed.exchange(true);
			{
				std::lock_guard<std::mutex> lock(gstate.mutex);
				gstate.state.phase = mssql::CTASPhase::FAILED;
				gstate.state.error_message = "BCP phase failed";
			}
			if (cleanup_here && gstate.state.config.drop_on_failure) {
				// Not under the lock: this takes its own connection and talks to
				// the server, and the other writers are unwinding meanwhile. They
				// bail out at the top of Sink rather than finishing their batches,
				// which is what keeps this DROP from waiting on them.
				gstate.state.AttemptCleanup(context.client);
			}
			throw;
		}
		return SinkResultType::NEED_MORE_INPUT;
	}

	// Thread-safe execution: threads without a session of their own share the
	// global writer, which is what every thread did before this was parallel.
	std::lock_guard<std::mutex> lock(gstate.mutex);

	// Execute data transfer using BCP or INSERT mode (Spec 027)
	try {
		if (gstate.state.config.use_bcp && gstate.state.bcp_writer) {
			// BCP mode: delegate to AddChunkBCP
			gstate.state.AddChunkBCP(context.client, chunk);
		} else if (gstate.state.insert_executor) {
			// Legacy INSERT mode
			gstate.state.rows_produced += chunk.size();
			idx_t rows_inserted = gstate.state.insert_executor->Execute(chunk);
			gstate.state.rows_inserted += rows_inserted;
		}
	} catch (...) {
		// Data transfer phase failed - attempt cleanup if configured
		const bool cleanup_here = !gstate.load_failed.exchange(true);
		gstate.state.phase = mssql::CTASPhase::FAILED;
		gstate.state.error_message = gstate.state.config.use_bcp ? "BCP phase failed" : "Insert phase failed";

		if (cleanup_here && gstate.state.config.drop_on_failure) {
			gstate.state.AttemptCleanup(context.client);
		}

		gstate.state.LogMetrics();
		throw;
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType MSSQLPhysicalCreateTableAs::Combine(ExecutionContext &context,
														  OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();
	auto &lstate = input.local_state.Cast<MSSQLCTASLocalSinkState>();

	// A thread that never got a session of its own has nothing to close; its rows
	// went out through the global writer and Finalize accounts for them.
	if (!lstate.writer && lstate.rows_written == 0) {
		return SinkCombineResultType::FINISHED;
	}

	FinishLocalWriter(catalog_, lstate);

	std::lock_guard<std::mutex> lock(gstate.mutex);
	gstate.state.rows_produced += lstate.rows_written;
	gstate.state.rows_inserted += lstate.rows_confirmed;
	lstate.rows_written = 0;
	lstate.rows_confirmed = 0;
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType MSSQLPhysicalCreateTableAs::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
													  OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();

	// Thread-safe finalization
	std::lock_guard<std::mutex> lock(gstate.mutex);

	// If skipped (IF NOT EXISTS and table existed), just return success (Issue #44)
	if (gstate.state.phase == mssql::CTASPhase::SKIPPED) {
		return SinkFinalizeType::READY;
	}

	try {
		// Flush any remaining INSERT batches
		gstate.state.FlushInserts(context);

		// Mark complete
		gstate.state.phase = mssql::CTASPhase::COMPLETE;

		// Invalidate catalog cache so the new table is visible
		gstate.state.InvalidateCache();

		// Log success metrics
		gstate.state.LogMetrics();

	} catch (...) {
		// Finalize failed - attempt cleanup if configured
		gstate.state.phase = mssql::CTASPhase::FAILED;
		gstate.state.error_message = "Finalize phase failed";

		if (gstate.state.config.drop_on_failure) {
			gstate.state.AttemptCleanup(context);
		}

		gstate.state.LogMetrics();
		throw;
	}

	return SinkFinalizeType::READY;
}

//===----------------------------------------------------------------------===//
// Source Implementation (for returning row count)
//===----------------------------------------------------------------------===//

SourceResultType MSSQLPhysicalCreateTableAs::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
															 OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<MSSQLCTASGlobalSinkState>();

	// Thread-safe access
	std::lock_guard<std::mutex> lock(gstate.mutex);

	if (gstate.returned) {
		return SourceResultType::FINISHED;
	}

	gstate.returned = true;

	// Return the count of inserted rows
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(static_cast<int64_t>(gstate.state.rows_inserted)));

	return SourceResultType::FINISHED;
}

}  // namespace duckdb

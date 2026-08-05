#include "dml/ctas/mssql_physical_ctas.hpp"
#include "catalog/mssql_catalog.hpp"
#include "connection/mssql_connection_provider.hpp"
#include "copy/bcp_config.hpp"
#include "mssql_counters.hpp"

#include <cstdio>
#include <cstdlib>
#include "duckdb/common/exception.hpp"
#include "query/mssql_simple_query.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// MSSQLCTASGlobalSinkState
//===----------------------------------------------------------------------===//

MSSQLCTASGlobalSinkState::MSSQLCTASGlobalSinkState(ClientContext &context, MSSQLCatalog &catalog,
												   const mssql::CTASTarget &target,
												   const vector<mssql::CTASColumnDef> &columns,
												   const mssql::CTASConfig &config) {
	state.Initialize(catalog, target, columns, config, ConnectionProvider::ShouldResetOnRelease(context));
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
// Sink Implementation
//===----------------------------------------------------------------------===//

// House debug pattern (see CLAUDE.md): a static level read from MSSQL_DEBUG.
// Added because "did this CTAS go parallel?" was unanswerable without timing it
// — COPY logged the slot and CTAS was silent (spec 063 D5).
static int GetCtasSinkDebugLevel() {
	static const int level = []() {
		const char *env = std::getenv("MSSQL_DEBUG");
		return env ? std::atoi(env) : 0;
	}();
	return level;
}

#define CTAS_SINK_LOG(fmt, ...)                                       \
	do {                                                              \
		if (GetCtasSinkDebugLevel() >= 1) {                           \
			fprintf(stderr, "[MSSQL CTAS] " fmt "\n", ##__VA_ARGS__); \
		}                                                             \
	} while (0)

static uint64_t ElapsedNsSince(std::chrono::steady_clock::time_point start) {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

//! The CTAS peer of PrintWriteCounters in copy_function.cpp — same field names
//! and the same units, so a CTAS and a COPY of the same data can be compared
//! line for line. That was impossible before: CTAS reported nothing at all.
static void PrintCtasCounters(MSSQLCTASGlobalSinkState &gstate) {
	if (!mssql::CountersEnabled()) {
		return;
	}
	if (mssql::CountersConfoundedByLogging()) {
		fprintf(stderr,
				"[MSSQL COUNTERS] NOTE: MSSQL_DEBUG is set, and its logging runs inside the phases timed "
				"below — these numbers include it. For timings use MSSQL_COUNTERS=1 with MSSQL_DEBUG unset.\n");
	}
	const uint64_t sink_ns = gstate.counter_sink_ns.load();
	const uint64_t encode_ns = gstate.counter_encode_ns.load();
	const uint64_t flush_ns = gstate.counter_flush_ns.load();
	const uint64_t other_ns = sink_ns > encode_ns + flush_ns ? sink_ns - encode_ns - flush_ns : 0;
	const idx_t rows = gstate.state.rows_produced;
	const idx_t chunks = gstate.counter_sink_calls.load();

	// The phase totals below are SUMMED ACROSS THREADS, not wall clock: with
	// N writers they can exceed the elapsed time of the statement, and a
	// rise from 1 to N threads does NOT mean it got slower. Said in the
	// output too, because these numbers get quoted.
	fprintf(stderr, "[MSSQL COUNTERS]   (phase totals are summed across threads, not wall clock)\n");
	fprintf(stderr,
			"[MSSQL COUNTERS] ctas close: rows=%llu cols=%llu chunks=%llu writers=%llu/%llu | sink=%lluus "
			"(encode=%lluus flush=%lluus other=%lluus)\n",
			(unsigned long long)rows, (unsigned long long)gstate.state.bcp_columns.size(), (unsigned long long)chunks,
			(unsigned long long)gstate.parallel_writers_used.load(), (unsigned long long)gstate.parallel_writer_limit,
			(unsigned long long)(sink_ns / 1000), (unsigned long long)(encode_ns / 1000),
			(unsigned long long)(flush_ns / 1000), (unsigned long long)(other_ns / 1000));
	if (rows > 0) {
		const double r = static_cast<double>(rows);
		fprintf(stderr, "[MSSQL COUNTERS]   ns/row: sink=%.1f encode=%.1f flush=%.1f other=%.1f\n", sink_ns / r,
				encode_ns / r, flush_ns / r, other_ns / r);
	}
}

SinkResultType MSSQLPhysicalCreateTableAs::Sink(ExecutionContext &context, DataChunk &chunk,
												OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<MSSQLCTASGlobalSinkState>();
	auto &lstate = input.local_state.Cast<MSSQLCTASLocalSinkState>();
	const bool counters = mssql::CountersEnabled();
	const auto sink_start = std::chrono::steady_clock::now();

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

	// Ctrl+C. CTAS had no interrupt check at all — `IsInterrupted` appeared zero
	// times in this file against five in the COPY sink — so a cancelled CTAS ran
	// to completion at the server while the client had stopped caring, holding a
	// bulk load open on a table nobody was going to keep (spec 063 D5).
	if (context.client.IsInterrupted()) {
		throw InterruptException();
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
		mssql::BulkLoadSessionParams params;
		params.pool = &catalog_.GetConnectionPool();
		params.pool_handle = catalog_.GetConnectionPoolHandle();
		params.insert_bulk_sql = &gstate.state.insert_bulk_sql;
		params.target = &gstate.state.bcp_target;
		params.columns = &gstate.state.bcp_columns;
		params.flush_rows = gstate.state.config.bcp_flush_rows;
		params.reset_on_release = gstate.state.reset_on_release;
		params.collect_timings = counters;
		if (lstate.session.TryStart(params, gstate.parallel_writers_used, gstate.parallel_writer_limit)) {
			CTAS_SINK_LOG("parallel writer started (limit=%llu)", (unsigned long long)gstate.parallel_writer_limit);
		} else {
			CTAS_SINK_LOG("sharing the global writer (limit=%llu)", (unsigned long long)gstate.parallel_writer_limit);
		}
	}

	// A thread with its own session writes without touching the global writer at
	// all — no mutex, no shared accumulator. Its batch boundary re-opens its own
	// INSERT BULK, exactly as the global one does.
	if (lstate.session.IsOwned()) {
		try {
			const auto written = lstate.session.Write(chunk);
			lstate.rows_written += written.rows_written;
			lstate.rows_confirmed += written.rows_confirmed;
			if (counters) {
				gstate.counter_sink_calls.fetch_add(1, std::memory_order_relaxed);
				gstate.counter_sink_ns.fetch_add(ElapsedNsSince(sink_start), std::memory_order_relaxed);
				gstate.counter_encode_ns.fetch_add(written.encode_ns, std::memory_order_relaxed);
				gstate.counter_flush_ns.fetch_add(written.flush_ns, std::memory_order_relaxed);
			}
		} catch (...) {
			// Close this thread's own stream FIRST, before anything below can issue
			// a DROP. The connection is sitting mid-bulk-load on the very table the
			// DROP has to take a schema lock on, so leaving it open until
			// ~MSSQLCTASLocalSinkState runs would have the cleanup block on this
			// same thread's work.
			lstate.session.Abandon();

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
			const uint64_t encode_before = gstate.state.counter_encode_ns;
			const uint64_t flush_before = gstate.state.counter_flush_ns;
			gstate.state.AddChunkBCP(context.client, chunk);
			if (counters) {
				gstate.counter_sink_calls.fetch_add(1, std::memory_order_relaxed);
				gstate.counter_sink_ns.fetch_add(ElapsedNsSince(sink_start), std::memory_order_relaxed);
				gstate.counter_encode_ns.fetch_add(gstate.state.counter_encode_ns - encode_before,
												   std::memory_order_relaxed);
				gstate.counter_flush_ns.fetch_add(gstate.state.counter_flush_ns - flush_before,
												  std::memory_order_relaxed);
			}
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
	if (!lstate.session.IsOwned() && lstate.rows_written == 0) {
		return SinkCombineResultType::FINISHED;
	}

	lstate.rows_confirmed += lstate.session.Finish();

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
		PrintCtasCounters(gstate);

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

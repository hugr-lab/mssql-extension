//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// copy/load_policy.hpp
//
// Who supplies a bulk-load writer's connection, and how many writers there may
// be (spec 063 D1).
//
// This exists because the answer is needed in four places and was being derived
// independently in each: COPY, CTAS, INSERT via BCP (spec 062), and the `#temp`
// staging table that UPDATE/DELETE will fill. COPY and CTAS derived it twice
// already and disagreed — one asked the provider for its connection, the other
// the pool — and the disagreement was safe only by accident (see below).
//
// Reading all four together collapses the question to ONE predicate about the
// TARGET:
//
//     Can a session other than the one driving the statement see it, and may
//     its rows land outside the current transaction?
//
// A session-scoped `#temp` table answers no to the first. A target inside an
// explicit transaction whose rows must roll back answers no to the second. Both
// noes mean exactly one writer. CTAS's own new table answers yes to both.
//
// The test that this is the right predicate rather than a restatement of the two
// cases we already had: UPDATE/DELETE staging lands in the first two rows AT
// ONCE — a `#temp` table inside a transaction — and needs no new rule.
//
// Deliberately a self-contained header of plain types, like
// MSSQLResolveTablock in catalog/mssql_index_kind.hpp: a pure function over
// values can be unit-tested with `-I src/include` and nothing linked, which is
// what keeps a policy this consequential from being verified only end-to-end.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

namespace duckdb {

//! Ceiling on concurrent bulk-load sessions one statement may open.
//!
//! A writer is a pooled connection holding an open bulk load, which is a
//! different resource from a CPU thread. Measured on 44 columns x 1M rows:
//! 10.55 s at 1 writer, 3.24 s at 4, 3.51 s at 8 — it plateaus past 4, and the
//! cap is deliberately above the plateau rather than at it.
constexpr uint64_t MSSQL_MAX_COPY_PARALLEL_WRITERS = 8;

//! Where the load's connection comes from.
enum class MSSQLLoadConnectionSource : uint8_t {
	//! The DuckDB transaction's pinned connection. There is exactly one, and a
	//! parallel writer must NEVER be given it: two writers on one connection
	//! interleave their ROW tokens into one bulk load.
	Pinned,
	//! Acquired from the catalog's pool, independent of any transaction.
	Pool,
};

//! Whether the statement's rows have to roll back with the DuckDB transaction.
//!
//! This is the one input that differs per operator, and it is stated as a
//! property of the STATEMENT rather than as "which operator is asking" — so a
//! future caller answers the same question instead of adding a branch here.
enum class MSSQLLoadTransactionRole : uint8_t {
	//! The load must be INSIDE the transaction. The target may have existed
	//! before the statement, so nothing else can undo the rows it adds.
	//! COPY, and INSERT once spec 062 lands.
	JoinsTransaction,
	//! The statement created the target, so the undo is dropping it and the load
	//! may run outside the transaction. CTAS.
	//!
	//! Not a shortcut: CTAS used to take the pinned connection and could not
	//! read and bulk-load through it at once (spec 057).
	OwnsTarget,
};

struct MSSQLLoadPolicy {
	MSSQLLoadConnectionSource source = MSSQLLoadConnectionSource::Pool;
	//! Bulk-load sessions this statement may open, the first one included.
	//! 1 disables parallel loading.
	uint64_t max_writers = 1;
};

//! `mssql_copy_parallel_writers` resolved: an explicit value wins, 0 derives one
//! from the thread count and caps it.
inline uint64_t MSSQLDeriveWriterLimit(int64_t configured, uint64_t thread_count) {
	if (configured > 0) {
		return static_cast<uint64_t>(configured);
	}
	const uint64_t derived =
		thread_count < MSSQL_MAX_COPY_PARALLEL_WRITERS ? thread_count : MSSQL_MAX_COPY_PARALLEL_WRITERS;
	return derived > 0 ? derived : 1;
}

//! Resolve the whole policy.
//!
//! `target_is_session_scoped` is the LOCAL temp flag (`#name`) alone. A GLOBAL
//! temp table (`##name`) is visible to every session, so it keeps N writers —
//! which is the workflow issue #189 asks for, and passing an accessor that
//! merges the two would serialise it for no reason.
//! Rows the spec 070 W2 warm-up gate holds extra bulk-load writers for. 0 means
//! the gate is open — claim immediately.
//!
//! Pure so it can be tested at its boundaries without a server; the gate's
//! failure mode is SILENT (writers that never open, or open too early and land
//! everything in the delta store uncompressed), so an integration test that
//! nobody routinely runs is not coverage. See test/cpp/test_load_policy.cpp.
//!
//! The threshold is SQL Server's own rowgroup constant, never the user's
//! `flush_rows` — keying it on the setting let `flush_rows = 1000000` serialize
//! the first million rows onto one writer (PR #270 review).
//!
//! `flush_rows == 0` means NO INTERMEDIATE FLUSH — one unbounded batch per
//! writer — NOT "batches too small to compress". It is the case the gate matters
//! MOST for: every writer's single end-of-load batch can compress, and the gate
//! is what decides how many of them clear the threshold. Treating 0 as
//! sub-threshold (it compares numerically less) opened the gate immediately and
//! sent four uncompressed part-batches to a columnstore target — precisely what
//! W2 exists to prevent (job 1116).
inline uint64_t MSSQLWarmupGateRows(bool warmup_gate, uint64_t flush_rows, uint64_t rowgroup_rows) {
	if (!warmup_gate) {
		return 0;  // heap target: no compression to protect, fan out immediately
	}
	if (flush_rows == 0) {
		return rowgroup_rows;  // unbounded batch — gate applies
	}
	if (flush_rows < rowgroup_rows) {
		return 0;  // no batch can reach a compressed rowgroup; nothing to protect
	}
	return rowgroup_rows;
}

inline MSSQLLoadPolicy MSSQLResolveLoadPolicy(bool target_is_session_scoped, bool in_transaction,
											  MSSQLLoadTransactionRole role, int64_t configured_writers,
											  uint64_t thread_count) {
	MSSQLLoadPolicy policy;

	policy.source = (in_transaction && role == MSSQLLoadTransactionRole::JoinsTransaction)
						? MSSQLLoadConnectionSource::Pinned
						: MSSQLLoadConnectionSource::Pool;

	// A pinned connection is the transaction's, and there is one of it. A second
	// writer would sit OUTSIDE the transaction and its rows would not roll back
	// with the rest — a correctness question, not a tuning one.
	if (policy.source == MSSQLLoadConnectionSource::Pinned) {
		policy.max_writers = 1;
		return policy;
	}

	// A `#temp` table exists only in the session that created it, so a writer on
	// any other pooled connection cannot see it.
	//
	// Today that is nearly free to get wrong: a `#temp` target is only usable
	// inside a transaction at all, so it is already pinned and caught above, and
	// the parallel path is reached only in the autocommit case where the table is
	// about to vanish anyway. What made even THAT safe was not the writer but the
	// pool's reset — `RESET_CONNECTION` drops any `#tmp` an earlier statement
	// left behind, so a second writer failed cleanly instead of finding a STALE
	// same-named table.
	//
	// `mssql_reset_connection = false` (issue #189) removes that guard, and then
	// the difference between this branch and its absence is rows landing in the
	// wrong table with no error on either side. Which is why this is here BEFORE
	// the setting is.
	//
	// What this cannot do, and no policy could: guarantee that the ONE session
	// used is the one holding the table. With the reset off, a `#` table is
	// reachable only if the pool happens to hand back its creator — which is
	// exactly why #189 asks for `##`, not `#`.
	if (target_is_session_scoped) {
		policy.max_writers = 1;
		return policy;
	}

	policy.max_writers = MSSQLDeriveWriterLimit(configured_writers, thread_count);
	return policy;
}

}  // namespace duckdb

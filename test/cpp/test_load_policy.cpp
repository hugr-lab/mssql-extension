// test/cpp/test_load_policy.cpp
//
// Unit tests for MSSQLResolveLoadPolicy (spec 063 D1).
//
// No SQL Server, no linking, no DuckDB submodule: copy/load_policy.hpp is
// deliberately self-contained, so -I src/include is the whole build recipe.
//
// Why this exists rather than leaving the policy to the .test suite. Every case
// below is one where getting it wrong is SILENT:
//
//   * a parallel writer handed the pinned connection interleaves its ROW tokens
//     into someone else's bulk load;
//   * a second writer inside a transaction lands rows that will not roll back
//     with the rest;
//   * a second writer against a `#temp` target writes to whatever same-named
//     table that pooled session happens to hold — which today cannot happen
//     because the pool resets, and which `mssql_reset_connection = false`
//     (issue #189) is about to make possible.
//
// None of those raises an error, so an end-to-end test sees a load that
// succeeded. The policy is a pure function precisely so the answer can be
// asserted directly.
//
// Run:
//   ./build/test/test_load_policy

#include <cassert>
#include <iostream>
#include <string>

#include "copy/load_policy.hpp"

using namespace duckdb;

static int g_failures = 0;

static const char *SourceName(MSSQLLoadConnectionSource s) {
	return s == MSSQLLoadConnectionSource::Pinned ? "Pinned" : "Pool";
}

static void Check(const char *what, MSSQLLoadPolicy actual, MSSQLLoadConnectionSource want_source,
				  uint64_t want_writers, const char *why) {
	if (actual.source != want_source || actual.max_writers != want_writers) {
		std::cerr << "FAIL: " << what << " -> " << SourceName(actual.source) << "/" << actual.max_writers
				  << ", expected " << SourceName(want_source) << "/" << want_writers << " (" << why << ")\n";
		++g_failures;
	} else {
		std::cout << "ok: " << what << " -> " << SourceName(want_source) << "/" << want_writers << " (" << why << ")\n";
	}
}

// The four consumers the policy is designed for, each asking the question the
// way it actually will. These are the rows of the table in the spec; if one of
// them changes, this is where it has to be argued.
static void TestTheFourConsumers() {
	std::cout << "\n-- the four consumers --\n";

	// COPY, permanent target, no transaction: the ordinary fast path.
	Check("COPY / permanent / autocommit",
		  MSSQLResolveLoadPolicy(false, false, MSSQLLoadTransactionRole::JoinsTransaction, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 8, "derived from threads, capped at 8");

	// COPY inside a transaction: it may be loading into a table that existed
	// before the statement, so the transaction has to own the load.
	Check("COPY / permanent / in transaction",
		  MSSQLResolveLoadPolicy(false, true, MSSQLLoadTransactionRole::JoinsTransaction, 0, 16),
		  MSSQLLoadConnectionSource::Pinned, 1, "a second writer's rows would not roll back");

	// CTAS: its table is its own and the undo is dropping it, so a transaction
	// does not cap it. This is the spec 057 decision, pinned here.
	Check("CTAS / in transaction", MSSQLResolveLoadPolicy(false, true, MSSQLLoadTransactionRole::OwnsTarget, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 8, "owns its target; load is outside the transaction");

	Check("CTAS / autocommit", MSSQLResolveLoadPolicy(false, false, MSSQLLoadTransactionRole::OwnsTarget, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 8, "same answer in or out of a transaction");

	// UPDATE/DELETE staging (spec 062 and after): a #temp table inside a
	// transaction. It is BOTH session-scoped and transactional, and needs no
	// rule of its own — which is the test that the predicate generalises.
	Check("DML staging / #temp / in transaction",
		  MSSQLResolveLoadPolicy(true, true, MSSQLLoadTransactionRole::JoinsTransaction, 0, 16),
		  MSSQLLoadConnectionSource::Pinned, 1, "session-scoped AND transactional — no new rule needed");
}

// The case mssql_reset_connection is about to make dangerous.
static void TestSessionScopedTarget() {
	std::cout << "\n-- #temp targets --\n";

	Check("#temp / autocommit", MSSQLResolveLoadPolicy(true, false, MSSQLLoadTransactionRole::JoinsTransaction, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 1, "no other session can see a # table");

	// The one that must not be inherited from the thread count: an explicit
	// setting does NOT buy parallel writers against a session-scoped target.
	Check("#temp / autocommit / mssql_copy_parallel_writers=4",
		  MSSQLResolveLoadPolicy(true, false, MSSQLLoadTransactionRole::JoinsTransaction, 4, 16),
		  MSSQLLoadConnectionSource::Pool, 1, "an explicit setting cannot make a # table visible elsewhere");

	Check("#temp / CTAS-shaped role", MSSQLResolveLoadPolicy(true, false, MSSQLLoadTransactionRole::OwnsTarget, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 1, "owning the target does not make it visible either");

	// ## is a DIFFERENT answer, and the reason the flag passed in is the LOCAL
	// one rather than BCPCopyTarget::IsTempTable(), which merges the two. A
	// global temp table is visible across sessions, so it keeps its writers —
	// that is the workflow #189 asks for.
	Check("##global / autocommit",
		  MSSQLResolveLoadPolicy(false, false, MSSQLLoadTransactionRole::JoinsTransaction, 0, 16),
		  MSSQLLoadConnectionSource::Pool, 8, "## is visible across sessions — N writers is correct");
}

static void TestWriterLimitDerivation() {
	std::cout << "\n-- writer limit --\n";

	struct {
		int64_t configured;
		uint64_t threads;
		uint64_t want;
		const char *why;
	} const cases[] = {
		{0, 16, 8, "0 derives from threads, capped at 8"},
		{0, 4, 4, "fewer threads than the cap"},
		{0, 1, 1, "single-threaded"},
		{0, 0, 1, "a thread count of 0 must not yield 0 writers"},
		{1, 16, 1, "1 disables parallel loading"},
		{4, 16, 4, "explicit value wins over the derivation"},
		{32, 4, 32, "explicit value is NOT capped — the cap bounds the DERIVATION"},
		{-1, 16, 8, "a negative setting is not an explicit value"},
	};
	for (const auto &c : cases) {
		const uint64_t got = MSSQLDeriveWriterLimit(c.configured, c.threads);
		if (got != c.want) {
			std::cerr << "FAIL: configured=" << c.configured << " threads=" << c.threads << " -> " << got
					  << ", expected " << c.want << " (" << c.why << ")\n";
			++g_failures;
		} else {
			std::cout << "ok: configured=" << c.configured << " threads=" << c.threads << " -> " << got << " (" << c.why
					  << ")\n";
		}
	}
}

// The invariant that outranks every tuning answer above: a writer must never be
// given the pinned connection unless it is the only writer.
static void TestPinnedImpliesExactlyOneWriter() {
	std::cout << "\n-- invariant: Pinned implies exactly one writer --\n";
	int checked = 0;
	for (int scoped = 0; scoped <= 1; ++scoped) {
		for (int txn = 0; txn <= 1; ++txn) {
			for (int owns = 0; owns <= 1; ++owns) {
				for (int64_t configured : {int64_t(0), int64_t(1), int64_t(4), int64_t(64)}) {
					for (uint64_t threads : {uint64_t(0), uint64_t(1), uint64_t(64)}) {
						const auto role =
							owns ? MSSQLLoadTransactionRole::OwnsTarget : MSSQLLoadTransactionRole::JoinsTransaction;
						const auto p = MSSQLResolveLoadPolicy(scoped != 0, txn != 0, role, configured, threads);
						++checked;
						if (p.source == MSSQLLoadConnectionSource::Pinned && p.max_writers != 1) {
							std::cerr << "FAIL: Pinned with max_writers=" << p.max_writers << " (scoped=" << scoped
									  << " txn=" << txn << " owns=" << owns << " configured=" << configured
									  << " threads=" << threads << ")\n";
							++g_failures;
						}
						if (p.max_writers < 1) {
							std::cerr << "FAIL: max_writers=" << p.max_writers << " — a load needs one writer\n";
							++g_failures;
						}
					}
				}
			}
		}
	}
	std::cout << "ok: " << checked << " combinations, Pinned always alone and max_writers never 0\n";
}

int main() {
	std::cout << "== MSSQLResolveLoadPolicy unit tests (spec 063 D1) ==\n";
	TestTheFourConsumers();
	TestSessionScopedTarget();
	TestWriterLimitDerivation();
	TestPinnedImpliesExactlyOneWriter();
	if (g_failures == 0) {
		std::cout << "\nAll load-policy tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " load-policy test(s) failed.\n";
	return 1;
}

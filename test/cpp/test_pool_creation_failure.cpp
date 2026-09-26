// test/cpp/test_pool_creation_failure.cpp
//
// What the connection pool does when it cannot CREATE a connection (issue
// #302, spec 073 W2). Before this, a factory failure fell through to the same
// wait an exhausted pool uses -- for a Release that, with nothing active,
// cannot come -- and the caller got "Failed to acquire connection from pool
// (timeout)" after the full acquire_timeout, with the factory's reason
// discarded. Measured on Azure with an expired token: 600 s, two login
// attempts, and a message indistinguishable from a wrong password or an
// unreachable host.
//
// The reason is PER CALL (review 1538): Acquire(timeout, &why) says why this
// call failed. A pool that has recovered clears its recorded error, and the
// backoff between creation attempts applies only while others are active.
//
// No server: the factories here throw, return nullptr, or hand back an
// unconnected TdsConnection to stand in for an active one. Part of
// STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

using duckdb::tds::ConnectionPool;
using duckdb::tds::PoolConfiguration;
using duckdb::tds::TdsConnection;

static int g_failures = 0;

static void Check(bool ok, const std::string &what) {
	if (ok) {
		return;
	}
	std::cerr << "FAIL: " << what << std::endl;
	g_failures++;
}

static bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

static long long MsSince(std::chrono::steady_clock::time_point t0) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static PoolConfiguration SmallPool(size_t limit = 4) {
	PoolConfiguration cfg;
	cfg.connection_limit = limit;
	cfg.acquire_timeout = 5;  // seconds; every Acquire below passes its own, shorter, budget
	return cfg;
}

//! Nothing active, factory throws: Acquire must come back at once with the
//! reason, not after the acquire timeout with "(timeout)".
static void TestThrowingFactoryFailsFast() {
	ConnectionPool pool("throws", SmallPool(), []() -> std::shared_ptr<TdsConnection> {
		throw std::runtime_error("Azure AD token for secret 'sp': expired at 2026-09-10 08:49:00 UTC");
	});

	std::string why;
	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000, &why);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "throwing factory: no connection");
	Check(ms < 500, "throwing factory: returned in " + std::to_string(ms) + " ms, not after the 2000 ms budget");
	Check(Contains(why, "pool 'throws' could not create a connection: ") && Contains(why, "expired at"),
		  "throwing factory: this call's reason names the failure (" + why + ")");
	Check(Contains(pool.GetLastCreateError(), "expired at"), "throwing factory: the reason is kept for pool stats");
	auto stats = pool.GetStats();
	Check(stats.creation_failures == 1, "throwing factory: one creation failure counted");
	Check(stats.acquire_timeout_count == 0, "throwing factory: NOT counted as an acquire timeout");
	Check(stats.total_connections == 0, "throwing factory: nothing leaked into the pool");
}

//! A factory that returns nullptr without saying why still gets a reason
//! recorded, so the caller never sees an empty one.
static void TestSilentNullptrFactoryFailsFast() {
	ConnectionPool pool("silent", SmallPool(), []() -> std::shared_ptr<TdsConnection> { return nullptr; });

	std::string why;
	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000, &why);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "silent factory: no connection");
	Check(ms < 500, "silent factory: returned in " + std::to_string(ms) + " ms");
	Check(Contains(why, "could not create a connection: ") && why.size() > 50,
		  "silent factory: a reason is rendered anyway (" + why + ")");
	Check(pool.GetStats().creation_failures == 1, "silent factory: one creation failure counted");
}

//! Nothing active: EVERY failing Acquire dials once and fails at once. The
//! first draft applied the creation backoff here too, so the second call
//! waited 250 ms, the third 500 ms ... for a Release that could not come.
static void TestBackToBackFailuresStayFast() {
	int calls = 0;
	ConnectionPool pool("empty", SmallPool(), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		throw std::runtime_error("Login failed for user 'sa'. (attempt " + std::to_string(calls) + ")");
	});

	for (int i = 1; i <= 3; i++) {
		std::string why;
		auto t0 = std::chrono::steady_clock::now();
		auto conn = pool.Acquire(2000, &why);
		const auto ms = MsSince(t0);
		Check(conn == nullptr, "back to back #" + std::to_string(i) + ": no connection");
		Check(ms < 200, "back to back #" + std::to_string(i) + ": " + std::to_string(ms) + " ms, no backoff stall");
		Check(Contains(why, "(attempt " + std::to_string(i) + ")"),
			  "back to back #" + std::to_string(i) + ": carries ITS OWN attempt's reason (" + why + ")");
	}
	Check(calls == 3, "back to back: three dials, one per Acquire");
	Check(pool.GetStats().creation_failures == 3, "back to back: three creation failures counted");
	Check(pool.GetStats().acquire_timeout_count == 0, "back to back: none counted as a timeout");
}

//! Something IS active, so a Release could still serve the request: the pool
//! keeps waiting for the caller's budget -- but retries creation on a backoff,
//! not on every wakeup, and the timeout message carries this call's own
//! creation failure.
static void TestWaitsWhenOthersActiveWithBackoff() {
	int calls = 0;
	ConnectionPool pool("busy", SmallPool(), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		if (calls == 1) {
			// Stands in for a live connection: the pool holds it as active.
			return std::make_shared<TdsConnection>();
		}
		throw std::runtime_error("Login failed for user 'sa'.");
	});

	auto held = pool.Acquire(1000);
	Check(held != nullptr, "busy pool: first creation succeeds");
	Check(pool.GetStats().active_connections == 1, "busy pool: one active");

	std::string why;
	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(800, &why);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "busy pool: second acquire fails");
	Check(ms >= 700 && ms < 2000,
		  "busy pool: waited the caller's budget (" + std::to_string(ms) + " ms), because a Release could still come");
	Check(Contains(why, "pool 'busy' timed out (1 active of 1, limit 4)"),
		  "busy pool: reported as the timeout it is (" + why + ")");
	Check(Contains(why, "this call's own attempt to create a connection failed: Login failed"),
		  "busy pool: and carries this call's own creation failure (" + why + ")");
	// Backoff 250 -> 500 -> ... inside an 800 ms budget: attempts near 0, 250
	// and 750 ms. Without backoff every spurious wakeup would dial again, and
	// on the real server each attempt costs a full login read.
	const auto failures = pool.GetStats().creation_failures;
	Check(failures >= 2 && failures <= 4,
		  "busy pool: creation retried on a backoff, " + std::to_string(failures) + " attempts in 800 ms");
	Check(pool.GetStats().acquire_timeout_count == 1, "busy pool: this one IS an acquire timeout");

	pool.Release(std::move(held));
}

//! After a failure, a success clears the recorded error and resets the
//! backoff; with nothing active the retry is immediate.
static void TestSuccessClearsTheError() {
	int calls = 0;
	ConnectionPool pool("recovers", SmallPool(), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		if (calls == 1) {
			throw std::runtime_error("transient");
		}
		return std::make_shared<TdsConnection>();
	});

	Check(pool.Acquire(500) == nullptr, "recovers: first attempt fails fast");
	Check(Contains(pool.GetLastCreateError(), "transient"), "recovers: the failure is recorded");
	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000);
	const auto ms = MsSince(t0);
	Check(conn != nullptr, "recovers: second attempt succeeds");
	Check(ms < 200, "recovers: immediately, nothing was active to wait for (" + std::to_string(ms) + " ms)");
	Check(pool.GetLastCreateError().empty(), "recovers: the recorded error is cleared by the success");
	Check(pool.GetStats().connections_created == 1, "recovers: one connection created");
	if (conn) {
		pool.Release(std::move(conn));
	}
}

//! The review-1538 scenario: one failure in the morning, a healthy pool all
//! day, then genuine exhaustion. The exhaustion must be reported as a timeout
//! -- not as that morning's creation failure.
static void TestRecoveredPoolReportsTimeoutNotOldError() {
	int calls = 0;
	ConnectionPool pool("day", SmallPool(/*limit=*/1), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		if (calls == 1) {
			throw std::runtime_error("TCP connect to host:1433 failed: Connection refused");
		}
		return std::make_shared<TdsConnection>();
	});

	Check(pool.Acquire(500) == nullptr, "day: 09:00, the server is restarting");
	auto held = pool.Acquire(500);
	Check(held != nullptr, "day: the next attempt succeeds and stays busy");

	std::string why;
	auto conn = pool.Acquire(300, &why);
	Check(conn == nullptr, "day: 14:00, the pool is at its limit");
	Check(Contains(why, "pool 'day' timed out (1 active of 1, limit 1)"),
		  "day: exhaustion is reported as exhaustion (" + why + ")");
	Check(!Contains(why, "Connection refused"), "day: the morning's error is NOT what the user is told (" + why + ")");
	Check(pool.GetLastCreateError().empty(), "day: pool stats no longer show the morning's error");
	Check(pool.GetStats().acquire_timeout_count == 1, "day: counted as a timeout");

	pool.Release(std::move(held));
}

//! TryAcquire (review of #382): an optional connection -- an extra bulk-load
//! writer -- on a full pool is the normal case, not a timeout. It returns at
//! once and leaves acquire_timeout_count alone; Acquire(0) still counts.
static void TestTryAcquireDoesNotCountATimeout() {
	ConnectionPool pool("try", SmallPool(/*limit=*/1), []() { return std::make_shared<TdsConnection>(); });
	auto held = pool.Acquire(500);
	Check(held != nullptr, "try: the one connection is taken");

	// A probe that comes back empty is not an acquisition and never waited, so
	// neither acquire_count nor acquire_wait_total_ms may move: an extra
	// bulk-load writer asks once per chunk while it shares the global writer,
	// and counting those grows acquire_count with the chunk count and skews any
	// acquire_wait_total_ms / acquire_count average (review of #382 / 7f13a0a).
	// Asserted so a refactor that moves the increment back to the top of
	// AcquireImpl cannot re-inflate the metric silently.
	const auto acquires_before_miss = pool.GetStats().acquire_count;
	const auto wait_before_miss = pool.GetStats().acquire_wait_total_ms;

	auto t0 = std::chrono::steady_clock::now();
	auto none = pool.TryAcquire();
	Check(none == nullptr, "try: nothing free on a full pool");
	Check(MsSince(t0) < 100, "try: returned at once");
	Check(pool.GetStats().acquire_timeout_count == 0, "try: not counted as an acquire timeout");
	Check(pool.GetStats().acquire_count == acquires_before_miss, "try: nor as an acquisition");
	Check(pool.GetStats().acquire_wait_total_ms == wait_before_miss, "try: nor as a wait");

	Check(pool.Acquire(0) == nullptr, "try: Acquire(0) on the same pool also finds nothing");
	Check(pool.GetStats().acquire_timeout_count == 1, "try: but Acquire(0) IS counted");

	pool.Release(std::move(held));
	// The other half of the split: a probe that GETS a connection is a real
	// checkout -- counted in connections_created / active_connections and later
	// Released -- so it IS counted, at the success return rather than at the top.
	const auto acquires_before_hit = pool.GetStats().acquire_count;
	auto got = pool.TryAcquire();
	Check(got != nullptr, "try: a released connection is taken");
	Check(pool.GetStats().acquire_count == acquires_before_hit + 1, "try: and a probe that gets one IS counted");
	if (got) {
		pool.Release(std::move(got));
	}
}

//! TryAcquire's two nulls (review of #382): a pool with nothing free is
//! transient, a creation the factory refuses is not -- an extra writer that
//! read the second as the first re-dialled on every backoff window.
static void TestTryAcquireTellsCreationFailureFromBusy() {
	int calls = 0;
	ConnectionPool pool("try2", SmallPool(/*limit=*/2), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		if (calls >= 2) {
			throw std::runtime_error("Login failed for user 'x'.");
		}
		return std::make_shared<TdsConnection>();
	});
	auto held = pool.Acquire(500);
	Check(held != nullptr, "try2: the first connection is created");
	const auto acquires_before = pool.GetStats().acquire_count;

	std::string why;
	bool creation_failed = false;
	auto none = pool.TryAcquire(&why, &creation_failed);
	Check(none == nullptr, "try2: the second creation fails");
	Check(creation_failed, "try2: and says so (" + why + ")");
	Check(Contains(why, "Login failed"), "try2: with the factory's reason (" + why + ")");

	ConnectionPool full("try3", SmallPool(/*limit=*/1), []() { return std::make_shared<TdsConnection>(); });
	auto only = full.Acquire(500);
	creation_failed = true;
	Check(full.TryAcquire(nullptr, &creation_failed) == nullptr, "try3: nothing free at the limit");
	Check(!creation_failed, "try3: which is not a creation failure");

	Check(pool.GetStats().acquire_count == acquires_before, "try2: TryAcquire is not counted as an acquire");
	pool.Release(std::move(held));
	full.Release(std::move(only));
}

int main() {
	TestThrowingFactoryFailsFast();
	TestSilentNullptrFactoryFailsFast();
	TestBackToBackFailuresStayFast();
	TestWaitsWhenOthersActiveWithBackoff();
	TestSuccessClearsTheError();
	TestRecoveredPoolReportsTimeoutNotOldError();
	TestTryAcquireDoesNotCountATimeout();
	TestTryAcquireTellsCreationFailureFromBusy();

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All pool creation-failure tests passed" << std::endl;
	return 0;
}

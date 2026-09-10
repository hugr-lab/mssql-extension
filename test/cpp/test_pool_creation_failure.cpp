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

static long long MsSince(std::chrono::steady_clock::time_point t0) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static PoolConfiguration SmallPool() {
	PoolConfiguration cfg;
	cfg.connection_limit = 4;
	cfg.acquire_timeout = 5;  // seconds; every Acquire below passes its own, shorter, budget
	return cfg;
}

//! Nothing active, factory throws: Acquire must come back at once with the
//! reason, not after the acquire timeout with "(timeout)".
static void TestThrowingFactoryFailsFast() {
	ConnectionPool pool("throws", SmallPool(), []() -> std::shared_ptr<TdsConnection> {
		throw std::runtime_error("Azure AD token for secret 'sp': expired at 2026-09-10 08:49:00 UTC");
	});

	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "throwing factory: no connection");
	Check(ms < 500, "throwing factory: returned in " + std::to_string(ms) + " ms, not after the 2000 ms budget");
	const std::string err = pool.GetLastCreateError();
	Check(err.find("expired at") != std::string::npos, "throwing factory: the reason is kept (" + err + ")");
	auto stats = pool.GetStats();
	Check(stats.creation_failures == 1, "throwing factory: one creation failure counted");
	Check(stats.acquire_timeout_count == 0, "throwing factory: NOT counted as an acquire timeout");
	Check(stats.total_connections == 0, "throwing factory: nothing leaked into the pool");
}

//! A factory that returns nullptr without saying why still gets a reason
//! recorded, so the caller never sees an empty one.
static void TestSilentNullptrFactoryFailsFast() {
	ConnectionPool pool("silent", SmallPool(), []() -> std::shared_ptr<TdsConnection> { return nullptr; });

	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "silent factory: no connection");
	Check(ms < 500, "silent factory: returned in " + std::to_string(ms) + " ms");
	Check(!pool.GetLastCreateError().empty(), "silent factory: a reason is recorded anyway");
	Check(pool.GetStats().creation_failures == 1, "silent factory: one creation failure counted");
}

//! Something IS active, so a Release could still serve the request: the pool
//! keeps waiting for the caller's budget -- but retries creation on a backoff,
//! not on every wakeup, and still hands the reason back on timeout.
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

	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(800);
	const auto ms = MsSince(t0);

	Check(conn == nullptr, "busy pool: second acquire fails");
	Check(ms >= 700 && ms < 2000,
		  "busy pool: waited the caller's budget (" + std::to_string(ms) + " ms), because a Release could still come");
	Check(pool.GetLastCreateError().find("Login failed") != std::string::npos,
		  "busy pool: the reason survives the wait (" + pool.GetLastCreateError() + ")");
	// Backoff 250 -> 500 -> ... inside an 800 ms budget: attempts near 0, 250
	// and 750 ms. Without backoff every spurious wakeup would dial again, and
	// on the real server each attempt costs a full login read.
	const auto failures = pool.GetStats().creation_failures;
	Check(failures >= 2 && failures <= 4,
		  "busy pool: creation retried on a backoff, " + std::to_string(failures) + " attempts in 800 ms");
	Check(pool.GetStats().acquire_timeout_count == 1, "busy pool: this one IS an acquire timeout");

	pool.Release(std::move(held));
}

//! After a failure, a success resets the backoff so the next refill is not
//! slowed by an old transient error.
static void TestSuccessResetsBackoff() {
	int calls = 0;
	ConnectionPool pool("recovers", SmallPool(), [&calls]() -> std::shared_ptr<TdsConnection> {
		calls++;
		if (calls == 1) {
			throw std::runtime_error("transient");
		}
		return std::make_shared<TdsConnection>();
	});

	Check(pool.Acquire(500) == nullptr, "recovers: first attempt fails fast");
	// The backoff after one failure is 250 ms; a second Acquire inside that
	// window waits for it rather than failing immediately, then succeeds.
	auto t0 = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(2000);
	const auto ms = MsSince(t0);
	Check(conn != nullptr, "recovers: second attempt succeeds");
	Check(ms < 1000, "recovers: and did so within the first backoff step (" + std::to_string(ms) + " ms)");
	Check(pool.GetStats().connections_created == 1, "recovers: one connection created");
	if (conn) {
		pool.Release(std::move(conn));
	}
}

int main() {
	TestThrowingFactoryFailsFast();
	TestSilentNullptrFactoryFailsFast();
	TestWaitsWhenOthersActiveWithBackoff();
	TestSuccessResetsBackoff();

	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All pool creation-failure tests passed" << std::endl;
	return 0;
}

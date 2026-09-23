// test/cpp/test_pool_prewarm.cpp
//
// ConnectionPool::Prewarm (issue #324): open connections up front until the
// pool holds `target` of them, the logins CONCURRENTLY. mssql_min_connections
// used to open nothing, so a session paid its logins one by one at the first
// queries that needed them.
//
// No server: the factories sleep to stand in for a login and hand back an
// unconnected TdsConnection. Part of STANDALONE_TEST_SOURCES (`make
// test-cpp`), which CI runs.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

static PoolConfiguration Pool(size_t limit) {
	PoolConfiguration cfg;
	cfg.connection_limit = limit;
	cfg.acquire_timeout = 5;
	return cfg;
}

static const int LOGIN_MS = 200;

//! Four 200 ms "logins" in about the time of one, all landing idle.
static void TestConcurrentLogins() {
	std::atomic<int> in_flight(0), max_in_flight(0);
	ConnectionPool pool("prewarm", Pool(8), [&]() {
		const int now = ++in_flight;
		int seen = max_in_flight.load();
		while (now > seen && !max_in_flight.compare_exchange_weak(seen, now)) {
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(LOGIN_MS));
		--in_flight;
		return std::make_shared<TdsConnection>();
	});
	const auto t0 = std::chrono::steady_clock::now();
	const size_t opened = pool.Prewarm(4);
	const auto ms = MsSince(t0);
	Check(opened == 4, "concurrent: opened " + std::to_string(opened) + " of 4");
	Check(max_in_flight.load() == 4,
		  "concurrent: " + std::to_string(max_in_flight.load()) + " logins in flight at once");
	Check(ms < 2 * LOGIN_MS, "concurrent: took " + std::to_string(ms) + " ms, one after another would be 800");
	auto stats = pool.GetStats();
	Check(stats.total_connections == 4 && stats.idle_connections == 4 && stats.connections_created == 4,
		  "concurrent: 4 total, 4 idle, 4 created");
	Check(pool.Prewarm(4) == 0, "concurrent: a pool already at the target opens nothing more");
}

//! Never past the limit.
static void TestCappedAtLimit() {
	ConnectionPool pool("prewarm-cap", Pool(2), []() { return std::make_shared<TdsConnection>(); });
	Check(pool.Prewarm(5) == 2, "cap: opened up to the limit of 2");
	Check(pool.GetStats().total_connections == 2, "cap: total 2");
}

//! A failing login is counted and its reason kept; the others still land.
static void TestPartialFailure() {
	std::atomic<int> calls(0);
	ConnectionPool pool("prewarm-fail", Pool(4), [&]() -> std::shared_ptr<TdsConnection> {
		if (++calls == 2) {
			throw std::runtime_error("Login failed for user 'x'.");
		}
		return std::make_shared<TdsConnection>();
	});
	std::string why;
	const size_t opened = pool.Prewarm(3, &why);
	auto stats = pool.GetStats();
	Check(opened == 2, "partial: opened 2 of 3");
	Check(stats.total_connections == 2,
		  "partial: the failed slot is given back (" + std::to_string(stats.total_connections) + " total)");
	Check(stats.creation_failures == 1, "partial: one creation failure counted");
	Check(why.find("Login failed") != std::string::npos, "partial: the reason is handed back (" + why + ")");
}

int main() {
	TestConcurrentLogins();
	TestCappedAtLimit();
	TestPartialFailure();
	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All pool prewarm tests passed" << std::endl;
	return 0;
}

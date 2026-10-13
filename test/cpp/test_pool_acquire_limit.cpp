// test/cpp/test_pool_acquire_limit.cpp
//
// ConnectionPool::Acquire never opens more connections than the limit (issue
// #461). The login runs without the pool mutex, and the count of connections
// used to grow only after it: every caller arriving during a login passed the
// same "below the limit" test and dialled too -- 21 connections on a limit of
// 8, 79 on 64, measured against a server. The slot is now taken before the
// login and given back if it fails, as Prewarm always did.
//
// No server: the factories sleep to stand in for a login and hand back an
// unconnected TdsConnection (Acquire does not validate a new one; Release
// closes it). Part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

static PoolConfiguration Pool(size_t limit) {
	PoolConfiguration cfg;
	cfg.connection_limit = limit;
	cfg.acquire_timeout = 10;
	return cfg;
}

//! Until a login has started -- the head start the tests below need, without
//! a sleep a loaded runner can outlast (review of #461).
static void WaitFor(const std::atomic<bool> &flag) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	Check(flag.load(), "the first login started");
}

static void RaiseTo(std::atomic<int> &peak, int value) {
	int seen = peak.load();
	while (value > seen && !peak.compare_exchange_weak(seen, value)) {
	}
}

//! Twelve callers at once on a limit of three: three logins, never more open.
static void TestConcurrentAcquireStaysAtLimit() {
	const size_t LIMIT = 3;
	const int CALLERS = 12;
	std::atomic<int> logins(0), in_flight(0), max_in_flight(0);
	ConnectionPool pool("limit", Pool(LIMIT), [&]() {
		++logins;
		RaiseTo(max_in_flight, ++in_flight);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		--in_flight;
		return std::make_shared<TdsConnection>();
	});

	// The pool's own count, sampled while the callers run.
	std::atomic<bool> done(false);
	std::atomic<int> peak_total(0);
	std::thread monitor([&]() {
		while (!done.load()) {
			RaiseTo(peak_total, static_cast<int>(pool.GetStats().total_connections));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});

	// Every caller holds what it got until all have returned, so the callers
	// past the limit wait out their timeout rather than reuse a release.
	std::atomic<int> returned(0), acquired(0);
	std::vector<std::thread> callers;
	for (int i = 0; i < CALLERS; i++) {
		callers.emplace_back([&]() {
			auto conn = pool.Acquire(1000);
			if (conn) {
				++acquired;
			}
			++returned;
			while (returned.load() < CALLERS) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			if (conn) {
				pool.Release(conn);
			}
		});
	}
	for (auto &caller : callers) {
		caller.join();
	}
	done.store(true);
	monitor.join();

	Check(logins.load() == static_cast<int>(LIMIT),
		  "limit: " + std::to_string(logins.load()) + " logins for a limit of " + std::to_string(LIMIT));
	Check(max_in_flight.load() <= static_cast<int>(LIMIT),
		  "limit: " + std::to_string(max_in_flight.load()) + " logins in flight at once");
	Check(peak_total.load() <= static_cast<int>(LIMIT),
		  "limit: the pool counted " + std::to_string(peak_total.load()) + " connections at its peak");
	Check(acquired.load() == static_cast<int>(LIMIT),
		  "limit: " + std::to_string(acquired.load()) + " callers got a connection");
	Check(pool.GetStats().acquire_timeout_count == static_cast<size_t>(CALLERS) - LIMIT,
		  "limit: the others timed out (" + std::to_string(pool.GetStats().acquire_timeout_count) + ")");
}

//! A failed login gives its slot back and wakes a caller waiting on a pool
//! that read full only because of it: that caller dials at once instead of at
//! the end of its timeout. (This guards the wake-up: before the reservation
//! the second caller simply dialled.)
static void TestFailedLoginWakesWaiter() {
	std::atomic<int> calls(0);
	std::atomic<bool> entered(false);
	ConnectionPool pool("limit-fail", Pool(1), [&]() -> std::shared_ptr<TdsConnection> {
		if (++calls == 1) {
			entered.store(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			throw std::runtime_error("Login failed for user 'x'.");
		}
		return std::make_shared<TdsConnection>();
	});
	std::string first_why;
	std::thread first([&]() { Check(!pool.Acquire(10000, &first_why), "fail: the failing login gets nothing"); });
	// The second caller arrives while the first login holds the only slot.
	WaitFor(entered);
	const auto start = std::chrono::steady_clock::now();
	auto conn = pool.Acquire(10000);
	const auto waited =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	first.join();
	Check(conn != nullptr, "fail: the waiting caller gets a connection");
	// Without the wake-up it dials at the end of its 10 s timeout. The bound is
	// far from both, for a loaded runner.
	Check(waited < 5000, "fail: woken by the failure, not by its timeout (" + std::to_string(waited) + " ms)");
	Check(first_why.find("Login failed") != std::string::npos, "fail: the reason is handed back (" + first_why + ")");
	auto stats = pool.GetStats();
	Check(stats.total_connections == 1 && stats.creation_failures == 1,
		  "fail: one connection, one failure (" + std::to_string(stats.total_connections) + " total)");
	pool.Release(conn);
}

//! A login that throws something that is not a std::exception: a failed
//! login, its slot given back -- not an exception that leaves the slot taken.
static void TestNonStandardThrowGivesSlotBack() {
	std::atomic<int> calls(0);
	ConnectionPool pool("limit-throw", Pool(1), [&]() -> std::shared_ptr<TdsConnection> {
		if (++calls == 1) {
			throw 42;
		}
		return std::make_shared<TdsConnection>();
	});
	std::string why;
	Check(!pool.Acquire(1000, &why), "throw: the login fails");
	Check(why.find("non-standard") != std::string::npos, "throw: named as a failed login (" + why + ")");
	auto conn = pool.Acquire(1000);
	Check(conn != nullptr, "throw: the slot came back, the next login uses it");
	pool.Release(conn);
}

//! An exception out of the login path -- here ErrorData failing to parse a
//! factory's what() that starts with '{' -- still gives the slot back: on a
//! pool of one the next Acquire must get a connection, not wait out the
//! timeout on a pool that reads full (review of #461).
static void TestExceptionGivesSlotBack() {
	std::atomic<int> calls(0);
	ConnectionPool pool("limit-exception", Pool(1), [&]() -> std::shared_ptr<TdsConnection> {
		if (++calls == 1) {
			throw std::runtime_error("{not json");
		}
		return std::make_shared<TdsConnection>();
	});
	bool threw = false;
	try {
		pool.Acquire(1000);
	} catch (...) {
		threw = true;
	}
	Check(threw, "exception: the login path threw");
	Check(pool.GetStats().total_connections == 0,
		  "exception: the slot came back (" + std::to_string(pool.GetStats().total_connections) + " total)");
	auto conn = pool.Acquire(1000);
	Check(conn != nullptr, "exception: the next Acquire gets a connection");
	pool.Release(conn);
}

//! TryAcquire (an extra bulk-load writer) does not dial over a slot a login
//! in flight holds: it answers at once with nothing.
static void TestTryAcquireRespectsReservation() {
	std::atomic<int> calls(0);
	std::atomic<bool> entered(false);
	ConnectionPool pool("limit-try", Pool(1), [&]() {
		++calls;
		entered.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		return std::make_shared<TdsConnection>();
	});
	std::shared_ptr<TdsConnection> held;
	std::thread first([&]() { held = pool.Acquire(5000); });
	WaitFor(entered);
	Check(!pool.TryAcquire(), "try: nothing while the only slot is logging in");
	first.join();
	Check(held != nullptr, "try: the first login lands");
	Check(calls.load() == 1, "try: one login, not two (" + std::to_string(calls.load()) + ")");
	pool.Release(held);
}

int main() {
	TestConcurrentAcquireStaysAtLimit();
	TestFailedLoginWakesWaiter();
	TestNonStandardThrowGivesSlotBack();
	TestExceptionGivesSlotBack();
	TestTryAcquireRespectsReservation();
	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All pool acquire-limit tests passed" << std::endl;
	return 0;
}

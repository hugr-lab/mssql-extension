// test/cpp/test_pool_prewarm.cpp
//
// ConnectionPool::Prewarm (issue #324): open connections up front until the
// pool holds `target` of them, the logins CONCURRENTLY. mssql_min_connections
// used to open nothing, so a session paid its logins one by one at the first
// queries that needed them.
//
// No server: the factories sleep to stand in for a login and hand back a
// TdsConnection whose socket is connected to a loopback listener and whose
// state is Idle -- what a real login leaves, and what the pool now checks
// before a connection goes idle (review of #386). Part of
// STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs on Linux and macOS.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "tds/tds_connection.hpp"
#include "tds/tds_connection_pool.hpp"

using duckdb::tds::ConnectionPool;
using duckdb::tds::ConnectionState;
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

//! Accepts on 127.0.0.1 and holds every socket open until destroyed.
class LoopbackServer {
public:
	LoopbackServer() {
		listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		listen(listen_fd_, 64);
		socklen_t len = sizeof(addr);
		getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
		port_ = ntohs(addr.sin_port);
		thread_ = std::thread([this]() {
			for (;;) {
				int fd = accept(listen_fd_, nullptr, nullptr);
				if (fd < 0 || stop_.load()) {
					if (fd >= 0) {
						close(fd);
					}
					return;
				}
				std::lock_guard<std::mutex> guard(lock_);
				accepted_.push_back(fd);
			}
		});
	}
	~LoopbackServer() {
		// Woken by a connection of its own: closing the listening socket does
		// not interrupt accept() on every platform (macOS).
		stop_.store(true);
		int wake = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port_);
		connect(wake, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
		thread_.join();
		close(wake);
		close(listen_fd_);
		for (int fd : accepted_) {
			close(fd);
		}
	}
	uint16_t Port() const {
		return port_;
	}

private:
	int listen_fd_ = -1;
	std::atomic<bool> stop_{false};
	uint16_t port_ = 0;
	std::thread thread_;
	std::mutex lock_;
	std::vector<int> accepted_;
};

static LoopbackServer *g_server = nullptr;

//! What a login leaves: a connected socket, state Idle.
static std::shared_ptr<TdsConnection> LoggedIn() {
	auto conn = std::make_shared<TdsConnection>();
	if (!conn->Connect("127.0.0.1", g_server->Port(), 5) ||
		!conn->TransitionState(ConnectionState::Authenticating, ConnectionState::Idle)) {
		throw std::runtime_error("loopback connect failed");
	}
	return conn;
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
		return LoggedIn();
	});
	const size_t opened = pool.Prewarm(4);
	Check(opened == 4, "concurrent: opened " + std::to_string(opened) + " of 4");
	// Concurrency is asserted by overlap, not by wall time: a loaded runner
	// stretches the clock, never the overlap (review of #386).
	Check(max_in_flight.load() == 4,
		  "concurrent: " + std::to_string(max_in_flight.load()) + " logins in flight at once");
	auto stats = pool.GetStats();
	Check(stats.total_connections == 4 && stats.idle_connections == 4 && stats.connections_created == 4,
		  "concurrent: 4 total, 4 idle, 4 created");
	Check(pool.Prewarm(4) == 0, "concurrent: a pool already at the target opens nothing more");
}

//! Never past the limit.
static void TestCappedAtLimit() {
	ConnectionPool pool("prewarm-cap", Pool(2), []() { return LoggedIn(); });
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
		return LoggedIn();
	});
	std::string why;
	const size_t opened = pool.Prewarm(3, &why);
	auto stats = pool.GetStats();
	Check(opened == 2, "partial: opened 2 of 3");
	Check(stats.total_connections == 2,
		  "partial: the failed slot is given back (" + std::to_string(stats.total_connections) + " total)");
	Check(stats.creation_failures == 1, "partial: one creation failure counted");
	Check(why.find("Login failed") != std::string::npos, "partial: the reason is handed back (" + why + ")");
	// Beside connections that opened, a refused login is not the state of a
	// healthy pool: counted, handed back, but not left as the pool's recorded
	// error -- mssql_pool_stats would report it on a pool that works (review
	// of #386, the rule spec 073 set for Acquire).
	Check(pool.GetLastCreateError().empty(),
		  "partial: a healthy pool records no error (" + pool.GetLastCreateError() + ")");
}

//! Nothing opened: that IS the pool's state, recorded like a failed Acquire.
static void TestTotalFailure() {
	ConnectionPool pool("prewarm-dead", Pool(4), []() -> std::shared_ptr<TdsConnection> {
		throw std::runtime_error("Login failed for user 'x'.");
	});
	std::string why;
	Check(pool.Prewarm(2, &why) == 0, "dead: opened nothing");
	auto stats = pool.GetStats();
	Check(stats.total_connections == 0, "dead: every reserved slot given back");
	Check(stats.creation_failures == 2, "dead: two creation failures counted");
	Check(pool.GetLastCreateError().find("Login failed") != std::string::npos,
		  "dead: the pool records the reason (" + pool.GetLastCreateError() + ")");
}

//! A factory that throws something that is not a std::exception: a failed
//! login, not std::terminate over running threads, and the slot given back.
static void TestNonStandardThrow() {
	std::atomic<int> calls(0);
	ConnectionPool pool("prewarm-throw", Pool(4), [&]() -> std::shared_ptr<TdsConnection> {
		if (++calls == 1) {
			throw 42;
		}
		return LoggedIn();
	});
	const size_t opened = pool.Prewarm(3);
	auto stats = pool.GetStats();
	Check(opened == 2, "throw: opened 2 of 3 (" + std::to_string(opened) + ")");
	Check(stats.total_connections == 2, "throw: the failed slot is given back");
	Check(stats.creation_failures == 1, "throw: counted as a creation failure");
}

//! mssql_connection_cache = false means no idle connection, ever: nothing is
//! prewarmed, and a released connection is closed.
static void TestCachingOff() {
	auto cfg = Pool(4);
	cfg.connection_cache = false;
	std::atomic<int> calls(0);
	ConnectionPool pool("prewarm-nocache", cfg, [&]() {
		++calls;
		return LoggedIn();
	});
	Check(pool.Prewarm(3) == 0, "nocache: prewarm opens nothing");
	Check(calls.load() == 0, "nocache: no login at all");
	pool.Release(pool.Acquire());
	auto stats = pool.GetStats();
	Check(stats.total_connections == 0 && stats.idle_connections == 0, "nocache: the pool stays empty");
}

int main() {
	LoopbackServer server;
	g_server = &server;
	TestConcurrentLogins();
	TestCappedAtLimit();
	TestPartialFailure();
	TestNonStandardThrow();
	TestTotalFailure();
	TestCachingOff();
	if (g_failures > 0) {
		std::cerr << g_failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All pool prewarm tests passed" << std::endl;
	return 0;
}

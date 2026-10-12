// Issue #456: a pushed DML whose connection dies mid-statement fails within
// seconds -- it does not spin.
//
// A pushed statement (mssql_dml, spec 080 PR 3b-3d) waits with no deadline:
// MSSQLSimpleQuery::ExecuteInterruptible reads in 1 s slices and goes round
// again after a read that ended while the socket still says it is connected.
// Under TLS (the default) a closed peer came back from TlsImpl::Receive as 0,
// like a timeout, and a TLS read error left the socket "connected", so a
// connection that died -- a server restart, a KILL, a failover, a network cut
// -- had the loop calling SSL_read at once on every pass, at 100% CPU, until
// Ctrl-C.
//
// The cut is made here without touching the server: the catalog is attached
// through a loopback TCP proxy, the statement waits on the server (WAITFOR, no
// locks), and the proxy shuts both ends of every connection down mid-wait.
// The same is done over plain TCP (Encrypt=no), whose recv() == 0 path always
// failed fast -- the control.
//
// A watchdog interrupts a statement still running after 20 s and the test
// fails; with the fix it returns about 1.5 s after it starts, when the cut lands.
//
// Skips (exit 0) when MSSQL_TEST_PASS is unset, like the other live C++ tests.
// POSIX only (the proxy uses BSD sockets and poll), as the live C++ gates run.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "duckdb.hpp"
#include "mssql_extension.hpp"

using namespace duckdb;

namespace {

std::string getenv_or(const char *name, const char *fallback) {
	const char *v = std::getenv(name);
	return v && *v ? v : fallback;
}

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

bool Run(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError() << std::endl;
		return false;
	}
	return true;
}

int64_t Scalar(Connection &conn, const std::string &sql) {
	auto result = conn.Query(sql);
	if (result->HasError()) {
		std::cerr << "query failed: " << result->GetError() << std::endl;
		return -1;
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return -1;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>();
}

//! A loopback TCP proxy to the SQL Server under test. Every accepted
//! connection is pumped to a fresh upstream connection; Cut() shuts every live
//! pair down at once, which the client sees as the server going away.
class CutProxy {
public:
	bool Start(const std::string &host, const std::string &port) {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &upstream_) != 0 || !upstream_) {
			return false;
		}
		listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(listen_fd_, 16) != 0) {
			return false;
		}
		socklen_t len = sizeof(addr);
		getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
		port_ = ntohs(addr.sin_port);
		acceptor_ = std::thread([this]() { AcceptLoop(); });
		return true;
	}

	int Port() const {
		return port_;
	}

	//! Shut every live connection down, both directions.
	void Cut() {
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto fd : live_) {
			shutdown(fd, SHUT_RDWR);
		}
	}

	~CutProxy() {
		stop_ = true;
		Cut();
		if (acceptor_.joinable()) {
			acceptor_.join();
		}
		for (auto &t : pumps_) {
			if (t.joinable()) {
				t.join();
			}
		}
		if (listen_fd_ >= 0) {
			close(listen_fd_);
		}
		if (upstream_) {
			freeaddrinfo(upstream_);
		}
	}

private:
	void AcceptLoop() {
		while (!stop_) {
			pollfd p{listen_fd_, POLLIN, 0};
			if (poll(&p, 1, 200) <= 0) {
				continue;
			}
			int client = accept(listen_fd_, nullptr, nullptr);
			if (client < 0) {
				continue;
			}
			// Every address the name resolves to, in order, as TdsSocket::Connect
			// tries them: `localhost` may come back as ::1 first.
			int server = -1;
			for (auto *ai = upstream_; ai && server < 0; ai = ai->ai_next) {
				server = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
				if (server >= 0 && connect(server, ai->ai_addr, ai->ai_addrlen) != 0) {
					close(server);
					server = -1;
				}
			}
			if (server < 0) {
				close(client);
				continue;
			}
			{
				std::lock_guard<std::mutex> lock(mutex_);
				live_.push_back(client);
				live_.push_back(server);
			}
			pumps_.emplace_back([this, client, server]() { Pump(client, server); });
		}
	}

	void Pump(int a, int b) {
		char buf[16384];
		for (;;) {
			pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
			if (poll(p, 2, 200) < 0) {
				break;
			}
			if (stop_ && !(p[0].revents | p[1].revents)) {
				break;
			}
			bool done = false;
			for (int i = 0; i < 2 && !done; i++) {
				if (!p[i].revents) {
					continue;
				}
				ssize_t n = recv(p[i].fd, buf, sizeof(buf), 0);
				if (n <= 0 || send(p[1 - i].fd, buf, n, 0) != n) {
					done = true;
				}
			}
			if (done) {
				break;
			}
		}
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto fd : {a, b}) {
			shutdown(fd, SHUT_RDWR);
			close(fd);
			for (auto it = live_.begin(); it != live_.end(); ++it) {
				if (*it == fd) {
					live_.erase(it);
					break;
				}
			}
		}
	}

	addrinfo *upstream_ = nullptr;
	int listen_fd_ = -1;
	int port_ = 0;
	std::atomic<bool> stop_{false};
	std::thread acceptor_;
	std::vector<std::thread> pumps_;
	std::mutex mutex_;
	std::vector<int> live_;
};

std::string Dsn(const std::string &host, const std::string &port, const char *encrypt) {
	return "Server=" + host + "," + port + ";Database=TestDB;User Id=" + getenv_or("MSSQL_TEST_USER", "sa") +
		   ";Password=" + getenv_or("MSSQL_TEST_PASS", "") + ";TrustServerCertificate=yes;Encrypt=" + encrypt;
}

//! The pushed statement, run through the proxy; the proxy is cut 1.5 s in.
void DeadMidStatement(Connection &conn, const std::string &host, const std::string &port, const char *encrypt) {
	const std::string label = std::string("Encrypt=") + encrypt;
	std::cout << "=== " << label << ": the connection dies while a pushed statement waits ===" << std::endl;
	CutProxy proxy;
	if (!proxy.Start(host, port)) {
		Check(false, label + ": the proxy starts");
		return;
	}
	// A pool of one: the statement takes the connection the warm-up opened,
	// and after the cut the next statement has to open a new one.
	Run(conn, "SET mssql_connection_limit = 1");
	if (!Run(conn, "ATTACH '" + Dsn("127.0.0.1", std::to_string(proxy.Port()), encrypt) + "' AS px (TYPE mssql)")) {
		Check(false, label + ": ATTACH through the proxy");
		Run(conn, "RESET mssql_connection_limit");
		return;
	}
	Run(conn, "RESET mssql_connection_limit");
	Check(Scalar(conn, "SELECT count(*) FROM px.dbo.dead_conn") == 1, label + ": the catalog reads through the proxy");

	std::mutex m;
	std::condition_variable cv;
	bool finished = false;
	bool watchdog_fired = false;
	std::thread cutter([&proxy]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		proxy.Cut();
	});
	std::thread watchdog([&]() {
		std::unique_lock<std::mutex> lock(m);
		if (!cv.wait_for(lock, std::chrono::seconds(20), [&]() { return finished; })) {
			watchdog_fired = true;
			conn.Interrupt();
		}
	});
	const auto started = std::chrono::steady_clock::now();
	auto result = conn.Query(
		"SELECT * FROM mssql_dml('px', 'WAITFOR DELAY ''00:00:30''', kind := 'update', "
		"target_schema := 'dbo', target_table := 'dead_conn')");
	const auto seconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() /
		1000.0;
	{
		std::lock_guard<std::mutex> lock(m);
		finished = true;
	}
	cv.notify_all();
	cutter.join();
	watchdog.join();

	Check(result->HasError(), label + ": the statement fails");
	Check(!watchdog_fired, label + ": on its own, not by the watchdog's interrupt (" + std::to_string(seconds) + " s)");
	if (result->HasError()) {
		std::cout << "       " << result->GetError().substr(0, 160) << std::endl;
	}
	Check(Scalar(conn, "SELECT count(*) FROM px.dbo.dead_conn") == 1,
		  label + ": the next statement runs, on a new connection");
	Check(Scalar(conn, "SELECT active_connections FROM mssql_pool_stats('px')") == 0,
		  label + ": nothing is left checked out");
	Run(conn, "DETACH px");
}

}  // namespace

int main() {
	// The proxy's send() may hit a connection Cut() just shut down: EPIPE, not
	// a SIGPIPE that ends the test with no message.
	signal(SIGPIPE, SIG_IGN);
	if (getenv_or("MSSQL_TEST_PASS", "").empty()) {
		std::cout << "MSSQL_TEST_PASS unset: skipping the dead-connection test" << std::endl;
		return 0;
	}
	const std::string host = getenv_or("MSSQL_TEST_HOST", "localhost");
	const std::string port = getenv_or("MSSQL_TEST_PORT", "1433");
	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection conn(db);
	if (!Run(conn, "ATTACH '" + Dsn(host, port, "yes") + "' AS direct (TYPE mssql)") ||
		!Run(conn,
			 "SELECT mssql_exec('direct', 'DROP TABLE IF EXISTS dbo.dead_conn; CREATE TABLE dbo.dead_conn "
			 "(id int PRIMARY KEY, v int); INSERT INTO dbo.dead_conn VALUES (1, 0)')")) {
		return 2;
	}

	DeadMidStatement(conn, host, port, "yes");
	DeadMidStatement(conn, host, port, "no");

	Run(conn, "SELECT mssql_exec('direct', 'DROP TABLE dbo.dead_conn')");
	if (failures) {
		std::cout << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}

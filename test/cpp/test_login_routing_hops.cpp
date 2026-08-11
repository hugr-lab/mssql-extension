// test/cpp/test_login_routing_hops.cpp
// Unit tests for login-time routing (ENVCHANGE type 20) -- spec 068.
//
// These tests do NOT require a running SQL Server, an Azure subscription or an
// Active Directory. They stand up a loopback fake TDS server on 127.0.0.1 that
// speaks exactly three things -- a PRELOGIN response, a LOGIN7 response, and
// nothing else -- and drive a REAL TdsConnection against it.
//
// What they pin (spec 068):
//   D1: ROUTING outranks success. A routed answer is a hop whether or not a
//       LOGINACK came with it. Before 068 the FEDAUTH path failed the
//       no-LOGINACK shape as "authentication failed", and SQL auth silently
//       "succeeded" against a gateway that had told it to leave.
//   D2: one hop driver behind all three auth paths, with MAX_ROUTING_HOPS=5,
//       a full handshake per hop, and a per-hop packet-id reset.
//   D3: integrated auth takes an authenticator FACTORY, so a hop obtains a
//       ticket for the routed host's SPN rather than retrying the gateway's.
//
// The fake server runs unencrypted (use_encrypt=false), so no TLS keys and no
// OpenSSL handshake are involved -- OpenSSL is linked only because
// tds_socket.hpp pulls the TLS context in.
//
// Build + run via the Makefile (matches the CI source set exactly):
//   make test-login-routing-hops
//
// Or compile manually (macOS/brew example):
//   c++ -std=c++17 -pthread -I src/include \
//       -I/opt/homebrew/opt/simdutf/include -I/opt/homebrew/opt/openssl@3/include \
//       test/cpp/test_login_routing_hops.cpp \
//       src/tds/tds_connection.cpp src/tds/tds_socket.cpp \
//       src/tds/tls/tds_tls_context.cpp src/tds/tls/tds_tls_impl.cpp \
//       src/tds/tds_packet.cpp src/tds/tds_protocol.cpp src/tds/tds_types.cpp \
//       src/tds/encoding/utf16.cpp \
//       -L/opt/homebrew/opt/simdutf/lib -L/opt/homebrew/opt/openssl@3/lib \
//       -lsimdutf -lssl -lcrypto -o build/test/test_login_routing_hops

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET ::close
#define INVALID_SOCK (-1)
#endif

#include "tds/tds_connection.hpp"
#include "tds/tds_protocol.hpp"
#include "tds/tds_types.hpp"

using namespace duckdb;
using namespace duckdb::tds;

namespace {

int g_checks = 0;

#define CHECK(cond)                                                                                             \
	do {                                                                                                        \
		g_checks++;                                                                                             \
		if (!(cond)) {                                                                                          \
			std::cerr << "ASSERT FAILED: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
			std::exit(1);                                                                                       \
		}                                                                                                       \
	} while (0)

//===----------------------------------------------------------------------===//
// Token builders (same wire shapes as test_login_error_state.cpp)
//===----------------------------------------------------------------------===//

void AppendU16LE(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFF));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32LE(std::vector<uint8_t> &buf, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
	}
}

void AppendU64LE(std::vector<uint8_t> &buf, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
	}
}

void AppendUtf16LE(std::vector<uint8_t> &buf, const std::string &s) {
	for (char c : s) {
		buf.push_back(static_cast<uint8_t>(c));
		buf.push_back(0x00);
	}
}

// Minimal successful LOGINACK (interface + TDS 7.4 + empty progname + progver).
void AppendLoginAck(std::vector<uint8_t> &buf) {
	std::vector<uint8_t> body;
	body.push_back(0x01);
	AppendU32LE(body, 0x74000004);
	body.push_back(0x00);
	AppendU32LE(body, 0x00000000);
	buf.push_back(static_cast<uint8_t>(TokenType::LOGINACK));
	AppendU16LE(buf, static_cast<uint16_t>(body.size()));
	buf.insert(buf.end(), body.begin(), body.end());
}

// Final DONE in the TDS 7.2+ layout: Status(2) CurCmd(2) RowCount(8).
void AppendDone(std::vector<uint8_t> &buf) {
	buf.push_back(static_cast<uint8_t>(TokenType::DONE));
	AppendU16LE(buf, 0x0000);
	AppendU16LE(buf, 0x0000);
	AppendU64LE(buf, 0);
}

// ROUTING ENVCHANGE (type 20) per [MS-TDS] 2.2.7.13.
void AppendRoutingEnvChange(std::vector<uint8_t> &buf, const std::string &server, uint16_t port) {
	std::vector<uint8_t> routing;
	routing.push_back(0x00);  // Protocol: TCP
	AppendU16LE(routing, port);
	AppendU16LE(routing, static_cast<uint16_t>(server.size()));	 // length in CHARACTERS
	AppendUtf16LE(routing, server);

	std::vector<uint8_t> body;
	body.push_back(20);	 // ENVCHANGE type 20 = ROUTING
	AppendU16LE(body, static_cast<uint16_t>(routing.size()));
	body.insert(body.end(), routing.begin(), routing.end());
	AppendU16LE(body, 0);  // OldValue length

	buf.push_back(static_cast<uint8_t>(TokenType::ENVCHANGE));
	AppendU16LE(buf, static_cast<uint16_t>(body.size()));
	buf.insert(buf.end(), body.begin(), body.end());
}

// Pull the ServerName out of a LOGIN7 payload. [MS-TDS] 2.2.6.4: the fixed
// portion is 94 bytes and the (offset, length) pairs start at byte 36 in field
// order HostName, UserName, Password, AppName, ServerName -- so ibServerName is
// at 52 and cchServerName (in UTF-16 code units, not bytes) at 54. Offsets are
// relative to the start of the LOGIN7 payload.
//
// Anomalies return a DISTINGUISHABLE sentinel rather than "". This helper exists
// to make the ServerName fix non-revertible, so when a CHECK on its result
// fails, the message has to say whether the harness misread the packet or the
// code under test sent the wrong name -- collapsing both into "" would read as
// a production regression when it might be a bug in this file.
std::string ExtractLogin7ServerName(const std::vector<uint8_t> &payload) {
	if (payload.size() < 94) {	// [MS-TDS] 2.2.6.4 fixed portion is 94 bytes
		return "<short-payload>";
	}
	const uint16_t ib = static_cast<uint16_t>(payload[52] | (payload[53] << 8));
	const uint16_t cch = static_cast<uint16_t>(payload[54] | (payload[55] << 8));
	if (ib < 94) {	// variable data cannot start inside the fixed portion
		return "<offset-in-header>";
	}
	if (static_cast<size_t>(ib) + static_cast<size_t>(cch) * 2 > payload.size()) {
		return "<oob-offset>";
	}
	std::string out;
	for (uint16_t i = 0; i < cch; i++) {
		const uint16_t unit = static_cast<uint16_t>(payload[ib + i * 2] | (payload[ib + i * 2 + 1] << 8));
		// Every fixture here is ASCII-range; anything else would be a bug in
		// the test, not in the code under test.
		out.push_back(static_cast<char>(unit & 0xFF));
	}
	return out;
}

//===----------------------------------------------------------------------===//
// Loopback fake TDS server
//
// Binds 127.0.0.1:0 (kernel-assigned port, read back with getsockname) so
// nothing collides with a developer's SQL Server or a parallel CI job. One
// thread accepts connections in a loop and answers PRELOGIN + LOGIN7 with
// caller-supplied bytes. Accept and recv both poll with a timeout, so a wiring
// bug fails the test in seconds instead of hanging the job.
//===----------------------------------------------------------------------===//

constexpr int POLL_TIMEOUT_MS = 5000;

class FakeTdsServer {
public:
	// `login_response` is the token stream sent in answer to LOGIN7. It is a
	// callback so a scenario can vary its answer per connection (e.g. route on
	// the first, accept on the second).
	using LoginResponder = std::function<std::vector<uint8_t>(int connection_index)>;

	explicit FakeTdsServer(LoginResponder responder) : responder_(std::move(responder)) {
		listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
		if (listen_fd_ == INVALID_SOCK) {
			std::cerr << "fake server: socket() failed" << std::endl;
			std::exit(1);
		}
		int reuse = 1;
		::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
		addr.sin_port = 0;	// kernel picks
		if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
			std::cerr << "fake server: bind() failed" << std::endl;
			std::exit(1);
		}
		if (::listen(listen_fd_, 8) != 0) {
			std::cerr << "fake server: listen() failed" << std::endl;
			std::exit(1);
		}
		socklen_t len = sizeof(addr);
		if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
			std::cerr << "fake server: getsockname() failed" << std::endl;
			std::exit(1);
		}
		port_ = ntohs(addr.sin_port);

		thread_ = std::thread([this]() { Serve(); });
	}

	~FakeTdsServer() {
		Stop();
	}

	void Stop() {
		if (stopped_) {
			return;
		}
		stopped_ = true;
		running_ = false;
		if (thread_.joinable()) {
			thread_.join();
		}
		if (listen_fd_ != INVALID_SOCK) {
			CLOSE_SOCKET(listen_fd_);
			listen_fd_ = INVALID_SOCK;
		}
	}

	uint16_t GetPort() const {
		return port_;
	}
	int ConnectionCount() const {
		return connections_.load();
	}
	// Packet id of the FIRST packet of the FIRST message on each connection.
	// A hop must reset the sequence, so every connection must see 1.
	// Returns a COPY under the lock: the server thread appends to the vector
	// and could reallocate under a returned reference.
	std::vector<int> FirstPacketIds() const {
		std::lock_guard<std::mutex> guard(mutex_);
		return first_packet_ids_;
	}

	// The LOGIN7 ServerName field ([MS-TDS] 2.2.6.4) captured per connection,
	// decoded from UTF-16LE. This is what pins the routed-target handling: on a
	// hop it must be the "hostname\instance" form, not the DNS name.
	std::vector<std::string> ServerNames() const {
		std::lock_guard<std::mutex> guard(mutex_);
		return server_names_;
	}

private:
	void Serve() {
		while (running_) {
			socket_t client = AcceptWithTimeout();
			if (client == INVALID_SOCK) {
				continue;
			}
			const int index = connections_.fetch_add(1);
			HandleConnection(client, index);
			CLOSE_SOCKET(client);
		}
	}

	socket_t AcceptWithTimeout() {
#if defined(_WIN32)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(listen_fd_, &readfds);
		timeval tv = {0, 200 * 1000};
		int rc = ::select(0, &readfds, nullptr, nullptr, &tv);
		if (rc <= 0) {
			return INVALID_SOCK;
		}
#else
		pollfd pfd = {};
		pfd.fd = listen_fd_;
		pfd.events = POLLIN;
		int rc = ::poll(&pfd, 1, 200);
		if (rc <= 0) {
			return INVALID_SOCK;
		}
#endif
		return ::accept(listen_fd_, nullptr, nullptr);
	}

	// Read one complete TDS message (packets until the EOM status bit).
	// Returns false on timeout / peer close. Records the first packet's id, and
	// appends the reassembled payload to `payload_out` when given.
	bool ReadMessage(socket_t fd, bool record_first_id, std::vector<uint8_t> *payload_out = nullptr) {
		bool eom = false;
		bool first = true;
		while (!eom) {
			uint8_t header[8];
			if (!ReadExactly(fd, header, sizeof(header))) {
				return false;
			}
			if (first && record_first_id) {
				std::lock_guard<std::mutex> guard(mutex_);
				first_packet_ids_.push_back(static_cast<int>(header[6]));
				first = false;
			}
			const uint16_t length = (static_cast<uint16_t>(header[2]) << 8) | header[3];
			if (length < 8) {
				return false;
			}
			std::vector<uint8_t> payload(length - 8);
			if (!payload.empty() && !ReadExactly(fd, payload.data(), payload.size())) {
				return false;
			}
			if (payload_out) {
				payload_out->insert(payload_out->end(), payload.begin(), payload.end());
			}
			eom = (header[1] & 0x01) != 0;
		}
		return true;
	}

	static bool ReadExactly(socket_t fd, uint8_t *buf, size_t n) {
		size_t got = 0;
		while (got < n) {
#if defined(_WIN32)
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(fd, &readfds);
			timeval tv = {POLL_TIMEOUT_MS / 1000, 0};
			if (::select(0, &readfds, nullptr, nullptr, &tv) <= 0) {
				return false;
			}
			int rc = ::recv(fd, reinterpret_cast<char *>(buf + got), static_cast<int>(n - got), 0);
#else
			pollfd pfd = {};
			pfd.fd = fd;
			pfd.events = POLLIN;
			if (::poll(&pfd, 1, POLL_TIMEOUT_MS) <= 0) {
				return false;
			}
			ssize_t rc = ::recv(fd, buf + got, n - got, 0);
#endif
			if (rc <= 0) {
				return false;
			}
			got += static_cast<size_t>(rc);
		}
		return true;
	}

	static bool SendAll(socket_t fd, const std::vector<uint8_t> &data) {
		size_t sent = 0;
		while (sent < data.size()) {
#if defined(_WIN32)
			int rc =
				::send(fd, reinterpret_cast<const char *>(data.data() + sent), static_cast<int>(data.size() - sent), 0);
#else
			ssize_t rc = ::send(fd, data.data() + sent, data.size() - sent, 0);
#endif
			if (rc <= 0) {
				return false;
			}
			sent += static_cast<size_t>(rc);
		}
		return true;
	}

	// Wrap a payload in one TDS TABULAR_RESULT packet with EOM set.
	static std::vector<uint8_t> Frame(const std::vector<uint8_t> &payload) {
		std::vector<uint8_t> out;
		const uint16_t length = static_cast<uint16_t>(payload.size() + 8);
		out.push_back(static_cast<uint8_t>(PacketType::TABULAR_RESULT));
		out.push_back(0x01);  // EOM
		out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>(length & 0xFF));
		out.push_back(0x00);  // SPID hi
		out.push_back(0x00);  // SPID lo
		out.push_back(0x01);  // packet id
		out.push_back(0x00);  // window
		out.insert(out.end(), payload.begin(), payload.end());
		return out;
	}

	// PRELOGIN response advertising VERSION + ENCRYPT_NOT_SUP. The client runs
	// with use_encrypt=false, which accepts anything except ENCRYPT_REQ.
	static std::vector<uint8_t> BuildPreloginResponse() {
		// Option headers: VERSION(0), ENCRYPTION(1), TERMINATOR(0xFF).
		// Offsets are big-endian and relative to the start of the payload.
		const uint16_t header_size = 5 + 5 + 1;
		std::vector<uint8_t> out;
		out.push_back(0x00);  // VERSION
		out.push_back(static_cast<uint8_t>((header_size >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>(header_size & 0xFF));
		out.push_back(0x00);
		out.push_back(0x06);  // len 6
		out.push_back(0x01);  // ENCRYPTION
		out.push_back(static_cast<uint8_t>(((header_size + 6) >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>((header_size + 6) & 0xFF));
		out.push_back(0x00);
		out.push_back(0x01);  // len 1
		out.push_back(0xFF);  // TERMINATOR
		// VERSION data: 16.0.1000 build, 0 sub-build
		out.push_back(16);
		out.push_back(0);
		out.push_back(0x03);
		out.push_back(0xE8);
		out.push_back(0);
		out.push_back(0);
		// ENCRYPTION data
		out.push_back(static_cast<uint8_t>(EncryptionOption::ENCRYPT_NOT_SUP));
		return out;
	}

	void HandleConnection(socket_t client, int index) {
		// 1. PRELOGIN
		if (!ReadMessage(client, /*record_first_id=*/true)) {
			return;
		}
		if (!SendAll(client, Frame(BuildPreloginResponse()))) {
			return;
		}
		// 2. LOGIN7 -> one answer, then we are done with this connection. The
		//    integrated scenarios need no SPNEGO round trip: StubAuthenticator
		//    completes on InitialBytes(), so the server's single answer is the
		//    LOGINACK (or the ROUTING) the client is waiting for.
		std::vector<uint8_t> login_payload;
		if (!ReadMessage(client, /*record_first_id=*/false, &login_payload)) {
			return;
		}
		{
			std::lock_guard<std::mutex> guard(mutex_);
			server_names_.push_back(ExtractLogin7ServerName(login_payload));
		}
		std::vector<uint8_t> answer = responder_(index);
		if (answer.empty()) {
			return;
		}
		SendAll(client, Frame(answer));
	}

	LoginResponder responder_;
	mutable std::mutex mutex_;	// guards first_packet_ids_ / server_names_
	socket_t listen_fd_ = INVALID_SOCK;
	uint16_t port_ = 0;
	std::thread thread_;
	std::atomic<bool> running_{true};
	bool stopped_ = false;
	std::atomic<int> connections_{0};
	std::vector<int> first_packet_ids_;
	std::vector<std::string> server_names_;
};

// Response streams the scenarios reuse.
std::vector<uint8_t> LoginAckStream() {
	std::vector<uint8_t> s;
	AppendLoginAck(s);
	AppendDone(s);
	return s;
}

std::vector<uint8_t> RouteWithLoginAckStream(uint16_t port, const std::string &host = "127.0.0.1") {
	std::vector<uint8_t> s;
	AppendLoginAck(s);
	AppendRoutingEnvChange(s, host, port);
	AppendDone(s);
	return s;
}

std::vector<uint8_t> RouteWithoutLoginAckStream(uint16_t port) {
	std::vector<uint8_t> s;
	AppendRoutingEnvChange(s, "127.0.0.1", port);
	AppendDone(s);
	return s;
}

//===----------------------------------------------------------------------===//
// A stub authenticator: records its lifecycle so the integrated-path tests can
// assert the factory was re-invoked per hop and the previous context freed.
//===----------------------------------------------------------------------===//

struct AuthTrace {
	std::vector<std::pair<std::string, uint16_t>> factory_calls;
	int frees = 0;
	int frees_before_second_build = -1;
};

class StubAuthenticator : public IAuthenticator {
public:
	explicit StubAuthenticator(AuthTrace *trace) : trace_(trace) {}
	std::vector<uint8_t> InitialBytes() override {
		return {0x60, 0x01, 0x02, 0x03};  // non-empty SPNEGO-shaped blob
	}
	std::vector<uint8_t> NextBytes(const std::vector<uint8_t> &) override {
		return {};	// negotiation complete on our side
	}
	void Free() override {
		trace_->frees++;
	}

private:
	AuthTrace *trace_;
};

//===----------------------------------------------------------------------===//
// Scenarios
//===----------------------------------------------------------------------===//

// Baseline: no ROUTING anywhere. Proves the hop driver did not change the
// zero-hop path when AuthenticateWithFedAuth's loop was extracted into it.
void TestZeroHopLoginUnchanged() {
	FakeTdsServer server([](int) { return LoginAckStream(); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", server.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	CHECK(conn.GetState() == ConnectionState::Idle);
	CHECK(conn.GetPort() == server.GetPort());
	CHECK(conn.GetDatabase() == "TestDB");	// logged in -> the accessor names the database
	CHECK(server.ConnectionCount() == 1);
	CHECK(server.FirstPacketIds().size() == 1);
	CHECK(server.FirstPacketIds()[0] == 1);
	conn.Close();
	server.Stop();
	std::cout << "  [ok] zero-hop login unchanged by the hop driver" << std::endl;
}

// D1 + Gap 2: SQL auth follows a ROUTING that arrives WITH a LOGINACK.
// Before spec 068 this "succeeded" against the gateway -- silently.
void TestSqlAuthSingleHopWithLoginAck() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();
	FakeTdsServer gateway([target_port](int) { return RouteWithLoginAckStream(target_port); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	CHECK(conn.GetState() == ConnectionState::Idle);
	CHECK(conn.GetPort() == target_port);  // we ended up on the ROUTED server
	CHECK(conn.GetHost() == "127.0.0.1");
	CHECK(gateway.ConnectionCount() == 1);
	CHECK(target.ConnectionCount() == 1);
	conn.Close();
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] SQL auth follows ROUTING + LOGINACK to the routed server (spec 068 Gap 2)" << std::endl;
}

// D1 + Gap 1: the routed answer carries NO LOGINACK. This is the shape that
// used to abort the login as an authentication failure.
void TestRouteWithoutLoginAckFollowsHop() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();
	FakeTdsServer gateway([target_port](int) { return RouteWithoutLoginAckStream(target_port); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	CHECK(conn.GetState() == ConnectionState::Idle);
	CHECK(conn.GetPort() == target_port);
	conn.Close();
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] ROUTING without LOGINACK follows the hop instead of failing (spec 068 Gap 1)" << std::endl;
}

// The per-hop packet-id reset: every new socket must start its sequence at 1.
// A carried-over id makes SQL Server drop the connection mid-login.
void TestPerHopPacketIdReset() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();
	FakeTdsServer gateway([target_port](int) { return RouteWithLoginAckStream(target_port); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	CHECK(target.FirstPacketIds().size() == 1);
	CHECK(target.FirstPacketIds()[0] == 1);	 // the HOP's first packet, not 3 or 4
	conn.Close();
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] packet id resets to 1 on the routed connection" << std::endl;
}

// A server that routes to itself must be stopped by MAX_ROUTING_HOPS, and the
// error must name both the count and the last routed target so a field report
// is diagnosable in one round.
void TestHopLimitAborts() {
	// The responder reads self_port from the server thread while the main
	// thread writes it right after construction; atomic because "the socket
	// round-trip orders it in practice" is not something the memory model
	// promises, and TSAN would rightly complain.
	std::atomic<uint16_t> self_port{0};
	FakeTdsServer server([&self_port](int) { return RouteWithLoginAckStream(self_port.load()); });
	self_port.store(server.GetPort());

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", server.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false) == false);

	CHECK(conn.GetState() == ConnectionState::Disconnected);
	// ...and the connection is not "in" any database: GetDatabase() means the
	// database this connection is logged INTO, not the last one attempted.
	CHECK(conn.GetDatabase().empty());
	const std::string err = conn.GetLastError();
	CHECK(err.find("Too many routing hops") != std::string::npos);
	CHECK(err.find("127.0.0.1") != std::string::npos);	// last routed target named
	// 1 initial login + MAX_ROUTING_HOPS(5) hops = 6 connections, then abort.
	CHECK(server.ConnectionCount() == 6);
	server.Stop();
	std::cout << "  [ok] hop limit aborts after 5 hops, naming count + last target" << std::endl;
}

// The per-attempt reset covers the FIRST attempt too, not just hops 2..N.
// Close() resets state_ but leaves next_packet_id_ where the last login left
// it, so a reused connection would open its second PRELOGIN at 3. Nothing else
// in the suite discriminates that: TestPerHopPacketIdReset only exercises the
// hop, so deleting `next_packet_id_ = 1` from the first-attempt reset leaves it
// green.
void TestReusedConnectionRestartsPacketSequence() {
	FakeTdsServer server([](int) { return LoginAckStream(); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", server.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));
	CHECK(conn.GetDatabase() == "TestDB");
	conn.Close();
	// The other exit from "logged in": a normal close must leave the accessor
	// empty too, not just an aborted login.
	CHECK(conn.GetDatabase().empty());

	// Same object, second login: PRELOGIN must be packet id 1 again.
	CHECK(conn.Connect("127.0.0.1", server.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	const std::vector<int> ids = server.FirstPacketIds();
	CHECK(ids.size() == 2);
	CHECK(ids[0] == 1);
	CHECK(ids[1] == 1);	 // without the reset this is 3 (PRELOGIN 1, LOGIN7 2)
	conn.Close();
	server.Stop();
	std::cout << "  [ok] a reused connection restarts its packet sequence at 1" << std::endl;
}

// The routed target in its full Fabric shape: "host\INSTANCE:port". Three
// separate rules meet here, and with a bare "127.0.0.1" target none of them are
// exercised — which is how the whole suite stayed green with the ServerName fix
// reverted:
//   1. the trailing ":port" beats the ENVCHANGE port field (deliberately wrong
//      below, so taking the ENVCHANGE value connects to the wrong place);
//   2. "\INSTANCE" is stripped for DNS/TCP;
//   3. "\INSTANCE" is KEPT for the LOGIN7 ServerName field, which is the
//      spec-068 R7 fix — before it, DoLogin7 sent host_ and the routed named
//      instance received the instance-stripped name.
void TestRoutedTargetWithInstanceAndPortSuffix() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();

	// ENVCHANGE port is 9 (discard protocol) — if the suffix does not win, the
	// hop connects there and the login fails.
	const std::string routed = "127.0.0.1\\INST01:" + std::to_string(target_port);
	FakeTdsServer gateway([routed](int) {
		std::vector<uint8_t> s;
		AppendLoginAck(s);
		AppendRoutingEnvChange(s, routed, 9);
		AppendDone(s);
		return s;
	});

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false));

	CHECK(conn.GetState() == ConnectionState::Idle);
	CHECK(conn.GetPort() == target_port);  // rule 1: suffix beat the ENVCHANGE port
	CHECK(conn.GetHost() == "127.0.0.1");  // rule 2: instance stripped for TCP
	// Exactly one attempt against each: no retry, no second pass at the gateway.
	CHECK(gateway.ConnectionCount() == 1);
	CHECK(target.ConnectionCount() == 1);

	// rule 3: the routed server's LOGIN7 carried the instance form...
	const std::vector<std::string> routed_names = target.ServerNames();
	CHECK(routed_names.size() == 1);
	CHECK(routed_names[0] == "127.0.0.1\\INST01");
	// ...while the gateway's first LOGIN7 carried the plain dialled host, so
	// this pins the hop specifically and not just "ServerName is ever set".
	const std::vector<std::string> gw_names = gateway.ServerNames();
	CHECK(gw_names.size() == 1);
	CHECK(gw_names[0] == "127.0.0.1");

	conn.Close();
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] routed host\\instance:port — suffix wins, instance stripped for TCP, kept in ServerName"
			  << std::endl;
}

// A routed target that cannot be reached. The gateway's answer is perfectly
// well-formed; the hop just fails to connect -- a stale AlwaysOn routing list,
// a firewalled replica, a gateway naming an address the client cannot see.
// Before this, RunWithRoutingHops's reconnect-failure branch and its error
// string were the only paths in the driver with no coverage at all.
//
// The unreachable port is a listener's own port after it has been stopped, so
// the connection is REFUSED immediately. A black-holed address (the
// 10.255.255.x trick Microsoft's mssql-rs failover tests use) would exercise
// the timeout instead, but DEFAULT_CONNECTION_TIMEOUT is compiled in at 30s
// here, which is not a unit test.
void TestUnreachableRoutedTargetFails() {
	// Take a listener's port and stop it, so the port is genuinely closed and
	// the connect is REFUSED immediately.
	//
	// The tempting alternative -- keep a socket bound but never listen(), so the
	// port cannot be reassigned -- is wrong on macOS: measured here, connecting
	// to a bound-but-unlistening 127.0.0.1 port does NOT get an RST, the SYN is
	// dropped and connect() sits for ~7.8s. That is a Linux behaviour, not a
	// portable one. So the port is freed, and the premise the test rests on
	// ("nothing is listening there") is enforced by assertion instead: if the
	// kernel handed the gateway that same port, the gateway would route to
	// itself and this would fail with the hop-limit message, pointing at the
	// wrong code.
	uint16_t dead_port = 0;
	{
		FakeTdsServer doomed([](int) { return LoginAckStream(); });
		dead_port = doomed.GetPort();
		doomed.Stop();
	}
	FakeTdsServer gateway([dead_port](int) { return RouteWithLoginAckStream(dead_port); });
	CHECK(gateway.GetPort() != dead_port);	// the premise, enforced

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false) == false);

	CHECK(conn.GetState() == ConnectionState::Disconnected);
	CHECK(conn.GetDatabase().empty());
	const std::string err = conn.GetLastError();
	// The message must name the target that could not be reached -- "connection
	// failed" without the routed address sends the reader back to the gateway,
	// which was working fine.
	CHECK(err.find("Failed to connect to routed server") != std::string::npos);
	CHECK(err.find(std::to_string(dead_port)) != std::string::npos);
	CHECK(gateway.ConnectionCount() == 1);

	gateway.Stop();
	std::cout << "  [ok] an unreachable routed target fails with the routed address named" << std::endl;
}

// The caller's connect timeout must apply to the HOP, not just the first dial.
// Connect() used to take timeout_seconds, use it once and forget it, so a hop
// always reconnected on the compiled-in 30s default -- a user asking for 5s and
// routed to a black-holed replica waited 30s per hop, up to five times.
//
// 192.0.2.1 is TEST-NET-1 (RFC 5737): reserved for documentation, so it is
// never a real host. What the network does with it varies -- a SYN that gets
// dropped exercises the timeout, an ICMP unreachable comes back immediately --
// so this asserts a generous upper bound rather than a precise duration. It
// discriminates the fix wherever packets are dropped (30s -> ~2s) and merely
// passes cheaply where they are rejected outright.
void TestHopHonoursCallerConnectTimeout() {
	FakeTdsServer gateway([](int) { return RouteWithLoginAckStream(1433, "192.0.2.1"); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), /*timeout_seconds=*/2));
	const auto started = std::chrono::steady_clock::now();
	CHECK(conn.Authenticate("sa", "pw", "TestDB", /*use_encrypt=*/false) == false);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	CHECK(conn.GetLastError().find("Failed to connect to routed server") != std::string::npos);
	const auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
	CHECK(secs < 15);  // the old behaviour would sit here for DEFAULT_CONNECTION_TIMEOUT
	gateway.Stop();
	std::cout << "  [ok] a hop reconnects on the caller's timeout, not the compiled-in default (" << secs << "s)"
			  << std::endl;
}

// D3: the integrated path takes a FACTORY, and a hop re-invokes it with the
// ROUTED host/port -- that is what makes the Kerberos/SSPI ticket match the
// routed server's SPN instead of the gateway's.
void TestIntegratedAuthFactoryRebuiltPerHop() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();
	FakeTdsServer gateway([target_port](int) { return RouteWithLoginAckStream(target_port); });

	AuthTrace trace;
	auto factory = [&trace](const std::string &host, uint16_t port) -> std::shared_ptr<IAuthenticator> {
		if (trace.factory_calls.size() == 1) {
			// About to build the SECOND authenticator: how many Free()s so far?
			trace.frees_before_second_build = trace.frees;
		}
		trace.factory_calls.emplace_back(host, port);
		return std::make_shared<StubAuthenticator>(&trace);
	};

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.AuthenticateIntegrated("TestDB", factory, /*use_encrypt=*/false));

	CHECK(conn.GetState() == ConnectionState::Idle);
	CHECK(trace.factory_calls.size() == 2);						// gateway, then routed server
	CHECK(trace.factory_calls[0].second == gateway.GetPort());	// first attempt: the gateway
	CHECK(trace.factory_calls[1].second == target_port);		// hop: the ROUTED target
	CHECK(trace.factory_calls[1].first == "127.0.0.1");
	// The gateway's context must be released before the hop's is built,
	// otherwise every hop leaks a GSSAPI/SSPI context.
	CHECK(trace.frees_before_second_build == 1);
	conn.Close();
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] integrated auth rebuilds the authenticator for the routed host, freeing the old one"
			  << std::endl;
}

// A factory that cannot produce a ticket for the ROUTED host must fail with an
// error naming that host -- the spec's mitigation for shipping the SPN-over-hop
// design without a live AD + routing environment to verify it against.
void TestIntegratedAuthFactoryNullOnHopNamesRoutedHost() {
	FakeTdsServer target([](int) { return LoginAckStream(); });
	const uint16_t target_port = target.GetPort();
	FakeTdsServer gateway([target_port](int) { return RouteWithLoginAckStream(target_port); });

	AuthTrace trace;
	auto factory = [&trace](const std::string &host, uint16_t port) -> std::shared_ptr<IAuthenticator> {
		trace.factory_calls.emplace_back(host, port);
		if (trace.factory_calls.size() == 1) {
			return std::make_shared<StubAuthenticator>(&trace);
		}
		return nullptr;	 // no ticket for the routed host
	};

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", gateway.GetPort(), 5));
	CHECK(conn.AuthenticateIntegrated("TestDB", factory, /*use_encrypt=*/false) == false);

	const std::string err = conn.GetLastError();
	CHECK(err.find("routed server") != std::string::npos);
	CHECK(err.find(std::to_string(target_port)) != std::string::npos);
	// Must NOT be the "compiled without Kerberos support" message -- that one
	// belongs to a first-attempt nullptr only.
	CHECK(err.find("compiled without") == std::string::npos);
	gateway.Stop();
	target.Stop();
	std::cout << "  [ok] a hop with no obtainable ticket names the routed host in the error" << std::endl;
}

// A first-attempt nullptr keeps the pre-068 wording, which users have in their
// notes and issue reports.
void TestIntegratedAuthNullFactoryFirstAttemptKeepsWording() {
	FakeTdsServer server([](int) { return LoginAckStream(); });

	TdsConnection conn;
	CHECK(conn.Connect("127.0.0.1", server.GetPort(), 5));
	auto factory = [](const std::string &, uint16_t) -> std::shared_ptr<IAuthenticator> { return nullptr; };
	CHECK(conn.AuthenticateIntegrated("TestDB", factory, /*use_encrypt=*/false) == false);

	CHECK(conn.GetLastError().find("compiled without Kerberos / SSPI support") != std::string::npos);
	CHECK(conn.GetState() == ConnectionState::Disconnected);
	server.Stop();
	std::cout << "  [ok] first-attempt null factory keeps the pre-068 error wording" << std::endl;
}

}  // namespace

int main() {
#if defined(_WIN32)
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	std::cout << "test_login_routing_hops (spec 068: ENVCHANGE 20 on every auth path)" << std::endl;

	TestZeroHopLoginUnchanged();
	TestSqlAuthSingleHopWithLoginAck();
	TestRouteWithoutLoginAckFollowsHop();
	TestPerHopPacketIdReset();
	TestReusedConnectionRestartsPacketSequence();
	TestRoutedTargetWithInstanceAndPortSuffix();
	TestUnreachableRoutedTargetFails();
	TestHopHonoursCallerConnectTimeout();
	TestHopLimitAborts();
	TestIntegratedAuthFactoryRebuiltPerHop();
	TestIntegratedAuthFactoryNullOnHopNamesRoutedHost();
	TestIntegratedAuthNullFactoryFirstAttemptKeepsWording();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}

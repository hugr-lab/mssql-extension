// test/cpp/test_instance_resolver.cpp
// Unit tests for the SQL Server Browser parser (spec 045, Phase 0).
//
// These tests do NOT require a running SQL Server or any network access.
// The parser is exercised against canned SVR_RESP byte buffers.
//
// Phase 1 will add tests for InstanceResolver::Resolve via a loopback UDP
// listener spun inside the test process.
//
// Build + run:
//   make test-instance-resolver

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "connection/instance_resolver.hpp"

using duckdb::mssql::BrowserInstance;
using duckdb::mssql::InstanceResolver;
using duckdb::mssql::ParseBrowserResponse;
using duckdb::mssql::ResolveError;
using duckdb::mssql::ResolveResult;

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                                                              \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::cerr << "  FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl;               \
			++g_failures;                                                                                              \
		}                                                                                                              \
	} while (0)

#define EXPECT_EQ(a, b, msg)                                                                                           \
	do {                                                                                                               \
		auto va = (a);                                                                                                 \
		auto vb = (b);                                                                                                 \
		if (!(va == vb)) {                                                                                             \
			std::cerr << "  FAIL: " << (msg) << " — got '" << va << "' expected '" << vb << "' (" << __FILE__ << ":"   \
			          << __LINE__ << ")" << std::endl;                                                                 \
			++g_failures;                                                                                              \
		}                                                                                                              \
	} while (0)

#define EXPECT_THROWS(stmt, msg)                                                                                       \
	do {                                                                                                               \
		bool threw = false;                                                                                            \
		try {                                                                                                          \
			stmt;                                                                                                      \
		} catch (const std::runtime_error &) { threw = true; }                                                         \
		if (!threw) {                                                                                                  \
			std::cerr << "  FAIL: expected runtime_error — " << (msg) << " (" << __FILE__ << ":" << __LINE__ << ")"    \
			          << std::endl;                                                                                    \
			++g_failures;                                                                                              \
		}                                                                                                              \
	} while (0)

//===--------------------------------------------------------------------===//
// Helper - build a well-formed SVR_RESP packet given an ASCII body.
// Body is the NUL-terminated key;value;... payload; the helper prepends
// the opcode + LE u16 size header.
//===--------------------------------------------------------------------===//
std::vector<uint8_t> BuildResp(const std::string &body) {
	std::vector<uint8_t> body_bytes(body.begin(), body.end());
	body_bytes.push_back(0x00);  // trailing NUL
	std::vector<uint8_t> out;
	out.reserve(3 + body_bytes.size());
	out.push_back(0x05);
	out.push_back(static_cast<uint8_t>(body_bytes.size() & 0xFF));
	out.push_back(static_cast<uint8_t>((body_bytes.size() >> 8) & 0xFF));
	out.insert(out.end(), body_bytes.begin(), body_bytes.end());
	return out;
}

//===--------------------------------------------------------------------===//
// T003 - happy paths
//===--------------------------------------------------------------------===//

void TestSingleInstanceAllFields() {
	std::cout << "TestSingleInstanceAllFields" << std::endl;
	auto pkt = BuildResp("ServerName;SQLHOST;InstanceName;SS2022;IsClustered;No;Version;15.0.4123.1;tcp;1433;;");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record");
	if (rs.empty()) return;
	EXPECT_EQ(rs[0].server_name, std::string("SQLHOST"), "ServerName");
	EXPECT_EQ(rs[0].instance_name, std::string("SS2022"), "InstanceName");
	EXPECT_EQ(rs[0].version, std::string("15.0.4123.1"), "Version");
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(1433), "tcp_port");
	EXPECT(rs[0].tcp_enabled, "tcp_enabled");
	EXPECT(!rs[0].is_clustered, "is_clustered=No");
}

void TestMultipleInstances() {
	std::cout << "TestMultipleInstances" << std::endl;
	auto pkt =
	    BuildResp("ServerName;HOST;InstanceName;SS2019;IsClustered;No;Version;14.0.0.1;tcp;1434;;"
	              "ServerName;HOST;InstanceName;SS2022;IsClustered;Yes;Version;15.0.0.1;tcp;1435;;");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 2u, "two records");
	if (rs.size() < 2) return;
	EXPECT_EQ(rs[0].instance_name, std::string("SS2019"), "first instance");
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(1434), "first port");
	EXPECT(!rs[0].is_clustered, "first not clustered");
	EXPECT_EQ(rs[1].instance_name, std::string("SS2022"), "second instance");
	EXPECT_EQ(rs[1].tcp_port, static_cast<uint16_t>(1435), "second port");
	EXPECT(rs[1].is_clustered, "second clustered");
}

void TestUnknownFieldsIgnored() {
	std::cout << "TestUnknownFieldsIgnored" << std::endl;
	// Real SQL Browser emits np (named pipes) and may emit rpc, via, etc.
	auto pkt = BuildResp("ServerName;H;InstanceName;X;IsClustered;No;Version;15.0;np;\\\\H\\pipe\\sql\\query;tcp;14000;rpc;1;;");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record despite extra fields");
	if (rs.empty()) return;
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(14000), "tcp parsed past np field");
}

void TestTcpDisabled() {
	std::cout << "TestTcpDisabled" << std::endl;
	// Instance with TCP turned off entirely - no 'tcp' field at all.
	auto pkt = BuildResp("ServerName;H;InstanceName;PIPESONLY;IsClustered;No;Version;15.0;np;\\\\H\\pipe\\sql\\query;;");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record");
	if (rs.empty()) return;
	EXPECT(!rs[0].tcp_enabled, "tcp_enabled false when no tcp field");
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(0), "tcp_port zero when disabled");
}

void TestEmptyTcpValue() {
	std::cout << "TestEmptyTcpValue" << std::endl;
	auto pkt = BuildResp("ServerName;H;InstanceName;X;IsClustered;No;Version;15.0;tcp;;;");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record");
	if (rs.empty()) return;
	EXPECT(!rs[0].tcp_enabled, "tcp_enabled false when tcp value empty");
}

void TestNoTrailingDoubleSemicolon() {
	std::cout << "TestNoTrailingDoubleSemicolon" << std::endl;
	// Some implementations omit the closing ";;" before the NUL.
	auto pkt = BuildResp("ServerName;H;InstanceName;X;IsClustered;No;Version;15.0;tcp;1433");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record despite missing record terminator");
	if (rs.empty()) return;
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(1433), "tcp parsed without trailing ;;");
}

void TestEmptyResponse() {
	std::cout << "TestEmptyResponse" << std::endl;
	// SQL Browser returns an empty body when the requested instance is unknown.
	auto pkt = BuildResp("");
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 0u, "no records for empty body");
}

//===--------------------------------------------------------------------===//
// T004 - error paths
//===--------------------------------------------------------------------===//

void TestTruncatedHeader() {
	std::cout << "TestTruncatedHeader" << std::endl;
	uint8_t pkt[2] = {0x05, 0x10};
	EXPECT_THROWS({ ParseBrowserResponse(pkt, sizeof(pkt)); }, "header shorter than 3 bytes");
}

void TestWrongOpcode() {
	std::cout << "TestWrongOpcode" << std::endl;
	uint8_t pkt[] = {0x04, 0x05, 0x00, 'x', 'y', 'z', 0x00, 0x00};  // 0x04 is CLNT_UCAST_INST (request opcode)
	EXPECT_THROWS({ ParseBrowserResponse(pkt, sizeof(pkt)); }, "opcode != 0x05");
}

void TestAdvertisedSizeExceedsBuffer() {
	std::cout << "TestAdvertisedSizeExceedsBuffer" << std::endl;
	// Header claims 200 bytes but only 8 follow.
	uint8_t pkt[] = {0x05, 0xC8, 0x00, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
	EXPECT_THROWS({ ParseBrowserResponse(pkt, sizeof(pkt)); }, "advertised size > buffer");
}

void TestRandomGarbage() {
	std::cout << "TestRandomGarbage" << std::endl;
	uint8_t pkt[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
	EXPECT_THROWS({ ParseBrowserResponse(pkt, sizeof(pkt)); }, "garbage bytes throw");
}

void TestBodyWithoutNul() {
	std::cout << "TestBodyWithoutNul" << std::endl;
	// Hand-build a payload where the body is not NUL-terminated. The parser
	// must tolerate this (the parser strips an optional trailing NUL, not a
	// required one) and still return any complete records.
	const std::string body = "ServerName;H;InstanceName;X;IsClustered;No;Version;15.0;tcp;1500;;";
	std::vector<uint8_t> pkt;
	pkt.push_back(0x05);
	pkt.push_back(static_cast<uint8_t>(body.size() & 0xFF));
	pkt.push_back(static_cast<uint8_t>((body.size() >> 8) & 0xFF));
	pkt.insert(pkt.end(), body.begin(), body.end());
	auto rs = ParseBrowserResponse(pkt.data(), pkt.size());
	EXPECT_EQ(rs.size(), 1u, "one record without trailing NUL");
	if (rs.empty()) return;
	EXPECT_EQ(rs[0].tcp_port, static_cast<uint16_t>(1500), "port parsed without NUL terminator");
}

//===--------------------------------------------------------------------===//
// T009 - loopback UDP echo: spin a tiny UDP listener on 127.0.0.1:<ephemeral>
// and verify Resolve drives it correctly. POSIX-only — the standalone test
// binary doesn't initialise Winsock, and Phase 2's docker stack provides the
// cross-platform integration coverage.
//===--------------------------------------------------------------------===//

#ifndef _WIN32

// MockBrowser - opens a UDP socket on 127.0.0.1:0, returns the ephemeral
// port, and serves a fixed number of requests in a background thread before
// shutting down. Modes mirror the docker-stack mock for parity.
class MockBrowser {
public:
	enum class Mode { Respond, Silent, Garbage };

	MockBrowser(Mode m, std::vector<uint8_t> response) : mode_(m), response_(std::move(response)) {
		sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (sock_ < 0) throw std::runtime_error("mock: socket() failed");

		struct sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (::bind(sock_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(sock_);
			throw std::runtime_error("mock: bind() failed");
		}
		socklen_t len = sizeof(addr);
		if (::getsockname(sock_, reinterpret_cast<struct sockaddr *>(&addr), &len) < 0) {
			::close(sock_);
			throw std::runtime_error("mock: getsockname() failed");
		}
		port_ = ntohs(addr.sin_port);

		thread_ = std::thread([this] { Serve(); });
	}

	~MockBrowser() {
		stop_.store(true);
		// Send a self-poke so the blocking recvfrom unblocks even in
		// Silent mode (otherwise the thread would hang for ever).
		struct sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port_);
		int s = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			::sendto(s, "x", 1, 0, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
			::close(s);
		}
		if (thread_.joinable()) thread_.join();
		::close(sock_);
	}

	uint16_t Port() const {
		return port_;
	}

private:
	void Serve() {
		while (!stop_.load()) {
			uint8_t buf[1500];
			struct sockaddr_in peer;
			socklen_t plen = sizeof(peer);
			ssize_t n = ::recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr *>(&peer), &plen);
			if (n < 0 || stop_.load()) break;
			if (mode_ == Mode::Silent) continue;
			if (mode_ == Mode::Garbage) {
				uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
				::sendto(sock_, garbage, sizeof(garbage), 0, reinterpret_cast<struct sockaddr *>(&peer), plen);
				continue;
			}
			::sendto(sock_, response_.data(), response_.size(), 0, reinterpret_cast<struct sockaddr *>(&peer), plen);
		}
	}

	int sock_ = -1;
	uint16_t port_ = 0;
	Mode mode_;
	std::vector<uint8_t> response_;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

void TestResolveHappyPath() {
	std::cout << "TestResolveHappyPath" << std::endl;
	auto resp = BuildResp("ServerName;HOST;InstanceName;TESTINST;IsClustered;No;Version;15.0;tcp;11433;;");
	MockBrowser mock(MockBrowser::Mode::Respond, std::move(resp));
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "TESTINST", 2);
	EXPECT(r.ok, "Resolve returned ok");
	if (r.ok) {
		EXPECT_EQ(r.port, static_cast<uint16_t>(11433), "resolved port");
	} else {
		std::cerr << "    error: " << r.error.message << std::endl;
	}
}

void TestResolveCaseInsensitive() {
	std::cout << "TestResolveCaseInsensitive" << std::endl;
	auto resp = BuildResp("ServerName;H;InstanceName;TESTINST;IsClustered;No;Version;15.0;tcp;14000;;");
	MockBrowser mock(MockBrowser::Mode::Respond, std::move(resp));
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "testinst", 2);
	EXPECT(r.ok, "Resolve matched case-insensitively");
}

void TestResolveInstanceNotFound() {
	std::cout << "TestResolveInstanceNotFound" << std::endl;
	auto resp = BuildResp("ServerName;H;InstanceName;OTHER;IsClustered;No;Version;15.0;tcp;14000;;");
	MockBrowser mock(MockBrowser::Mode::Respond, std::move(resp));
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "MISSING", 2);
	EXPECT(!r.ok, "Resolve returned failure");
	EXPECT(r.error.kind == ResolveError::Kind::InstanceNotFound, "InstanceNotFound kind");
}

void TestResolveTcpDisabled() {
	std::cout << "TestResolveTcpDisabled" << std::endl;
	auto resp = BuildResp("ServerName;H;InstanceName;PIPESONLY;IsClustered;No;Version;15.0;np;\\\\H\\pipe\\sql\\query;;");
	MockBrowser mock(MockBrowser::Mode::Respond, std::move(resp));
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "PIPESONLY", 2);
	EXPECT(!r.ok, "Resolve returned failure");
	EXPECT(r.error.kind == ResolveError::Kind::TcpDisabled, "TcpDisabled kind");
}

void TestResolveMalformedResponse() {
	std::cout << "TestResolveMalformedResponse" << std::endl;
	MockBrowser mock(MockBrowser::Mode::Garbage, {});
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "ANYTHING", 2);
	EXPECT(!r.ok, "Resolve returned failure");
	EXPECT(r.error.kind == ResolveError::Kind::Malformed, "Malformed kind");
}

void TestResolveSilentBrowser() {
	std::cout << "TestResolveSilentBrowser (~2s wall)" << std::endl;
	MockBrowser mock(MockBrowser::Mode::Silent, {});
	auto start = std::chrono::steady_clock::now();
	auto r = InstanceResolver::ResolveForTest("127.0.0.1", mock.Port(), "WHATEVER", 1);
	auto elapsed = std::chrono::steady_clock::now() - start;
	auto elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
	EXPECT(!r.ok, "Resolve returned failure");
	EXPECT(r.error.kind == ResolveError::Kind::Unreachable, "Unreachable kind");
	// 1s timeout + 1 retry = ~2s wall. Allow some slack.
	EXPECT(elapsed_s >= 1.5 && elapsed_s < 4.0, "elapsed time within retry window");
	if (!(elapsed_s >= 1.5 && elapsed_s < 4.0)) {
		std::cerr << "    elapsed: " << elapsed_s << "s" << std::endl;
	}
}

void TestNormaliseLocalAliases() {
	std::cout << "TestNormaliseLocalAliases" << std::endl;
	// (local) and "." must map to localhost. We can't run the full Resolve
	// since the host won't bind 1434, but a Resolve against (local) with a
	// short timeout proves NormaliseHost runs (no "DNS lookup failed for
	// '(local)'" error — we'd get an unreachable-port error instead).
	auto r = InstanceResolver::ResolveForTest("(local)", 1, "x", 1);
	EXPECT(!r.ok, "Resolve failed as expected (no listener)");
	// The exact error depends on the platform — could be unreachable or
	// connection refused. The important thing is the host name didn't fail
	// DNS, which would be a different error category.
	EXPECT(r.error.message.find("(local)") == std::string::npos ||
	           r.error.message.find("DNS lookup failed") == std::string::npos,
	       "(local) was normalised before DNS");
}

#else  // _WIN32

void TestResolveHappyPath() { std::cout << "TestResolveHappyPath (skipped on Windows)" << std::endl; }
void TestResolveCaseInsensitive() { std::cout << "TestResolveCaseInsensitive (skipped on Windows)" << std::endl; }
void TestResolveInstanceNotFound() { std::cout << "TestResolveInstanceNotFound (skipped on Windows)" << std::endl; }
void TestResolveTcpDisabled() { std::cout << "TestResolveTcpDisabled (skipped on Windows)" << std::endl; }
void TestResolveMalformedResponse() { std::cout << "TestResolveMalformedResponse (skipped on Windows)" << std::endl; }
void TestResolveSilentBrowser() { std::cout << "TestResolveSilentBrowser (skipped on Windows)" << std::endl; }
void TestNormaliseLocalAliases() { std::cout << "TestNormaliseLocalAliases (skipped on Windows)" << std::endl; }

#endif  // _WIN32

}  // namespace

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "InstanceResolver parser unit tests (spec 045, Phase 0)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		// happy paths
		TestSingleInstanceAllFields();
		TestMultipleInstances();
		TestUnknownFieldsIgnored();
		TestTcpDisabled();
		TestEmptyTcpValue();
		TestNoTrailingDoubleSemicolon();
		TestEmptyResponse();
		// error paths
		TestTruncatedHeader();
		TestWrongOpcode();
		TestAdvertisedSizeExceedsBuffer();
		TestRandomGarbage();
		TestBodyWithoutNul();
		// Phase 1: end-to-end with a loopback mock browser
		TestResolveHappyPath();
		TestResolveCaseInsensitive();
		TestResolveInstanceNotFound();
		TestResolveTcpDisabled();
		TestResolveMalformedResponse();
		TestResolveSilentBrowser();
		TestNormaliseLocalAliases();
	} catch (const std::exception &e) {
		std::cerr << "UNCAUGHT EXCEPTION: " << e.what() << std::endl;
		return 2;
	}

	std::cout << "============================================================" << std::endl;
	if (g_failures == 0) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	}
	std::cerr << g_failures << " TEST(S) FAILED" << std::endl;
	return 1;
}

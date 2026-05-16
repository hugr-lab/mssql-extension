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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "connection/instance_resolver.hpp"

using duckdb::mssql::BrowserInstance;
using duckdb::mssql::ParseBrowserResponse;

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

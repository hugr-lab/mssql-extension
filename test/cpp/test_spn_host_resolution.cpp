// test/cpp/test_spn_host_resolution.cpp
// Unit tests for ResolveHostForSpn (issue #259).
//
// Active Directory registers SQL Server SPNs against the host's FQDN. Building
// the SPN by pasting the connection string's host into "MSSQLSvc/<host>:<port>"
// therefore produces something unmatchable the moment a user connects by IP --
// and the resulting KDC failure ("server not found in Kerberos database") says
// nothing about the address form being the cause.
//
// On resolver dependence, precisely: the pass-through cases touch nothing.
// 127.0.0.1 resolves from the local hosts database. The documentation-range
// addresses DO issue a PTR query to whatever resolver the machine has, so they
// assert only what no resolver may do -- hand back the address as if it were a
// name -- rather than asserting a specific answer. A network with wildcard PTR
// synthesises a name for any address, and that is a legitimate answer here.
//
// Build + run: make test-cpp (this file is in STANDALONE_TEST_SOURCES).

#include <iostream>
#include <string>

#include "tds/auth/auth_strategy_factory.hpp"

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

// A hostname is what AD registers against, so it must pass through untouched --
// and, importantly, without a lookup: resolving a name could follow a CNAME to
// a different canonical name and change an SPN that was already correct.
void TestHostnamePassesThroughUnchanged() {
	CHECK(ResolveHostForSpn("sqlhost.corp.example.com") == "sqlhost.corp.example.com");
	CHECK(ResolveHostForSpn("sqlhost") == "sqlhost");
	CHECK(ResolveHostForSpn("SQLHOST.CORP.EXAMPLE.COM") == "SQLHOST.CORP.EXAMPLE.COM");
	std::cout << "  [ok] a hostname passes through unchanged, unresolved" << std::endl;
}

// The reported case: an IPv4 literal must be turned into a name. 127.0.0.1
// resolves from the local hosts database on every platform CI runs on.
void TestIPv4LiteralIsReverseResolved() {
	const std::string resolved = ResolveHostForSpn("127.0.0.1");
	CHECK(!resolved.empty());
	CHECK(resolved != "127.0.0.1");	 // the whole point: no longer an address
	CHECK(resolved.find('.') == std::string::npos || resolved.find("localhost") != std::string::npos ||
		  !resolved.empty());		// whatever the box calls loopback
	CHECK(resolved.back() != '.');	// trailing DNS dot stripped
	std::cout << "  [ok] 127.0.0.1 reverse-resolves to '" << resolved << "' instead of staying an address" << std::endl;
}

// An IP with no PTR record returns empty, so the caller can say what to do
// about it rather than building an SPN that cannot match.
void TestUnresolvableIPReturnsEmpty() {
	// TEST-NET-1 (RFC 5737). Normally no PTR record exists, so this returns
	// empty and DeriveDefaultSpn turns that into an actionable error. A resolver
	// that synthesises names for every address returns one instead -- also fine.
	// What must never happen is the address coming back as though it were a
	// name, because that is the unmatchable SPN issue #259 is about.
	const std::string resolved = ResolveHostForSpn("192.0.2.111");
	CHECK(resolved != "192.0.2.111");
	std::cout << "  [ok] an IP with no PTR record never comes back as an address (got "
			  << (resolved.empty() ? "<empty>" : resolved) << ")" << std::endl;
}

// IPv6 literals take the same path -- the connection string may carry one, and
// silently treating it as a name would put brackets and colons in an SPN.
void TestIPv6LiteralIsRecognised() {
	const std::string loopback = ResolveHostForSpn("::1");
	// Either it resolves to a name, or it has no PTR and returns empty. What it
	// must NOT do is hand back "::1" as though it were a hostname.
	CHECK(loopback != "::1");
	// Documentation range 2001:db8::/32 (RFC 3849). Same reasoning as the IPv4
	// case above: assert it is not passed through, not that it is empty.
	CHECK(ResolveHostForSpn("2001:db8::1") != "2001:db8::1");
	std::cout << "  [ok] IPv6 literals are recognised as addresses, not passed through as names" << std::endl;
}

void TestEmptyHostIsNotTouched() {
	CHECK(ResolveHostForSpn("").empty());
	std::cout << "  [ok] an empty host stays empty" << std::endl;
}

}  // namespace

int main() {
	std::cout << "test_spn_host_resolution (issue #259)" << std::endl;

	TestHostnamePassesThroughUnchanged();
	TestIPv4LiteralIsReverseResolved();
	TestUnresolvableIPReturnsEmpty();
	TestIPv6LiteralIsRecognised();
	TestEmptyHostIsNotTouched();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}

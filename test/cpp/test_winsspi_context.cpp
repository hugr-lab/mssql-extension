// test/cpp/test_winsspi_context.cpp
// Unit tests for Windows SSPI context creation (issue #260).
//
// WHY THIS EXISTS. Spec 042 Phase 4 shipped WinSspiAuthenticator, and the only
// test for it -- test/sql/integrated_auth/winsspi_basic.test -- is gated behind
// MSSQL_WINSSPI_TEST, which sits on the opt-in allowlist in
// scripts/ci/check_require_env.sh and is therefore never set. Its stated reason
// is that CI runners are not domain-joined. True, and stronger than it needs to
// be: acquiring a Negotiate credential handle and producing the first SPNEGO
// token needs the LOCAL logon session, not a domain and not a SQL Server. So
// the whole path from "construct the authenticator" to "we have bytes to put in
// LOGIN7" is testable on a bare windows-latest runner, which is where the
// platform-specific bugs live.
//
// Microsoft's own Rust TDS driver does exactly this in
// mssql-tds/tests/test_windows_sspi.rs (test_windows_sspi_context_creation /
// test_windows_sspi_generate_initial_token).
//
// On non-Windows this compiles to a no-op so the file can live in the normal
// test set without a platform fence around every build recipe.
//
// Build + run (Windows): part of the MSVC build job in .github/workflows/ci.yml.

#include <iostream>
#include <string>

#if defined(_WIN32)

#include <vector>

#include "tds/auth/winsspi_authenticator.hpp"

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

// The SPN the caller asked for must be the SPN the authenticator holds -- no
// rewriting, no defaulting. This is the value that decides which ticket the
// KDC is asked for, and a routing hop rebuilds the authenticator precisely to
// change it (spec 068 D3).
void TestSpnIsHeldVerbatim() {
	WinSspiConfig config;
	config.spn = "MSSQLSvc/localhost:1433";
	WinSspiAuthenticator auth(config);
	CHECK(auth.GetSpn() == "MSSQLSvc/localhost:1433");
	std::cout << "  [ok] the configured SPN is held verbatim" << std::endl;
}

// An empty SPN is a configuration error and must be rejected at construction,
// before any TDS traffic -- otherwise it surfaces mid-login as an opaque SSPI
// status.
void TestEmptySpnRejectedAtConstruction() {
	WinSspiConfig config;
	config.spn = "";
	bool threw = false;
	try {
		WinSspiAuthenticator auth(config);
	} catch (const std::exception &) {
		threw = true;
	}
	CHECK(threw);
	std::cout << "  [ok] an empty SPN is rejected at construction" << std::endl;
}

// The real one: acquire a Negotiate credential from the local logon session and
// produce the first token. No domain, no SQL Server, no network. A token that
// comes back empty, or that is not a GSS-API framed blob, means the SSPI wiring
// is broken however well the code compiles.
void TestInitialTokenIsAGssApiBlob() {
	WinSspiConfig config;
	// localhost is deliberate: the SPN need not exist for InitializeSecurityContext
	// to produce an initial NTLM/SPNEGO token. Only the server would reject it.
	config.spn = "MSSQLSvc/localhost:1433";
	WinSspiAuthenticator auth(config);

	std::vector<uint8_t> token;
	try {
		token = auth.InitialBytes();
	} catch (const std::exception &e) {
		std::cerr << "InitialBytes() threw: " << e.what() << std::endl;
		std::exit(1);
	}

	CHECK(!token.empty());
	// SPNEGO/NTLM tokens are DER: 0x60 is the ASN.1 APPLICATION 0 tag that wraps
	// a GSS-API token. Raw NTLM ("NTLMSSP\0") is also legitimate when Negotiate
	// picks NTLM directly on an undomained box, so accept either -- but not
	// arbitrary bytes. (The 0x60 check is the one mssql-rs makes.)
	const bool gssapi_framed = token[0] == 0x60;
	const bool raw_ntlm =
		token.size() >= 8 && std::string(reinterpret_cast<const char *>(token.data()), 7) == "NTLMSSP";
	CHECK(gssapi_framed || raw_ntlm);

	auth.Free();
	auth.Free();  // Free() is documented idempotent; calling twice must not crash
	std::cout << "  [ok] initial token is " << token.size() << " bytes, "
			  << (gssapi_framed ? "GSS-API framed (0x60)" : "raw NTLMSSP") << std::endl;
}

}  // namespace

int main() {
	std::cout << "test_winsspi_context (issue #260)" << std::endl;

	TestSpnIsHeldVerbatim();
	TestEmptySpnRejectedAtConstruction();
	TestInitialTokenIsAGssApiBlob();

	std::cout << "All " << g_checks << " checks passed." << std::endl;
	return 0;
}

#else  // !_WIN32

int main() {
	std::cout << "test_winsspi_context: skipped (Windows-only; SSPI is not present on this platform)" << std::endl;
	return 0;
}

#endif

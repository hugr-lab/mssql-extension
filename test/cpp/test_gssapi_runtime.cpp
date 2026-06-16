// test/cpp/test_gssapi_runtime.cpp
//
// Unit tests for the lazy GSSAPI/krb5 runtime loader (spec 053, issue #161).
//
// These do NOT require SQL Server or a Kerberos KDC. They exercise the loader
// contract on whatever platform runs them:
//   * On a host WITH the Kerberos runtime (CI Linux, macOS framework): the
//     accessors return a fully-populated function table.
//   * On a host WITHOUT it (the #161 reproduction image): the accessors throw
//     Krb5RuntimeUnavailable whose message names the missing library and the
//     package to install (SC-004 / FR-005).
//
// Either outcome is a PASS — both are documented, correct behavior. What must
// NEVER happen is a crash or a silent/empty table.
//
// Run:
//   ./build/test/test_gssapi_runtime

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

#include "tds/auth/gssapi_runtime.hpp"

using namespace duckdb::tds;

static int g_failures = 0;

static void Check(bool cond, const char *msg) {
	if (!cond) {
		std::cerr << "FAIL: " << msg << "\n";
		++g_failures;
	} else {
		std::cout << "ok: " << msg << "\n";
	}
}

static bool Contains(const std::string &haystack, const char *needle) {
	return haystack.find(needle) != std::string::npos;
}

// The error message contract (FR-005): names the missing object AND the
// package to install for at least one supported distro family.
static void TestUnavailableMessageContract() {
	Krb5RuntimeUnavailable e(
		"MSSQL Kerberos auth: could not load the system Kerberos runtime 'libgssapi_krb5.so.2' "
		"(dlopen: not found). Install: Debian/Ubuntu 'apt install libgssapi-krb5-2', "
		"RHEL/Fedora 'dnf install krb5-libs'.");
	std::string msg = e.what();
	Check(Contains(msg, "libgssapi_krb5.so.2"), "error message names the missing shared object");
	Check(Contains(msg, "libgssapi-krb5-2"), "error message names the Debian/Ubuntu package");
	Check(Contains(msg, "krb5-libs"), "error message names the RHEL/Fedora package");
}

// Exercise the real accessor. Both the loaded and the unavailable outcomes are
// acceptable; we only assert the loader honors its contract in each case.
static void TestGetGssApiContract() {
	try {
		const GssApiFns &gss = GetGssApi();
		// Runtime present: every used function pointer must be resolved.
		Check(gss.display_status != nullptr, "GetGssApi: display_status resolved");
		Check(gss.release_buffer != nullptr, "GetGssApi: release_buffer resolved");
		Check(gss.import_name != nullptr, "GetGssApi: import_name resolved");
		Check(gss.init_sec_context != nullptr, "GetGssApi: init_sec_context resolved");
		Check(gss.delete_sec_context != nullptr, "GetGssApi: delete_sec_context resolved");
		Check(gss.release_cred != nullptr, "GetGssApi: release_cred resolved");
		Check(gss.release_name != nullptr, "GetGssApi: release_name resolved");
		std::cout << "(Kerberos runtime present — loaded path exercised)\n";
	} catch (const Krb5RuntimeUnavailable &e) {
		// Runtime absent: message must point at the missing lib + a package.
		std::string msg = e.what();
		Check(Contains(msg, "libgssapi_krb5.so.2"), "unavailable: message names the missing object");
		Check(Contains(msg, "krb5"), "unavailable: message recommends a Kerberos package");
		std::cout << "(Kerberos runtime absent — unavailable path exercised)\n";
	}
}

int main() {
	std::cout << "== gssapi_runtime loader unit tests (spec 053) ==\n";
	TestUnavailableMessageContract();
	TestGetGssApiContract();
	if (g_failures == 0) {
		std::cout << "\nAll gssapi_runtime tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << g_failures << " gssapi_runtime test(s) failed.\n";
	return 1;
}

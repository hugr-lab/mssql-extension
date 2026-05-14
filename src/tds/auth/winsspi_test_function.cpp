//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// winsspi_test_function.cpp
//
// mssql_winsspi_auth_test() scalar function -- Windows SSPI peer of
// mssql_kerberos_auth_test(). Exercises AcquireCredentialsHandleW +
// the first InitializeSecurityContextW round (i.e. WinSspiAuthenticator
// ::InitialBytes()) without ever connecting to SQL Server, so users can
// diagnose SPN / ticket problems in isolation.
//
// Spec 042 Phase 4.
//
// Mirrors krb5_test_function.cpp in shape and return-string format so a
// future cross-platform `mssql_integrated_auth_test()` could replace both.
//===----------------------------------------------------------------------===//

#include "tds/auth/winsspi_test_function.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <string>

#if defined(MSSQL_ENABLE_SSPI)
#include "tds/auth/winsspi_authenticator.hpp"

// secur32.dll provides GetUserNameExW (via security.h) for the principal-
// name lookup. SECURITY_WIN32 is already defined by winsspi_authenticator.hpp.
#include <security.h>
#endif

namespace duckdb {
namespace mssql {
namespace winsspi {

#if defined(MSSQL_ENABLE_SSPI)

//===----------------------------------------------------------------------===//
// LookupCurrentPrincipal - read the current logon session's UPN.
//
// Best-effort: returns "<unknown>" on any failure since principal-name
// lookup failure shouldn't mask the actual auth result. Matches the
// Kerberos LookupCcachePrincipal() shape.
//===----------------------------------------------------------------------===//
static std::string LookupCurrentPrincipal() {
	// User principal name (alice@CORP.EXAMPLE.COM) is the closest analog to
	// the kinit ccache principal we report on POSIX. Falls back to NameSamCompatible
	// (DOMAIN\user) on machines where the UPN isn't set on the account.
	// Note: we use &buf[0] / &utf8[0] (not .data()) for writable pointers because
	// this project is pinned to C++11 for ODR compat with DuckDB, and
	// std::wstring::data() / std::string::data() returned only const T* until
	// C++17. The buffers are pre-sized non-empty before each &[0] is taken, so
	// the addresses are valid storage. Calls reading from the buffer
	// (WideCharToMultiByte's LPCWCH source) can keep using .data().
	ULONG size = 0;
	GetUserNameExW(NameUserPrincipal, nullptr, &size);
	if (size > 0) {
		std::wstring buf(size, L'\0');
		if (GetUserNameExW(NameUserPrincipal, &buf[0], &size)) {
			buf.resize(size);
			int needed = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);
			if (needed > 0) {
				std::string utf8(needed - 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, &utf8[0], needed, nullptr, nullptr);
				return utf8;
			}
		}
	}
	// UPN unavailable -- try NameSamCompatible (DOMAIN\user).
	size = 0;
	GetUserNameExW(NameSamCompatible, nullptr, &size);
	if (size > 0) {
		std::wstring buf(size, L'\0');
		if (GetUserNameExW(NameSamCompatible, &buf[0], &size)) {
			buf.resize(size);
			int needed = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);
			if (needed > 0) {
				std::string utf8(needed - 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, &utf8[0], needed, nullptr, nullptr);
				return utf8;
			}
		}
	}
	return "<unknown>";
}

//===----------------------------------------------------------------------===//
// RunWinSspiTest - the shared core
//
// Constructs a WinSspiAuthenticator with the supplied config, runs
// InitialBytes() (which calls AcquireCredentialsHandleW +
// InitializeSecurityContextW), returns a one-line status string. Catches
// std::exception so the verbatim "MSSQL Kerberos auth failed: ..." message
// surfaces to the caller verbatim instead of being thrown as a DuckDB error.
//
// Note the deliberate "MSSQL Kerberos auth ..." error-prefix shared with
// Krb5Authenticator: from the user's perspective both paths are
// "integrated authentication", and Negotiate falls back to Kerberos on
// the wire when the SPN resolves -- so the same vocabulary is correct.
//===----------------------------------------------------------------------===//
static std::string RunWinSspiTest(tds::WinSspiConfig config) {
	try {
		tds::WinSspiAuthenticator authn(std::move(config));
		auto blob = authn.InitialBytes();
		std::string principal = LookupCurrentPrincipal();
		return "OK: principal=" + principal + ", spn=" + authn.GetSpn() +
			   ", mech=Negotiate, token_size=" + std::to_string(blob.size()) + " bytes";
	} catch (const std::exception &e) {
		return std::string(e.what());
	}
}

//===----------------------------------------------------------------------===//
// DeriveDefaultSpn - canonical "MSSQLSvc/<host>:<port>" form
//===----------------------------------------------------------------------===//
static std::string DeriveDefaultSpn(const std::string &host, uint16_t port) {
	return "MSSQLSvc/" + host + ":" + std::to_string(port);
}

//===----------------------------------------------------------------------===//
// WinSspiTestHost - mssql_winsspi_auth_test(host VARCHAR) -> VARCHAR
//===----------------------------------------------------------------------===//
static void WinSspiTestHost(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(host_vec, result, args.size(), [&](string_t host) {
		std::string h = host.GetString();
		tds::WinSspiConfig cfg;
		cfg.spn = DeriveDefaultSpn(h, 1433);
		return StringVector::AddString(result, RunWinSspiTest(std::move(cfg)));
	});
}

//===----------------------------------------------------------------------===//
// WinSspiTestHostPort - mssql_winsspi_auth_test(host VARCHAR, port INTEGER)
//===----------------------------------------------------------------------===//
static void WinSspiTestHostPort(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	auto &port_vec = args.data[1];
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
		host_vec, port_vec, result, args.size(), [&](string_t host, int32_t port) {
			std::string h = host.GetString();
			tds::WinSspiConfig cfg;
			cfg.spn = DeriveDefaultSpn(h, static_cast<uint16_t>(port));
			return StringVector::AddString(result, RunWinSspiTest(std::move(cfg)));
		});
}

//===----------------------------------------------------------------------===//
// WinSspiTestSpn - mssql_winsspi_auth_test_spn(spn VARCHAR)
//
// Lets the user pass an explicit SPN that overrides default derivation,
// e.g. when the host is an IP/alias and the registered SPN is something
// else. Equivalent to the service_principal_name connection-string knob.
//===----------------------------------------------------------------------===//
static void WinSspiTestSpn(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &spn_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(spn_vec, result, args.size(), [&](string_t spn) {
		tds::WinSspiConfig cfg;
		cfg.spn = spn.GetString();
		return StringVector::AddString(result, RunWinSspiTest(std::move(cfg)));
	});
}

#else  // !MSSQL_ENABLE_SSPI

//===----------------------------------------------------------------------===//
// Stub implementations on POSIX / when the extension was built without SSPI
//
// Registered unconditionally so a user on Linux who runs `SELECT
// mssql_winsspi_auth_test(...)` gets a clear "wrong platform" message
// rather than "no such function".
//===----------------------------------------------------------------------===//

static const char *kNoSspiMsg =
	"MSSQL Windows SSPI auth test: this build of the mssql extension was compiled without "
	"Windows SSPI support. Either this is a non-Windows build (POSIX uses mssql_kerberos_auth_test "
	"instead) or it was built without -DENABLE_KRB5=ON on a Windows toolchain.";

static void WinSspiTestHost(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(host_vec, result, args.size(),
											   [&](string_t) { return StringVector::AddString(result, kNoSspiMsg); });
}
static void WinSspiTestHostPort(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &host_vec = args.data[0];
	auto &port_vec = args.data[1];
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
		host_vec, port_vec, result, args.size(),
		[&](string_t, int32_t) { return StringVector::AddString(result, kNoSspiMsg); });
}
static void WinSspiTestSpn(DataChunk &args, ExpressionState & /*state*/, Vector &result) {
	auto &spn_vec = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(spn_vec, result, args.size(),
											   [&](string_t) { return StringVector::AddString(result, kNoSspiMsg); });
}

#endif	// MSSQL_ENABLE_SSPI

void RegisterWinSspiTestFunction(ExtensionLoader &loader) {
	// mssql_winsspi_auth_test(host)
	ScalarFunction f1("mssql_winsspi_auth_test", {LogicalType::VARCHAR}, LogicalType::VARCHAR, WinSspiTestHost);
	loader.RegisterFunction(f1);

	// mssql_winsspi_auth_test(host, port)
	ScalarFunction f2("mssql_winsspi_auth_test", {LogicalType::VARCHAR, LogicalType::INTEGER}, LogicalType::VARCHAR,
					  WinSspiTestHostPort);
	loader.RegisterFunction(f2);

	// mssql_winsspi_auth_test_spn(spn)
	ScalarFunction f3("mssql_winsspi_auth_test_spn", {LogicalType::VARCHAR}, LogicalType::VARCHAR, WinSspiTestSpn);
	loader.RegisterFunction(f3);
}

}  // namespace winsspi
}  // namespace mssql
}  // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// winsspi_authenticator.hpp
//
// Windows implementation of IAuthenticator via secur32.dll's Negotiate
// security package. The peer of Krb5Authenticator (POSIX, Phase 3).
//
// Uses Negotiate (which is SPNEGO on the wire) to negotiate with SQL Server's
// SSPI server side. Negotiate transparently falls back to NTLM when the KDC
// isn't reachable for the target SPN -- if you need Kerberos-only, the user
// must register the SPN correctly in AD; we don't expose a separate
// "Kerberos" package selection because go-mssqldb / mssql-jdbc / pyodbc
// all use Negotiate.
//
// Compiled only when MSSQL_ENABLE_SSPI is defined (CMake sets it on _WIN32
// via secur32 linkage). See spec 042 phase4-windows-sspi.md.
//
// IMPORTANT: this header MUST NOT include any DuckDB headers. The TDS auth
// layer is reusable outside DuckDB.
//===----------------------------------------------------------------------===//

#pragma once

#if defined(MSSQL_ENABLE_SSPI)

#include "tds/auth/iauthenticator.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Windows SSPI headers. Must define SECURITY_WIN32 before including <security.h>
// so the right interface flavor is selected (vs SECURITY_KERNEL / SECURITY_MAC).
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
// Avoid Windows.h's min/max macros polluting std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <security.h>
#include <sspi.h>
#include <windows.h>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// WinSspiConfig - inputs to WinSspiAuthenticator
//
// Far simpler than Krb5Config -- Windows SSPI uses the current logon session
// only; no keytab path, no raw credentials, no krb5.conf override. The only
// knob is the target SPN.
//===----------------------------------------------------------------------===//
struct WinSspiConfig {
	std::string spn;  // e.g. "MSSQLSvc/host.example.com:1433"
};

//===----------------------------------------------------------------------===//
// WinSspiAuthenticator - IAuthenticator impl via Windows SSPI Negotiate
//===----------------------------------------------------------------------===//

class WinSspiAuthenticator : public IAuthenticator {
public:
	// Validates the config at construction time so misconfigurations surface
	// before any TDS traffic. Throws std::runtime_error if the SPN is empty.
	// Native SSPI handles are NOT allocated here -- those happen in
	// InitialBytes(), so handle-allocation failures surface at the right point
	// in the connection lifecycle.
	explicit WinSspiAuthenticator(WinSspiConfig config);

	~WinSspiAuthenticator() override;

	// Non-copyable / non-movable. SSPI handles aren't safe to copy.
	WinSspiAuthenticator(const WinSspiAuthenticator &) = delete;
	WinSspiAuthenticator &operator=(const WinSspiAuthenticator &) = delete;

	std::vector<uint8_t> InitialBytes() override;
	std::vector<uint8_t> NextBytes(const std::vector<uint8_t> &server_blob) override;
	void Free() override;

	// Testability accessor (parallel to Krb5Authenticator).
	const std::string &GetSpn() const {
		return config_.spn;
	}

private:
	// One InitializeSecurityContext round. Used by both InitialBytes (first
	// call, no input token) and NextBytes (continuation, server's 0xED blob
	// as input). Returns the output blob; sets complete_ on SEC_E_OK.
	std::vector<uint8_t> DoSecContextStep(const uint8_t *input_blob, size_t input_blob_len);

	// Acquire credentials once on the first InitialBytes call. Uses
	// AcquireCredentialsHandleW with Negotiate package + SECPKG_CRED_OUTBOUND
	// + NULL auth data (current logon session).
	void AcquireCredentials();

	// Throw a runtime_error formatted like Krb5Authenticator's errors:
	//   "MSSQL Kerberos auth failed: <what>: sspi_status=0x<hex>, <FormatMessageW text> [Hint: ...]"
	// Same prefix and hint vocabulary so user-facing errors are platform-agnostic.
	[[noreturn]] static void ThrowSspiError(const char *what, SECURITY_STATUS status);

	WinSspiConfig config_;
	bool complete_ = false;
	bool acquired_ = false;
	bool ctx_initialized_ = false;

	// Native SSPI handles. SECINVALIDHANDLE-equivalent zero-initialization.
	CredHandle cred_;
	CtxtHandle ctx_;

	// SPN in wide-char form, owned by the authenticator so the pointer passed
	// to InitializeSecurityContextW stays valid across multiple calls.
	std::wstring spn_w_;
};

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_SSPI

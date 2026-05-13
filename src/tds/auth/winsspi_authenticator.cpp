//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// winsspi_authenticator.cpp
//
// Windows SSPI implementation of IAuthenticator via the Negotiate package.
// Mirrors src/tds/auth/krb5_authenticator.cpp 1:1 in structure -- only the
// native API call differs.
//
// Spec 042 Phase 4. Compiled only when MSSQL_ENABLE_SSPI is defined.
//===----------------------------------------------------------------------===//

#include "tds/auth/winsspi_authenticator.hpp"

#if defined(MSSQL_ENABLE_SSPI)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace duckdb {
namespace tds {

namespace {

// "Negotiate" security package. SSPI's Negotiate is SPNEGO on the wire --
// exactly what SQL Server expects. NTLM fallback happens transparently when
// the KDC is unreachable for the target SPN; we don't disable that because
// it matches go-mssqldb / mssql-jdbc / pyodbc behavior.
static const wchar_t *kNegotiatePackage = L"Negotiate";

// Convert UTF-8 -> UTF-16 (Windows wchar_t). Used for the SPN passed to
// InitializeSecurityContextW. Returns an empty wstring for empty input.
static std::wstring Utf8ToWide(const std::string &utf8) {
	if (utf8.empty()) {
		return std::wstring();
	}
	int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(needed - 1), L'\0');  // -1: don't include null terminator
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], needed);
	return out;
}

// Render a SECURITY_STATUS as a human-readable string. FormatMessageW writes
// into a system-allocated buffer that we must LocalFree.
static std::string FormatSspiStatus(SECURITY_STATUS status) {
	LPWSTR buf = nullptr;
	DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
								   FORMAT_MESSAGE_IGNORE_INSERTS,
							   nullptr, static_cast<DWORD>(status), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
	std::string result;
	if (len > 0 && buf) {
		// Convert wide -> UTF-8 for the std::string error.
		int needed = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		if (needed > 0) {
			result.resize(static_cast<size_t>(needed));
			WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), &result[0], needed, nullptr, nullptr);
			// FormatMessageW commonly appends "\r\n"; strip trailing whitespace.
			while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
				result.pop_back();
			}
		}
	}
	if (buf) {
		LocalFree(buf);
	}
	if (result.empty()) {
		result = "(no FormatMessage text)";
	}
	return result;
}

// Spec 042 R8: map common SECURITY_STATUS codes to the same actionable hint
// vocabulary as Krb5Authenticator::HintForMinor on POSIX. Returns nullptr if
// the status doesn't match a known cause, so the caller can fall back to the
// raw FormatMessage text only.
static const char *HintForSspiStatus(SECURITY_STATUS status) {
	switch (status) {
	case SEC_E_NO_CREDENTIALS:
		return "(Hint: no Windows credentials available. Log in to a domain account before running DuckDB, or use SQL authentication.)";
	case SEC_E_CONTEXT_EXPIRED:
		return "(Hint: Kerberos ticket expired. Log out and back in, or run 'klist purge && klist tgt' to refresh.)";
	case SEC_E_TARGET_UNKNOWN:
		return "(Hint: server SPN not registered. Verify with 'setspn -L <account>' on a Windows admin host.)";
	case SEC_E_TIME_SKEW:
		return "(Hint: clock skew between client and KDC exceeds 5 minutes. Sync system clock via Windows Time service: 'w32tm /resync'.)";
	case SEC_E_NO_AUTHENTICATING_AUTHORITY:
		return "(Hint: KDC / domain controller unreachable. Verify network connectivity and DNS resolution for the target realm.)";
	case SEC_E_WRONG_PRINCIPAL:
		return "(Hint: SPN mismatch -- the SPN your client requested does not match the SPN registered for the SQL Server account.)";
	case SEC_E_LOGON_DENIED:
		return "(Hint: authentication denied -- check that your domain account is enabled and has access to the target server.)";
	default:
		return nullptr;
	}
}

}  // namespace

WinSspiAuthenticator::WinSspiAuthenticator(WinSspiConfig config) : config_(std::move(config)) {
	if (config_.spn.empty()) {
		throw std::runtime_error(
			"MSSQL Kerberos auth: empty SPN. AuthStrategyFactory should derive 'MSSQLSvc/<fqdn>:<port>' "
			"from the host before constructing WinSspiAuthenticator.");
	}
	// Zero-init the native handles. SSPI uses sentinel zero values for "no
	// handle" -- the same convention DeleteSecurityContext / FreeCredentialsHandle
	// treats as a no-op.
	SecInvalidateHandle(&cred_);
	SecInvalidateHandle(&ctx_);

	// Cache the SPN in wide-char form once. The pointer passed to
	// InitializeSecurityContextW must remain valid across multiple calls (the
	// API is "stable across continuation rounds").
	spn_w_ = Utf8ToWide(config_.spn);
	if (spn_w_.empty() && !config_.spn.empty()) {
		throw std::runtime_error("MSSQL Kerberos auth: failed to convert SPN '" + config_.spn + "' to UTF-16.");
	}
}

WinSspiAuthenticator::~WinSspiAuthenticator() {
	Free();
}

void WinSspiAuthenticator::ThrowSspiError(const char *what, SECURITY_STATUS status) {
	std::string text(what);
	char buf[32];
	std::snprintf(buf, sizeof(buf), ": sspi_status=0x%08lx, ", static_cast<unsigned long>(status));
	text.append(buf);
	text.append(FormatSspiStatus(status));
	const char *hint = HintForSspiStatus(status);
	if (hint) {
		text.append(" ");
		text.append(hint);
	}
	throw std::runtime_error(std::string("MSSQL Kerberos auth failed: ") + text);
}

void WinSspiAuthenticator::AcquireCredentials() {
	if (acquired_) {
		return;
	}
	// AcquireCredentialsHandleW with NULL auth data uses the current logon
	// session's credentials. SECPKG_CRED_OUTBOUND because we're the client
	// initiating a connection (vs accepting one as a server).
	TimeStamp expiry;
	SECURITY_STATUS status = AcquireCredentialsHandleW(
		/*pszPrincipal=*/nullptr,				   // use default principal (logon session)
		/*pszPackage=*/const_cast<LPWSTR>(kNegotiatePackage),
		/*fCredentialUse=*/SECPKG_CRED_OUTBOUND,
		/*pvLogonId=*/nullptr,					   // no specific logon ID
		/*pAuthData=*/nullptr,					   // use default creds
		/*pGetKeyFn=*/nullptr,
		/*pvGetKeyArgument=*/nullptr,
		/*phCredential=*/&cred_,
		/*ptsExpiry=*/&expiry);
	if (status != SEC_E_OK) {
		// cred_ wasn't populated -- mark it invalid so Free() doesn't try to
		// release an unallocated handle.
		SecInvalidateHandle(&cred_);
		ThrowSspiError("AcquireCredentialsHandleW (Negotiate)", status);
	}
	acquired_ = true;
}

std::vector<uint8_t> WinSspiAuthenticator::DoSecContextStep(const uint8_t *input_blob, size_t input_blob_len) {
	// Build the input buffer descriptor (continuation case) or pass nullptr
	// for the initial call. SSPI uses a SecBufferDesc with .cBuffers and an
	// array of SecBuffer; for the input we have at most one SECBUFFER_TOKEN.
	SecBuffer in_buf;
	SecBufferDesc in_buf_desc;
	SecBufferDesc *in_buf_desc_ptr = nullptr;
	if (input_blob && input_blob_len > 0) {
		in_buf.cbBuffer = static_cast<unsigned long>(input_blob_len);
		in_buf.BufferType = SECBUFFER_TOKEN;
		in_buf.pvBuffer = const_cast<uint8_t *>(input_blob);

		in_buf_desc.ulVersion = SECBUFFER_VERSION;
		in_buf_desc.cBuffers = 1;
		in_buf_desc.pBuffers = &in_buf;
		in_buf_desc_ptr = &in_buf_desc;
	}

	// Output buffer: SSPI allocates the token when we set ISC_REQ_ALLOCATE_MEMORY.
	// We FreeContextBuffer'd it after copying out.
	SecBuffer out_buf;
	out_buf.cbBuffer = 0;
	out_buf.BufferType = SECBUFFER_TOKEN;
	out_buf.pvBuffer = nullptr;

	SecBufferDesc out_buf_desc;
	out_buf_desc.ulVersion = SECBUFFER_VERSION;
	out_buf_desc.cBuffers = 1;
	out_buf_desc.pBuffers = &out_buf;

	// Flags mirror the GSSAPI flags in Krb5Authenticator::DoSecContextStep
	// (mutual auth + replay/sequence detect + integrity/confidentiality) +
	// ALLOCATE_MEMORY so SSPI owns the output buffer lifecycle.
	const unsigned long flags = ISC_REQ_MUTUAL_AUTH | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
								ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY;

	unsigned long attrs = 0;
	TimeStamp expiry;

	// First call: phContext == nullptr (or invalid); subsequent calls pass the
	// existing context handle. SSPI threads "is this the first call?" by the
	// validity of phContext, NOT by some flag.
	PCtxtHandle ctx_in = ctx_initialized_ ? &ctx_ : nullptr;

	SECURITY_STATUS status = InitializeSecurityContextW(
		/*phCredential=*/&cred_,
		/*phContext=*/ctx_in,
		/*pszTargetName=*/const_cast<LPWSTR>(spn_w_.c_str()),
		/*fContextReq=*/flags,
		/*Reserved1=*/0,
		/*TargetDataRep=*/SECURITY_NATIVE_DREP,
		/*pInput=*/in_buf_desc_ptr,
		/*Reserved2=*/0,
		/*phNewContext=*/&ctx_,
		/*pOutput=*/&out_buf_desc,
		/*pfContextAttr=*/&attrs,
		/*ptsExpiry=*/&expiry);

	// Mark context initialized after the first successful (or continue-needed)
	// call so subsequent rounds pass it back in as phContext.
	if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
		ctx_initialized_ = true;
	}

	if (status == SEC_E_OK) {
		complete_ = true;
	} else if (status != SEC_I_CONTINUE_NEEDED) {
		// Error case. If the output buffer was allocated, free it before throwing.
		if (out_buf.pvBuffer) {
			FreeContextBuffer(out_buf.pvBuffer);
		}
		ThrowSspiError("InitializeSecurityContextW", status);
	}

	// Copy the output blob into a std::vector that the caller owns; then free
	// SSPI's allocation. The vector outlives this function; the SSPI buffer
	// would otherwise leak per call.
	std::vector<uint8_t> out;
	if (out_buf.pvBuffer && out_buf.cbBuffer > 0) {
		const uint8_t *p = static_cast<const uint8_t *>(out_buf.pvBuffer);
		out.assign(p, p + out_buf.cbBuffer);
	}
	if (out_buf.pvBuffer) {
		FreeContextBuffer(out_buf.pvBuffer);
	}
	return out;
}

std::vector<uint8_t> WinSspiAuthenticator::InitialBytes() {
	AcquireCredentials();
	auto blob = DoSecContextStep(nullptr, 0);
	if (blob.empty() && !complete_) {
		throw std::runtime_error(
			"MSSQL Kerberos auth failed: SSPI returned an empty initial SPNEGO token. "
			"This is unexpected from the Negotiate package; check that the Windows logon session has Kerberos enabled.");
	}
	return blob;
}

std::vector<uint8_t> WinSspiAuthenticator::NextBytes(const std::vector<uint8_t> &server_blob) {
	if (complete_) {
		// Server sent more data after we were done. Some servers emit a
		// final ack wrapper -- just return an empty blob to stop the loop.
		return {};
	}
	return DoSecContextStep(server_blob.empty() ? nullptr : server_blob.data(), server_blob.size());
}

void WinSspiAuthenticator::Free() {
	if (ctx_initialized_) {
		DeleteSecurityContext(&ctx_);
		SecInvalidateHandle(&ctx_);
		ctx_initialized_ = false;
	}
	if (acquired_) {
		FreeCredentialsHandle(&cred_);
		SecInvalidateHandle(&cred_);
		acquired_ = false;
	}
	complete_ = true;
}

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_SSPI

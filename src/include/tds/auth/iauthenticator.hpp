//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// iauthenticator.hpp
//
// Multi-round authentication interface for Integrated Authentication
// (SPNEGO/Kerberos via GSSAPI on POSIX, SSPI on Windows).
//
// Mirrors microsoft/go-mssqldb integratedauth.IntegratedAuthenticator
// interface verbatim: InitialBytes / NextBytes / Free, plus an optional
// SetChannelBinding for future Extended Protection for Authentication (EPA).
//
// IMPORTANT: This header MUST NOT include any DuckDB headers. The TDS auth
// layer is reusable outside DuckDB; this is the interface seam.
//
// Spec: specs/042-integrated-authentication/
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// ChannelBindings - placeholder for RFC 5929 tls-server-end-point bindings
//
// Empty by design for v1 (Extended Protection for Authentication is out of
// scope per spec 042). The struct exists so the IAuthenticator interface is
// forward-compatible; SetChannelBinding is a virtual no-op until v2.
//===----------------------------------------------------------------------===//

struct ChannelBindings {
	// Reserved for future use. Will eventually carry the TLS peer
	// certificate hash + binding type for tls-server-end-point.
};

//===----------------------------------------------------------------------===//
// IAuthenticator - three-method multi-round auth interface
//
// Drives the SPNEGO/Kerberos exchange embedded in TDS LOGIN7 SSPI field +
// 0xED SSPI continuation tokens. See [MS-TDS] 2.2.6.4 and 2.2.7.21.
//
// Lifecycle:
//   1. Construct the concrete authenticator (Krb5Authenticator on POSIX,
//      WinSspiAuthenticator on Windows). All configuration (SPN, keytab
//      path, credential mode) is set at construction time so config errors
//      surface before any TDS traffic.
//   2. Call InitialBytes() - returns the first SPNEGO blob to embed in
//      LOGIN7's SSPI field.
//   3. While the server returns 0xED SSPI tokens, call NextBytes(token).
//      If the returned blob is non-empty, send it in an SSPI Message
//      packet (TDS packet type 0x11). If empty, the negotiation is
//      complete and the server should follow with LOGINACK + DONE.
//   4. Call Free() to release native auth context handles.
//
// Implementations MUST be exception-safe. Errors from the underlying
// GSSAPI/SSPI calls are translated into std::runtime_error (or a
// DuckDB-flavored exception in the strategy adapter layer) with the
// underlying status text rendered via gss_display_status / FormatMessage.
//===----------------------------------------------------------------------===//

class IAuthenticator {
public:
	virtual ~IAuthenticator() = default;

	// Produce the initial SPNEGO blob to embed in LOGIN7.SSPI.
	// Called exactly once, before the LOGIN7 packet is built.
	virtual std::vector<uint8_t> InitialBytes() = 0;

	// Continue the negotiation with the server's last 0xED blob.
	// Return value: next blob to send (in an SSPI Message packet), or
	// an empty vector if the negotiation has completed from the client's
	// perspective. An empty return is not the same as a failure - the
	// client treats it as "stop sending; wait for LOGINACK".
	virtual std::vector<uint8_t> NextBytes(const std::vector<uint8_t> &server_blob) = 0;

	// Release native handles (gss_delete_sec_context, DeleteSecurityContext,
	// etc). MUST be idempotent. Called by the destructor; may be called
	// explicitly to release handles before destruction.
	virtual void Free() = 0;

	// Optional: install channel binding data for EPA. Default no-op.
	// Servers that require EPA will reject the connection with a specific
	// error when this is not implemented; that escalation lives in v2.
	virtual void SetChannelBinding(const ChannelBindings & /*cb*/) {
	}

protected:
	IAuthenticator() = default;
};

//===----------------------------------------------------------------------===//
// Type alias for shared ownership (parallel to AuthStrategyPtr)
//===----------------------------------------------------------------------===//

using AuthenticatorPtr = std::shared_ptr<IAuthenticator>;

}  // namespace tds
}  // namespace duckdb

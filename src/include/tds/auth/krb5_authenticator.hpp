//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// krb5_authenticator.hpp
//
// POSIX implementation of IAuthenticator via system GSSAPI
// (libgssapi_krb5 on Linux, GSS.framework on macOS).
//
// Uses the SPNEGO mechanism (1.3.6.1.5.5.2) to negotiate with SQL Server's
// SSPI / Negotiate package. Drives the multi-round LOGIN7 + 0xED SSPI-token
// exchange driven by TdsConnection::AuthenticateIntegrated().
//
// Compiled only when MSSQL_ENABLE_KRB5 is defined (set by CMake when
// GSSAPI is discovered on POSIX). See spec 042 plan.md / research.md R3.
//
// IMPORTANT: this header MUST NOT include any DuckDB headers. The TDS
// auth layer is reusable outside DuckDB.
//===----------------------------------------------------------------------===//

#pragma once

#if defined(MSSQL_ENABLE_KRB5)

#include "tds/auth/iauthenticator.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// GSSAPI headers. On Linux this is gssapi/gssapi.h; on macOS via the GSS
// framework. Both expose the standard GSSAPI types and the krb5_gssapi
// extensions used here. Include the krb5-specific header for OID constants.
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// Credential acquisition mode
//
// Drives which gss_acquire_cred_* path is used at InitialBytes() time. The
// constructor selects the mode based on which Krb5Config fields are populated:
// keytab path wins, then raw user+password+realm, then ccache (default).
//===----------------------------------------------------------------------===//
enum class Krb5CredentialMode {
	CredCache,	// Default: GSS_C_NO_CREDENTIAL -> KRB5CCNAME or /tmp/krb5cc_<uid>
	Keytab,		// gss_acquire_cred_from with keytab element
	Raw			// krb5_get_init_creds_password into a MEMORY: ccache
};

//===----------------------------------------------------------------------===//
// Krb5Config -- inputs to Krb5Authenticator
//
// All fields optional except spn (which is computed from host/port if the user
// did not override it). When username/password/realm are all set AND no
// keytab path is given, the constructor selects Raw mode.
//===----------------------------------------------------------------------===//
struct Krb5Config {
	std::string spn;			   // MSSQLSvc@<fqdn>  (NOTE: @ not /)
	std::string configfile;		   // /etc/krb5.conf override (KRB5_CONFIG)
	std::string keytabfile;		   // Path to keytab; selects Keytab mode
	std::string credcachefile;	   // ccache path override (KRB5CCNAME)
	std::string realm;			   // AD realm for raw / keytab modes (uppercased)
	std::string raw_username;	   // Principal for raw mode (without @REALM is OK)
	std::string raw_password;	   // Cleartext password for raw mode
};

//===----------------------------------------------------------------------===//
// Krb5Authenticator -- IAuthenticator impl via system GSSAPI
//===----------------------------------------------------------------------===//

class Krb5Authenticator : public IAuthenticator {
public:
	// Constructs and validates the config. Throws std::runtime_error if the
	// requested mode is impossible (e.g. keytab path is unreadable, raw mode
	// is asked for but the realm is missing). Resource handles are NOT
	// allocated here -- those happen in InitialBytes() so failure modes are
	// observable at the right point in the connection lifecycle.
	explicit Krb5Authenticator(Krb5Config config);

	~Krb5Authenticator() override;

	// Non-copyable / non-movable. GSSAPI handles are not safe to copy.
	Krb5Authenticator(const Krb5Authenticator &) = delete;
	Krb5Authenticator &operator=(const Krb5Authenticator &) = delete;

	std::vector<uint8_t> InitialBytes() override;
	std::vector<uint8_t> NextBytes(const std::vector<uint8_t> &server_blob) override;
	void Free() override;

	// Internal helpers exposed for testability. Not part of the abstract
	// IAuthenticator interface.
	Krb5CredentialMode GetMode() const {
		return mode_;
	}
	const std::string &GetSpn() const {
		return config_.spn;
	}

private:
	// One gss_init_sec_context round, used by both InitialBytes and NextBytes.
	std::vector<uint8_t> DoSecContextStep(const uint8_t *input_blob, size_t input_blob_len);

	// Acquire creds in the configured mode (called once, on first InitialBytes call).
	void AcquireCredentials();

	// Translate the GSS major/minor pair into a human-readable error and
	// throw std::runtime_error with the standard "MSSQL Kerberos auth failed: ..."
	// prefix. Includes both the major and minor status text per spec 042 R8.
	[[noreturn]] static void ThrowGssError(const char *what, uint32_t major, uint32_t minor, gss_OID mech = nullptr);

	Krb5Config config_;
	Krb5CredentialMode mode_;
	bool complete_ = false;
	bool acquired_ = false;

	// GSSAPI handles. Default "empty" values per gssapi.h conventions.
	gss_ctx_id_t ctx_ = GSS_C_NO_CONTEXT;
	gss_cred_id_t cred_ = GSS_C_NO_CREDENTIAL;
	gss_name_t target_name_ = GSS_C_NO_NAME;
};

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_KRB5

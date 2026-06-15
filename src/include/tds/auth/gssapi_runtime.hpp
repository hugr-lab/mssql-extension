//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// gssapi_runtime.hpp
//
// Lazy runtime loader for the system GSSAPI / MIT Kerberos libraries.
//
// Spec 053 (issue #161): the Linux community binary must NOT carry a
// link-time (DT_NEEDED) dependency on libgssapi_krb5.so.2 / libkrb5.so.3,
// otherwise the dynamic loader demands them at LOAD time even for users
// who never use Kerberos. Instead the function symbols are resolved at
// runtime via dlopen/dlsym, triggered only when integrated authentication
// is actually selected.
//
// On Linux the tables are filled by dlsym; on macOS the GSS framework is a
// system framework that is always present, so the tables are filled with
// the addresses of the linked symbols (no dlopen). Call-site code is the
// same on both platforms.
//
// Compiled only when MSSQL_ENABLE_KRB5 is defined (see CMakeLists.txt).
//
// IMPORTANT: this header MUST NOT include any DuckDB headers. The TDS auth
// layer is reusable outside DuckDB. See [[feedback-iauthenticator-layering]].
//===----------------------------------------------------------------------===//

#pragma once

#if defined(MSSQL_ENABLE_KRB5)

#include <stdexcept>
#include <string>

// GSSAPI headers. On Linux this is gssapi/gssapi.h; on macOS via the GSS
// framework. We still need the headers at BUILD time for the types,
// constants and OID structs -- none of which emit a link-time symbol (the
// OIDs are constructed inline as DER bytes). Only the gss_*/krb5_* function
// references would create a DT_NEEDED, and those now go through the tables
// below instead of direct calls.
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_krb5.h>

// MIT-only extensions (keytab / raw credential modes and the krb5_* helpers)
// are Linux-only; macOS's bundled GSS framework (Heimdal subset) lacks them.
#if !defined(__APPLE__)
#include <krb5.h>
#endif

namespace duckdb {
namespace tds {

//===----------------------------------------------------------------------===//
// Krb5RuntimeUnavailable
//
// Thrown by the accessors below when the Kerberos runtime library (or a
// required symbol) cannot be loaded. The message names the missing object
// and the system package to install (spec 053 FR-005).
//===----------------------------------------------------------------------===//
class Krb5RuntimeUnavailable : public std::runtime_error {
public:
	explicit Krb5RuntimeUnavailable(const std::string &msg) : std::runtime_error(msg) {}
};

//===----------------------------------------------------------------------===//
// GssApiFns -- resolved addresses of the GSSAPI functions used by the
// authenticator. Pointer types are derived via decltype from the platform's
// own declarations so signatures always match (MIT vs Heimdal differ in
// const-qualifiers).
//===----------------------------------------------------------------------===//
struct GssApiFns {
	decltype(&gss_display_status) display_status;
	decltype(&gss_release_buffer) release_buffer;
	decltype(&gss_import_name) import_name;
	decltype(&gss_init_sec_context) init_sec_context;
	decltype(&gss_delete_sec_context) delete_sec_context;
	decltype(&gss_release_cred) release_cred;
	decltype(&gss_release_name) release_name;
#if !defined(__APPLE__)
	// Used only by the keytab / raw / ccache-override paths (MIT extensions).
	decltype(&gss_acquire_cred_from) acquire_cred_from;
#endif
};

// Returns the GSSAPI function table, loading the runtime on first call.
// Thread-safe (std::call_once). Throws Krb5RuntimeUnavailable on failure.
const GssApiFns &GetGssApi();

#if !defined(__APPLE__)
//===----------------------------------------------------------------------===//
// Krb5Fns -- resolved addresses of the MIT krb5_* functions (raw-credentials
// mode in the authenticator + the ccache lookup in the test function).
// Linux only; never requested on macOS.
//===----------------------------------------------------------------------===//
struct Krb5Fns {
	decltype(&krb5_init_context) init_context;
	decltype(&krb5_parse_name) parse_name;
	decltype(&krb5_get_init_creds_password) get_init_creds_password;
	decltype(&krb5_free_principal) free_principal;
	decltype(&krb5_get_error_message) get_error_message;
	decltype(&krb5_free_error_message) free_error_message;
	decltype(&krb5_free_context) free_context;
	decltype(&krb5_cc_resolve) cc_resolve;
	decltype(&krb5_cc_initialize) cc_initialize;
	decltype(&krb5_cc_store_cred) cc_store_cred;
	decltype(&krb5_cc_close) cc_close;
	decltype(&krb5_cc_destroy) cc_destroy;
	decltype(&krb5_free_cred_contents) free_cred_contents;
	// Test-function-only helpers (mssql_kerberos_auth_test).
	decltype(&krb5_cc_default) cc_default;
	decltype(&krb5_cc_get_principal) cc_get_principal;
	decltype(&krb5_unparse_name) unparse_name;
	decltype(&krb5_free_unparsed_name) free_unparsed_name;
};

// Returns the MIT krb5 function table, loading libkrb5 on first call.
// Thread-safe. Throws Krb5RuntimeUnavailable on failure.
const Krb5Fns &GetKrb5();
#endif	// !__APPLE__

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_KRB5

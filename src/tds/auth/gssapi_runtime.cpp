//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// gssapi_runtime.cpp
//
// Implements the lazy GSSAPI / MIT Kerberos runtime loader declared in
// gssapi_runtime.hpp. Spec 053 (issue #161).
//
//   * Linux: dlopen(libgssapi_krb5.so.2) / dlopen(libkrb5.so.3) on first use,
//     dlsym each symbol into the function tables. No DT_NEEDED, so the
//     extension loads on images without the Kerberos runtime installed.
//   * macOS: the GSS framework is a system framework that is always present;
//     the table is filled with the addresses of the linked symbols (no
//     dlopen). krb5_* (MIT extensions) are not used on macOS.
//
// Loading is guarded by std::call_once so concurrent first-connection
// attempts on worker threads load the library exactly once.
//===----------------------------------------------------------------------===//

#include "tds/auth/gssapi_runtime.hpp"

#if defined(MSSQL_ENABLE_KRB5)

#include <mutex>

#if !defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace duckdb {
namespace tds {

#if !defined(__APPLE__)
//===----------------------------------------------------------------------===//
// Linux: dlopen + dlsym
//===----------------------------------------------------------------------===//
namespace {

std::string SafeDlerror() {
	const char *e = dlerror();
	return e ? std::string(e) : std::string("(no detail)");
}

// dlopen the first SONAME that succeeds. names is null-terminated.
void *OpenFirst(const char *const *names) {
	for (const char *const *n = names; *n != nullptr; ++n) {
		void *h = dlopen(*n, RTLD_NOW | RTLD_LOCAL);
		if (h) {
			return h;
		}
	}
	return nullptr;
}

// Resolve a symbol or throw Krb5RuntimeUnavailable naming it.
template <typename T>
void Resolve(void *handle, const char *sym, const char *libname, T &out) {
	void *p = dlsym(handle, sym);
	if (!p) {
		throw Krb5RuntimeUnavailable(std::string("MSSQL Kerberos auth: symbol '") + sym + "' missing from " + libname +
									 " (dlsym: " + SafeDlerror() +
									 "). The installed Kerberos runtime is incomplete or too old.");
	}
	// POSIX guarantees dlsym's void* can be cast to a function pointer.
	out = reinterpret_cast<T>(p);
}

GssApiFns LoadGss() {
	static const char *const kGssNames[] = {"libgssapi_krb5.so.2", "libgssapi_krb5.so", nullptr};
	void *h = OpenFirst(kGssNames);
	if (!h) {
		throw Krb5RuntimeUnavailable(
			std::string("MSSQL Kerberos auth: could not load the system Kerberos runtime "
						"'libgssapi_krb5.so.2' (dlopen: ") +
			SafeDlerror() +
			"). Integrated/Kerberos authentication is loaded on demand -- the extension itself "
			"loads fine without it. Install the Kerberos runtime to use authenticator=krb5: "
			"Debian/Ubuntu 'apt install libgssapi-krb5-2', RHEL/Fedora 'dnf install krb5-libs'.");
	}
	GssApiFns f;
	Resolve(h, "gss_display_status", "libgssapi_krb5", f.display_status);
	Resolve(h, "gss_release_buffer", "libgssapi_krb5", f.release_buffer);
	Resolve(h, "gss_import_name", "libgssapi_krb5", f.import_name);
	Resolve(h, "gss_init_sec_context", "libgssapi_krb5", f.init_sec_context);
	Resolve(h, "gss_delete_sec_context", "libgssapi_krb5", f.delete_sec_context);
	Resolve(h, "gss_release_cred", "libgssapi_krb5", f.release_cred);
	Resolve(h, "gss_release_name", "libgssapi_krb5", f.release_name);
	Resolve(h, "gss_acquire_cred_from", "libgssapi_krb5", f.acquire_cred_from);
	// Handle is intentionally not dlclose'd: the tables hold function
	// pointers into it for the process lifetime.
	return f;
}

Krb5Fns LoadKrb5() {
	static const char *const kKrb5Names[] = {"libkrb5.so.3", "libkrb5.so", nullptr};
	void *h = OpenFirst(kKrb5Names);
	if (!h) {
		throw Krb5RuntimeUnavailable(std::string("MSSQL Kerberos auth: could not load 'libkrb5.so.3' (dlopen: ") +
									 SafeDlerror() +
									 "). Install the Kerberos runtime: Debian/Ubuntu 'apt install libkrb5-3', "
									 "RHEL/Fedora 'dnf install krb5-libs'.");
	}
	Krb5Fns f;
	Resolve(h, "krb5_init_context", "libkrb5", f.init_context);
	Resolve(h, "krb5_parse_name", "libkrb5", f.parse_name);
	Resolve(h, "krb5_get_init_creds_password", "libkrb5", f.get_init_creds_password);
	Resolve(h, "krb5_free_principal", "libkrb5", f.free_principal);
	Resolve(h, "krb5_get_error_message", "libkrb5", f.get_error_message);
	Resolve(h, "krb5_free_error_message", "libkrb5", f.free_error_message);
	Resolve(h, "krb5_free_context", "libkrb5", f.free_context);
	Resolve(h, "krb5_cc_resolve", "libkrb5", f.cc_resolve);
	Resolve(h, "krb5_cc_initialize", "libkrb5", f.cc_initialize);
	Resolve(h, "krb5_cc_store_cred", "libkrb5", f.cc_store_cred);
	Resolve(h, "krb5_cc_close", "libkrb5", f.cc_close);
	Resolve(h, "krb5_cc_destroy", "libkrb5", f.cc_destroy);
	Resolve(h, "krb5_free_cred_contents", "libkrb5", f.free_cred_contents);
	Resolve(h, "krb5_cc_default", "libkrb5", f.cc_default);
	Resolve(h, "krb5_cc_get_principal", "libkrb5", f.cc_get_principal);
	Resolve(h, "krb5_unparse_name", "libkrb5", f.unparse_name);
	Resolve(h, "krb5_free_unparsed_name", "libkrb5", f.free_unparsed_name);
	return f;
}

}  // namespace

const GssApiFns &GetGssApi() {
	static GssApiFns fns;
	static std::once_flag flag;
	// If LoadGss throws, call_once rethrows and leaves the flag unset, so a
	// later connection attempt retries the load (e.g. after the user installs
	// the package without restarting).
	std::call_once(flag, [&]() { fns = LoadGss(); });
	return fns;
}

const Krb5Fns &GetKrb5() {
	static Krb5Fns fns;
	static std::once_flag flag;
	std::call_once(flag, [&]() { fns = LoadKrb5(); });
	return fns;
}

#else	// __APPLE__
//===----------------------------------------------------------------------===//
// macOS: GSS is a system framework, always present. Fill the table with the
// addresses of the linked symbols -- no dlopen. (krb5_* / GetKrb5 are not
// used on macOS; the authenticator rejects keytab/raw modes at construction.)
//===----------------------------------------------------------------------===//
const GssApiFns &GetGssApi() {
	static const GssApiFns fns = []() {
		GssApiFns f;
		f.display_status = &gss_display_status;
		f.release_buffer = &gss_release_buffer;
		f.import_name = &gss_import_name;
		f.init_sec_context = &gss_init_sec_context;
		f.delete_sec_context = &gss_delete_sec_context;
		f.release_cred = &gss_release_cred;
		f.release_name = &gss_release_name;
		return f;
	}();
	return fns;
}
#endif	// __APPLE__

}  // namespace tds
}  // namespace duckdb

#endif	// MSSQL_ENABLE_KRB5

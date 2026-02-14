# Research: Replace libcurl with DuckDB Built-in httplib

## R1: httplib availability in DuckDB

**Decision**: Use `duckdb/third_party/httplib/httplib.hpp` (cpp-httplib v0.27.0) bundled with DuckDB.

**Rationale**: Already present in DuckDB source tree. Header-only — no additional build or link steps. No new dependency.

**Alternatives considered**:
- External cpp-httplib via vcpkg: Adds a dependency, potential version conflicts with DuckDB's bundled copy
- Raw OpenSSL sockets: Too low-level for HTTP POST, would need to implement HTTP/1.1 framing manually
- DuckDB's HTTPUtil wrapper: POST not implemented in DuckDB core's wrapper class (`throw NotImplementedException`)

## R2: OpenSSL namespace isolation

**Decision**: Compile our httplib translation unit with `CPPHTTPLIB_OPENSSL_SUPPORT` defined. This activates namespace `duckdb_httplib_openssl`, separate from DuckDB core's `duckdb_httplib`.

**Rationale**: DuckDB's httplib.hpp uses conditional namespaces:
```cpp
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_NAMESPACE duckdb_httplib_openssl
#else
#define CPPHTTPLIB_NAMESPACE duckdb_httplib
```

This prevents ODR violations — our OpenSSL-backed httplib symbols live in a different namespace than DuckDB core's non-SSL httplib symbols.

**Alternatives considered**:
- Using DuckDB core's `duckdb_httplib::Client` (HTTP only, no HTTPS — Azure AD requires HTTPS)
- Vendoring a separate copy of httplib with custom namespace — unnecessary, DuckDB already solves this

## R3: URL encoding approach

**Decision**: Use httplib's `Post(path, Params)` overload which accepts `std::multimap<std::string, std::string>` and performs URL encoding internally.

**Rationale**: httplib's Params-based Post handles form encoding automatically. This eliminates the need for `curl_easy_escape()` or a custom percent-encode function entirely.

**Example**:
```cpp
httplib::Params params = {
    {"grant_type", "client_credentials"},
    {"client_id", info.client_id},
    {"client_secret", info.client_secret},
    {"scope", AZURE_SQL_SCOPE}
};
auto res = client.Post("/tenant_id/oauth2/v2.0/token", params);
```

**Alternatives considered**:
- Custom `UrlEncode()` function (10-15 lines): Not needed — httplib handles it
- `httplib::detail::encode_url()`: Internal API, not guaranteed stable

## R4: MSVC linker impact after removing curl

**Decision**: Remove `tds_win32_compat.cpp` entirely after removing libcurl. If OpenSSL alone still produces `__imp_` symbols, keep only the needed wrappers (likely `read`, `write`, `close` for OpenSSL's BIO layer).

**Rationale**: The bulk of `__imp_` symbols (`__imp__time64`, `__imp_strspn`, `__imp_strncpy`, `__imp_wcsncpy`, `__imp_fdopen`, `__imp_fileno`, `__imp_unlink`) came from libcurl. OpenSSL's static library may still reference `read`/`write`/`close` as POSIX I/O for its BIO layer, but this is a much smaller surface.

**Action**: After removing curl, do a test build with the community triplet. If it links clean, delete `tds_win32_compat.cpp`. If not, reduce it to only the remaining unresolved symbols.

## R5: Build dependency changes

**Decision**:
- Remove `curl` from `vcpkg.json` dependencies
- Remove `find_package(CURL REQUIRED)` from `CMakeLists.txt`
- Remove `CURL::libcurl` from `target_link_libraries`
- Remove MSVC-specific `wldap32`, `normaliz`, `CURL_STATICLIB` (these are curl-only)
- Keep `ws2_32`, `crypt32` (needed for OpenSSL on Windows)
- Add `target_include_directories(...PRIVATE .../duckdb/third_party/httplib)`
- Add `target_compile_definitions(...PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)`

**Rationale**: httplib is header-only, compiled into our `.cpp` file. OpenSSL is already linked. No new binary dependencies.

## R6: httplib compilation unit strategy

**Decision**: Create a single new source file `src/azure/azure_http.cpp` that `#define CPPHTTPLIB_OPENSSL_SUPPORT` and `#include "httplib.hpp"`. This file provides the HTTP POST wrapper used by `azure_token.cpp` and `azure_device_code.cpp`.

**Rationale**: httplib.hpp is a large header (~10K lines). Compiling it in a single translation unit keeps build times reasonable and avoids multiple-definition issues. The wrapper provides a simple `HttpPost()` function hiding httplib details.

**Alternatives considered**:
- Including httplib.hpp directly in azure_token.cpp and azure_device_code.cpp: Would compile the header twice, slow build, potential ODR issues
- Using httplib only in headers: Bad — large header would pollute all includers

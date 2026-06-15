# Contract: GSSAPI/krb5 Runtime Loader

Internal C++ interface exposed by `src/include/tds/auth/gssapi_runtime.hpp`.
Compiled only under `MSSQL_ENABLE_KRB5`. No DuckDB headers (TDS-layer reusable).
Namespace `duckdb::tds`.

## Accessors

```cpp
// Returns the GSSAPI function table, loading the runtime on first call.
// Thread-safe (std::call_once). Throws Krb5RuntimeUnavailable on failure.
const GssApiFns &GetGssApi();

// Returns the MIT krb5 function table (raw/keytab/test paths). Linux only.
// Thread-safe. Throws Krb5RuntimeUnavailable on failure.
const Krb5Fns &GetKrb5();
```

**Behavioral guarantees**
- First successful call loads the library exactly once; subsequent calls return the
  cached table with no further `dlopen`.
- Never invoked during extension load or on non-Kerberos connection paths
  (caller-enforced: first call is inside `Krb5Authenticator::InitialBytes()` /
  test-function body).
- On macOS, `GetGssApi()` returns a table of directly-linked addresses and never
  calls `dlopen`; `GetKrb5()` is not used on macOS.

## Error contract

```cpp
class Krb5RuntimeUnavailable : public std::runtime_error { ... };
```

- Thrown by an accessor when the library or a required symbol cannot be resolved.
- `what()` MUST contain: (1) the missing object name (e.g. `libgssapi_krb5.so.2`),
  (2) the underlying `dlerror()` text, (3) an install-package recommendation
  (`Debian/Ubuntu: libgssapi-krb5-2; RHEL/Fedora: krb5-libs`).
- In a connection path: propagates as the connection's auth failure.
- In `mssql_kerberos_auth_test` / `mssql_kerberos_auth_test_secret`: caught and
  returned as the function's result string (function stays registered, no crash).

## Build contract (CMakeLists.txt)

- `src/tds/auth/gssapi_runtime.cpp` is added to extension sources when
  `MSSQL_KRB5_AVAILABLE`.
- **Linux**: GSSAPI/krb5 include dirs are still added; their `target_link_libraries`
  entries are removed; `${CMAKE_DL_LIBS}` is linked. Result: no
  `libgssapi_krb5.so.2` / `libkrb5.so.3` in the binary's `DT_NEEDED`.
- **macOS**: `-framework GSS` link is retained unchanged.
- **Windows**: unaffected (SSPI path; `MSSQL_ENABLE_KRB5` undefined).

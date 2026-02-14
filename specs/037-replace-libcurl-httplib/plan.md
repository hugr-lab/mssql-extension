# Implementation Plan: Replace libcurl with DuckDB Built-in httplib

**Branch**: `037-replace-libcurl-httplib` | **Date**: 2026-02-14 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/037-replace-libcurl-httplib/spec.md`

## Summary

Replace libcurl dependency with DuckDB's bundled cpp-httplib (`duckdb/third_party/httplib/httplib.hpp`) for Azure OAuth2 HTTP requests. Compiled with `CPPHTTPLIB_OPENSSL_SUPPORT` to use our existing OpenSSL for HTTPS (namespace `duckdb_httplib_openssl`, no conflicts with DuckDB core). Removes curl from vcpkg.json, eliminates Windows MSVC `__imp_` linker errors, and reduces or removes `tds_win32_compat.cpp`.

## Technical Context

**Language/Version**: C++17 (C++11-compatible for ODR on Linux)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), cpp-httplib (bundled in DuckDB third_party)
**Storage**: N/A (in-memory token cache, no change)
**Testing**: DuckDB unittest framework (C++ tests, no SQL Server needed for build verification; Azure auth tests need real Azure credentials)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC /MD + MinGW)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: N/A (Azure OAuth2 requests are infrequent, latency dominated by network)
**Constraints**: Must use OpenSSL for HTTPS (not mbedtls). Must compile clean with community-extensions MSVC triplet.
**Scale/Scope**: 2 source files changed, 1 new wrapper file, build config updates

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | No ODBC/JDBC. httplib is open-source (MIT), bundled in DuckDB |
| II. Streaming First | N/A | HTTP requests are for token acquisition, not data streaming |
| III. Correctness over Convenience | PASS | Auth behavior unchanged, error messages preserved |
| IV. Explicit State Machines | N/A | No state machine changes |
| V. DuckDB-Native UX | PASS | Uses DuckDB's own bundled library |
| VI. Incremental Delivery | PASS | Single atomic change, all auth flows tested |

**Gate result**: PASS — no violations.

## Project Structure

### Documentation (this feature)

```text
specs/037-replace-libcurl-httplib/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── spec.md              # Feature specification
└── checklists/
    └── requirements.md  # Spec quality checklist
```

### Source Code (changes)

```text
src/
├── azure/
│   ├── azure_http.cpp           # NEW: httplib wrapper (single compilation unit)
│   ├── azure_token.cpp          # MODIFY: replace curl → HttpPost()
│   └── azure_device_code.cpp    # MODIFY: replace curl → HttpPost()
├── include/
│   └── azure/
│       └── azure_http.hpp       # NEW: HttpPost/HttpResponse/UrlEncode declarations
└── tds/
    └── tds_win32_compat.cpp     # REMOVE or REDUCE (depends on OpenSSL-only build result)

CMakeLists.txt                   # MODIFY: remove curl, add httplib include + CPPHTTPLIB_OPENSSL_SUPPORT
vcpkg.json                       # MODIFY: remove "curl" dependency
```

**Structure Decision**: Minimal changes — new HTTP wrapper in azure/ directory following existing pattern. Single compilation unit for httplib to avoid header bloat.

## Implementation Approach

### Phase 1: Create HTTP wrapper (azure_http)

Create `src/include/azure/azure_http.hpp` and `src/azure/azure_http.cpp`:

**Header** (`azure_http.hpp`):
```cpp
namespace duckdb {
namespace mssql {
namespace azure {

struct HttpResponse {
    int status;
    std::string body;
    std::string error;  // non-empty on network failure
    bool Success() const { return error.empty() && status >= 200 && status < 300; }
};

// POST with form-encoded body (url-encodes params automatically)
HttpResponse HttpPost(const std::string &host, const std::string &path,
                      const std::map<std::string, std::string> &params,
                      int timeout_seconds = 30);

// POST with raw body
HttpResponse HttpPost(const std::string &host, const std::string &path,
                      const std::string &body, const std::string &content_type,
                      int timeout_seconds = 30);

// URL-encode a single value (for manual body construction if needed)
std::string UrlEncode(const std::string &value);

}  // namespace azure
}  // namespace mssql
}  // namespace duckdb
```

**Implementation** (`azure_http.cpp`):
- `#define CPPHTTPLIB_OPENSSL_SUPPORT` before `#include "httplib.hpp"`
- Creates `duckdb_httplib_openssl::SSLClient` for each request
- Maps httplib errors to `HttpResponse.error` string
- Single compilation unit for the entire httplib header

### Phase 2: Migrate azure_token.cpp

Replace curl calls with `HttpPost()`:
- `AcquireTokenForServicePrincipal()`: use `HttpPost(AZURE_AD_BASE_URL, path, params)`
- Remove `WriteCallback`, `UrlEncode(CURL*, ...)` helper functions
- Remove `#include <curl/curl.h>`
- Keep `ParseJsonString()`, `ParseJsonInt()` unchanged (no curl dependency)

### Phase 3: Migrate azure_device_code.cpp

Replace curl calls with `HttpPost()`:
- `HttpPost()` local function → use `azure::HttpPost()`
- `RequestDeviceCode()`: use `HttpPost(host, path, params)`
- `PollForToken()`: use `HttpPost(host, path, params)` in polling loop
- Remove `WriteCallback`, `UrlEncode(CURL*, ...)` helper functions
- Remove `#include <curl/curl.h>`

### Phase 4: Update build configuration

**CMakeLists.txt**:
- Remove `find_package(CURL REQUIRED)` and related status messages
- Remove `CURL::libcurl` from both `target_link_libraries` calls
- Remove MSVC-specific `wldap32`, `normaliz`, `CURL_STATICLIB`
- Keep MSVC-specific `ws2_32`, `crypt32` (needed for OpenSSL)
- Add `duckdb/third_party/httplib` to include directories
- Add `CPPHTTPLIB_OPENSSL_SUPPORT` to compile definitions
- Add `src/azure/azure_http.cpp` to source list
- Remove `src/tds/tds_win32_compat.cpp` from source list (tentative — verify in Phase 5)

**vcpkg.json**:
- Remove `"curl"` from dependencies array

### Phase 5: Test MSVC build and clean up compat wrappers

- Build with community-extensions triplet (`x64-windows-static-md-release-vs2019comp`)
- If clean: delete `tds_win32_compat.cpp` entirely
- If OpenSSL still has `__imp_` symbols: reduce file to only those wrappers
- Verify Linux and macOS builds pass

## Complexity Tracking

No constitution violations — no complexity tracking needed.

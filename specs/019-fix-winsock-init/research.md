# Research: Fix Windows Winsock Initialization

## R1: WSAStartup Best Practices

**Decision**: Use `std::call_once` with a static `std::once_flag` to call `WSAStartup(MAKEWORD(2, 2), &wsaData)` exactly once, and register `WSACleanup` via `atexit()`.

**Rationale**:
- `std::call_once` is the C++11/17 standard mechanism for thread-safe one-time initialization. It handles all concurrency concerns without manual locking.
- `atexit()` ensures `WSACleanup` is called during normal process termination. This is the standard pattern used by many libraries (e.g., libcurl, Python's socket module).
- Placing the initialization call at the top of `TdsSocket::Connect()` ensures it runs before any Winsock API is used, without requiring explicit initialization from the caller.

**Alternatives considered**:
- **Static constructor (global object with constructor/destructor)**: Rejected because static initialization order across translation units is undefined in C++. Could cause issues with other static objects.
- **DLL_PROCESS_ATTACH (DllMain)**: Rejected because the extension may be statically linked, and DllMain has severe restrictions on what can be called.
- **Extension load function**: Rejected because it would require the caller to explicitly initialize, violating the principle of minimal API surface.

## R2: Where to Place the Initialization

**Decision**: Add the initialization function and `EnsureWinsockInitialized()` call at the top of `TdsSocket::Connect()` in `src/tds/tds_socket.cpp`.

**Rationale**:
- `tds_socket.cpp` already contains all Winsock includes and platform-specific defines.
- `Connect()` is the entry point for all socket operations — no Winsock function is called before Connect.
- Keeps the change to a single file with zero impact on the public API.

**Alternatives considered**:
- **Separate initialization file (e.g., `tds_winsock_init.cpp`)**: Rejected as over-engineering for ~15 lines of platform-specific code.
- **`tds_platform.hpp`**: Rejected because it's a header — initialization code belongs in an implementation file.

## R3: Error Handling

**Decision**: If `WSAStartup` fails, set `last_error_` on the TdsSocket and return false from `Connect()`.

**Rationale**:
- Consistent with existing error handling pattern in `Connect()` (e.g., getaddrinfo failure sets `last_error_` and returns false).
- The caller already checks the return value and surfaces the error message.
- WSAStartup failure is extremely rare (only on severely broken Windows installations), so a simple error return is sufficient.

**Alternatives considered**:
- **Throw exception**: Rejected because the existing Connect method uses return-value error reporting, not exceptions.
- **Abort/fatal error**: Rejected as too aggressive for a recoverable condition.

## R4: Cleanup Strategy

**Decision**: Register `WSACleanup` via `atexit()` immediately after successful `WSAStartup`.

**Rationale**:
- Simple and reliable. `atexit` handlers run during normal process termination.
- No need for a custom destructor or reference counting.
- Even if `WSACleanup` is not called (e.g., process killed), Windows handles cleanup automatically.

**Alternatives considered**:
- **Static destructor object (RAII)**: Rejected due to static destruction order issues — socket objects may still be alive when the destructor runs.
- **Manual cleanup in extension unload**: Rejected because DuckDB extension unload lifecycle is not well-defined for cleanup hooks.
- **No cleanup**: Acceptable in practice (Windows cleans up on process exit), but `atexit` is trivial to add and follows best practices.

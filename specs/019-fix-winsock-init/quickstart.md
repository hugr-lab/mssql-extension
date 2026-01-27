# Quickstart: Fix Windows Winsock Initialization

## What Changed

Added one-time Winsock initialization (`WSAStartup`) in `TdsSocket::Connect()` on Windows, guarded by `#ifdef _WIN32` and `std::call_once` for thread safety. Cleanup via `atexit(WSACleanup)`.

## Files Modified

| File | Change |
| ---- | ------ |
| `src/tds/tds_socket.cpp` | Add `EnsureWinsockInitialized()` function and call it at the top of `Connect()` |

## Implementation

In `src/tds/tds_socket.cpp`, add after the existing `#ifdef _WIN32` block (around line 26):

```cpp
#ifdef _WIN32
#include <mutex>

static std::once_flag winsock_init_flag;
static bool winsock_initialized = false;

static void InitializeWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result == 0) {
        winsock_initialized = true;
        atexit([]() { WSACleanup(); });
    }
}
#endif
```

Then at the top of `TdsSocket::Connect()`, before any Winsock call:

```cpp
#ifdef _WIN32
    std::call_once(winsock_init_flag, InitializeWinsock);
    if (!winsock_initialized) {
        last_error_ = "Failed to initialize Windows socket library (WSAStartup failed)";
        return false;
    }
#endif
```

## Verification

1. Build on all platforms: `GEN=ninja make release`
2. Windows CI: `gh workflow run ci.yml --ref 019-fix-winsock-init -f run_windows_build=true`
3. Linux/macOS tests: `make test && make integration-test` (no regression)

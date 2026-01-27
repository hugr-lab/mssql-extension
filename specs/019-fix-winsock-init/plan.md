# Implementation Plan: Fix Windows Winsock Initialization

**Branch**: `019-fix-winsock-init` | **Date**: 2026-01-27 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `/specs/019-fix-winsock-init/spec.md`

## Summary

The mssql extension fails to connect to SQL Server from Windows with the error: `IO Error: MSSQL connection validation failed: Connection failed to 100.100.249.81:1434: Failed to connect to 100.100.249.81:1434`. The same connection string works from Linux. SSMS and PyTDS also connect to the same server and port from Windows without issues.

The root cause is that `WSAStartup()` is never called. On Windows, all Winsock functions (socket, connect, getaddrinfo, send, recv) fail with `WSANOTINITIALISED` unless WSAStartup has been called first. Linux/macOS have no such requirement, which is why the extension works on those platforms.

The fix adds a one-time, thread-safe WSAStartup call in `tds_socket.cpp` using a C++ `std::call_once` pattern, with cleanup via `atexit`. The change is entirely `#ifdef _WIN32` guarded.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (vcpkg), Winsock2 (Windows system library)
**Storage**: N/A
**Testing**: DuckDB sqllogictest framework, C++ unit tests (Catch2)
**Target Platform**: Windows (MSVC, MinGW/Rtools 4.2), Linux (GCC), macOS (Clang)
**Project Type**: Single (DuckDB extension)
**Performance Goals**: N/A (initialization is a one-time call)
**Constraints**: Must be thread-safe, must not affect Linux/macOS
**Scale/Scope**: 1 file modified, ~20 lines added

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
| --------- | ------ | ----- |
| I. Native and Open | PASS | Uses Windows system Winsock2 library (ships with Windows), no external drivers |
| II. Streaming First | PASS | No change to streaming behavior |
| III. Correctness over Convenience | PASS | Fixes a correctness bug (connections fail silently) |
| IV. Explicit State Machines | PASS | No change to connection state machine |
| V. DuckDB-Native UX | PASS | No change to catalog UX |
| VI. Incremental Delivery | PASS | Standalone fix, independently testable |

All gates pass. No violations.

## Project Structure

### Documentation (this feature)

```text
specs/019-fix-winsock-init/
├── spec.md              # Feature specification
├── plan.md              # This file
├── research.md          # Phase 0 output
└── checklists/
    └── requirements.md  # Quality checklist
```

### Source Code (repository root)

```text
src/
└── tds/
    └── tds_socket.cpp   # MODIFY: Add WSAStartup/WSACleanup
```

**Structure Decision**: Single file modification. The WSAStartup initialization is added directly in `tds_socket.cpp` where the Winsock includes and platform-specific defines already exist. No new files needed.

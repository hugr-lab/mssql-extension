# Implementation Plan: TDS Connection, Authentication, and Pooling

**Branch**: `003-tds-connection-pooling` | **Date**: 2026-01-15 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/003-tds-connection-pooling/spec.md`

## Summary

Implement native TDS 7.4 protocol connectivity to SQL Server 2019+ with:
- TCP transport and TDS packet framing
- PRELOGIN/LOGIN7 handshake for SQL Server authentication
- Explicit connection state machine (disconnected, authenticating, idle, executing, cancelling)
- Thread-safe connection pool with configurable limits
- DuckDB settings for pool configuration
- Diagnostic functions (mssql_open, mssql_close, mssql_ping, mssql_pool_stats)

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB main branch (extension API), POSIX sockets (TCP)
**Storage**: In-memory (connection metadata, pool state)
**Testing**: DuckDB SQL tests + integration tests with SQL Server
**Target Platform**: Linux, macOS, Windows (cross-platform sockets)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: <2s connection establishment, 90% reduction in connection overhead via pooling
**Constraints**: No external dependencies (FreeTDS, ODBC), unencrypted connections only in this phase
**Scale/Scope**: Up to 64 concurrent connections per attached database (configurable)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | ✅ PASS | Native TDS implementation, no ODBC/FreeTDS |
| II. Streaming First | ✅ PASS | Connection layer only, no result buffering |
| III. Correctness over Convenience | ✅ PASS | Explicit state machine, fail on errors |
| IV. Explicit State Machines | ✅ PASS | 5 explicit states with documented transitions |
| V. DuckDB-Native UX | ✅ PASS | Standard DuckDB settings and functions |
| VI. Incremental Delivery | ✅ PASS | Connection layer before query execution |

**Post-Design Re-check**: All principles satisfied. Design uses native TDS implementation with explicit state machine and standard DuckDB extension patterns.

## Project Structure

### Documentation (this feature)

```text
specs/003-tds-connection-pooling/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # TDS protocol, DuckDB API, pool patterns
├── data-model.md        # Entities and relationships
├── quickstart.md        # Usage guide
├── contracts/
│   ├── diagnostic-functions.md  # mssql_open/close/ping/pool_stats
│   └── settings.md              # DuckDB configuration variables
├── checklists/
│   └── requirements.md  # Specification quality checklist
└── tasks.md             # (Created by /speckit.tasks)
```

### Source Code (repository root)

```text
src/
├── include/
│   ├── mssql_extension.hpp         # (existing)
│   ├── mssql_secret.hpp            # (existing)
│   ├── mssql_storage.hpp           # (existing)
│   ├── mssql_functions.hpp         # (existing)
│   │
│   ├── tds/                        # Pure TDS headers (no DuckDB deps)
│   │   ├── tds_types.hpp           # Common types, enums, constants
│   │   ├── tds_packet.hpp          # TDS packet framing
│   │   ├── tds_socket.hpp          # TCP socket wrapper
│   │   ├── tds_connection.hpp      # Single connection with state machine
│   │   ├── tds_protocol.hpp        # PRELOGIN, LOGIN7, ping, attention
│   │   └── connection_pool.hpp     # Thread-safe pool (pure C++)
│   │
│   └── connection/                 # Connection management headers
│       ├── mssql_settings.hpp      # DuckDB settings registration
│       ├── mssql_diagnostic.hpp    # Diagnostic scalar/table functions
│       └── mssql_pool_manager.hpp  # Pool lifecycle tied to ATTACH/DETACH
│
├── tds/                            # Pure TDS implementation (no DuckDB deps)
│   ├── tds_types.cpp               # Enum/constant definitions
│   ├── tds_packet.cpp              # Packet construction/parsing
│   ├── tds_socket.cpp              # TCP connect/read/write/close
│   ├── tds_connection.cpp          # State machine, authenticate, ping
│   ├── tds_protocol.cpp            # PRELOGIN, LOGIN7 packet builders
│   └── connection_pool.cpp         # Thread-safe pool implementation
│
├── connection/                     # Connection management (DuckDB integration)
│   ├── mssql_settings.cpp          # AddExtensionOption calls
│   ├── mssql_diagnostic.cpp        # mssql_open/close/ping/pool_stats
│   └── mssql_pool_manager.cpp      # Pool creation/destruction per context
│
├── mssql_extension.cpp             # (modify: register settings, functions)
├── mssql_secret.cpp                # (existing)
├── mssql_storage.cpp               # (modify: integrate pool manager)
└── mssql_functions.cpp             # (existing)

test/
├── sql/
│   └── tds_connection/
│       ├── open_close.test         # Connection lifecycle
│       ├── ping.test               # Health check
│       ├── pool_stats.test         # Statistics
│       └── settings.test           # Configuration variables
└── integration/
    └── sqlserver/
        └── (integration tests requiring SQL Server instance)
```

**Structure Decision**: Clear separation between:
1. **`src/tds/`** - Pure C++17 TDS protocol implementation with no DuckDB dependencies. Can be unit tested independently.
2. **`src/connection/`** - Connection management layer that integrates TDS with DuckDB (settings, diagnostic functions, pool lifecycle).

This separation enables:
- Unit testing TDS code without DuckDB
- Potential reuse of TDS library in other contexts
- Clear dependency boundaries

## Complexity Tracking

No constitution violations requiring justification.

## Key Design Decisions

### 1. TDS Protocol Implementation

- **TDS 7.4 only**: Targeting SQL Server 2019+ (per clarification)
- **Packet framing**: 8-byte header with type, status, length (big-endian), SPID, packet ID, window
- **PRELOGIN**: Negotiate version and encryption (always 0x00 = off)
- **LOGIN7**: SQL Server authentication with XOR-encoded password in UTF-16LE
- **Ping**: Empty SQLBATCH packet for connection health check
- **Attention**: Cancel signal with 0xFF marker

### 2. Connection State Machine

States: `disconnected`, `authenticating`, `idle`, `executing`, `cancelling`

Transitions enforced by `std::atomic<ConnectionState>` with `compare_exchange_strong()`.

### 3. Thread-Safe Pool

- `std::mutex` + `std::condition_variable` for synchronization
- Separate idle queue and active map
- Background cleanup thread (1-second interval)
- Tiered connection validation (quick check + TDS ping for long-idle)

### 4. DuckDB Integration

- Settings via `DBConfig::AddExtensionOption()` with GLOBAL scope
- Diagnostic functions as DuckDB table functions
- Pool integrated with ATTACH mechanism in mssql_storage

## Dependencies on Prior Specs

- **Spec 002 (DuckDB Surface API)**: MssqlSecret for credentials, ATTACH mechanism for context management

## Artifacts Generated

| Artifact | Path | Description |
|----------|------|-------------|
| Research | `research.md` | TDS protocol, DuckDB settings API, pool patterns |
| Data Model | `data-model.md` | Entities: TdsPacket, TdsConnection, ConnectionPool, etc. |
| Quickstart | `quickstart.md` | Usage guide with examples |
| Diagnostic Functions Contract | `contracts/diagnostic-functions.md` | mssql_open/close/ping/pool_stats |
| Settings Contract | `contracts/settings.md` | DuckDB configuration variables |

## Next Steps

Run `/speckit.tasks` to generate implementation tasks based on this plan.

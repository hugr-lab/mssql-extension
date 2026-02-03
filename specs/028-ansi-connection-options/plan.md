# Implementation Plan: ANSI Connection Options Fix

**Branch**: `028-ansi-connection-options` | **Date**: 2026-02-03 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/028-ansi-connection-options/spec.md`

## Summary

Fix TDS connection initialization to set required ANSI session options (`CONCAT_NULL_YIELDS_NULL`, `ANSI_WARNINGS`, `ANSI_NULLS`, `ANSI_PADDING`, `QUOTED_IDENTIFIER`) after successful LOGIN7 authentication. This enables DDL commands (ALTER DATABASE, DBCC, BACKUP LOG) and operations on indexed views/computed columns that require these settings.

## Technical Context

**Language/Version**: C++17 (DuckDB extension standard)
**Primary Dependencies**: DuckDB (main branch), OpenSSL (via vcpkg for TLS)
**Storage**: N/A (remote SQL Server connection)
**Testing**: SQLLogicTest (integration tests require SQL Server container)
**Target Platform**: Linux (GCC), macOS (Clang), Windows (MSVC, MinGW/Rtools 4.2)
**Project Type**: Single project (DuckDB extension)
**Performance Goals**: ANSI initialization overhead < 50ms per new connection
**Constraints**: No changes to LOGIN7 packet flags (post-login SQL_BATCH approach), SQL Server 2016+ compatibility
**Scale/Scope**: Bug fix affecting all DDL command execution paths

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native and Open | PASS | Uses existing TDS protocol implementation, no external dependencies |
| II. Streaming First | PASS | No impact on result streaming; ANSI init is one-time per connection |
| III. Correctness over Convenience | PASS | Fails explicitly if ANSI initialization fails; connection not usable without it |
| IV. Explicit State Machines | PASS | Modifies `DoLogin7()` to include ANSI init before transitioning to Idle |
| V. DuckDB-Native UX | PASS | Transparent fix; no user-facing API changes |
| VI. Incremental Delivery | PASS | Standalone bug fix; independently testable |

**Gate Status**: PASS - No violations to justify.

## Project Structure

### Documentation (this feature)

```text
specs/028-ansi-connection-options/
├── plan.md              # This file
├── research.md          # Phase 0 output (ANSI options research)
├── data-model.md        # Phase 1 output (N/A for this fix - no new entities)
├── quickstart.md        # Phase 1 output (testing guide)
└── tasks.md             # Phase 2 output (/speckit.tasks command)
```

### Source Code (repository root)

```text
src/
├── tds/
│   └── tds_connection.cpp    # Modify DoLogin7() to add ANSI initialization
├── include/
│   └── tds/
│       └── tds_connection.hpp  # Add InitializeAnsiSettings() private method
test/
└── sql/
    └── integration/
        └── ddl_ansi_settings.test  # New integration test for DDL commands
```

**Structure Decision**: Minimal modification - single file change in `tds_connection.cpp` with new private helper method. One new integration test file.

## Complexity Tracking

> No Constitution Check violations - this section is intentionally empty.

---

## Phase 0: Research

### Research Questions

1. **ANSI SET Options Best Practice**: What is the recommended set of ANSI options for SQL Server DDL compatibility?
2. **RESET_CONNECTION Behavior**: When `RESET_CONNECTION` flag is used in TDS, are session options reset? Do we need to re-apply ANSI settings?
3. **Error Handling Patterns**: How should ANSI initialization failures be handled in the existing code patterns?
4. **SQL Server Version Compatibility**: Are all required ANSI options available in SQL Server 2016+?

### Research Findings

#### 1. ANSI SET Options for DDL Compatibility

**Decision**: Use the following SET statements in a single batch:
```sql
SET CONCAT_NULL_YIELDS_NULL ON;
SET ANSI_WARNINGS ON;
SET ANSI_NULLS ON;
SET ANSI_PADDING ON;
SET QUOTED_IDENTIFIER ON;
```

**Rationale**:
- These are the exact options mentioned in SQL Server error 1934
- Required for: indexed views, computed columns, filtered indexes, XML data type methods, and many DDL operations
- All options are available since SQL Server 2000, fully compatible with SQL Server 2016+

**Alternatives Considered**:
- Setting options via LOGIN7 packet flags: Rejected because not all options are available as packet flags (e.g., `CONCAT_NULL_YIELDS_NULL`)
- Setting `ANSI_DEFAULTS ON`: Rejected because it's a connection property, not a SET command, and doesn't include all required options

#### 2. RESET_CONNECTION and Session Options

**Decision**: Session options ARE reset when `RESET_CONNECTION` flag is used. ANSI settings must be re-applied.

**Rationale**:
- `RESET_CONNECTION` resets the session to its default state (as if a fresh login)
- The existing `needs_reset_` flag in `TdsConnection` already tracks when reset is needed
- The fix must ensure ANSI settings are re-applied when `RESET_CONNECTION` is used

**Implementation Approach**:
- After `RESET_CONNECTION` is processed by SQL Server, the connection returns to pool with default settings
- When connection is acquired from pool, if `needs_reset_` was set, ANSI settings should be re-applied
- Alternative: Apply ANSI settings on EVERY first query after acquiring from pool
- Simpler approach: Send ANSI SET in same batch as the first query (avoiding extra round-trip)

**Note**: Current code sets `RESET_CONNECTION` flag on the first SQL_BATCH after reuse. We need to ensure ANSI settings are included in that batch OR sent as a separate batch before the first user query.

#### 3. Error Handling Patterns

**Decision**: Follow existing `DoLogin7()` error handling pattern - fail the entire connection if ANSI initialization fails.

**Rationale**:
- Existing pattern in `DoLogin7()`: on error, set `last_error_`, return false, caller closes socket
- A connection without proper ANSI settings is not usable for DDL commands
- Better to fail fast than allow partial functionality

**Error Cases**:
- SQL Server rejects SET statement (rare, permissions issue)
- Network timeout during ANSI batch execution
- Partial response received (connection corruption)

#### 4. SQL Server Version Compatibility

**Decision**: All required SET options are available in SQL Server 2016+.

**Rationale**:
- `CONCAT_NULL_YIELDS_NULL`: Available since SQL Server 7.0
- `ANSI_WARNINGS`: Available since SQL Server 7.0
- `ANSI_NULLS`: Available since SQL Server 7.0
- `ANSI_PADDING`: Available since SQL Server 6.5
- `QUOTED_IDENTIFIER`: Available since SQL Server 6.0

**Verification**: Constitution specifies SQL Server 2019+ baseline; all options well-established.

---

## Phase 1: Design

### Implementation Approach

The fix will be implemented by modifying `DoLogin7()` to send an ANSI initialization SQL_BATCH immediately after successful authentication, before the connection transitions to Idle state.

#### Code Changes

**File: `src/tds/tds_connection.cpp`**

1. Add new private method `InitializeAnsiSettings()`:
   - Builds SQL_BATCH with ANSI SET statements
   - Sends the batch and waits for response
   - Validates response is successful (DONE token without error)
   - Returns false on any failure

2. Modify `DoLogin7()`:
   - After successful LOGIN7 response parsing
   - Call `InitializeAnsiSettings()`
   - If it fails, return false (connection will be closed by caller)

**File: `src/include/tds/tds_connection.hpp`**

1. Add private method declaration: `bool InitializeAnsiSettings()`

#### RESET_CONNECTION Handling

Current code sets `RESET_CONNECTION` flag on the first SQL_BATCH after pool reuse. Two options:

**Option A: Prepend ANSI SET to user SQL** (Chosen)
- When `needs_reset_` is true, prepend ANSI SET statements to the first user query
- Single round-trip, no extra latency
- Risk: If user query is extremely large, combined SQL might exceed limits

**Option B: Separate batch before user query**
- Send ANSI SET as separate batch before user query
- Extra round-trip overhead
- Cleaner separation of concerns

**Decision**: Use **Option A** for efficiency. The ANSI SET statements are ~150 characters; this is negligible compared to typical SQL size limits.

### Data Model

No new entities or data models required. This is a protocol-level fix.

### API Contracts

No API changes. This is an internal fix transparent to users.

### Test Plan

**New Integration Test: `test/sql/integration/ddl_ansi_settings.test`**

Test cases:
1. `mssql_exec()` with `ALTER DATABASE CURRENT SET RECOVERY SIMPLE` succeeds
2. `mssql_exec()` with `DBCC SHRINKFILE` succeeds
3. Query against indexed view succeeds (if test DB has one)
4. Multiple DDL commands in sequence to verify connection reuse
5. DDL command after connection pool reuse (to test RESET_CONNECTION + ANSI re-init)

**Existing Tests**: Run full test suite to verify no regressions.

---

## Quickstart Guide

See [quickstart.md](./quickstart.md) for testing instructions.

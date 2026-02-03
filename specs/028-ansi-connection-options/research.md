# Research: ANSI Connection Options Fix

**Feature**: 028-ansi-connection-options
**Date**: 2026-02-03

## Research Findings

### 1. SQL Server ANSI Options Required

**Decision**: Enable 5 specific ANSI SET options on every connection

**Rationale**: These options are required by SQL Server for:
- Indexed views
- Computed column indexes
- Filtered indexes
- Query notifications
- XML data type methods
- Spatial index operations

**SET Options Required**:
| Option | Default | Required |
|--------|---------|----------|
| `CONCAT_NULL_YIELDS_NULL` | OFF | ON |
| `ANSI_WARNINGS` | OFF | ON |
| `ANSI_NULLS` | OFF | ON |
| `ANSI_PADDING` | ON | ON |
| `QUOTED_IDENTIFIER` | OFF | ON |

**Source**: SQL Server documentation, Error 1934 message text

**Alternatives Considered**:
1. **LOGIN7 Option Flags** - TDS LOGIN7 packet has option flags but they don't map to these SET options. ODBC flag (OptionFlags2 bit 0) only affects ODBC-specific behaviors.
2. **Per-query SET statements** - Would require prepending to every query, higher overhead.
3. **User-configurable settings** - Over-engineering for a bug fix; can be added later if needed.

---

### 2. Implementation Location

**Decision**: Initialize in `DoLogin7()` after successful authentication, before state transitions to Idle

**Rationale**:
- Connection is authenticated but not yet available for queries
- Failure handling is straightforward (don't add to pool)
- All new connections automatically get settings
- Pattern matches BEGIN TRANSACTION initialization in ConnectionProvider

**Alternatives Considered**:
1. **Pool factory lambda** - Would require modifying pool manager, more invasive
2. **First ExecuteBatch call** - Adds latency to first query, complex state tracking
3. **Connection provider GetConnection** - Too late, connection already in pool

---

### 3. RESET_CONNECTION Handling

**Decision**: Prepend ANSI SET statements to SQL when RESET_CONNECTION flag is set

**Rationale**:
- RESET_CONNECTION clears session state (temp tables, variables, SET options)
- Prepending ensures settings are re-applied atomically with the query
- No additional round-trip required

**Alternatives Considered**:
1. **Separate initialization call after reset** - Extra round-trip, complex state management
2. **Don't use RESET_CONNECTION** - Would break session cleanup expectations
3. **Flag to skip RESET_CONNECTION for DDL** - Complex, prone to state leaks

---

### 4. Error Handling Strategy

**Decision**: Fail connection establishment if ANSI init fails

**Rationale**:
- A connection that can't execute DDL is incomplete
- User gets immediate feedback vs. confusing errors later
- Matches existing authentication failure behavior

**Alternatives Considered**:
1. **Warn and continue** - Leads to confusing errors when DDL fails
2. **Lazy init on first DDL** - Complex, doesn't help indexed view queries

---

### 5. @@OPTIONS Bitmask Verification

**Decision**: Test using `@@OPTIONS & 0x00005000` (bits 12, 14 for CONCAT_NULL_YIELDS_NULL, ANSI_WARNINGS)

**Rationale**:
- @@OPTIONS returns a bitmask of current session options
- Allows verification without executing actual DDL
- Portable across SQL Server versions

**Bit Positions** (from SQL Server docs):
| Bit | Value | Option |
|-----|-------|--------|
| 2 | 4 | IMPLICIT_TRANSACTIONS |
| 3 | 8 | CURSOR_CLOSE_ON_COMMIT |
| 4 | 16 | ANSI_WARNINGS |
| 5 | 32 | ANSI_PADDING |
| 6 | 64 | ANSI_NULLS |
| 7 | 128 | ARITHABORT |
| 8 | 256 | ARITHIGNORE |
| 9 | 512 | QUOTED_IDENTIFIER |
| 10 | 1024 | NOCOUNT |
| 11 | 2048 | ANSI_NULL_DFLT_ON |
| 12 | 4096 | ANSI_NULL_DFLT_OFF |
| 13 | 8192 | CONCAT_NULL_YIELDS_NULL |
| 14 | 16384 | NUMERIC_ROUNDABORT |
| 15 | 32768 | XACT_ABORT |

**Expected bitmask after init**: At minimum bits 4, 5, 6, 9, 13 should be set.

---

## Implementation Checklist

- [ ] Add `InitializeAnsiSettings()` method to TdsConnection
- [ ] Call after successful LOGIN7 in `DoLogin7()`
- [ ] Modify `ExecuteBatch()` to prepend ANSI settings when `needs_reset_`
- [ ] Add integration test for ANSI settings verification
- [ ] Add integration test for DDL command execution
- [ ] Update docs/architecture.md with ANSI initialization details
- [ ] Update README with ANSI-compliant connection behavior note

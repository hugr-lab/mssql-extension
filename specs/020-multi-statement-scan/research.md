# Research: Support Multi-Statement SQL in mssql_scan

**Date**: 2026-01-27
**Feature**: [spec.md](spec.md)

## Research Decision 1: Token Loop Fix Strategy

**Decision**: Check `DoneToken::IsFinal()` on DONE tokens before COLMETADATA; if not final, continue the token processing loop instead of transitioning to Complete state.

**Rationale**: The `DoneToken` struct already has `IsFinal()` which checks the `DONE_MORE` flag (bit 0x01). SQL Server sets `DONE_MORE` on all DONE tokens except the last one in a batch. The current code in `Initialize()` (line ~142-152 of `mssql_result_stream.cpp`) unconditionally transitions to `Complete` on any DONE token. The fix is minimal: add an `IsFinal()` check and only complete on final DONE.

**Alternatives considered**:
- Parse SQL to detect multi-statement batches before execution — rejected: fragile, complex, unnecessary since TDS protocol already signals this via DONE_MORE
- Buffer all results and pick the first result set — rejected: violates Streaming First principle

## Research Decision 2: Accessing DoneToken in Initialize()

**Decision**: Use `parser_.GetDone()` to retrieve the `DoneToken` struct and check its `IsFinal()` method.

**Rationale**: The token parser already exposes `GetDone()` which returns the most recently parsed `DoneToken`. The `DoneToken::IsFinal()` method checks `(status & DONE_MORE) == 0`. This is a simple accessor call, no new infrastructure needed.

**Alternatives considered**:
- Track DONE_MORE state externally — rejected: redundant, the parser already tracks this
- Check for `ParsedTokenType::DoneInProc` vs `ParsedTokenType::Done` separately — could be useful but IsFinal() is the more robust approach since it checks the actual protocol flags

## Research Decision 3: Connection Reset Mechanism

**Decision**: Execute `exec sp_reset_connection` via `ExecuteBatch()` + drain response before returning the connection to the pool. Only in autocommit mode — never for pinned transaction connections.

**Rationale**: `sp_reset_connection` is the standard SQL Server mechanism for resetting session state. It:
- Drops all temp tables (#tables)
- Resets all SET options to defaults
- Clears session variables
- Releases locks
- Closes open cursors

ADO.NET, JDBC, and ODBC drivers all use this. It adds ~1ms per connection return, which is negligible compared to connection creation time.

**Alternatives considered**:
- Drop specific temp tables by name — rejected: would require tracking what was created, fragile
- Close and recreate the connection — rejected: too expensive (full TCP + TLS + auth handshake)
- Use `DBCC FREEPROCCACHE` — rejected: too aggressive, affects server-wide plan cache
- Reset only on connections that executed multi-statement batches — rejected: any query can change SET options or create temp tables; blanket reset is safer and simpler

## Research Decision 4: Reset Placement in Code

**Decision**: Two reset points:
1. **Autocommit**: In `ConnectionProvider::ReleaseConnection()` before calling `pool.Release()`
2. **After transaction**: In `MSSQLTransactionManager::CommitTransaction()` and `RollbackTransaction()` before calling `pool.Release()`

**Rationale**: These are the two paths where connections transition from "in use" to "available in pool". The reset must happen before the connection is visible to other pool consumers. Pinned connections in a transaction must preserve session state (temp tables, variables) for the duration of the transaction.

**Alternatives considered**:
- Reset in `ConnectionPool::Release()` — rejected: pool layer is transport-agnostic, doesn't know about SQL Server session semantics
- Reset in `ConnectionPool::Acquire()` — rejected: would add latency to every connection acquisition, including cases where the connection is already clean
- Reset in `MSSQLResultStream` destructor — rejected: not all connection usage goes through result streams (e.g., DML operations)

## Research Decision 5: Error Handling in Initialize() for Multi-Statement

**Decision**: When a non-final DONE token is received that has accumulated errors (from ERROR tokens), throw the error immediately — do not continue looking for COLMETADATA from subsequent statements.

**Rationale**: If an intermediate statement fails, the user needs to know. SQL Server may continue processing subsequent statements in the batch, but the results may be incorrect (e.g., a temp table wasn't created because the SELECT INTO failed, so the subsequent SELECT would fail too). Failing fast with the first error is the safest behavior.

**Alternatives considered**:
- Continue past errors and try to find a result set — rejected: would mask errors and potentially return incorrect data
- Collect all errors and report them together — could be a future enhancement, but for now first-error-wins is simpler and safer

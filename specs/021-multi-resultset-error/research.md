# Research: Multi-Resultset Error Detection in FillChunk

## Root Cause Analysis

### The Crash Path

When a multi-statement SQL batch produces two or more result sets with different schemas, the following sequence occurs:

1. `Initialize()` reads the first `COLMETADATA` token, stores column metadata, and transitions to `Streaming` state
2. `FillChunk()` processes rows from the first result set
3. The first result set ends with a non-final `DONE` token (has `DONE_MORE` flag)
4. The TDS parser encounters the second `COLMETADATA` token and **automatically updates** its internal column metadata
5. `FillChunk()`'s switch statement falls through to `default: // Skip other tokens` for the `ColMetadata` case
6. Subsequent `Row` tokens are parsed using the **new** column schema (from the parser's updated metadata)
7. `ProcessRow()` calls `TypeConverter::ConvertValue()` using the **old** `column_metadata_` (from step 1) to write into DuckDB vectors that were initialized for the original schema
8. Type mismatch causes DuckDB internal assertion: `Expected vector of type INT16, but found vector of type INT32`

### Key Code Locations

**`FillChunk()` switch statement** (`src/query/mssql_result_stream.cpp:213-300`):
- Handles: `Row`, `Done`, `Error`, `Info`, `NeedMoreData`, `None`
- **Missing**: `ColMetadata` — falls through to `default` (line 297-299)

**`ProcessRow()`** (`src/query/mssql_result_stream.cpp:316-361`):
- Uses `column_metadata_` (set once in `Initialize()`) to determine types
- Calls `TypeConverter::ConvertValue()` which writes to DuckDB vectors
- Crash occurs here when types don't match

**Parser auto-update behavior**:
- `TdsTokenParser::TryParseNext()` returns `ParsedTokenType::ColMetadata` when it sees a new COLMETADATA token
- The parser internally updates its `columns_` vector with the new metadata
- This is by design — the parser doesn't know whether the caller wants to handle multiple result sets

### Why `DrainAfterCancel()` Can't Be Reused Directly

`DrainAfterCancel()` sends an `ATTENTION` signal and waits for an `ATTN` acknowledgment DONE token. For the multi-resultset error case, we don't need ATTENTION — we just need to read and discard remaining tokens until the final DONE. The approach is simpler:

1. Set skip mode on the parser (`parser_.SetSkipMode(true)`) to skip ROW token parsing
2. Continue reading tokens until a final DONE is received
3. Transition connection to Idle

## Fix Design

### Approach: Add `ColMetadata` Case in FillChunk

Add a `case tds::ParsedTokenType::ColMetadata` handler in `FillChunk()`'s switch statement that:

1. Sets `state_` to `Error`
2. Drains remaining TDS tokens (to leave connection clean)
3. Transitions connection to `Idle`
4. Throws `InvalidInputException` with a clear error message

### Token Draining Strategy

After detecting the second result set, we need to drain all remaining TDS response data:

1. Enable `parser_.SetSkipMode(true)` — skips ROW token value parsing for speed
2. Loop: parse tokens, read more data when needed
3. Stop when a final DONE token is received (or timeout)
4. Transition connection from `Executing` to `Idle`

This is similar to `DrainAfterCancel()` but without the ATTENTION signal.

### Error Message Design

The error message should:
- Clearly state what happened: the batch produced multiple result sets
- Provide actionable guidance: only one result-producing statement per batch
- Suggest the fix: split into separate `mssql_scan()` calls

Proposed message:
```
MSSQL Error: The SQL batch produced multiple result sets.
Only one result-producing statement is allowed per mssql_scan() call.
Ensure your batch contains only one SELECT or other result-producing statement.
Use separate mssql_scan() calls for multiple result sets.
```

## Alternatives Considered

### Alternative 1: Detect in Initialize()
Could track whether we've already seen COLMETADATA in `Initialize()` and error if a second one appears. But this wouldn't catch the case where the second result set arrives during `FillChunk()` streaming — which is the actual crash scenario.

### Alternative 2: Support Multiple Result Sets
Could append rows from subsequent result sets if schemas match. Rejected per spec clarification: the user chose to always error on any second result set, regardless of schema match.

### Alternative 3: Use ATTENTION to Cancel
Could send ATTENTION signal like `DrainAfterCancel()`. Unnecessary complexity — we can simply drain the remaining response without cancellation since all the data is already being sent by SQL Server.

# Data Model: Table Scan and Filter Pushdown

**Feature**: 013-table-scan-filter-refactor
**Date**: 2026-01-20

## Overview

This document defines the internal data structures for the refactored table scan module and filter encoder. These are C++ structures, not database entities.

**Naming Convention**: Types in `duckdb::mssql` namespace do NOT use MSSQL prefix. Types directly in `duckdb` namespace MUST use MSSQL prefix.

---

## 1. Filter Encoder Entities

### FilterEncoderResult

Result returned by the filter encoder after processing a filter expression.

| Field | Type | Description |
|-------|------|-------------|
| `where_clause` | string | Generated T-SQL WHERE clause fragment (empty if nothing pushable) |
| `needs_duckdb_filter` | bool | True if DuckDB must re-apply all original filters |

**Invariants**:
- If `where_clause` is empty, `needs_duckdb_filter` MUST be true
- If any expression was not fully encoded, `needs_duckdb_filter` MUST be true

### FunctionMapping

Mapping from DuckDB function names to SQL Server equivalents.

| Field | Type | Description |
|-------|------|-------------|
| `duckdb_name` | string | DuckDB function name (lowercase) |
| `sql_server_template` | string | T-SQL template with `{0}`, `{1}` placeholders for arguments |
| `arg_count` | int | Expected number of arguments (-1 for variadic) |

**Examples**:
- `{"lower", "LOWER({0})", 1}`
- `{"trim", "LTRIM(RTRIM({0}))", 1}`
- `{"prefix", "{0} LIKE {1} + N'%'", 2}`
- `{"year", "YEAR({0})", 1}`
- `{"date_diff", "DATEDIFF({0}, {1}, {2})", 3}`
- `{"date_add", "DATEADD({1}, {2}, {0})", 3}` (note: parameter reorder)

### ExpressionEncodeContext

Context passed through recursive expression encoding.

| Field | Type | Description |
|-------|------|-------------|
| `column_ids` | vector&lt;idx_t&gt; | Projection mapping: projected index → table column index |
| `column_names` | vector&lt;string&gt; | All table column names |
| `in_or_branch` | bool | True if currently inside an OR expression |

---

## 2. Table Scan Entities (duckdb::mssql namespace)

### TableScanBindData

Bind-time data structure containing table metadata. Refactored from existing `MSSQLCatalogScanBindData`.

| Field | Type | Description |
|-------|------|-------------|
| `context_name` | string | MSSQL connection context name |
| `schema_name` | string | SQL Server schema name |
| `table_name` | string | SQL Server table name |
| `all_types` | vector&lt;LogicalType&gt; | All table column types |
| `all_column_names` | vector&lt;string&gt; | All table column names |
| `return_types` | vector&lt;LogicalType&gt; | Projected column types |
| `column_names` | vector&lt;string&gt; | Projected column names |
| `result_stream_id` | uint64_t | Registry ID for pre-executed result stream |

**Lifecycle**: Created in bind phase, passed to init and execute phases.

### TableScanGlobalState

Global execution state for table scan.

| Field | Type | Description |
|-------|------|-------------|
| `result_stream` | unique_ptr&lt;MSSQLResultStream&gt; | Active result stream from SQL Server |
| `context_name` | string | Connection context name |
| `projected_column_count` | idx_t | Number of columns to fill in output |
| `done` | bool | True when scan is complete |
| `timing_started` | bool | Whether timing has been started |
| `scan_start` | time_point | Start time for performance logging |
| `filter_pushdown_applied` | bool | True if any filters were pushed to SQL Server |
| `needs_duckdb_filter` | bool | True if DuckDB must re-apply filters |

**Lifecycle**: Created in InitGlobal, used throughout execution, destroyed when scan completes.

### TableScanLocalState

Per-thread local state (currently minimal due to single-threaded execution).

| Field | Type | Description |
|-------|------|-------------|
| `current_chunk` | idx_t | Current chunk index for progress tracking |

---

## 3. Expression Type Enumeration

### SupportedExpressionType

Internal classification of expression support level.

| Value | Description |
|-------|-------------|
| `FULLY_SUPPORTED` | Expression can be completely pushed to SQL Server |
| `PARTIALLY_SUPPORTED` | Some sub-expressions cannot be pushed (AND only) |
| `NOT_SUPPORTED` | Expression cannot be pushed at all |

**Usage**: Returned by expression encoder to guide OR/AND handling.

---

## 4. Relationships

```
TableScanBindData (duckdb::mssql)
    │
    ├──► ExpressionEncodeContext (created from bind data for encoding)
    │         │
    │         └──► FilterEncoderResult (output of filter encoding)
    │
    └──► TableScanGlobalState (uses bind data for query generation)
              │
              └──► MSSQLResultStream (duckdb namespace - external type)
```

---

## 5. State Transitions

### Filter Encoding State Machine

```
┌─────────────┐
│   START     │
└──────┬──────┘
       │
       ▼
┌─────────────────────┐
│ Process TableFilter │
└──────┬──────────────┘
       │
       ├─── CONSTANT_COMPARISON ──► Encode directly ──► ENCODED
       ├─── IS_NULL/IS_NOT_NULL ──► Encode directly ──► ENCODED
       ├─── IN_FILTER ────────────► Encode directly ──► ENCODED
       ├─── CONJUNCTION_AND ──────► Recurse children ──► Partial OK ──► ENCODED
       ├─── CONJUNCTION_OR ───────► Recurse children ──► All or none ──► ENCODED/SKIP
       ├─── EXPRESSION_FILTER ────► Parse Expression tree ──► ENCODED/SKIP
       └─── Others ───────────────► SKIP

┌──────────┐                    ┌──────────┐
│ ENCODED  │                    │   SKIP   │
│ (sql!=∅) │                    │ (sql=∅)  │
└────┬─────┘                    └────┬─────┘
     │                               │
     ▼                               ▼
┌────────────────────────────────────────────┐
│            Combine with AND                │
│  needs_duckdb_filter = any_skipped         │
└────────────────────────────────────────────┘
```

### Table Scan Lifecycle

```
BIND ──► Parse table metadata, store in BindData
  │
  ▼
INIT_GLOBAL ──► Build query with filter pushdown
  │              ├── Encode filters → WHERE clause
  │              ├── Build SELECT with projections
  │              └── Execute query, get ResultStream
  │
  ▼
EXECUTE (loop) ──► FillChunk from ResultStream
  │                 ├── Apply DuckDB filters if needs_duckdb_filter
  │                 └── Return chunk to DuckDB
  │
  ▼
DONE ──► Cleanup ResultStream, return connection to pool
```

---

## 6. Data Validation Rules

### FilterEncoderResult Validation

- `where_clause` MUST be valid T-SQL syntax (no SQL injection)
- All string literals MUST use N'' prefix for Unicode safety
- All identifiers MUST be bracket-escaped
- Parentheses MUST be balanced

### Column Reference Validation

- Column index MUST be within bounds of `column_ids`
- Resolved table column index MUST be within bounds of `column_names`
- Column names MUST be escaped before use in T-SQL

### Expression Depth Limit

- Maximum expression nesting depth: 100 levels
- Prevents stack overflow on pathological expressions
- Exceeding limit → mark expression as NOT_SUPPORTED

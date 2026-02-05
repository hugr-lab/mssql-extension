# Data Model: CTAS Fixes - IF NOT EXISTS and Auto-TABLOCK

## Overview

This document describes the data structures involved in implementing IF NOT EXISTS handling and auto-TABLOCK optimization for CTAS and COPY operations.

## Entity: CTASTarget (Modified)

**Location**: `src/include/dml/ctas/mssql_ctas_types.hpp`

**Purpose**: Represents the target table information for CREATE TABLE AS SELECT operations.

### Current Structure

```cpp
struct CTASTarget {
    string catalog_name;    // Attached database name (e.g., "mssql")
    string schema_name;     // SQL Server schema (e.g., "dbo")
    string table_name;      // Table name (e.g., "new_orders")
    bool or_replace;        // CREATE OR REPLACE TABLE mode
    OnCreateConflict on_conflict;  // ON CONFLICT behavior from DuckDB
};
```

### Modified Structure

```cpp
struct CTASTarget {
    string catalog_name;
    string schema_name;
    string table_name;
    bool or_replace;
    bool if_not_exists;     // NEW: CREATE TABLE IF NOT EXISTS mode
    OnCreateConflict on_conflict;
};
```

### State Transitions

```
on_conflict value          → or_replace  → if_not_exists
─────────────────────────────────────────────────────────
ERROR_ON_CONFLICT           false         false
IGNORE_ON_CONFLICT          false         true      ← NEW
REPLACE_ON_CONFLICT         true          false
```

## Entity: CTASConfig (Modified)

**Location**: `src/include/dml/ctas/mssql_ctas_config.hpp`

**Purpose**: Configuration for CTAS execution including BCP settings.

### Current Structure

```cpp
struct CTASConfig {
    CTASTextType text_type;
    bool drop_on_failure;
    idx_t batch_size;
    idx_t max_rows_per_statement;
    idx_t max_sql_bytes;
    bool use_bcp;
    idx_t bcp_flush_rows;
    bool bcp_tablock;       // From mssql_copy_tablock setting
};
```

### Modified Structure

```cpp
struct CTASConfig {
    // ... existing fields ...
    bool bcp_tablock;           // User setting from mssql_copy_tablock
    bool bcp_tablock_explicit;  // NEW: true if user explicitly set mssql_copy_tablock
    bool is_new_table;          // NEW: true if creating a brand-new table
};
```

### TABLOCK Decision Logic

```
                    ┌─────────────────────────────┐
                    │ bcp_tablock_explicit?       │
                    └─────────────┬───────────────┘
                                  │
              ┌───────────────────┴───────────────────┐
              │ YES                                   │ NO
              ▼                                       ▼
    ┌─────────────────┐                    ┌─────────────────┐
    │ use bcp_tablock │                    │ is_new_table?   │
    │ (user setting)  │                    └────────┬────────┘
    └─────────────────┘                             │
                                     ┌──────────────┴──────────────┐
                                     │ YES                         │ NO
                                     ▼                             ▼
                            ┌─────────────────┐          ┌─────────────────┐
                            │ TABLOCK = true  │          │ TABLOCK = false │
                            │ (auto-enabled)  │          │ (existing table)│
                            └─────────────────┘          └─────────────────┘
```

## Entity: BCPCopyConfig (Modified)

**Location**: `src/include/copy/bcp_config.hpp`

**Purpose**: Configuration for COPY TO operations using BCP protocol.

### Current Structure

```cpp
struct BCPCopyConfig {
    idx_t flush_rows;
    bool tablock;
    bool create_table;
    bool overwrite;
};
```

### Modified Structure

```cpp
struct BCPCopyConfig {
    idx_t flush_rows;
    bool tablock;              // User setting
    bool tablock_explicit;     // NEW: true if user explicitly set tablock
    bool create_table;
    bool overwrite;
    bool is_new_table;         // NEW: set during ValidateTarget when table is created
};
```

## Entity: CTASExecutorState (Modified)

**Location**: `src/dml/ctas/mssql_ctas_executor.hpp`

**Purpose**: Execution state for CTAS operations.

### New Field

```cpp
struct CTASExecutorState {
    // ... existing fields ...
    bool skipped;              // NEW: true if IF NOT EXISTS skipped creation
};
```

### State Machine Update

```
                    ┌────────────────────┐
                    │ CTAS Start         │
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │ TableExists()?     │
                    └─────────┬──────────┘
                              │
          ┌───────────────────┼───────────────────┐
          │ NO                │ YES               │
          │                   │                   │
          ▼                   │     ┌─────────────▼─────────────┐
   ┌──────────────┐           │     │ if_not_exists?            │
   │ CreateTable  │           │     └─────────────┬─────────────┘
   │ is_new=true  │           │                   │
   └──────┬───────┘           │     ┌─────────────┴─────────────┐
          │                   │     │ YES               │ NO    │
          │                   │     ▼                   ▼       │
          │                   │  ┌─────────┐      ┌──────────┐  │
          │                   │  │ SKIP    │      │ or_replace?│ │
          │                   │  │ skipped │      └─────┬─────┘  │
          │                   │  │ = true  │            │        │
          │                   │  │ return 0│      ┌─────┴─────┐  │
          │                   │  └─────────┘      │YES    │NO │  │
          │                   │                   ▼       ▼   │  │
          │                   │            ┌──────────┐ ┌─────┴──┴─┐
          │                   │            │ DropTable│ │ ERROR    │
          │                   │            │ CreateNew│ │ "already │
          │                   │            │ is_new=T │ │  exists" │
          │                   │            └────┬─────┘ └──────────┘
          │                   │                 │
          └───────────────────┴─────────────────┘
                              │
                    ┌─────────▼──────────┐
                    │ BCP Data Transfer  │
                    │ TABLOCK if is_new  │
                    └─────────┬──────────┘
                              │
                    ┌─────────▼──────────┐
                    │ COMPLETE           │
                    └────────────────────┘
```

## Validation Rules

### IF NOT EXISTS

- `if_not_exists` can only be true when `on_conflict == IGNORE_ON_CONFLICT`
- `if_not_exists` and `or_replace` are mutually exclusive
- When `if_not_exists` is true and table exists:
  - No DDL is executed
  - No data is inserted
  - Returns 0 rows affected
  - No error is raised

### Auto-TABLOCK

- `is_new_table` is set to true only when:
  - Table did not exist before CTAS/COPY started, OR
  - Table was dropped due to `or_replace` or `overwrite`
- User explicit `tablock` setting always takes precedence over auto-detection
- `tablock_explicit` is determined by checking if the setting was explicitly set in the session

## Relationships

```
CTASTarget
    │
    ├── on_conflict ──────────► Determines if_not_exists flag
    │
    └── is_new_table ─────────► Flows to CTASConfig

CTASConfig
    │
    ├── bcp_tablock ──────────► User setting (can be overridden)
    │
    ├── bcp_tablock_explicit ─► Tracks if user explicitly set
    │
    └── is_new_table ─────────► Auto-TABLOCK decision

BCPCopyConfig
    │
    ├── tablock ──────────────► User setting
    │
    ├── tablock_explicit ─────► Tracks if user explicitly set
    │
    └── is_new_table ─────────► Auto-TABLOCK decision (from ValidateTarget)
```

# Contract: CTAS Behavior with IF NOT EXISTS and Auto-TABLOCK

## Overview

This contract defines the expected behavior for CREATE TABLE AS SELECT operations with IF NOT EXISTS clause and automatic TABLOCK optimization.

## Contract 1: IF NOT EXISTS Behavior

### Preconditions
- User issues `CREATE TABLE IF NOT EXISTS <target> AS SELECT ...`
- Target catalog is an attached MSSQL database
- User has appropriate permissions

### Postconditions (Table Does Not Exist)
- Table is created with schema derived from SELECT query
- Data from SELECT query is inserted into the new table
- Returns count of rows inserted
- Catalog cache is invalidated

### Postconditions (Table Already Exists)
- **No error is raised**
- Table is not modified
- No data is inserted
- Returns 0 rows affected
- Catalog cache is not invalidated

### Error Conditions
- Schema does not exist → Error with message
- Permission denied → Error with message
- Type mapping failure → Error with column name

### SQL Examples

```sql
-- First execution: creates table with 1000 rows
CREATE TABLE IF NOT EXISTS db.dbo.my_table AS
SELECT * FROM generate_series(1, 1000) t(id);
-- Returns: 1000

-- Second execution: table exists, no error
CREATE TABLE IF NOT EXISTS db.dbo.my_table AS
SELECT * FROM generate_series(1, 1000) t(id);
-- Returns: 0

-- Verify table unchanged
SELECT COUNT(*) FROM db.dbo.my_table;
-- Returns: 1000 (not 2000)
```

## Contract 2: OR REPLACE vs IF NOT EXISTS

### Mutual Exclusivity
- `CREATE OR REPLACE TABLE IF NOT EXISTS` is not a valid combination
- DuckDB parser enforces this at syntax level

### Behavior Matrix

| Clause | Table Exists | Behavior |
|--------|--------------|----------|
| (none) | Yes | Error: "table already exists" |
| (none) | No | Create table |
| IF NOT EXISTS | Yes | Success, return 0, table unchanged |
| IF NOT EXISTS | No | Create table |
| OR REPLACE | Yes | Drop table, create new |
| OR REPLACE | No | Create table |

## Contract 3: Auto-TABLOCK for New Tables

### Trigger Conditions
- BCP protocol is being used (`use_bcp = true`)
- A new table is being created:
  - Table did not exist before operation, OR
  - Table was dropped due to OR REPLACE
- User has NOT explicitly set `mssql_copy_tablock = false`

### Non-Trigger Conditions
- Inserting into existing table (append mode)
- User explicitly set `mssql_copy_tablock = false`
- IF NOT EXISTS with table already existing (no insert occurs)
- INSERT mode (non-BCP)

### Expected Behavior

```sql
-- Scenario 1: New table → TABLOCK auto-enabled
CREATE TABLE db.dbo.new_table AS SELECT * FROM large_source;
-- Generated: INSERT BULK [dbo].[new_table] (...) WITH (TABLOCK)

-- Scenario 2: User disabled → No TABLOCK
SET mssql_copy_tablock = false;
CREATE TABLE db.dbo.new_table2 AS SELECT * FROM large_source;
-- Generated: INSERT BULK [dbo].[new_table2] (...)

-- Scenario 3: Existing table (IF NOT EXISTS, table exists) → No insert
CREATE TABLE IF NOT EXISTS db.dbo.new_table AS SELECT * FROM large_source;
-- No INSERT BULK generated (table exists, skipped)

-- Scenario 4: OR REPLACE → TABLOCK auto-enabled
CREATE OR REPLACE TABLE db.dbo.new_table AS SELECT * FROM large_source;
-- Table dropped first
-- Generated: INSERT BULK [dbo].[new_table] (...) WITH (TABLOCK)
```

## Contract 4: COPY TO Auto-TABLOCK

### Trigger Conditions
- `CREATE_TABLE = true` option is used
- Table does not exist (or REPLACE = true)
- User has NOT explicitly set `tablock = false`

### SQL Examples

```sql
-- Scenario 1: New table via COPY → TABLOCK auto-enabled
COPY (SELECT * FROM source) TO 'mssql://db/dbo/new_table' (CREATE_TABLE true);
-- Generated: INSERT BULK [dbo].[new_table] (...) WITH (TABLOCK)

-- Scenario 2: Existing table → No auto-TABLOCK
COPY (SELECT * FROM source) TO 'mssql://db/dbo/existing_table';
-- Generated: INSERT BULK [dbo].[existing_table] (...)

-- Scenario 3: User explicit TABLOCK on existing table
COPY (SELECT * FROM source) TO 'mssql://db/dbo/existing_table' (TABLOCK true);
-- Generated: INSERT BULK [dbo].[existing_table] (...) WITH (TABLOCK)
```

## Contract 5: Return Values

### CTAS Return Value

| Scenario | Return Value |
|----------|--------------|
| New table created | Count of rows inserted |
| IF NOT EXISTS, table exists | 0 |
| OR REPLACE | Count of rows inserted |
| Error | Exception thrown |

### COPY TO Return Value

| Scenario | Return Value |
|----------|--------------|
| Rows inserted | Count of rows |
| Error | Exception thrown |

## Contract 6: Catalog Cache Invalidation

### When to Invalidate
- After successful table creation (new table)
- After successful DROP + CREATE (OR REPLACE)

### When NOT to Invalidate
- IF NOT EXISTS when table already exists (no changes made)
- On error (partial state should not be cached)

## Verification Checklist

- [ ] `CREATE TABLE IF NOT EXISTS` succeeds silently when table exists
- [ ] `CREATE TABLE IF NOT EXISTS` creates table when it doesn't exist
- [ ] `CREATE TABLE IF NOT EXISTS` returns 0 when table exists
- [ ] Auto-TABLOCK enabled for new tables in CTAS
- [ ] Auto-TABLOCK enabled for new tables in COPY TO
- [ ] User `mssql_copy_tablock = false` disables auto-TABLOCK
- [ ] OR REPLACE triggers auto-TABLOCK (new table after drop)
- [ ] Existing table append does not trigger auto-TABLOCK
- [ ] All existing tests pass (backward compatibility)

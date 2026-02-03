# Quickstart: CTAS BCP Integration

**Date**: 2026-02-02
**Feature**: 027-ctas-bcp-integration

## Problem

CTAS (CREATE TABLE AS SELECT) uses batched INSERT statements for data transfer, which is slower than the TDS BulkLoadBCP protocol used by COPY TO. Additionally, the `mssql_copy_tablock` default of `true` can cause unexpected blocking in multi-user environments.

## Solution

1. Add `mssql_ctas_use_bcp` setting (default: `true`) to enable BCP protocol for CTAS
2. Change `mssql_copy_tablock` default from `true` to `false` for safer defaults
3. Update documentation to reflect new behavior and settings

## What Changes

### For Users

**CTAS is now faster by default**. No code changes needed:

```sql
-- This now uses BCP protocol automatically (2-10x faster)
CREATE TABLE mssql.dbo.products_copy AS
SELECT * FROM local_products;
```

**TABLOCK is now opt-in** for both COPY TO and CTAS:

```sql
-- Old behavior (TABLOCK enabled)
SET mssql_copy_tablock = true;

-- New default (TABLOCK disabled, safer for concurrent access)
COPY local_data TO 'mssql://db/dbo/target' (FORMAT 'bcp');
```

### New Setting

```sql
-- Use BCP for CTAS (default: true)
SET mssql_ctas_use_bcp = true;

-- Disable BCP to use legacy INSERT batching
SET mssql_ctas_use_bcp = false;
```

### Changed Default

```sql
-- TABLOCK is now false by default
-- Enable explicitly for performance (15-30% faster, but blocks reads)
SET mssql_copy_tablock = true;
```

## Usage Examples

### Basic CTAS with BCP (Default)

```sql
ATTACH 'Server=localhost;Database=TestDB;...' AS mssql (TYPE mssql);

-- Creates table and transfers data via BCP protocol
CREATE TABLE mssql.dbo.summary AS
SELECT category, COUNT(*) as cnt, SUM(amount) as total
FROM mssql.dbo.orders
GROUP BY category;
```

### CTAS with Legacy INSERT Mode

```sql
-- Disable BCP for this session
SET mssql_ctas_use_bcp = false;

-- Uses batched INSERT statements (slower but more familiar error messages)
CREATE TABLE mssql.dbo.backup AS SELECT * FROM mssql.dbo.source;
```

### CTAS with TABLOCK for Maximum Performance

```sql
-- Enable TABLOCK for BCP operations
SET mssql_copy_tablock = true;

-- BCP with table lock hint (fastest, but blocks concurrent reads)
CREATE TABLE mssql.dbo.large_copy AS SELECT * FROM generate_series(1, 1000000);
```

## Settings Reference

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `mssql_ctas_use_bcp` | BOOLEAN | true | Use BCP protocol for CTAS data transfer |
| `mssql_copy_tablock` | BOOLEAN | false | Use TABLOCK hint for BCP operations |
| `mssql_copy_flush_rows` | INTEGER | 100000 | Rows before flush (applies to BCP mode) |

## Limitations

1. **BCP auto-commits**: Like COPY TO, CTAS with BCP auto-commits each batch
2. **Type support**: All DuckDB types must have valid TDS token mappings
3. **Transaction context**: BCP mode may behave differently in explicit transactions

## Implementation Files

| File | Change |
|------|--------|
| `src/include/dml/ctas/mssql_ctas_config.hpp` | Add `use_bcp` flag |
| `src/dml/ctas/mssql_ctas_executor.cpp` | Add BCP execution path |
| `src/connection/mssql_settings.cpp` | Add `mssql_ctas_use_bcp` setting |
| `src/copy/bcp_config.cpp` | Change `mssql_copy_tablock` default |
| `README.md` | Document new settings |
| `docs/architecture.md` | Document BCP integration |
| `docs/testing.md` | Add CTAS test guidance |

## Testing

Run integration tests:
```bash
make docker-up
make integration-test
```

Test specific CTAS scenarios:
```bash
./build/release/test/unittest "test/sql/ctas/*" --force-reload
```

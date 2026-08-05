---
title: Settings
sidebar_position: 1
---

# Configuration Reference

### Connection Pool Settings

| Setting                    | Type    | Default | Range | Description                              |
| -------------------------- | ------- | ------- | ----- | ---------------------------------------- |
| `mssql_connection_limit`   | BIGINT  | 64      | ≥1    | Max connections per attached database    |
| `mssql_connection_cache`   | BOOLEAN | true    | -     | Enable connection pooling and reuse      |
| `mssql_reset_connection`   | BOOLEAN | true    | -     | Reset session state when a connection returns to the pool ([details](#owning-the-session-mssql_reset_connection)) |
| `mssql_connection_timeout` | BIGINT  | 30      | ≥0    | TCP connection timeout (seconds)         |
| `mssql_idle_timeout`       | BIGINT  | 300     | ≥0    | Idle connection timeout (seconds, 0=none)|
| `mssql_min_connections`    | BIGINT  | 0       | ≥0    | Minimum connections to maintain          |
| `mssql_acquire_timeout`    | BIGINT  | 30      | ≥0    | Connection acquire timeout (seconds)     |
| `mssql_query_timeout`      | BIGINT  | 30      | ≥0    | Query execution timeout (seconds, 0=infinite) |
| `mssql_metadata_timeout`   | BIGINT  | 300     | ≥0    | Metadata query timeout (seconds, 0=no timeout) |
| `mssql_catalog_cache_ttl`  | BIGINT  | 0       | ≥0    | Metadata cache TTL (seconds, 0=manual)   |
| `mssql_exec_invalidate_cache` | BOOLEAN | false | true/false | Auto-invalidate the catalog cache after DDL run via `mssql_exec()`. Default `false` (like the Postgres extension's `postgres_execute`): invalidate manually with `mssql_invalidate_cache()` after schema-changing DDL. Set `true` to auto-invalidate. |
| `mssql_attach_validation_timeout` | BIGINT | 0 | ≥0 | ATTACH-time eager-validation timeout (seconds). `0` inherits `mssql_connection_timeout`. Spec 047 FR-011. |

### Statistics Settings

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_enable_statistics`          | BOOLEAN | true    | -     | Enable statistics collection          |
| `mssql_statistics_level`           | BIGINT  | 0       | ≥0    | Detail: 0=rowcount, 1=+histogram, 2=+NDV |
| `mssql_statistics_use_dbcc`        | BOOLEAN | false   | -     | Use DBCC SHOW_STATISTICS (requires permissions) |
| `mssql_statistics_cache_ttl_seconds` | BIGINT | 300    | ≥0    | Statistics cache TTL (seconds)        |

### ORDER BY Pushdown Settings (Experimental)

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_order_pushdown`             | BOOLEAN | false   | -     | Enable ORDER BY pushdown to SQL Server |

The `order_pushdown` ATTACH option provides per-database control. See [ORDER BY Pushdown](#order-by-pushdown-experimental) for details.

### INSERT Settings

| Setting                            | Type    | Default  | Range  | Description                           |
| ---------------------------------- | ------- | -------- | ------ | ------------------------------------- |
| `mssql_insert_batch_size`          | BIGINT  | 1000     | ≥1     | Rows per INSERT (SQL Server limit: 1000) |
| `mssql_insert_max_rows_per_statement` | BIGINT | 1000   | ≥1     | Hard cap on rows per INSERT           |
| `mssql_insert_max_sql_bytes`       | BIGINT  | 8388608  | ≥1024  | Max SQL statement size (8MB)          |
| `mssql_insert_use_returning_output`| BOOLEAN | true     | -      | Use OUTPUT INSERTED for RETURNING     |

### UPDATE/DELETE Settings

| Setting                            | Type    | Default  | Range  | Description                           |
| ---------------------------------- | ------- | -------- | ------ | ------------------------------------- |
| `mssql_dml_batch_size`             | BIGINT  | 500      | ≥1     | Rows per UPDATE/DELETE batch          |
| `mssql_dml_max_parameters`         | BIGINT  | 2000     | ≥1     | Max parameters per statement (~2100 limit) |
| `mssql_dml_use_prepared`           | BOOLEAN | true     | -      | Use prepared statements for DML       |

### Usage Examples

```sql
-- Increase connection pool for high-concurrency workloads
SET mssql_connection_limit = 20;

-- Reduce batch size for tables with large rows
SET mssql_insert_batch_size = 100;

-- Enable detailed statistics for query optimization
SET mssql_statistics_level = 2;

-- Disable connection caching for debugging
SET mssql_connection_cache = false;
```


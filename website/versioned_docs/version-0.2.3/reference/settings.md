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
| `mssql_reset_connection`   | BOOLEAN | true    | -     | Reset session state when a connection returns to the pool ([details](/performance/#owning-the-session-mssql_reset_connection)) |
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


### Wire Protocol Settings

| Setting | Type | Default | Description |
|---|---|---|---|
| `mssql_tds_packet_size` | BIGINT | 16384 | TDS frame size requested at login, clamped to [512, 32767]. Bounds `recv()` count on reads and `send()` count on bulk loads; raised from 4096 in v0.2.3 (−28% client CPU / −43% wall on read). Costs the server ~16 KB per pooled connection; set 4096 to restore the old footprint |
| `mssql_utf8_support` | BOOLEAN | true | Advertise TDS UTF8SUPPORT at login. A granting server sends UTF-8-collated columns without UTF-16 transcoding (measured half the wire bytes). Safe to request everywhere; exists to turn the request off |
| `mssql_named_instance_resolution` | BOOLEAN | true | Resolve `Server=host\instance` to the instance's dynamic port via SQL Server Browser (UDP 1434) at ATTACH. Set `false` where outbound UDP 1434 is stripped — a named instance then errors instead of silently using 1433 |
| `mssql_browser_timeout_seconds` | BIGINT | 3 | Browser UDP query timeout (ATTACH critical path; one retry) |

### Bulk Load (COPY / CTAS) Settings

Details: [COPY TO](/writing/copy/) and [CTAS](/writing/ctas/).

| Setting | Type | Default | Description |
|---|---|---|---|
| `mssql_copy_flush_rows` | BIGINT | 102400 | Rows per bulk-load batch — the batch boundary the **server** sees. 102 400 is SQL Server's own threshold for writing compressed columnstore rowgroups directly; smaller batches land in the delta store and never compress |
| `mssql_copy_parallel_writers` | BIGINT | 0 | Concurrent bulk-load connections one COPY/CTAS may open. `0` derives from DuckDB threads (cap 8); `1` disables. Ignored inside explicit transactions (COPY pins one connection) |
| `mssql_copy_tablock` | VARCHAR | `auto` | `auto` \| `true` \| `false`. `auto` decides from the target's shape: heap ON, anything clustered OFF (the hint serialises parallel loaders against a clustered index) |
| `mssql_ctas_use_bcp` | BOOLEAN | true | CTAS transfers data over the bulk-load protocol (2–10× the text INSERT path) |
| `mssql_ctas_text_type` | VARCHAR | `NVARCHAR` | What an unannotated DuckDB `VARCHAR` becomes in created tables (`NVARCHAR`/`VARCHAR`); drives CTAS and COPY alike |
| `mssql_ctas_drop_on_failure` | BOOLEAN | false | Drop the created table when the load phase fails |

### Target Type Settings

Details: [Target Column Types and Table Shape](/writing/table-options/).

| Setting | Type | Default | Description |
|---|---|---|---|
| `mssql_utf8_collation` | VARCHAR | `Latin1_General_100_BIN2_UTF8` | Collation for created `varchar` columns when the server granted UTF8SUPPORT. BIN2 = binary comparison, case-/accent-**sensitive**; matches Fabric's default. Empty inherits the database default |
| `mssql_default_string_length` | BIGINT | 0 | Length for unannotated `VARCHAR` columns created by CTAS/COPY (`0` = MAX) |
| `mssql_default_table_kind` | VARCHAR | `HEAP` | Shape of created tables: `HEAP` or `COLUMNSTORE` (clustered columnstore index created before the load) |
| `mssql_catalog_native_types` | BOOLEAN | true | Report `MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` for bounded string columns of attached tables, so targets inherit declared lengths |
| `mssql_convert_varchar_max` | BOOLEAN | true | Convert `VARCHAR(MAX)` to `NVARCHAR(MAX)` in catalog scan SQL for UTF-8 safety on non-UTF-8 collations |

### ORDER BY Pushdown Settings (Experimental)

| Setting                            | Type    | Default | Range | Description                           |
| ---------------------------------- | ------- | ------- | ----- | ------------------------------------- |
| `mssql_order_pushdown`             | BOOLEAN | false   | -     | Enable ORDER BY pushdown to SQL Server |

The `order_pushdown` ATTACH option provides per-database control. See [ORDER BY Pushdown](/reading/queries/#order-by-pushdown-experimental) for details.

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


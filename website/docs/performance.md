---
title: Performance
sidebar_position: 6
---

# Performance Tuning

### Bulk Data Loading

For loading large datasets into SQL Server, use COPY TO with BCP protocol:

```sql
COPY large_dataset TO 'mssql://db/dbo/target' (FORMAT 'bcp');
```

**The defaults are the tuned values.** This section used to recommend
`TABLOCK = true` and `FLUSH_ROWS = 500000`; spec 057 measured both against a
live server and both are wrong more often than not.

| Setting | Leave it alone unless | Why |
|---------|----------------------|-----|
| `mssql_copy_tablock` | you know the target is a heap AND nothing else loads into it | `auto` already turns it on for a heap. Forcing it on for anything **clustered** serialises the load: 2M rows into a clustered columnstore took 8.92 s with the hint against 5.23 s without, one core busy instead of three, and compressed identically either way |
| `mssql_copy_flush_rows` | you are memory-constrained on the server | Lowering it below **102400** stops a columnstore target from compressing at all — every row lands in the delta store, which only closes on its own at 1048576. Raising it does not help: the value is a server-side batch boundary, not a client buffer, and throughput measured flat above the threshold |
| `mssql_copy_parallel_writers` | you must not open more than one session | The bound on this path is SQL Server's ingest rate, and it parallelises across sessions. 44 columns x 1M rows: 10.55 s at one writer, **3.24 s at four**, 3.51 s at eight — it plateaus past four, which is why the derived value is capped |

Inside an explicit transaction COPY uses one writer regardless: the connection is
pinned, and a second would sit outside the transaction. CTAS is not affected —
it loads outside the transaction by design.

### Connection Pool Tuning

```sql
-- High-concurrency workloads
SET mssql_connection_limit = 100; -- More connections (default: 64)
SET mssql_min_connections = 5;    -- Pre-warm more connections (default: 0)

-- Long-running analytics
SET mssql_query_timeout = 0;      -- No timeout (default: 30s)
SET mssql_idle_timeout = 600;     -- Keep connections longer (default: 300s)

-- Debugging connection issues
SET mssql_connection_cache = false;  -- Disable pooling for isolation
```

### Owning the session: `mssql_reset_connection`

Connections are pooled, and by default a connection returning to the pool is
flagged for a **session reset** before the next statement uses it — the TDS
`RESET_CONNECTION` bit, which is what `sp_reset_connection` does and what
ADO.NET and JDBC drivers do for the same reason.

That is why a temp table does not survive between statements:

```sql
SELECT mssql_exec('db', 'SELECT 1 AS x INTO ##staging');
SELECT * FROM mssql_scan('db', 'SELECT * FROM ##staging');
-- Invalid object name '##staging'
```

A `##global` table lives exactly as long as the session that created it, and the
reset ends that session. It is the same physical connection — `@@SPID` is
unchanged — but a new session on it.

There is no way to keep temp tables while still clearing everything else: the
reset is a single bit in the TDS packet header with two variants, and the other
one (`RESET_CONNECTION_SKIP_TRAN`) drops `##global` and local `#temp` tables just
the same; it differs only in how it treats transaction state.

So the choice is all or nothing:

```sql
SET mssql_reset_connection = false;

SELECT mssql_exec('db', 'SELECT 1 AS x INTO ##staging');
SELECT * FROM mssql_scan('db', 'SELECT * FROM ##staging');  -- 1
SELECT mssql_exec('db', 'DROP TABLE ##staging');            -- yours to clean up
```

**What you are taking on.** `false` does not mean "keep my temp tables". It means
**you own the session state** of every pooled connection, and the reset was
clearing more than temp objects:

- `SET` options (`ANSI_NULLS`, `DATEFIRST`, `ARITHABORT`, …) and the isolation
  level set by one statement are inherited by the next, unrelated one;
- session variables, `CONTEXT_INFO`, and open cursors persist;
- **an open transaction stays open**, and keeps its locks, until that connection
  is used again — the reset is what would have rolled it back.

Use it when you deliberately want a staging table to outlive the statement that
created it, prefer `##global` over `#local` (a `#` table is reachable only from
the session that made it, and a pool gives no guarantee about which connection
you get next), and drop what you create.

An alternative that needs no setting, if you have `CREATE TABLE` permission on
the database: attach a second catalog and use an ordinary table.

```sql
ATTACH 'Server=...;Database=tempdb;...' AS stg (TYPE mssql);
CREATE TABLE stg.dbo.staging AS SELECT * FROM local_data;
```

### INSERT vs COPY Performance

| Method | Rows/sec | Best For |
|--------|----------|----------|
| Single INSERT | ~1K | Small single-row operations |
| Batched INSERT | ~50K | INSERT with RETURNING clause |
| COPY TO / CTAS (BCP) | ~300K | Bulk loading without RETURNING |

The big lever above that is `mssql_copy_parallel_writers`, not TABLOCK: the
bound is SQL Server's ingest rate and it parallelises across sessions, measured
at 3.3x from one writer to four. TABLOCK is worth nothing extra on the default
path — `auto` already applies it where it helps — and costs about 1.7x where it
does not (see the tuning table above).

```sql
-- For bulk loads without RETURNING, always prefer COPY
-- Instead of:
INSERT INTO db.dbo.target SELECT * FROM large_source;

-- Use:
COPY (SELECT * FROM large_source) TO 'db.dbo.target' (FORMAT 'bcp');
```

### Query Optimization

```sql
-- Enable filter pushdown verification
SET mssql_enable_statistics = true;  -- Default

-- For complex queries, use mssql_scan with explicit SQL
-- DuckDB will still optimize joins with local tables
FROM mssql_scan('db', 'SELECT id, name FROM dbo.large_table WHERE region = ''US''')
JOIN local_lookup USING (id);
```

### Memory Management

| Setting | Impact | Recommendation |
|---------|--------|----------------|
| `mssql_copy_flush_rows` | SQL Server buffer memory | Increase for throughput, decrease for memory |
| `mssql_insert_batch_size` | DuckDB batch memory | Keep at 1000 (SQL Server limit) |
| `mssql_dml_batch_size` | UPDATE/DELETE memory | Decrease for wide tables |


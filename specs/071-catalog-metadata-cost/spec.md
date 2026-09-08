# Spec 071 — What catalog metadata actually costs

**Status**: PROPOSED.
**Closes**: [#86](https://github.com/hugr-lab/mssql-extension/issues/86) (`SHOW ALL TABLES` unusable on a database with many schemas).
**Relates to**: PR #308 (the 1205 deadlock-victim retry — the reset callback that
makes losing a deadlock recoverable). This spec attacks the same queries from the
other side: W1 removes the resource that 7 of 8 captured deadlock cycles were
fighting over, so the retry has far less to absorb.

Four defects in the metadata layer, found by measuring rather than reading. They
are independent, they land in ascending order of size, and three of them are the
same root cause seen from different paths: **the `sys.partitions` aggregate that
every catalog query carries, for a column nothing reads.**

---

## 0. How everything below was measured

Reproduce before trusting. SQL Server **2025** (17.0.4075.5) in Docker — the 2022
image crash-loops on Apple Silicon, see `docs/TESTING.md` — with a synthetic
catalog in `TestDB`:

```sql
-- 200,000 tables x 6 columns, one schema `big`, plus an empty schema `empty1`
CREATE TABLE big.tN (id INT, c1 VARCHAR(50), c2 INT, c3 DECIMAL(18,4),
                     c4 DATETIME2(3), c5 NVARCHAR(100));
```

Resulting size: **200,000 tables / 1,200,000 columns / 200,230 rows in
`sys.partitions`**. Creation ran at ~1 ms per table and did **not** degrade with
catalog size (0.8 ms/table at 2K, 0.97 ms/table at 97K) — the write side of the
catalog scales fine, which is why everything below is about reads.

- **Wall clock**: `sqlcmd -i q.sql -o /dev/null` with `GO N` inside one session,
  so connection setup is paid once. A `SELECT 1` script measured the same way is
  subtracted as the baseline (~20.7 ms/rep here — it is `docker exec` plus
  sqlcmd's own formatting, and it is large relative to the signal, which is why
  the per-invocation form of this measurement is useless and was discarded).
- **Server cost**: `SET STATISTICS TIME ON` (CPU ms) and `SET STATISTICS IO ON`
  (logical reads per base table). These are the numbers that carry the argument;
  wall clock is only corroboration.
- Variants were run **interleaved** and repeated, never one-after-another —
  three passes, and any effect that did not survive all three is reported as
  noise below rather than as a result.

---

## 1. The findings

### F1 — `sys.partitions` cannot be seeked, in any form

All three metadata queries fetch `approx_rows` / `index_type` /
`partition_count` from one uncorrelated subquery:

```sql
LEFT JOIN (SELECT p.object_id, SUM(p.[rows]) AS [rows],
                  MAX(ISNULL(i.type, 0)) AS index_type, COUNT(*) AS partition_count
           FROM sys.partitions p
           LEFT JOIN sys.indexes i ON i.object_id = p.object_id AND i.index_id = p.index_id
           WHERE p.index_id IN (0, 1)
           GROUP BY p.object_id) p ON p.object_id = o.object_id
```

`sys.partitions` reads `sys.sysrowsets`, which is clustered on `rowsetid`;
`object_id` is derived and has no index. Filtering by it changes nothing:

| lookup by `object_id` | base table | logical reads |
|---|---|---|
| `sys.partitions` | `sysrowsets` | **3200** (2 scans) |
| `sys.dm_db_partition_stats` | `sysrowsets` | **1600** (1 scan) |
| `sys.indexes` | `sysidxstats` | **6** (seek) |

So the row count is O(catalog) however it is asked for, and the index *shape* is
O(1). They do not belong in the same subquery.

### F2 — `partition_count` has no consumer

`ParseTableShape` writes `MSSQLTableMetadata::partition_count` and **nothing ever
reads it** — it is not even copied into `MSSQLTableEntry`. Spec 049 added it
"which the write path needs: TABLOCK helps a heap and serialises a clustered
rowstore index"; the write path never picked it up.

It is not free. `COUNT(*)` per `object_id` is what forces the `GROUP BY` over
`sysrowsets`, i.e. F1's scan exists to populate a dead field.

### F3 — on the lazy path, `approx_row_count` is loaded and then bypassed

`MSSQLTableEntry::GetStorageInfo` resolves cardinality as: statistics cache →
**acquire a connection and query the DMV** → and only on "no connection" or an
exception does it fall back to `approx_row_count_`. Meanwhile `PreloadRowCount`
— the only thing that puts catalog-sourced counts *into* that cache — is called
from `mssql_preload_catalog.cpp` and nowhere else.

Two consequences:

1. A lazy single-table load pays F1's 3200-read scan for a number
   `GetStorageInfo` then declines to use.
2. `SHOW ALL TABLES` / `duckdb_tables()` **without** a prior
   `mssql_preload_catalog()` takes a pooled connection and runs a DMV query
   **per table**. `CLAUDE.md` describes the exemption in
   `mssql_statistics_cache_ttl_seconds` as protecting "a catalog that had just
   loaded every count in one query" — on the lazy path that catalog never told
   the cache anything.

### F4 — per-schema discovery is O(objects in the database), not O(schema)

This is issue #86, and it is not about the reporter's table count. An **empty**
schema, in a database holding 200K objects:

| | CPU | logical reads |
|---|---|---|
| current query | **2708 ms** | `sysschobjs` 3025, `sysidxstats` 1520, `sysrowsets` 4800, Workfile 2584 (spill) |
| without the F1 aggregate | **362 ms** | `sysschobjs` 2570 |

362 ms to return zero rows, because `sys.objects` applies metadata-visibility
filtering per object in the database. It is not the predicate's fault — measured
identical (2570 reads) for `SCHEMA_NAME(o.schema_id) = 'x'`, for
`o.schema_id = SCHEMA_ID('x')`, for `sys.tables`, and with the `type` /
`is_ms_shipped` filters removed. There is no seek path to buy.

So N schemas cost N full passes over the catalog. The reporter's 10,000 schemas
with **zero tables** extrapolate to **~7.5 hours** at the current query, which
matches "30 minutes and still running". `mssql_preload_catalog()` was offered as
the workaround and is built the same way — `BulkLoadAll` loops per schema — so it
reduced the constant, not the exponent.

The alternative is one query:

| approach, 200K objects | cost |
|---|---|
| one query for **all** schemas (200,036 rows) | **1917 ms CPU** |
| current, per schema | 2708 ms CPU **x number of schemas** |

Break-even is under 5 schemas. There is no catalog size at which the present
design wins.

---

## 2. The work

### W1 — Take the row count out of the shape query

Replace the `sys.partitions` subquery in all three templates
(`TABLE_DISCOVERY`, `SINGLE_TABLE_METADATA`, `BULK_METADATA_SCHEMA`) with a
seekable shape lookup:

```sql
OUTER APPLY (SELECT MAX(i.type) AS index_type,
                    MAX(CASE WHEN ps.data_space_id IS NULL THEN 0 ELSE 1 END) AS is_partitioned
             FROM sys.indexes i
             LEFT JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id
             WHERE i.object_id = o.object_id AND i.index_id IN (0, 1)) ix
```

- `index_kind` keeps its meaning and its source semantics (`sys.indexes.type`:
  0 heap, 1 clustered rowstore, 5 clustered columnstore).
- `partition_count` is **deleted** — from the SQL, from `MSSQLTableMetadata`, and
  from `ParseTableShape` (F2). `is_partitioned` replaces it, which is what every
  comment in the tree claims the field is for ("`partition_count > 1` marks a
  partitioned object"). If a future caller genuinely needs the count, it is a
  seekable lookup at that point rather than a tax on every metadata load.

Measured, single table, 200K catalog: **18.1 ms → 0.7 ms net of baseline (26x)**,
`sysrowsets` **3200 → 0** logical reads.

Deadlock benefit, from the PR #308 investigation: `sysrowsets` was one of the two
resources in **7 of 8** captured cycles. Removing it from the joined statement is
what took index-DDL contention from **127/141/93 to 0/0/0** deadlocks per 150
metadata queries, and table-DDL from **63/73/46 to 32/24/24**.

### W2 — The single-table path stops fetching a row count at all

`SINGLE_TABLE_METADATA` returns columns plus the W1 shape, and no `approx_rows`.
The statistics provider already owns that number, has a cache and a TTL, and
`GetStorageInfo` already prefers it (F3). `MSSQLTableMetadata::approx_row_count`
stays for the paths that do load it; on this path it is simply not populated.

Behavioural note to state in the PR: with no connection available,
`GetStorageInfo`'s last-resort fallback now sees 0 instead of a catalog-loaded
count. That path already means "we could not ask the server", and reporting
nothing is what `mssql_enable_statistics` documents as correct when the count is
unknown — see the setting's note that a VIEW reports 0 while returning millions,
which is the failure mode a made-up number produces.

### W3 — The listing path issues one query for every schema

The change that closes #86.

- `MSSQLTableSet::Scan` currently calls `LoadAllTableMetadata(connection, schema)`
  and is invoked once per schema by DuckDB. `MSSQLTableSet::GetEntry` — the plain
  `SELECT * FROM db.sch.tbl` path — goes to `GetTableMetadata` instead and is
  **untouched**, so laziness for ordinary queries is preserved exactly.
- On the first `Scan` (or first `BulkLoadAll`), run **one** query covering every
  schema the filter admits, and populate the cache for all of them. Subsequent
  per-schema `Scan` calls are cache hits.
- **Drop the `ORDER BY`.** The per-schema loop exists because "each query sorts
  only within one schema, keeping the result set small enough to avoid tempdb
  spills" — but the current query spills anyway (Workfile 2584 reads above), and
  a global sort over 1.2M rows would be worse. Group client-side into a hash map
  keyed by `object_id` instead of relying on ordered arrival. This also removes
  the streaming group-by's dependence on `current_table`, which is the state PR
  #308's reset callback has to unwind.
- `BulkLoadAll` collapses into the same single query, so
  `mssql_preload_catalog()` stops being O(schemas x objects) too.

10,000 schemas, 200K objects: **~7.5 hours → ~2 s** plus transfer.

### W4 — Catalog-loaded counts reach the statistics cache

Where a path does load counts (W3's whole-catalog query), call
`PreloadRowCount` for each table, as `mssql_preload_catalog.cpp` already does.
Without this, `SHOW ALL TABLES` takes a pooled connection and runs a DMV query
per table (F3), each of which is itself a 1600-read scan of `sysrowsets`.

---

## 3. Dead ends — checked, and not to be re-proposed

Each of these looked right and measured wrong. Recorded so the next person does
not spend the afternoon.

| idea | verdict |
|---|---|
| `READ UNCOMMITTED` / `NOLOCK` on catalog reads to dodge the deadlock | **Catalog reads ignore the session isolation level.** Wrapping the query in `REPEATABLE READ` + `BEGIN TRAN` and inspecting `sys.dm_tran_locks` afterwards leaves only a `DATABASE` S lock — the key locks were taken and released inside the scan regardless. It would also reintroduce the duplicate-row hazard on an allocation-order scan, which is the "Column with name x already exists!" class. |
| `sys.dm_db_partition_stats` instead of `sys.partitions` | Still a full scan of `sysrowsets` (1600 vs 3200 reads). For the deadlock it reshuffles rather than removes: plan references to `sysrowsets` fall 30 → 10 but `sysidxstats` appears (12). |
| Correlate the aggregate to the one object (`OUTER APPLY`) for the single-table query | No change: 38.8–41.6 ms, same as the uncorrelated form. F1 explains why. |
| Split the aggregate into a second statement | Helps only a small catalog — **−42%** on 200 tables x 20 columns, where the three aggregate columns are otherwise repeated on every one of 4000 column rows. At 200K it is a wash (3109/2856 ms vs 2861/2793). W1 subsumes it. |
| Materialise into `#temp` server-side, then stream from tempdb, to shorten the catalog-lock window | **+89%** (74.0 ms vs 39.2 net). Writing 4000 rows to tempdb and reading them back costs far more than the window it saves. (First measurement of this variant looked like a 96% *win* — because `SELECT INTO` was failing with Msg 1038 on unnamed columns and the timing was of a query that did nothing.) |
| `o.schema_id = SCHEMA_ID('x')` instead of `SCHEMA_NAME(o.schema_id) = 'x'` | No effect. Identical 2570 logical reads, as do `sys.tables` and the form with `type`/`is_ms_shipped` removed. The scan is metadata-visibility filtering, not a sargability problem. |

Two measurements from this investigation were **retracted** after repetition and
should not be quoted from earlier notes: an `ALTER TABLE` deadlock rate of 22/33
per 150 (did not reproduce — leftover churn sessions from the previous DDL kind
were contaminating the run; it is 0/0/0 with sessions killed before each pass),
and a `TRUNCATE` comparison (4–80 per 150 on identical conditions — too noisy for
any conclusion).

---

## 4. Risks

- **W3 changes when work happens, not how much.** A user who touches one schema
  of a 10,000-schema catalog and never lists anything pays nothing extra: `Scan`
  is the listing path, `GetEntry` is the query path. But a user who lists *one*
  schema now loads all of them. At the measured break-even (<5 schemas) this is
  cheaper in every case that has been measured; it should still be stated
  plainly in the PR, and `schema_filter` remains the way to bound it.
- **W3's result set is large** — 200K rows for 200K objects, and with columns it
  is 1.2M. The hash-map grouping must not hold two copies; the parse should build
  entries in place as rows arrive, as the current streaming group-by does.
- **W1 changes `DESCRIBE`-visible nothing** but does change `MSSQLTableMetadata`.
  `partition_count` removal is a struct change; confirm no serialization depends
  on it.
- **W2 is a behaviour change in one corner**: cardinality when no connection can
  be acquired. Small, and stated above.

## 5. Acceptance

1. Single-table metadata against a 200K-table catalog: **zero** logical reads on
   `sysrowsets`, and net query time within noise of the columns-only query
   (≈0.7 ms vs the current 18.1 ms).
2. An empty schema's discovery costs no more than the fixed catalog pass
   (≈362 ms at 200K objects, from 2708 ms).
3. `SHOW ALL TABLES` over N schemas issues **one** metadata query, not N.
4. `SHOW ALL TABLES` acquires no per-table connection for row counts (W4).
5. `partition_count` appears nowhere in `src/`.
6. The SQL suite stays green, and the 12-worker `make test` deadlock rate does
   not regress from PR #308's baseline (four consecutive runs, 177 passed).

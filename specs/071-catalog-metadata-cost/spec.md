# Spec 071 — What catalog metadata actually costs

**Status**: PROPOSED. Implementation in progress.
**Closes**: [#86](https://github.com/hugr-lab/mssql-extension/issues/86) (`SHOW ALL TABLES` unusable on a database with many schemas).
**Relates to**: PR #308 (the 1205 deadlock-victim retry). This spec attacks the same
queries from the other side: W1 removes the resource that 7 of 8 captured deadlock
cycles were fighting over, so the retry has far less to absorb.

Two defects in the metadata layer, found by measuring rather than reading. They
share a cause: **the `sys.partitions` aggregate every catalog query carries.**

> **Revision note.** This spec was rewritten after measurement killed most of its
> first draft, including two of its own proposals and a claim about the code. §5
> keeps every dead end with its numbers, because the value of this investigation
> is as much in what does not work as in what does. Nothing in §2 is a design
> preference; each line is what survived a measurement.

---

## 0. How everything below was measured

Reproduce before trusting. SQL Server **2025** (17.0.4075.5) in Docker — the 2022
image crash-loops on Apple Silicon, see `docs/TESTING.md` — with a synthetic
catalog in `TestDB`:

```sql
-- 200,000 tables x 6 columns in schema `big`, plus an empty schema `empty1`
CREATE TABLE big.tN (id INT, c1 VARCHAR(50), c2 INT, c3 DECIMAL(18,4),
                     c4 DATETIME2(3), c5 NVARCHAR(100));
```

**200,000 tables / 1,200,000 columns / 200,230 rows in `sys.partitions`.** Creation
ran at ~1 ms per table and did **not** degrade with catalog size (0.8 ms/table at
2K, 0.97 ms/table at 97K) — the write side scales fine, which is why everything
below is about reads.

- **Server cost**: `SET STATISTICS TIME ON` (CPU ms) and `SET STATISTICS IO ON`
  (logical reads per base table). These carry the argument.
- **Wall clock**: `sqlcmd -i q.sql -o /dev/null` with `GO N` **inside one
  session**, minus a `SELECT 1` baseline measured the same way. The
  per-invocation form is useless here: `docker exec` plus sqlcmd startup costs
  ~330 ms and buries a 40 ms signal.
- **End-to-end**: two statically-linked builds differing only in the change under
  test, each run in a **fresh process**, `ATTACH` + query, interleaved.
- Variants interleaved and repeated. Anything that did not survive repetition is
  reported in §5 as retracted, not as a result.

---

## 1. The findings

### F1 — `sys.partitions` cannot be seeked by `object_id`, and the row count does not need it

All three metadata queries fetch `approx_rows` / `index_type` /
`partition_count` from one uncorrelated subquery over `sys.partitions`. That view
reads `sys.sysrowsets`, which is clustered on `rowsetid`; `object_id` is derived
and has no index, so filtering by it buys nothing:

| row count for ONE object | base table touched | logical reads | CPU |
|---|---|---|---|
| `sys.partitions` | `sysrowsets` | **3200** (2 scans) | 18 ms |
| `sys.dm_db_partition_stats` | `sysrowsets` | **1601** (1 scan) | 54 ms |
| `sys.sysindexes` | `sysidxstats` + `sysschobjs` | 6 | 1 ms |
| `sys.dm_db_stats_properties` | `sysidxstats` + `sysobjvalues` | ~17 | 1 ms |
| **`OBJECTPROPERTYEX(id, 'Cardinality')`** | `sysschobjs` | **3** | **0 ms** |

`OBJECTPROPERTYEX` answers out of object metadata. Verified to agree with
`sys.partitions` **exactly** — 5000/5000, 150000/150000, 0/0 for an empty table,
and `NULL` for a VIEW, which maps to the same 0 the catalog already carried for a
view. It needs no statistics to exist on the table, and it is not deprecated.

Fetching **every** row count in the database: **84 ms**, against **2400 ms**
through either `sys.partitions` or `sys.dm_db_partition_stats`.

The shape half splits the same way: `sys.indexes` **seeks** (6 logical reads),
while `sys.partitions` cannot. The two had no business sharing a subquery.

### F2 — `partition_count` has no consumer

`ParseTableShape` writes `MSSQLTableMetadata::partition_count` and **nothing ever
reads it** — it is not even copied into `MSSQLTableEntry`. Spec 049 added it
"which the write path needs: TABLOCK helps a heap and serialises a clustered
rowstore index"; the write path never picked it up.

It is not free: `COUNT(*)` per `object_id` is what forces the `GROUP BY` over
`sysrowsets`. The scan exists to populate a dead field.

### F3 — per-schema discovery is O(objects in the database), not O(schema)

This is issue #86, and it is not about the reporter's table count. An **empty**
schema, in a database holding 200K objects:

| | CPU | logical reads |
|---|---|---|
| current query | **2708 ms** | `sysschobjs` 3025, `sysidxstats` 1520, `sysrowsets` 4800, Workfile 2584 (spill) |
| with F1 applied — count and shape both seekable | **484 ms** | no `sysrowsets` |
| for reference, with no count at all | 362 ms | `sysschobjs` 2570 |

484 ms to return zero rows, because `sys.objects` applies metadata-visibility
filtering per object in the database. Not the predicate's fault: measured
identical (2570 reads) for `SCHEMA_NAME(o.schema_id) = 'x'`, for
`o.schema_id = SCHEMA_ID('x')`, for `sys.tables`, and with the `type` /
`is_ms_shipped` filters removed. There is no seek to buy.

So N schemas cost N passes over the catalog. The reporter's 10,000 schemas with
**zero tables** extrapolate to **~7.5 hours** at the current query, which matches
"30 minutes and still running"; at 484 ms they come to ~80 minutes, and the
remaining fix is to stop issuing one query per schema (W2).

Note the last row: dropping the count entirely would save a further 122 ms per
schema. It is **not** worth it — see §3.

**And there is no per-schema seek to buy.** `sysschobjs` carries a nonclustered
index on `(nsid, name)`, so the obvious hope is to make the schema predicate
select it. Measured: it never does. `sys.objects`, `sys.tables` and
`INFORMATION_SCHEMA.TABLES` all plan as a **Clustered Index Scan on
`sysschobjs.clst`**, with a literal `nsid = 1` as much as with `SCHEMA_ID()` or
`SCHEMA_NAME()` — the predicate is pushed into the scan, not turned into a seek,
because the query needs `type` / `nsclass` / `pclass`, which that index does not
cover. So the fixed catalog pass is a floor per statement, and the only lever left
is to issue **one statement instead of N**. That is W2, and it is the whole
remedy for the per-schema half of #86.

---

## 2. The work

### W1 — Seekable sources for both halves of the shape query

In all three templates (`TABLE_DISCOVERY`, `SINGLE_TABLE_METADATA`,
`BULK_METADATA_SCHEMA`), replace the `sys.partitions` aggregate with:

```sql
    CAST(ISNULL(OBJECTPROPERTYEX(o.object_id, 'Cardinality'), 0) AS BIGINT) AS approx_rows,
    ...
OUTER APPLY (SELECT MAX(i.type) AS index_type,
                    MAX(CASE WHEN ps.data_space_id IS NULL THEN 0 ELSE 1 END) AS is_partitioned
             FROM sys.indexes i
             LEFT JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id
             WHERE i.object_id = o.object_id AND i.index_id IN (0, 1)) shape
```

- `approx_rows` keeps its meaning and its exact values; only its source changes.
- `index_kind` keeps its meaning and its source semantics (`sys.indexes.type`:
  0 heap, 1 clustered rowstore, 5 clustered columnstore).
- `partition_count` is **deleted** — from the SQL, from `MSSQLTableMetadata`, and
  from `ParseTableShape` (F2). `is_partitioned` replaces it, which is what every
  comment in the tree already claimed the field meant ("`> 1` marks a partitioned
  object"). A caller that genuinely needs the count can seek it at that point
  instead of taxing every metadata load.

Measured at 200K tables: single-table metadata **18.1 ms → ~1 ms**;
empty-schema discovery **2708 ms → 484 ms**; all row counts in the database
**2400 ms → 84 ms**. `sysrowsets` logical reads on the single-table path:
**3200 → 0**.

Deadlock benefit, from PR #308's investigation: `sysrowsets` was one of the two
resources in **7 of 8** captured cycles. Removing it from the joined statement
took index-DDL contention from **127/141/93 to 0/0/0** deadlocks per 150 metadata
queries (3 reps), and table-DDL from **63/73/46 to 32/24/24**.

**Nothing is lost.** Counts stay, the planner's cardinality stays, behaviour is
unchanged. Only the dead field and the scan go.

### W2 — The listing path issues one query for every schema

The remaining half of #86, and the larger change.

- `MSSQLTableSet::Scan` calls `LoadAllTableMetadata(connection, schema)` and is
  invoked once per schema by DuckDB. `MSSQLTableSet::GetEntry` — the plain
  `SELECT * FROM db.sch.tbl` path — goes to `GetTableMetadata` instead and is
  **untouched**, so laziness for ordinary queries is preserved exactly.
- On the first `Scan` (or first `BulkLoadAll`), run **one** query covering every
  schema the filter admits, and populate the cache for all of them. Subsequent
  per-schema `Scan` calls are cache hits.
- **Drop the `ORDER BY`.** The per-schema loop exists because "each query sorts
  only within one schema, keeping the result set small enough to avoid tempdb
  spills" — but the current query spills anyway (Workfile 2584 reads above), and
  a global sort over 1.2M rows would be worse. Group client-side into a hash map
  keyed by `object_id` instead of relying on ordered arrival. This also retires
  the streaming group-by's `current_table` cursor, which is the fiddliest state
  PR #308's reset callback has to unwind.
- `BulkLoadAll` collapses into the same single query, so
  `mssql_preload_catalog()` stops being O(schemas x objects) too — which is why
  recommending it as the workaround on #86 lowered the constant, not the exponent.

One query for all schemas measured **1917 ms** against 200K objects, versus
484 ms **per schema** after W1. Break-even is under five schemas; there is no
catalog size at which one-query-per-schema wins.

---

## 3. What this spec no longer proposes

The first draft had two further workstreams. Both are withdrawn, and the reasons
are worth keeping.

- **"The single-table path stops fetching a row count."** Withdrawn: it is what
  gives the planner its cardinality — `MSSQLCatalogScanCardinality` reads the
  catalog's copy *first* — and `test/sql/catalog/scan_cardinality.test` catches
  the loss (`~1 row` instead of `~200000`). W1 makes the count cheap instead,
  which was the better answer all along.
- **"Feed catalog-loaded counts to `PreloadRowCount`."** Withdrawn as **already
  implemented**: `MSSQLTableSet::Scan` calls it for every table
  (`mssql_table_set.cpp:179`). The first draft asserted the only caller was
  `mssql_preload_catalog.cpp`; that was a truncated `grep`, and the conclusion
  drawn from it — that `SHOW ALL TABLES` runs a DMV query per table — was wrong.

---

## 4. Risks

- **`OBJECTPROPERTYEX` availability.** Standard T-SQL metadata function, present
  on SQL Server and Azure SQL Database. **Microsoft Fabric Warehouse is not
  verified here** and is a supported target — worth a check before merge. The
  fallback is mechanical (`ISNULL(..., 0)` already yields "unknown", which the
  cardinality callback handles as "no estimate").
- **W2 changes when work happens, not how much.** A user who touches one schema
  of a 10,000-schema catalog and never lists anything pays nothing extra: `Scan`
  is the listing path, `GetEntry` is the query path. A user who lists *one* schema
  now loads all of them — cheaper in every measured case, but it should be stated
  plainly in the PR, and `schema_filter` remains the way to bound it.
- **W2's result set is large** — 1.2M rows for 200K objects with columns. The
  hash-map grouping must build entries in place as rows arrive, as the current
  streaming group-by does, and must not hold two copies.
- **`partition_count` removal is a struct change.** Confirm no serialization
  depends on it.

## 5. Dead ends — measured, and not to be re-proposed

Each looked right. Recorded so the next person does not spend the afternoon.

| idea | verdict |
|---|---|
| `READ UNCOMMITTED` / `NOLOCK` on catalog reads, to dodge the deadlock | **Catalog reads ignore the session isolation level.** Wrapping the query in `REPEATABLE READ` + `BEGIN TRAN` and inspecting `sys.dm_tran_locks` afterwards leaves only a `DATABASE` S lock — the key locks were taken and released inside the scan regardless. It would also reintroduce the duplicate-row hazard on an allocation-order scan, which is the "Column with name x already exists!" class. |
| `sys.dm_db_partition_stats` instead of `sys.partitions` | **Still a full scan** of `sysrowsets` (1601 reads / 54 ms vs 3200 / 18 ms — fewer reads, *more* CPU). For the deadlock it reshuffles rather than removes: plan references to `sysrowsets` fall 30 → 10 but `sysidxstats` appears (12). |
| Correlate the aggregate to the one object (`OUTER APPLY`) | No change: 38.8–41.6 ms, same as the uncorrelated form. F1 explains why. |
| Split the aggregate into a second statement | Helps only a small catalog — **−42%** at 200 tables x 20 columns, where the three aggregate columns are otherwise repeated on every one of 4000 column rows. At 200K it is a wash (3109/2856 ms vs 2861/2793). W1 subsumes it. |
| Materialise into `#temp` server-side, then stream from tempdb, to shorten the catalog-lock window | **+89%** (74.0 ms vs 39.2 net). Writing 4000 rows to tempdb and reading them back costs far more than the window it saves. Its *first* measurement showed a 96% **win** — because `SELECT INTO` was failing with Msg 1038 on unnamed columns and the timing was of a query that did nothing. |
| `o.schema_id = SCHEMA_ID('x')` instead of `SCHEMA_NAME(o.schema_id) = 'x'` | No effect. Identical 2570 logical reads, as do `sys.tables` and the form with `type`/`is_ms_shipped` removed. The scan is metadata-visibility filtering, not a sargability problem. |
| Load **all** row counts once into the statistics cache, so metadata queries need none | **2400 ms** at 200K, and restricting it to one small schema barely helps (2105 ms) — the DMV enumerates the catalog either way, and the cost is the *scan*, not the transfer (rows discarded server-side: 2361 ms). Break-even against a per-table count is ~120 tables. W1 makes the per-table count 3 reads, which ends the question. |
| `sys.sysindexes` for the row count | Genuinely cheap (6 reads / 1 ms per table, 82 ms for all 200K) and exact. **Rejected as deprecated** — it is documented for removal and its availability on Azure SQL / Fabric is not something to build on. |
| `sys.dm_db_stats_properties` for the row count | Cheap (~17 reads / 1 ms) and supported, but **returns NULL for a table with no statistics** — verified on a 5000-row heap with zero statistics objects, which is exactly a table this extension has just created by CTAS or COPY. |

**Retracted measurements** from this investigation, which did not survive
repetition and should not be quoted from earlier notes: an `ALTER TABLE` deadlock
rate of 22/33 per 150 (churn sessions from the previous DDL kind were still
running; it is 0/0/0 with sessions killed *before* each pass), and a `TRUNCATE`
comparison (4–80 per 150 on identical conditions).

**End-to-end note.** At cold start the W1 saving is invisible for a single table
— `ATTACH` dominates at ~650 ms, and 18 ms hides inside it. Across 20 tables in
one session it is plain: 692/722 ms against 1092/1126 ms. The saving is per
distinct table touched, and #86's cost is per schema, not per query.

## 6. Acceptance

1. Single-table metadata against a 200K-table catalog: **zero** logical reads on
   `sysrowsets`, and net query time within noise of the columns-only query.
2. An empty schema's discovery costs no more than the fixed catalog pass
   (≈484 ms at 200K objects, from 2708 ms).
3. `scan_cardinality.test` still passes — the planner still sees the row count.
4. `SHOW ALL TABLES` over N schemas issues **one** metadata query, not N (W2).
5. `partition_count` appears nowhere in `src/`.
6. The SQL suite stays green, and the 12-worker `make test` deadlock rate does
   not regress from PR #308's baseline (four consecutive runs, 177 passed).

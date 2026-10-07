# Spec 084: the shape of the metadata queries

Status: recon done, step 1 in progress.

Origin: mssql-ducklake's Query Store showed the extension's own `sys.*` queries
to be the most expensive compiles on a DuckLake catalog database (key discovery
~33 ms, single-table metadata ~27 ms). This spec measures every metadata query
the extension sends, on three catalog sizes, and finds where the time really
goes. Mostly it is not compilation: it is the shape of the queries.

The branch carries #413 (issue #412, the names-only "did you mean" load),
merged in by the owner's decision, so `TABLE_NAMES_SQL` is in scope here and
#413 is closed in favour of this PR.

## Recon (2026-10-07)

### Method

Local docker SQL Server 2025 (17.0.4075.5), 12 CPUs, `cost threshold for
parallelism` 5, `max degree of parallelism` 0 (the defaults). Three generated
databases with ERP-like tables:
- 5-40 columns of mixed types;
- two tables in three carry a primary key;
- one in ten carries a UNIQUE constraint;
- one object in fifty is a view.

| database | objects | schemas | columns |
|---|---:|---:|---:|
| `md_small` | 300 | 2 | ~7k |
| `md_medium` | 7,500 | 4 | ~166k |
| `md_huge` | 200,000 | 100 | 4.4M |

**Queries.** Each query was taken verbatim from the source of this branch's
base (spec 083 + #413), with the `ORDER BY` the code appends. The harness sends
every one through `sp_executesql`. The extension does that for the
parameterized ones (`@s` / `@t` / `@o`); `SCHEMA_DISCOVERY_SQL` and
`BULK_METADATA_ALL_SQL` it sends as plain batches. The plan and its cost are
the same either way.

**Measurement.**
- Server cost comes from Query Store, with a marker comment per variant:
  `count_compiles`, `avg_compile_duration`, `is_parallel_plan`, and CPU,
  duration and logical reads per execution.
- Compilation is measured by clearing the database's procedure cache before
  each cold run.
- Variants were interleaved. The compile comparison was repeated 3 times
  (30-90 compiles per variant). Any single figure that a repetition did not
  confirm is reported as retracted.

### F1: compilation is bounded, and `MAXDOP 1` does not reduce it

Compile cost per query, steady state, any of the three sizes:

| query | compile |
|---|---:|
| `PK_DISCOVERY_SQL_TEMPLATE` (key discovery) | 36-38 ms |
| `SINGLE_TABLE_METADATA_SQL_TEMPLATE` | 30-32 ms |
| `TABLE_DISCOVERY_SQL_TEMPLATE` | 23-25 ms |
| `COLUMN_DISCOVERY_SQL_TEMPLATE` | 12-13 ms |
| `TABLE_NAMES_SQL` (#413) | 7 ms |
| `SCHEMA_DISCOVERY_SQL` | 4-5 ms |
| row count, collation | < 1 ms |

- `OPTION (MAXDOP 1)` changes none of these: 36.5 vs 38.4 ms, 32 vs 32, 4.8
  vs 4.5, 25 vs 25 (interleaved, 90 compiles each on `md_huge`). No metadata
  query ever got a parallel plan.
- A first measurement had shown 317 -> 34 ms and 117 -> 5 ms. It was taken on
  freshly generated databases and did not survive repetition. It was most
  likely the first compiles building statistics on the system base tables.
  **Retracted.**
- A plan is compiled once per plan lifetime (one text per shape, spec 075/076).
  mssql-ducklake's full scale bench saw 10 compiles of the key query and 8 of
  the single-table query. Their recompiles ("Statistics changed") were 6 and 7.
- So compilation is a bounded, one-off cost, and nothing here is proposed for it.

### F2: `OBJECTPROPERTYEX(..., 'Cardinality')` runs once per COLUMN row

Every query that returns columns also returns the table's row count, as
`OBJECTPROPERTYEX(o.object_id, 'Cardinality')`, in the same SELECT list as the
columns. The server evaluates it on every column row, not once per object:

| one schema's bulk load, `md_huge` (2,000 tables, 44k columns) | CPU | reads |
|---|---:|---:|
| as sent today | 1.79 s | 1.87M |
| without the row count | 0.47 s | 0.68M |
| without `COLLATIONPROPERTY` / `TYPE_NAME` / `ORDER BY` (each) | 1.65-1.67 s | 1.74M |
| row count per object: CTE with `GROUP BY o.object_id` | **0.32 s** | 0.23M |
| row count per object: objects, then columns | 0.20 + 0.14 s | 0.07M + 0.02M |

- A plain CTE does not help. SQL Server inlines it, and the scalar moves back
  onto the column rows (measured: 1.80 s).
- The `GROUP BY` is an aggregation boundary, and the scalar runs once per
  object. The `GROUP BY` form beats "without the row count" (0.32 vs 0.47 s)
  because the two forms also differ in plan shape: not every saving in that
  row is the row count's.
- **The call is dearer than spec 071 measured.** On the whole catalog, 200k
  calls cost 23 s and 21.9M logical reads, ~110 reads per call. Spec 071
  measured 3 reads per call on tables without keys or constraints, in one
  schema. The source comments that quote 071's "3 logical reads / 84 ms for
  every count" are wrong for catalogs like these and are corrected in step 5.
- A single pass over `sys.dm_db_partition_stats` into a table variable costs
  4.3 s for the same 200k objects. With and without `OPTION (MAXDOP 1)`
  repeated: 4.28 / 4.38 s, 2.8M reads both. A first unhinted run at 7.2 s did
  not repeat.
- The same pass written inline as a derived table, inside the statement, never
  finished: 24+ minutes, killed. It must be materialised first.

### F3: a per-row `SCHEMA_NAME()` over the whole database

Spec 071 measured that no form of the schema predicate turns the `sysschobjs`
scan into a seek, and that still holds: the reads stay. The CPU of the scan,
though, depends on what runs per row.

`TABLE_DISCOVERY_SQL_TEMPLATE`, `md_huge`, one schema of 2,000 in 200k:

| form | CPU | reads |
|---|---:|---:|
| `SCHEMA_NAME(o.schema_id) = @s`, `OUTER APPLY` over `sys.indexes` | 617-686 ms | 76-132k |
| `o.schema_id = SCHEMA_ID(@s)`, `LEFT JOIN sys.indexes` | **216 ms** | 71k |

`TABLE_NAMES_SQL` (#413), `md_huge`:

| form | CPU |
|---|---:|
| `SCHEMA_NAME(o.schema_id)` per row | 680-700 ms |
| `JOIN sys.schemas` | **297 ms** |

Who runs these:
- `TABLE_NAMES_SQL` runs on every missing-name "did you mean" walk (#412).
- `TABLE_DISCOVERY_SQL_TEMPLATE` is nearly dead code. `EnsureTablesLoaded` has
  no live caller, so it runs only from `Refresh()`, i.e. `mssql_refresh_cache`
  (see F7).

### F4: the whole-catalog load

`BULK_METADATA_ALL_SQL`, `md_huge`:

| form | server CPU | reads |
|---|---:|---:|
| as sent today | 168-174 s | 115M |
| CTE without `GROUP BY` | 140 s | 105M |
| `@rc` + CTE with `GROUP BY`, one result set | 26.6 s | 23M |
| **`@rc`, then objects, then columns (two result sets), no hints** | **~21 s** (4.3 + 2.8 + 13.9) | 8.4M |
| objects with per-object `OBJECTPROPERTYEX`, then columns | ~50 s | — |

- About 14 s of what remains is `sys.columns` itself: 4.4M rows under per-row
  metadata visibility. That is the floor.
- **Client side.** Measured with `mssql_exec` draining each batch through
  `MSSQLSimpleQuery`. The variants were the `@rc` + CTE one-result-set batch
  and the `@rc` two-result-set batch, both as first measured with
  `MAXDOP 1` -- which F2 shows changes nothing.

  | form | wall | client CPU |
  |---|---:|---:|
  | one wide result set | 25.4 s | 2.3-2.8 s |
  | two narrow result sets | 22.2-24.1 s | 2.4-2.9 s |

  The wire carries less with two result sets: the object fields are no longer
  repeated on every column row.
- **Joining the two result sets with DuckDB's own join was considered and
  rejected:**
  - the client work is one hash lookup per column row into a map of objects,
    which is what the bulk load already does (it groups by `object_id`);
  - the load runs inside catalog code, often during binding, where a nested
    DuckDB query on the same `ClientContext` is not possible;
  - the client's share is ~2.5 s of ~22.

### F5: the single-table first touch

| `md_huge`, one table | CPU | reads |
|---|---:|---:|
| as sent today (object x columns in one SELECT) | 1.86 ms | 986 |
| object row (`OBJECTPROPERTYEX` once, shape by `LEFT JOIN`) + columns | **0.18 + 0.21 ms** | 42 + 8 |

The key discovery that rides in the same batch (spec 076) costs 0.25-0.31 ms
and is unchanged.

### F6: CTAS existence checks are literals

`CTASExecutionState::TableExists` / `SchemaExists` formatted the names into
`INFORMATION_SCHEMA.TABLES` / `SCHEMATA` queries as `'...'` literals. That
cost three things:
- every new name compiles (~10 ms);
- every new name leaves an ad-hoc plan in the cache;
- a non-ASCII name was converted to the database's code page, so the check
  could answer wrongly.

The parameterized forms, through the RPC path (spec 083's
`BuildExecuteSqlRequest`), measured on `md_huge`:

| check | statement | compile | per check |
|---|---|---:|---:|
| table | `SELECT 1 WHERE EXISTS (SELECT 1 FROM sys.objects WHERE object_id = OBJECT_ID(QUOTENAME(@s) + N'.' + QUOTENAME(@t)) AND type IN ('U', 'V'))` | 9.2 ms, once for all names | 0.11 ms, 3 reads |
| schema | `SELECT 1 WHERE SCHEMA_ID(@s) IS NOT NULL` | 0.65 ms | 0.02 ms |

- `type IN ('U', 'V')` is what `INFORMATION_SCHEMA.TABLES` lists: tables and
  views.
- A row comes back only when the object exists, as with the old queries.
  `ProbeExists` decides on `HasRows()`, so the contract **"row present <=>
  exists"** is kept.
- A plain CTAS on a name a VIEW holds fails on the server's 2714 before the
  check is reached, before and after the change
  (`ctas_existence_checks.test` passes on both builds).

### F7: `mssql_refresh_cache` sends one columns query per table

`Refresh()` lists each schema (`LoadTables`, `TABLE_DISCOVERY_SQL_TEMPLATE`),
then runs `COLUMN_DISCOVERY_SQL_TEMPLATE` once per table: one round trip per
table, 200k on `md_huge`. It never loads keys (`pk_loaded` stays false). The
whole-catalog load (F4) does the same work in one batch.

### Not worth changing

| query | compile | execution | reads |
|---|---:|---:|---:|
| COPY's target probes (`target_resolver.cpp`: shape, existence, columns) | 7-17 ms | 0.1-0.3 ms | < 12 |
| `SCHEMA_DISCOVERY_SQL` | 4-5 ms | 0.1-0.3 ms | — |
| collation | < 1 ms | — | — |
| row count (fixed in spec 083) | < 1 ms | — | — |

### Correctness

Every proposed shape was compared with the current one on `md_medium`. The
database was seeded with:
- a heap of 777 rows;
- a keyed table of 1,500 rows;
- a table partitioned in 4 partitions (450 rows);
- a clustered columnstore with 333 rows in its delta store;
- a table named `odd]name.x` with a geography column.

Each pair was materialised into `#old` / `#new`. The comparison counted the
rows of each side and ran `EXCEPT` both ways: equal counts plus empty EXCEPTs
also rule out duplicated rows, which `EXCEPT` alone would hide. **All 13
comparisons were equal**:

| comparison | rows |
|---|---:|
| `TABLE_DISCOVERY_SQL_TEMPLATE` (schema `hr`) | 1,878 |
| `TABLE_NAMES_SQL` | 7,503 |
| schema bulk load | 41,422 |
| single table x 7 (keyed, heap, key + UNIQUE, view, partitioned, columnstore, odd name) | per table |
| whole catalog, two passes | 165,667 |
| CTAS table / schema exists | 6 / 4 names |

The `@rc` row count (`SUM(row_count)` over `index_id IN (0, 1)`) equals
`OBJECTPROPERTYEX` on every one of them, partitioned and columnstore included.

## Decisions

- **D1: the row count leaves the column rows.**
  - **Single table.** One batch, three result sets:
    1. the object row: type, row count by `OBJECTPROPERTYEX` (once), shape by
       `LEFT JOIN`;
    2. its columns;
    3. the key discovery, as today. It moves from ordinal 1 to ordinal 2.

    Existence keeps today's rule. The current query joins `sys.columns`, so
    "found" means at least one column row. The new rule is "an object row AND
    at least one column": a synonym, procedure or scalar function that
    `OBJECT_ID` resolves stays "not found", and an inline / table-valued
    function keeps being accepted as today.
  - **One schema (`BulkLoadAll` per schema).** Objects with a per-object
    `OBJECTPROPERTYEX`, then columns, as two result sets; or the CTE with
    `GROUP BY`. Whichever is simpler to parse wins: 0.34 vs 0.32 s.
  - **Whole catalog (`LoadAllSchemasMetadataLocked` / `BULK_METADATA_ALL_SQL`).**
    Two result sets, objects and columns, grouped on `object_id` client-side
    as now. The row count source depends on scope and permission, decided in
    the batch itself:
    - **`@rc`** (one pass over `sys.dm_db_partition_stats` into a table
      variable), only when the load is unfiltered AND
      `HAS_PERMS_BY_NAME(DB_NAME(), 'DATABASE', 'VIEW DATABASE STATE') = 1`.
      The DMV needs that permission, and `OBJECTPROPERTYEX` does not: a
      `db_datareader`-only login must not lose its whole catalog. The pass also
      scans the whole database whatever `schema_filter` / `table_filter` say;
      on this box it breaks even with per-object calls at ~38k objects.
    - **otherwise `OBJECTPROPERTYEX`** once per object, on the objects
      statement.
  - **Filters.** The `schema_filter` / `table_filter` `LIKE` clauses go into
    BOTH statements: the columns statement re-joins `sys.objects` /
    `sys.schemas`. The client-side regex filter still applies after.
  - **Consistency between the statements.** Objects and columns are read by
    separate statements and can disagree under concurrent DDL. Two rules:
    - an object with no column rows is not published (today's join could not
      produce it either);
    - a column row whose `object_id` is not among the objects is dropped.
- **D2: no per-row `SCHEMA_NAME()`.**
  - `TABLE_DISCOVERY_SQL_TEMPLATE` matches `o.schema_id = SCHEMA_ID(@s)`.
  - `TABLE_NAMES_SQL` gets the schema name by `JOIN sys.schemas`.
  - The shape comes from `LEFT JOIN sys.indexes ... index_id IN (0, 1)` instead
    of `OUTER APPLY` with `MAX`: a table has exactly one of index 0 and 1, so
    the join cannot fan out.
  - The callers pass the exact name from the cached schema list, so
    case-sensitivity, `schema_filter` and visibility are unchanged.
  - An object whose schema is not visible is dropped by the join instead of
    being returned with a NULL schema name.
- **D3: the CTAS existence checks are parameterized,** in the forms of F6,
  through `BuildExecuteSqlRequest`.
- **D4: no query hints.** `MAXDOP 1` was measured and does nothing for these
  queries (F1, F2). `KEEPFIXED PLAN` is not proposed either: the recompiles
  observed on a DDL-heavy DuckLake run were a handful.
- **D5: the bulk loads carry the keys** (mssql-ducklake's preload finding).
  - **Today:**
    - `BulkLoadAll` (per schema) and `LoadAllSchemasMetadataLocked` build fresh
      `MSSQLTableMetadata` without `pk_info`, replacing what a single-table
      load had learned (`pk_loaded = false`).
    - The per-schema path's comment promises to "seed the staged entry from the
      cache"; the code does not.
    - Inside a transaction, the first statement that touches such a table
      rebuilds its entry without a key (`GetEntryInTransaction`, step 4), and
      the scan bind runs `EnsurePKLoaded` on the pinned connection: one extra
      round trip per table per transaction.
    - The key is never published back: `PublishTableMetadata` refuses to
      overwrite a fresh shared entry.
    - Measured, key-query executions over 15 transactions each scanning two
      tables (`sys.dm_exec_query_stats`, noisy):

      | | key-query executions |
      |---|---:|
      | without a preload | 2 |
      | after a preload of the schema | 22 |
  - **Change.** The bulk loads add a key result set: every in-scope object's
    usable unique-index rows, with `object_id`, ordered
    `object_id, index_id, key_ordinal` (the whole-catalog load has no other
    ORDER BY), or grouped and sorted client-side.
    - Its SELECT list and filters are `PK_DISCOVERY_SQL_TEMPLATE`'s, generated
      from one definition so the two cannot drift (the #345 lesson).
    - Every in-scope table goes through
      `FinalizeChoice(database_collation_)` and gets `pk_loaded = true`,
      including tables with no key rows (the "no usable key" answer is an
      answer).
    - `MSSQLTableEntry`'s constructor already seeds `pk_info_` from
      `metadata.pk_loaded`; `mssql_table_set.cpp` needs no change.
  - **Cost.** The key result set's cost on `md_huge` is measured before it
    ships, because it lands on every whole-catalog load (every listing).
  - **Fallback.** If it is too dear, the cheaper alternative is to carry an
    existing entry's keys forward in the per-schema merge, which is what the
    comment promised.
- **D6: `Refresh()` goes through the whole-catalog load** (F7), picking up D1
  and D5, instead of one columns query per table.

## Risks

- **`sys.dm_db_partition_stats` and table variables on other platforms.** The
  permission fallback (D1) covers SQL Server and Azure SQL. Fabric Warehouse
  and Synapse (`IsFabricEndpoint` / `IsSynapseEndpoint`) are unverified for
  both the DMV and `DECLARE @rc TABLE`, so they take the `OBJECTPROPERTYEX`
  form unconditionally until verified.
- **sysrowsets returns.** The 1205 deadlock-victim retry in
  `RunMetadataQuerySets` exists because catalog joins against sysrowsets
  deadlocked under concurrent DDL (spec 071 removed them). The `@rc` pass
  reads sysrowsets again. The retry and reset cover it, but the 12-worker
  `make test` deadlock rate is checked against PR #308's baseline.
- **Table-variable cardinality.** Before compat level 150, a table variable is
  estimated at one row. The `@rc` join is an index seek by `object_id` (its
  primary key), which that estimate suits, but the plan is to be re-checked at
  compat 140/150. The extension supports SQL Server 2019+ and Azure.
- **Row count edge cases not in the seed.** These are to be added to the
  correctness comparison:
  - indexed views: the DMV has index 1 rows, so `@rc` may report a count where
    `OBJECTPROPERTYEX` reports 0;
  - memory-optimized tables;
  - a columnstore with deleted rows in compressed rowgroups;
  - counts above 2^31: `Cardinality` is an int.

## Plan

The branch is `spec/084-metadata-query-shapes`, branched from
`spec/083-rpc-params` (#414), with #413 merged in. Its PR targets
`spec/083-rpc-params`, by the owner's decision, because they touch the same
metadata code. One PR, these steps:

| step | what |
|---|---|
| 0/n | this spec and `recon/` |
| 1/n | D2 + D3: `TABLE_DISCOVERY_SQL_TEMPLATE`, `TABLE_NAMES_SQL`, the CTAS existence checks |
| 2/n | D1, single table: object row, columns, keys as three result sets |
| 3/n | D1, the bulk loads: one schema; the whole catalog with `@rc` and its fallbacks |
| 4/n | D5: the bulk loads carry the keys (measured on `md_huge` first); D6: `Refresh()` |
| 5/n | docs (`DATAMODEL.md`, `CLAUDE.md`, CHANGELOG, the source comments F2 contradicts); delete the dead `LoadAllTableMetadataForSchema`; the measurements |

## Tests

- **Shapes are behaviour-neutral, so the existing suites pin them:**
  - catalog, rowid, CTAS;
  - `scan_cardinality.test`: the row count reaches the planner;
  - `issue_375_376`: preload;
  - `missing_table_empty_schema.test`: #413;
  - partitioned tables (spec 049);
  - columnstore `index_kind`.
- **Additions:**
  - `ctas_existence_checks.test` (step 1): a name a view holds, a missing
    schema, `]` and `.` in a name; it passes on the old and the new build;
  - a table name with `]` and `.` through listing, single touch and preload;
  - **D5:** after a preload, `SET mssql_test_fail_metadata_after_rows = 1`,
    then a transaction that scans preloaded tables must succeed. Any metadata
    query in it would fail, so this is deterministic where the plan-cache
    counter is not;
  - **D1 permission fallback:** a login without `VIEW DATABASE STATE` lists the
    catalog and gets the same row counts;
  - D1 consistency: a filtered whole-catalog load returns exactly the
    filtered objects with their columns.
- **Measured in the PR:** the F2-F5 tables again on `md_huge`, the D5 key
  result set's cost, and mssql-ducklake's catalog-load and preload numbers.

## Reproduce

- `recon/` holds the material:
  - `gen_proc.tsql`: the generator, `dbo.md_gen @schema, @n`;
  - `queries.json`: every query and variant measured, extracted from the source;
  - `build_harness.py` / `read_qs.py` / `qs_reset.py`: the Query Store harness.

  Each script prints DuckDB CLI input with a `${DSN}` placeholder, run against
  the database under test.
- Query Store must be ON with `QUERY_CAPTURE_MODE = ALL`.
- `ALTER DATABASE SCOPED CONFIGURATION CLEAR PROCEDURE_CACHE` before each cold
  run.
- Interleave the variants, and repeat any comparison: the first compiles and
  the first runs on a new database are not representative (F1, F2).

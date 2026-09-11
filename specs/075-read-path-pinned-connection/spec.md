# Spec 075 — The read path on a pinned connection: bind without executing, a sink that reads its own catalog, one plan per shape for metadata, and parameters

Four issues, one seam. Inside an explicit transaction the extension pins **one**
TDS connection per catalog and routes every read and write on that catalog
through it; a TDS connection carries one open result set at a time. Everything
below is what that costs today and what changes:

- [#336](https://github.com/hugr-lab/mssql-extension/issues/336) — `mssql_scan`
  learns its result's columns by **executing the query at bind**.
- [#329](https://github.com/hugr-lab/mssql-extension/issues/329) — two
  `mssql_scan`s (or one beside a catalog scan) in one statement inside a
  transaction collided on the pinned connection.
- [#239](https://github.com/hugr-lab/mssql-extension/issues/239) — a sink that
  reads from the catalog it writes to fails inside a transaction; documented as
  a limitation, solved in the planner by duckdb-postgres.
- [#334](https://github.com/hugr-lab/mssql-extension/issues/334) — the per-table
  metadata queries carry the table name in their text, so every first touch of
  a table compiles a plan.
- and, from the same reconnaissance, **parameters** — `mssql_exec_params`
  and `mssql_scan_params`: the user's own repeated statements have the same
  problem as #334 and the same cure, and nothing lets them use it today.

Reconnaissance, spec and implementation in one PR on one branch from `main`
(`spec/075-read-path-pinned-connection`). § 0 is what was measured before a
line was written; § 1 what it means, including the two issues `main` has
already half-solved; § 2 the work.

## 0. Ground — measured on `main` at `d4f4c9d`, SQL Server 2025 in docker

**Which shapes fail inside an explicit transaction today.** Source and target
in the same attached catalog, `p75_src` three rows:

| statement inside `BEGIN … ROLLBACK` | today |
| --- | --- |
| `INSERT INTO t.dbo.p75_dst SELECT … FROM t.dbo.p75_src` (3 rows) | works |
| the same INSERT from a 5 000-row source | **fails** at the second batch: `INSERT failed at statement 1 (rows 1000-999): Cannot execute: connection not in Idle state (current: Executing)` |
| `COPY (SELECT … FROM t.dbo.p75_src) TO 't.dbo.p75_dst' (FORMAT bcp, CREATE_TABLE false)` | **fails**: `MSSQL COPY: Connection is busy executing another query` |
| `CREATE TABLE t.dbo.p75_ctas AS SELECT … FROM t.dbo.p75_src` | works (outside the transaction, as documented) |
| `UPDATE … WHERE id IN (SELECT id FROM t.dbo.p75_src)` and `DELETE …`, 5 000 rows | work |
| two `mssql_scan`s in one statement (#329's exact query) | works, 6 |
| `mssql_scan` beside a catalog scan as scalar subqueries (#329's comment) | works, 6 |

The last two are #329's failing cases on v0.2.5. They pass on `main` because
#316/#314 made `mssql_scan` **drain its result at bind** inside a transaction
(`MSSQLScanBind`, `src/mssql_functions.cpp:138`), and the optimizer pass filed
under #239 materialises catalog scans when two or more share a pinned
connection (`MaterializeSharedConnectionScans`, `src/table_scan/mssql_optimizer.cpp:649`).
Of the four issues, the read-side collisions are solved; what is left is the
*way* they were solved, two sinks, and the metadata compile.

**Bind executes the query, side effects included.**

```sql
EXPLAIN SELECT * FROM mssql_scan('t', 'INSERT INTO dbo.p75_log VALUES (1); SELECT 1 AS x');
SELECT * FROM mssql_scan('t', 'SELECT count(*) FROM dbo.p75_log');   -- 1
```

`EXPLAIN` inserted the row. So does `DESCRIBE`, `PREPARE`, and the bind of any
statement the plan later decides not to read.

**Two ways to learn a result's shape without running the query**, both plain
`SQL_BATCH` through the existing parser:

```sql
-- the supported one (SQL Server 2012+, Azure SQL): the shape as DATA, ~4 ms
EXEC sp_describe_first_result_set N'SELECT 1 AS x, CAST(NULL AS nvarchar(10)) AS y', NULL, 0
--   name  system_type_name  is_nullable  max_length  precision  scale  collation_name
--   x     int               false        4           10         0      NULL
--   y     nvarchar(10)      true         20          0          0      SQL_Latin1_General_CP1_CI_AS

-- the prepared one: the statement's own COLMETADATA, then the handle
DECLARE @h int; EXEC sp_prepare @h OUTPUT, NULL, N'SELECT 1 AS x, CAST(NULL AS nvarchar(10)) AS y'; SELECT @h AS handle
--   result set 1: COLMETADATA (x int, y nvarchar(10)), zero rows   <- DESCRIBE of the scan shows x INTEGER, y VARCHAR
--   result set 2: handle = 2
```

`sp_prepare` executes nothing (a prepared `INSERT …; SELECT 1` left its table
empty) and prepares a `#temp`-table batch without complaint — but for a
**batch** (more than one statement, or one that creates a temp table) it
returns **no metadata result set**, only the handle. The metadata comes for a
single statement.

**Where compilation happens**, `sys.dm_exec_query_optimizer_info`
(`optimizations`) around each step, on this docker server:

| step | counter |
| --- | ---: |
| `sp_describe_first_result_set` of a 2-view join | +1 |
| `sp_prepare` of the same statement (the batch + the prepared plan) | +2 |
| ad-hoc execution of the same text | +1 |
| ad-hoc execution of the same text **again** | **+1** |

The last row is #334's phenomenon: on this server (and on any with
`optimize for ad hoc workloads`, where a first execution stores only a stub)
an ad-hoc text compiles on every execution until it has been seen twice, so
"describe, then execute" is two compiles of the statement and "prepare, then
execute by handle" is one.

**`sp_executesql` works through the parser since #332**, and its plans are
reused. The single-table metadata query with `OBJECT_ID(@n)` and `@n =
N'[dbo].[test]'` returns its rows through `mssql_scan`. Three
`EXEC sp_executesql N'INSERT INTO p75_ins(a, b) VALUES (@a, @b)', N'@a int, @b
nvarchar(100)', @a = …, @b = …` with different values: **one** entry on the
optimizer counter; the same `SELECT` text twice ad hoc: two. (Three ad-hoc
`INSERT … VALUES (1, N'x')` also compiled once — SQL Server's simple
parameterization catches a bare VALUES list, and nothing with a JOIN.)
`mssql_exec` reports the affected rows correctly through the procedure — 2
for a two-row `INSERT`, 4 for an `UPDATE` — because the counts ride on the
inner DONEINPROC tokens, which #332 reads. And the compile #334 is meant to
remove, on this docker server: the first touch of a fresh table's metadata query
`RunMetadataQuery: completed in 29ms` / `30ms` for two tables; the same table
after `mssql_invalidate_cache(t, 'dbo', table)`: `0ms`. #334 measured 28–37 ms
and 45–51 ms (PK query) on a 1000-table DuckLake catalog, and 0–1 ms for every
table after the first under `sp_executesql` or `PARAMETERIZATION FORCED`.

## 1. The findings

### F1 — `mssql_scan` binds by executing, and everything downstream is shaped by that

`MSSQLScanBind` runs the query through `MSSQLQueryExecutor` to read the
COLMETADATA, then has a result stream on its hands that execution has not
asked for. Outside a transaction it parks it in a per-catalog registry
(`RegisterStream` / `RetrieveStream`, spec 047 US3) for `InitGlobal` to pick
up, so the query does not run twice; inside a transaction it cannot park it
— the stream holds the transaction's one connection — so it drains the whole
result into a `ColumnDataCollection` at bind and closes the stream (#316).

What that costs: the statement runs at plan time (§ 0: `EXPLAIN` inserts);
inside a transaction every `mssql_scan` is materialised in full at bind whether
or not the plan reads it, and bind cannot be cancelled; the registry exists
only to bridge bind to init; and the rule the DuckLake manager still follows
on v0.2.5 — "one `mssql_scan` per statement in a transaction" — is a rule about
the shape of a plan, which a caller cannot control.

### F2 — Two describes; the supported one is the default, the prepared one is opt-in

**`sp_describe_first_result_set`** is Microsoft's replacement for FMTONLY,
present on everything TDS 7.4 reaches (SQL Server 2012+, Azure SQL, Synapse;
Fabric to be confirmed on the Azure lane). It is a procedure, so it needed
#332; it returns the first result set's shape **as rows** — `system_type_name`,
`max_length`, `precision`, `scale`, `collation_name`, nullability — which are
exactly the inputs `sys.columns` gives the catalog, and the catalog already
maps them: `MSSQLColumnInfo::MapSQLServerTypeToDuckDB`
(`mssql_column_info.cpp:194`). Where the shape cannot be determined it says
so by number: 11526 for a batch that creates and reads a `#temp` table, 11509
for a procedure whose branches return different shapes — an error at bind,
never metadata that execution then contradicts. It touches no session state.
Its cost is one parse-and-bind (§ 0 counts it as an optimization) on top of
the execution's own compile.

**`sp_prepare`**, called from a T-SQL batch, returns the statement's own
COLMETADATA — the tokens `MSSQLResultStream` already turns into types with
`TypeConverter::GetDuckDBType`, no second mapping — and a handle that
`sp_execute` runs **without compiling again**. Two limits: it describes a
*single statement* (a batch gets a handle and no metadata, § 0), and the
handle lives in the session that prepared it, so bind and execution must
share a connection. Inside a transaction they do (the pinned one). In
autocommit the bind would have to hold its pooled connection until
`InitGlobal` — which is what today's stream registry does with an *open*
result set, so it is not a new cost, but it is a reservation for every scan.

**`SET FMTONLY ON`** does the prepared one's job without the handle, and is
not used: deprecated since 2008 R2, it is session state (`SET FMTONLY OFF`
must run or the pinned connection returns metadata for the rest of the
transaction), and for a procedure with branch-dependent result sets it
returns the first branch's shape silently where `sp_describe_first_result_set`
refuses.

So: the default `mssql_scan` describes with `sp_describe_first_result_set`
and executes ad hoc; `prepared := true` describes with `sp_prepare` and
executes by handle, one compile, for the single-statement queries a caller
runs often enough to care. Both fall back to executing at bind when the
describe fails.

### F3 — Two sinks collide: the ones that send while the source still streams

§ 0's table is explained by who touches the pinned connection when:

- **UPDATE**, **DELETE**: their executors run *deferred* inside a transaction
  (`defer_execution_`, `mssql_update_executor.cpp:47`): rowids are buffered as
  the source streams and every batch goes out in `Finalize`, after the source
  is drained, so the connection is never asked to do two things at once.
- **INSERT … SELECT** does not defer. `MSSQLInsertExecutor::Execute` flushes a
  batch the moment it fills (`mssql_insert_batch_size`, 1 000 rows) on the
  connection the provider gives it — the pinned one — while the source
  catalog scan is still streaming on it. Three rows pass because their one
  batch goes out in `Finalize`; 5 000 fail at row 1 000 (§ 0).
- **CTAS**: outside the transaction by design (autocommitting DDL, loads on
  connections of its own), so its source streaming on the pinned connection
  meets nothing.
- **COPY (bcp)**: `BCPCopyInitGlobal` takes the pinned connection and checks
  it is Idle **before the source has produced a row**; the source catalog scan
  is alone in the plan, so `MaterializeSharedConnectionScans` (which fires at
  two or more scans) leaves it streaming, and it is `Executing`. `INSERT BULK`
  is a streaming protocol with no deferred form.

Both are the same defect: a sink that sends on the pinned connection before
its source is drained. Deferring INSERT the way UPDATE does would buffer SQL
text for the whole source in process memory; materialising the *source*
buffers rows through the buffer manager, which spills, and is one mechanism
for both sinks.

duckdb-postgres, with the same one-connection constraint, fixes the class in
the planner: `PlanCreateTableAs` / `PlanInsert` walk the physical plan and set
`requires_materialization` on every scan of their own catalog
(`MaterializePostgresScans`); the scan then drains itself into a
`ColumnDataCollection` in its `InitGlobal`. Our catalog scan already has that
switch and that `InitGlobal` (`table_scan.cpp:523-720`, under the catalog's
`MaterializeMutex()`); what is missing is the trigger for the sink case.

### F4 — One ad-hoc plan per table

Six metadata templates put a name into their text. Per table:
`SINGLE_TABLE_METADATA_SQL_TEMPLATE` (`mssql_metadata_cache.cpp:108`, the
cache-miss load), `COLUMN_DISCOVERY_SQL_TEMPLATE` (`:227`, a table's column
reload), `PK_DISCOVERY_SQL_TEMPLATE` (`mssql_primary_key.cpp:38`) and
`ROW_COUNT_SQL_TEMPLATE` (`mssql_statistics.cpp:14`, the planner's cardinality
on first touch) — `WHERE o.object_id = OBJECT_ID('[dbo].[x]')`, `WHERE s.name =
'dbo' AND o.name = 'x'`. Per schema: `TABLE_DISCOVERY_SQL_TEMPLATE` (`:89`) and
`BULK_METADATA_SCHEMA_SQL_TEMPLATE` (`:136`). SQL Server caches ad-hoc plans by
text, so each table is its own compile of a five-view join — and under
`optimize for ad hoc workloads`, common in production, the first execution
stores only a stub, so a touch-each-table-once workload compiles every table
however large the cache. #334 measured the two hottest shapes at 28–37 ms and
45–51 ms per table on a 1000-table catalog, 0.9 s of a 1.9 s first read. All
six are plain `SQL_BATCH` text; `sp_executesql` with the names as parameters
is also plain `SQL_BATCH`, one plan per shape, and its RETURNSTATUS/DONEPROC
tail is what #332 taught the parser to read.

### F5 — A caller's repeated statement has #334's problem, and no way to the cure

DuckLake's SQL Server manager issues the same statements per commit with
different literals — snapshot ids, file ids, timestamps — and each text is its
own ad-hoc compile, twice on a server with `optimize for ad hoc workloads`
(§ 0). W4's fix for the extension's own queries is `sp_executesql` with the
varying parts as parameters: one plan per statement text, shared across calls,
sessions and transactions. Nothing exposes that to a caller: `mssql_exec` and
`mssql_scan` take a string, and a value has to be spelled into it. A
prepared handle (F2) would not do: it lives in one session and dies with it;
the reuse a caller wants is across calls, which is what the plan cache keyed
on the statement text gives for free.

The two obstacles are the ones the design has to answer: how a caller passes
values through SQL, and what SQL Server type each value is declared as —
declarations are part of the plan's cache key, and a `varchar` column compared
to an `nvarchar` parameter is converted on the column side and loses its
index seek.

## 2. The work

### W1 — Bind describes; it does not execute

`mssql_scan(context, query)` gains an optional named parameter,
`prepared := false`.

**Default.** `MSSQLScanBind` runs `EXEC sp_describe_first_result_set N'<query>',
NULL, 0` through `MSSQLSimpleQuery` on whatever connection the provider gives
it — pooled in autocommit, the pinned one in a transaction — and builds
`return_types` / `names` from the rows with `MapSQLServerTypeToDuckDB`,
called the way the stream's mapping behaves (no `MSSQL_VARCHAR(n)` extension
types: `mssql_scan` reports plain `VARCHAR` today and keeps doing so). A
column the server names `NULL` (an unnamed expression) gets the name the
stream would have given it. Nothing is left open, nothing is registered.

**`prepared := true`** changes how the query *executes* — once, by handle,
compiled at bind — and takes the shape from wherever it is available. Bind
runs `DECLARE @h int; EXEC sp_prepare @h OUTPUT, NULL, N'<query>'; SELECT @h`
through a small reader on the token parser that takes result set 1's
COLMETADATA, if the server sent one, and the last result set's one row (the
handle). A **single statement** comes with its COLMETADATA — the types via
`GetDuckDBType`, exactly as execution would map them, one compile in total.
A **batch** (several statements, § 0) comes with a handle and no shape, so
bind then asks `sp_describe_first_result_set` for the shape, as the default
mode does; the batch is still compiled once and executed by handle. The bind
data keeps the handle and, in autocommit, the connection it lives on
(released to the pool, with reset, when the bind data is destroyed — the
`weak_ptr` pool handle pattern `MSSQLResultStream` already uses;
`RESET_CONNECTION` frees the server-side handle); in a transaction it keeps
only the handle, which lives on the pinned connection, where the prepare
batch and the later `sp_execute` naturally run in the same session.

**The fallback is today's code**, for both modes: if the describe raises
(11526, 11509, an unsupported server) bind executes the query as it does now
— registry handoff in autocommit, drain at bind in a transaction — and says
so under `MSSQL_DEBUG`. So a `#temp`-table batch that works today keeps
working, at today's cost, and nothing needs a new option.

### W2 — Execution moves to `InitGlobal`, and materialises there in a transaction

`MSSQLScanInitGlobal` runs the query — `EXEC sp_execute @h` on the bound
connection when `prepared`, the text itself otherwise (the "registry miss"
branch it already has). Outside a transaction it streams, as today. Inside one
it materialises
into a `ColumnDataCollection(context, …)` under the catalog's
`MaterializeMutex()` and releases the connection before returning — the same
mechanics as the catalog scan's `InitGlobal` (#239), which is why two sources
on one pinned connection serialise at init instead of colliding. The result is
paid once, when the plan reads it, and only for results the plan reads.

Init checks the executed COLMETADATA against the bound types (count and
`LogicalType`), and a mismatch — the describe's mapping and the stream's
disagreeing on some type, or a procedure describing one shape and executing
another — fails with a message that names both shapes and the query, rather
than reading rows into the wrong columns. Today that mismatch cannot occur
because bind executed; after W1 it is the one new failure mode, and it is
loud. A prepared scan sends no `sp_unprepare`: the handle lives in one session, and that session is reset when the connection returns to the pool (autocommit — the held connection is released when the bind data dies) or when the transaction ends (the pinned connection); a round trip to free what the reset frees anyway would be the second compile's cost in another form.

`RegisterStream` / `RetrieveStream` and `result_stream_id` stay, used by the
fallback only; their comments say so.

### W3 — A sink into the catalog it reads from, inside a transaction

`MaterializeSharedConnectionScans` gains the sink case: in a transaction, when
the plan holds a sink into catalog *C* — a `LogicalCopyToFile` whose
`function.name == "bcp"` (`BCPCopyBindData::catalog_name`), or a
`LogicalInsert` whose `table` belongs to an mssql catalog — every catalog scan
of *C* is materialised. One scan is enough: the sink is the second user of the
connection. `mssql_scan` sources need no rule: W2 materialises them at init
in a transaction regardless. This is `MaterializePostgresScans` with the
trigger in the optimizer extension rather than in each `Plan*` hook, so the
COPY, which has no catalog hook, is covered by the same lines.

Either initialisation order works, because materialisation completes inside
the scan's `InitGlobal`: if the sink's `InitGlobal` runs first it finds the
connection Idle, the scan then drains on it and returns it Idle before a row
reaches the sink; if the scan's runs first the sink finds it Idle. The
transaction's connection is one `shared_ptr` handed to both — the sharing a
small INSERT already relies on today.

`BCPCopyInitGlobal`'s Idle check stays; its message stops telling the user to
"read into a local table first". The documented limitation in
`website/docs/writing/copy.md` and `transactions.md` becomes a description of
the cost (the source is buffered).

UPDATE and DELETE already defer; CTAS is outside the transaction by design.
Neither changes, and the INSERT executor is not touched: its source is what
changes. That is deliberate with spec 062 in view — it moves INSERT onto the
bulk-load path, where deferral is not available at all — and the trigger
here is the `LogicalInsert`, not the executor behind it, so a BCP INSERT is
covered by the same lines. It also answers the same-catalog half of 062's D3
("can `INSERT BULK` go out on a pinned connection mid-transaction"): COPY
already does exactly that today when its source is elsewhere
(`copy_transaction.test`), and with the source materialised the connection is
Idle when the bulk load starts.

### W4 — One plan per metadata shape

All six templates become

```sql
EXEC sp_executesql N'<template with OBJECT_ID(QUOTENAME(@s) + N''.'' + QUOTENAME(@t))>',
     N'@s sysname, @t sysname', @s = N'<schema>', @t = N'<table>'
```

— schema and table as two `sysname` parameters (each ≤ 128 characters, which
`[schema].[table]` in one `sysname` is not), quoted by the server; the
per-schema templates take `@s` alone, `ROW_COUNT` compares `@s` / `@t`
directly. The text is identical for every name, so one plan serves all of
them. The values are still embedded as `N'…'` literals in the batch and keep
today's quote doubling. Every one of them runs on `MSSQLSimpleQuery`
(`ExecuteMetadataQuery`, the PK loader, the statistics provider), which parses
the procedure's RETURNSTATUS and DONEPROC since #332; the row callbacks see the
same rows. The per-schema pair is one compile per schema rather than per
table and would have been left alone on cost; it is included so the whole
metadata path has one rule — a name never reaches the server inside the
query text.

### W5 — `mssql_exec_params` and `mssql_scan_params`

Two new functions, siblings of the two that exist; the existing ones keep
their signatures and their raw-batch meaning:

```sql
SELECT mssql_exec_params('cat', 'INSERT INTO dbo.t(a, b) VALUES (@a, @b)', {'a': 1, 'b': 'x'});
SELECT * FROM mssql_scan_params('cat', 'SELECT * FROM dbo.t WHERE a > @a', {'a': 1});
SELECT * FROM mssql_scan_params('cat', 'SELECT … WHERE a > @a', {'a': 1}, prepared := true);
```

The third argument is a **STRUCT** whose keys are the parameter names and
whose values are the values — a STRUCT and not a LIST or a MAP because its
children carry **their own types** (a MAP's values share one), and not
variadic arguments because names in the statement beat positions. Types come
from the STRUCT's child types at bind, so the declaration text is fixed per
call site, which is what makes the plan reusable. Separate names rather than
overloads of `mssql_exec` / `mssql_scan`: a third argument of type ANY on
the old names would make `mssql_exec('c', sql, NULL)` a question for the
overload resolver, and "this function takes a STRUCT of parameters" is a
sentence the docs and the error messages can say plainly.

**The batch** is `EXEC sp_executesql N'<statement>', N'<declarations>',
@a = <literal>, @b = <literal>` — the statement with its quotes doubled, the
declarations built from the child types, the values as T-SQL literals. Plain
`SQL_BATCH`, as W4. `mssql_exec_params` returns the affected rows as
`mssql_exec` does (§ 0).

**Declarations** come from the codec's DDL type mapping — the one CTAS uses
to create a column for a DuckDB type, so a value declared this way
round-trips: `INTEGER → int`, `BIGINT → bigint`, `SMALLINT → smallint`,
`UTINYINT → tinyint` (and `TINYINT`, signed, `→ smallint`), `BOOLEAN → bit`,
`FLOAT → real`, `DOUBLE → float`, `DECIMAL(p,s) → decimal(p,s)`,
`HUGEINT → decimal(38,0)`, `UBIGINT → decimal(20,0)`, `DATE → date`,
`TIME → time(6)`, `TIMESTAMP → datetime2(6)`, `TIMESTAMP_S/MS/NS →
datetime2(0/3/7)`, `TIMESTAMPTZ → datetimeoffset(6)`, `BLOB →
varbinary(max)`, `UUID → uniqueidentifier`. **Strings**: a plain `VARCHAR`
value is `nvarchar(4000)`, or `nvarchar(max)` past 4 000 characters — two
declaration texts, the buckets `Microsoft.Data.SqlClient` uses; a value cast
to `MSSQL_VARCHAR(n[, collation])` or `MSSQL_NVARCHAR(n)` (spec 060) is
declared exactly so, which is how a caller compares a `varchar` column to a
`varchar` parameter and keeps the seek. A `NULL` whose type is known
(`NULL::INTEGER`, a typed struct child) is passed as `NULL` of that type; a
bare `NULL` has no type and is refused with a message asking for the cast.
Nested types (LIST, STRUCT, MAP, UNION) are refused: table-valued parameters
need RPC.

**When the derived declaration is wrong, the caller states it.** The
derivation cannot know the column a value meets: a `TIMESTAMP` against a
`datetime` column, a `DECIMAL(2,1)` against `money`, a string against a
`varchar` column with a collation the cast cannot spell, a `datetime2(7)`
where the plan wants `date`. An optional fourth argument is the declaration
list itself — the text `sp_executesql` takes as its second argument:

```sql
SELECT mssql_exec_params('cat', 'UPDATE dbo.t SET seen = @ts WHERE code = @c',
                         {'ts': now(), 'c': 'AB'}, '@ts datetime, @c varchar(8)');
```

With it, the STRUCT supplies values only; names are matched to the
declarations, a name declared but not supplied or supplied but not declared
is an error naming it, and the value's literal is converted by the server to
the declared type (`@c varchar(8) = N'AB'`). Without it, the table above
applies. This is the escape hatch for every type the derivation gets wrong,
and it is exactly the string a user would write for `sp_executesql` by hand.

**Names** are T-SQL identifiers: a key must match `[A-Za-z_][A-Za-z0-9_]*`
(it becomes `@key`), and two keys that differ only in case are one parameter
to the server and are refused as duplicates.

**Values** are rendered by `codec::FormatSqlLiteral` — the INSERT text path's
formatter, so quoting, the `N` prefix, `0x` for binary and the ISO datetime
forms are the ones already exercised — and the server converts each literal
to its declared type, derived or stated.

**`mssql_scan_params`** binds and executes as `mssql_scan` does (W1, W2)
with the declarations in hand: the describe is
`sp_describe_first_result_set N'<statement>', N'<declarations>', 0` — the
procedure's second argument exists for exactly this — and `prepared := true`
prepares `sp_prepare @h OUTPUT, N'<declarations>', N'<statement>'` and
executes `sp_execute @h, <literals>`. The fallback (W1) executes the
`sp_executesql` batch at bind.

**Not in the form**: OUTPUT parameters and return values (`SELECT` them in
the batch through `mssql_scan_params`), and a `prepared` flag on
`mssql_exec_params` — with parameters the plan is already a cached one, and
a scalar function has no named arguments to hang a flag on; simple
parameterization covers the bare-literal case of `mssql_exec` (§ 0).

### W6 — Tests

- **`test/sql/query/mssql_scan_bind_describe.test`** (docker):
  `EXPLAIN` / `DESCRIBE` of a batch with an INSERT leaves the table empty
  (§ 0 inverted — fails on `main`); a batch the describe refuses (creates and
  reads a `#temp` table) still returns its rows, inside and outside a
  transaction (the fallback); #329's two shapes stay as the regression guard;
  a 20 000-row `mssql_scan` inside a transaction is read correctly
  (materialised at init).
- **`test/sql/query/mssql_scan_describe_types.test`**: one `SELECT` casting a
  value to every SQL Server type the extension reads — the integers, bit,
  float/real, decimal, money, (n)char/(n)varchar including `max`, binary,
  uniqueidentifier, date/time/datetime2/datetimeoffset/smalldatetime,
  xml — scanned through the default path and through `prepared := true`,
  `DESCRIBE` equal to the result's types in both. This is where the
  describe's mapping and the stream's are held to agree; W2's init check is
  what fails if they ever drift.
- **`test/sql/query/mssql_scan_prepared.test`**: `prepared := true` returns
  the rows in autocommit and inside a transaction, for a single statement
  and for a batch (shape from the describe, execution by handle); the same
  scan read twice in one statement; a `#temp` batch under `prepared` takes
  the fallback and still returns rows; `EXPLAIN` of a prepared scan prepares
  and leaves no side effect; two prepared scans inside one transaction.
- **`test/sql/transaction/sink_reads_own_catalog.test`**: § 0's failing COPY
  and the 5 000-row INSERT … SELECT succeed inside a transaction; the rows are
  visible before `COMMIT`; `ROLLBACK` undoes them; the source is untouched.
  Both halves fail on `main`.
- **`test/sql/query/mssql_params.test`**: every type in W5's table passed as
  a parameter to `mssql_scan_params('…', 'SELECT @p AS v', {'p': <value>})`
  comes back with the same value and the expected `DESCRIBE` type; a string with a
  quote, a bracket and a non-ASCII character; `MSSQL_VARCHAR(20)` declared as
  `varchar(20)`; the declaration override (`'@ts datetime'`) applied, and its
  three errors (undeclared, unsupplied, invalid name); a typed NULL; a bare
  NULL, a LIST and a key with a space refused with the messages naming the
  fix; `mssql_exec_params` returns the affected rows; parameters inside a
  transaction, with `prepared := true`, and through the describe.
  Plan reuse is measured in the PR with the optimizer counter, not asserted.
- **`test/sql/catalog/metadata_query_parameterized.test`**: a schema and a
  table whose names carry a quote and a bracket load their table list,
  columns, PK and row-count estimate through the parameterised text;
  `mssql_invalidate_cache` + reload still works. The compile saving is
  measured in the PR (§ 0's `RunMetadataQuery` timings on three fresh tables:
  the first ~30 ms, the next two ≤ 2 ms), not asserted.
- Existing: `transaction_mssql_scan_materialization.test`,
  `transaction_multi_scan_materialization.test`, the copy and ctas transaction
  tests, `regression/issue_316` stay green; the sqllogictests that DESCRIBE an
  `mssql_scan` no longer execute it.

### W7 — Documentation

`website/docs/reading/queries.md` (`mssql_scan`: bind describes, init
executes, `prepared := true` and when it pays, what the describe refuses and
the fallback, and `mssql_scan_params` with the type table);
`website/docs/reference/functions.md` (`mssql_exec_params` /
`mssql_scan_params` signatures); `CLAUDE.md`'s function table;
`website/docs/writing/transactions.md` (the COPY bullet goes; the "reads are
buffered" section says *at init*); `website/docs/writing/copy.md` (the
limitation section becomes the cost); `website/docs/performance.md` (#334: one
plan per shape; `PARAMETERIZATION FORCED` no longer needed for this);
`CLAUDE.md` function table and the DML/transaction bullets; `DATAMODEL.md`
(the `mssql_scan` flow: bind = describe, init = execute); `CHANGELOG.md`.

## 3. What this spec does not propose

- **Deferring `BEGIN TRANSACTION` to the first write** (#329's second option).
  The pin is what makes read-your-writes and one snapshot per transaction
  true; reads on pooled connections would see neither.
- **`SET FMTONLY ON`.** Deprecated, session state, and silent on
  branch-dependent procedures (F2). `sp_describe_first_result_set` is its
  documented replacement and refuses where FMTONLY guesses.
- **`prepared` as the default.** It describes single statements only, and in
  autocommit it reserves a pooled connection from bind to init for every
  scan; opt-in gives that cost to the callers who get the compile back.
- **A separate `mssql_prepared_query` function.** Same bind and init as
  `mssql_scan` with one flag flipped; a parameter keeps one function, one set
  of docs and one registry of behaviour.
- **`prepared` on `mssql_exec` / `mssql_exec_params`.** A handle lives in one
  session; the reuse a caller wants is across calls, and W5's parameters give
  it through the plan cache without any session state.
- **Parameters as overloads of the existing names.** See W5: overload
  resolution over a `NULL` third argument, and two meanings under one name.
- **Positional parameters** (`@p1`, variadic arguments). The STRUCT form
  covers it with names; positions can be added later without changing it.
- **Table-valued parameters, OUTPUT parameters.** RPC, or a `SELECT` in the
  batch.
- **Deferring INSERT the way UPDATE and DELETE do.** It would hold the SQL
  text of the whole source in process memory; buffering the source through
  the buffer manager (W3) spills, and covers COPY with the same lines. And it
  would be thrown away by spec 062, which moves INSERT onto `INSERT BULK`,
  which has no deferred form at all.
- **Projection or filter pushdown into `mssql_scan`.** The user wrote the
  T-SQL.

## 4. Risks

- **Two type mappings.** The default describe maps names
  (`MapSQLServerTypeToDuckDB`), execution maps TDS tokens
  (`GetDuckDBType`); they were written to agree and have never been checked
  against each other for every type. W6's type-matrix test does, and W2's
  init check makes any drift a named error rather than misread rows.
- **What the describe refuses** (`#temp` batches, branch-dependent
  procedures) takes the fallback and keeps today's behaviour and cost.
  Fabric's support for `sp_describe_first_result_set` and `sp_prepare` is
  confirmed on the Azure lane; an unsupported server also takes the fallback.
- **Prepared handles.** A handle whose scan is never read (an `EXPLAIN`, a
  pruned source) is freed by the `RESET_CONNECTION` the pool applies on
  release, or by the transaction's COMMIT/ROLLBACK release; nothing is sent
  from a destructor.
- **Memory.** W2 materialises `mssql_scan` results at init in a transaction —
  the same rows #316 materialises at bind today, later and only when read —
  and W3 materialises a COPY's source; both through the buffer manager, which
  spills. Autocommit streams as before.
- **W5's declarations decide plans.** A `VARCHAR` parameter compared to a
  `varchar` column converts the column to `nvarchar` and scans; the
  `MSSQL_VARCHAR(n)` cast is the documented way out, and the docs say so next
  to the type table. The 4 000 / max split is two plans per call site, not
  one per length.
- **W4 and quoting.** Names reach the server as parameter values, quoted by
  `QUOTENAME` there; the N'…' literals that carry them keep the doubling
  `StringUtil::Format` relied on. A name with `]` is the test.

## 5. Acceptance

- `EXPLAIN` of a side-effecting `mssql_scan` batch leaves no side effect;
  `MSSQL_DEBUG` shows bind describing and init executing.
- #329's shapes and § 0's sink table all pass inside a transaction, with the
  COPY and the 5 000-row INSERT rows now `works`.
- The second and third fresh tables' metadata queries complete in ≤ 2 ms where
  they took ~30 ms.
- `mssql_exec_params` executed three times with different values adds one
  entry to the optimizer counter; every type in W5's table round-trips.
- Full suite green; W6's new tests fail on `main` where § 0 says they should.

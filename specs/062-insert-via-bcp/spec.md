# Spec 062 — INSERT via BCP

**Status:** Draft (design settled 2026-09-14; the 2026-08-01 draft's questions are
answered in § 0 and § 1)
**Date:** 2026-09-14
**Depends on:** spec 024 (BulkLoadBCP), spec 027 (CTAS over BCP), spec 060
(target types + UTF-8 write wire), spec 063 (`BulkLoadSession`, load policy),
spec 075 W3 (sink catalogs materialise their scans in a transaction)
**Closes:** [#344](https://github.com/hugr-lab/mssql-extension/issues/344)
(partial application in autocommit — on the bulk path by W2, on the statement
path of INSERT, UPDATE and DELETE by W1c); the metadata half of
[#327](https://github.com/hugr-lab/mssql-extension/issues/327) (`is_identity`)

---

## 0. Measured ground

Local docker SQL Server 2022, release build at `cc5652d`, 1M rows × 3 columns
(`int`, `bigint`, `nvarchar(40)`) from a DuckDB table into an existing heap;
the same statement pair run twice, interleaved.

### 0.1 Three write paths, one of them not BCP

| path | wire | 1M rows | 3 rows |
| --- | --- | --- | --- |
| `COPY … (FORMAT 'bcp')` | `INSERT BULK` | 0.78 s / 0.51 s | 5–6 ms |
| `CREATE TABLE … AS SELECT` | `INSERT BULK` (`mssql_ctas_use_bcp`) | — | — |
| `INSERT INTO ms.dbo.t SELECT …` | 1000 × `INSERT … VALUES (…)×1000` | **73.7 s** / 4.4 s | 2–5 ms |

The two INSERT numbers are not noise. Every batch is a distinct 1000-row
`VALUES` list, and SQL Server compiles a large `VALUES` constant table slowly —
tens of milliseconds each. The second run sent the *same* 1000 statements and
hit the ad-hoc plan cache, which is why it took 4.4 s; a real workload never
inserts the same rows twice, so **73.7 s is the number a user sees**, about
90× the bulk load (§ 0.4 confirms it with fresh data). `INSERT INTO … SELECT` is
what a user writes once the target exists — the normal case after the first
load — and it is the slowest thing the extension does.

At three rows the text path is one round trip; BCP is three (`INSERT BULK`,
the stream, `DONE`) plus a column-metadata query — a fixed 3 ms locally, two
extra RTTs anywhere else. Small inserts stay text (W1).

### 0.2 What a bulk load does inside a transaction

The 2026-08-01 draft named this the risk that decides the shape of the feature.
It is answered: COPY has loaded through the transaction's pinned connection
since spec 063 (`MSSQLLoadTransactionRole::JoinsTransaction`), and this
experiment shows the batch boundaries inside it are invisible to the
transaction:

```sql
SET mssql_copy_flush_rows = 10;                 -- 100 rows -> 10 batches, 10 DONE tokens
BEGIN;
COPY src TO 'mssql://t/dbo/x062' (FORMAT 'bcp', CREATE_TABLE false);
SELECT cnt FROM mssql_scan('t', 'SELECT COUNT(*) AS cnt FROM dbo.x062');   -- 100
ROLLBACK;
SELECT COUNT(*) FROM t.dbo.x062;                -- 0
BEGIN; COPY …; COMMIT;
SELECT COUNT(*) FROM t.dbo.x062;                -- 100
```

An `INSERT BULK` batch commits with the transaction it runs in, not at its
`DONE`. Two consequences carry the design:

- On the pinned connection the load is part of the DuckDB transaction and rolls
  back with it. Nothing to build.
- In autocommit, an explicit `BEGIN TRANSACTION` on the load's own pool
  connection before the first `INSERT BULK`, and `COMMIT` after the last
  `DONE`, makes the whole INSERT **atomic** — batches and all — which is what
  a SQL `INSERT` is, and what the text path never was: it committed every
  1000-row statement on its own (#344). This is the reason the user chose to
  ship #344 as an interim throw and put the effort here instead.

### 0.3 What the binder hands `PlanInsert` on the 2.0 track

DuckDB 2.0 expands the child plan to every physical column of the table and
fills the ones the INSERT did not name with their bound default;
`MSSQLCatalog::PlanInsert` detects an unnamed column by its moved-away
`bound_defaults` slot and keeps it out of the generated column list, so the
server applies IDENTITY and DEFAULT (spec 069 G3,
`insert_server_defaults.test`). The chunk that reaches the sink therefore
carries every table column, already cast to the type the catalog reported
(`MSSQL_NVARCHAR(n)`, `TIMESTAMP_MS` for `datetime2(3)`, …), and
`target.insert_column_indices` says which of them are inserted. That is the
exact input `BCPWriter` takes: a chunk plus a target→source mapping.

### 0.4 Fresh data pays the compile again

A new session, same table: `src` 74.2 s, `src` again 5.8 s, then `src2` — a
second million rows of the same shape and different values — **74.9 s**. The
second number is the plan cache answering the same 1000 texts; the third is
what every INSERT of new data costs.

### 0.5 What the statement path costs, and where its cliff is

Measured before deciding how the sub-threshold path should look (100 or 50
statements per cell, fresh values every time, `DBCC FREEPROCCACHE` between
cells, plan cache read back by `sys.dm_exec_cached_plans`):

| statement | ms per statement | plans left in cache |
| --- | --- | --- |
| 1 row × 3 cols, literals | 0.8 | 1 Prepared |
| 10 rows, literals | 0.9 | 1 Prepared |
| 100 rows (300 constants), literals | 1.8 | 1 Prepared, 440 KB |
| 300 rows (900 constants), literals | 4.2 | 1 Prepared, 1.2 MB |
| **340 rows (1020 constants), literals** | **23** | **50 Adhoc, 29 MB** |
| 1000 rows × 1 col (1000 constants), literals | 6.0 | 1 Prepared |
| 500 rows × 3 cols (today's default is 1000) | 45 | Adhoc, one per statement |
| 1 row, `sp_executesql` + `DECLARE` block | 0.9 | 1 Prepared |
| 10 rows, `sp_executesql` | 1.7 | 1 Prepared |
| 100 rows (300 parameters), `sp_executesql` | **29** | 1 Prepared |

Two facts. **SQL Server auto-parameterises a multi-row `VALUES` INSERT up to
1000 constants** (simple parameterization; a 1-column 1000-row statement is
still one plan, a 3-column 340-row one is not): below that line a workload of
distinct inserts shares ONE plan per (table, column list, row count) and
costs 14 µs a row; above it every statement compiles its own ad-hoc plan at
70 µs a row and leaves it behind — which is the 74 s of § 0.1, since today's
1000-row statements carry 3000 constants. One refinement, found while
writing the test: auto-parameterisation needs a *trivial* plan, and an
INSERT into a table with a clustered index stops being trivial past about
250 rows per statement (250 Prepared, 256 Adhoc — the optimizer adds a
sort). Those statements compile their own plan but at the cheap end: 5.3 ms
against the heap's 4.7 ms for 333 rows, not 23 ms. The line that matters
for cost is the constant line; the plan-cache claim holds for heaps and for
clustered targets up to 250 rows a statement, and not at all for a statement
with an `OUTPUT` clause (`RETURNING`), which is never auto-parameterised. **An explicit `sp_executesql` with
the values in a `DECLARE` block does not help and hurts:** the outer batch
still carries the literals, is itself ad-hoc, and at 300 declarations costs
16× the literal form. The batch form of parameters the extension can send
today (spec 075 W5) is right for the three constants of a filter and wrong
for a VALUES list; only a TDS RPC call with typed parameters would change
that, and the auto-parameterisation makes it unnecessary here.

## 1. Findings

**F1 — the gap is server compile time, not bytes.** A 1000-row `VALUES` list
is a constant table the optimizer has to build a plan for, per batch, per
distinct text. BCP sends no statement per batch; the server parses `INSERT
BULK` once and reads rows. Client CPU is also lower (1.55 s user vs 0.12 s in
§ 0.1: literal rendering and escaping per value versus a columnar encode), but
that is not where the 73 s went.

**F2 — transactions are solved, and solved in our favour.** § 0.2. The load
joins the pinned connection's transaction; in autocommit the load's connection
can open its own. `BulkLoadSession` already has the failure path this needs:
a connection closed mid-load rolls its transaction back on the server, and
`ReleaseBcpConnectionOnError` closes it from any thread without a
`ClientContext` (#178, #191).

**F3 — the small-insert boundary must be exact, not estimated.** A plan-time
estimate is wrong for exactly the statements that matter here (`INSERT …
SELECT` from a filter, a `VALUES` list wrapped in a CTE). Buffering the first
rows and deciding at the threshold costs one `ColumnDataCollection` of at most
threshold rows and is right every time: at or below the threshold the buffered
rows go through today's text executor, byte-identical; above it the stream
opens and the buffer drains into it.

**F4 — IDENTITY needs the catalog to know.** The text path lets the *server*
decide about an explicit identity value: without `IDENTITY_INSERT` it is error
544, with it (set on the pinned session) it is honoured. `INSERT BULK` keeps an
explicit identity value unconditionally (`bcp -E` semantics;
`bcp_identity_column.test` scenario B relies on it for COPY). So an INSERT
whose column list names an identity column must stay on the text path — and
the catalog does not record which columns are identity (#327: the metadata
query selects everything from `sys.columns` except `is_identity`). One more
column in the four column-metadata queries, carried on `MSSQLColumnInfo`, is
the whole of that. The *binder* half of #327 — omitting the column from the
default insert list — stays blocked: DuckDB's insert binder sizes a
column-list-less `VALUES` by `PhysicalColumnCount()`, which excludes only
`Generated()` columns, and a generated column cannot be read back by a scan.
That needs an upstream hook and is noted on the issue, not built here.

**F5 — column metadata for COLMETADATA is already in the catalog.**
`TargetResolver::GetExistingTableColumnMetadata` queries `sys.columns` for
name, type name, `max_length`, precision, scale, nullability and collation and
maps them to a `BCPColumnMetadata`. `MSSQLColumnInfo` holds the same seven
fields — the catalog loaded them from the same view. The mapping becomes a
function of those fields, shared by the server-query path (COPY) and a catalog
path (INSERT), and INSERT pays no metadata round trip. Staleness is the same
risk the text path already runs (its column list is also the cache's), and
fails the same way: a server error naming the column.

**F6 — one session type, two entry points, three consumers.** Spec 063 wrote
`BulkLoadSession` for "the two consumers coming — INSERT via BCP and the
`#temp` staging fill", but only the parallel-writer half went through it; the
shared writer that runs on the operator's own connection is still inline in
COPY (`StartBulkStream` / `FlushToServer` / `BCPCopyFinalize`) and in CTAS
(`ExecuteBCPInsert` / `AddChunkBCP` / its finalize), and spec 063 D5's "unify
next" never came. INSERT must not become the third inline copy. The session
gets a second entry point that **adopts** a connection the caller already
holds (the transaction's pinned one, or a pool connection the sink acquired)
and opens the stream on it; the pooled `TryStart` stays. COPY's and CTAS's
shared writer move onto that entry point **in this PR, before INSERT is
built on it** (W0), so the path INSERT depends on is exercised by the 34 COPY
tests and the CTAS suite from the first commit, not only by the tests written
for it. The 063 rule — moving and changing in one commit hides both — is kept
by commit boundaries, not by a second PR: the moves are their own commits with
no test changed, and the reviewer reads them as moves.

**F7 — the `mssql_copy_*` settings are bulk-load settings.** Batch size
(`mssql_copy_flush_rows` — the 102400 columnstore rowgroup threshold), writer
count (`mssql_copy_parallel_writers`) and TABLOCK policy (`mssql_copy_tablock`)
describe the load, not the statement that started it. INSERT reads the same
three; a second set with `insert` in the name would be the same numbers under
different names and a fresh way for them to disagree.

## 2. Work

### W0 — the shared writer moves onto `BulkLoadSession`

Two commits, each a move with no test touched, each green on the full COPY
and CTAS suites before the next:

1. **COPY.** `BulkLoadSession::Adopt(connection, params, transaction_pinned)`
   opens the stream on a connection the caller holds (`INSERT BULK` →
   Executing → COLMETADATA, exactly what `StartBulkStream` does today) and
   `Write` / `Finish` / `Abandon` behave as for a pooled session, except that
   release goes back to whoever owns the connection: a pinned connection is
   left pinned, a pool connection is released with the captured
   `reset_on_release`. `MSSQLCopyGlobalState::writer` + `insert_bulk_sql` +
   the flush/reopen code in `FlushToServer` and the DONE/confirm code in
   `BCPCopyFinalize` collapse onto the session; the counters
   (`counter_encode_ns` / `counter_flush_ns`) come from `BulkLoadWriteResult`
   as the parallel writers' already do; the global state's destructor keeps
   `ReleaseBcpConnectionOnError` semantics through `~BulkLoadSession`.
   Accounting stays where it is (rows_sent / rows_confirmed / batches_flushed
   atomics in the global state) — 063 D5, still not this spec.
2. **CTAS.** `CTASExecutionState`'s `connection` / `bcp_writer` /
   `bcp_rows_in_batch` / the re-execute-INSERT-BULK block in `AddChunkBCP`
   become one adopted session on the pool connection the state acquires; the
   drop-on-failure path calls `Abandon()` before the cleanup DROP, which is
   what `Abandon` was written for (issue #191). Per-thread accumulation and
   the Combine fold are untouched.

What this buys beyond coverage: one place where a batch is closed and
reopened, one place where DONE is confirmed, and the INSERT sink (W2) is the
same object with `BEGIN TRANSACTION` in front of it.

### W1 — the decision, and the small-insert buffer

`PlanInsert` chooses the BCP sink when all of: `mssql_insert_use_bcp` is
true, no `RETURNING` (F: `INSERT BULK` returns no rows; `OUTPUT INSERTED` is a
text statement by construction), and no inserted column is `is_identity` (F4).
Otherwise today's operator, untouched.

The BCP sink buffers incoming chunks in a `ColumnDataCollection` (allocated
from `context`, per the allocator rule) until the buffered count exceeds
`mssql_insert_bcp_threshold`. At `Finalize` with the threshold never crossed,
the buffered rows run through the existing `MSSQLInsertExecutor` — the same
statements, the same connection choice, the same error messages as before
(acceptance 2). Crossing it opens the stream, drains the buffer into it and
streams every later chunk directly.

Settings: `mssql_insert_use_bcp` (BOOLEAN, default true, the escape hatch that
`mssql_ctas_use_bcp` is for CTAS) and `mssql_insert_bcp_threshold` (BIGINT,
rows, default 1000 — measured in § 6.2: the paths cross near 300 rows on a
local server and near 1000 at a 20 ms RTT).

### W1b — the statement path stays auto-parameterised

§ 0.5: the text path's cliff is 1000 constants per statement, and today's
`mssql_insert_batch_size = 1000` rows puts every table with two or more
columns past it. The batch builder caps rows per statement at
`min(mssql_insert_batch_size, MSSQL_AUTO_PARAM_MAX_CONSTANTS / inserted
columns)` with the constant at 1000, so every statement it sends is one the
server auto-parameterises: 3 columns → 333 rows a statement at 4 ms instead
of 1000 rows at 74 ms, no plan left behind per statement. Measured 5.5× at
the boundary, more as statements grow. This governs everything that stays on
the statement path — the rows below the threshold, `RETURNING`, the explicit
identity column. Not every one of them gains: a `RETURNING` insert of a
million rows measured 69.8 s before and 70.2 s after (§ 6.6) — an `OUTPUT
INSERTED` statement is never auto-parameterised, and its cost is ~70 µs a
row on the server whatever the statement size, so the cap neither helps nor
hurts it. Plain inserts under the threshold are what the cap is for. Still
20× slower than the bulk path, which is why the threshold exists.

A riding fix the cap made urgent: `MSSQLInsertExecutor::ExecuteWithReturning`
kept the LAST statement's `OUTPUT INSERTED` chunk only ("for simplicity"),
so a `RETURNING` insert spanning several statements inserted every row and
returned a fraction of them — above 1000 rows before, above `1000 /
columns` with the cap. It returns every statement's chunk now
(`insert_returning_batches.test`, hashed against the plain sequence).

Rejected by the same measurement, and recorded so it is not proposed again:
sending the sub-threshold statement as `sp_executesql` with a `DECLARE` block
(the spec 075 W5 form). Slower at every size and 16× slower at 100 rows,
because the outer batch is ad-hoc and carries the literals; the server's own
parameterisation already gives the one-plan-per-shape that form was meant to
buy. `mssql_dml_use_prepared` therefore stays what it is — a setting read by
nothing — and is not revived here.

### W1c — the statement path is atomic too (#344)

What stays on the statement path — every `RETURNING` insert, the explicit
identity column, the rows below the threshold, and UPDATE and DELETE until
spec 066 — is still sent as several statements, and W1b makes them more
numerous (333 rows each on a 3-column table), so #344's partial application
would get *more* likely on that path, not less. The interim of #344 (throw,
name the applied count) was accepted only until the BCP work; this is the
BCP work, and the fix is the same `LoadTransaction` W2 introduces, on the
statement executors:

- The three executors (`MSSQLInsertExecutor`, `MSSQLUpdateExecutor`,
  `MSSQLDeleteExecutor`) take ONE connection for the statement's lifetime —
  acquired on the first batch through `ConnectionProvider`, so it is the
  pinned one inside a transaction — and return it at `Finalize` or on the
  error path. Today each batch acquires and releases its own, which in
  autocommit can be a different pool connection per batch; nothing depends
  on that.
- In autocommit the executor brackets the batches with `LoadTransaction`:
  `BEGIN TRANSACTION` before the first, `COMMIT` after the last, `ROLLBACK`
  on any failure (a closed connection rolls back on its own). Inside a
  DuckDB transaction the helper is a no-op and the rows sit in the open
  server transaction, as they do today.
- The #344 message loses its "N row(s) from the K-1 batch(es) before it are
  applied" clause, which is no longer true: it says `rolled back` in
  autocommit and `part of the open transaction; ROLLBACK discards them`
  inside one — the same two sentences W5 gives the bulk path, rendered by
  the same `FormatMessage`. `issue_344_dml_desync.test` changes its
  expectations accordingly and gains the row-count assertion (0 rows after
  a failed autocommit INSERT / UPDATE / DELETE).

One statement, one connection, one server transaction — on both paths.

### W2 — the sink

`MSSQLPhysicalInsert` gains a BCP mode (a plan-time flag, so `ParallelSink()`
can return true in that mode: without it DuckDB drives the sink on one thread
and the writer policy is moot).

Global state: the buffer (W1); the load connection and a `BulkLoadSession`
adopting it (W0); `insert_bulk_sql` built once by `BuildInsertBulkSql` from
the target and the inserted columns' `BCPColumnMetadata` (W3), TABLOCK from
`MSSQLResolveTablock(choice, table shape)` — the shape is on
`MSSQLTableMetadata` since spec 049 (`index_kind`), no query; the policy from
`MSSQLResolveLoadPolicy(temp=false, in_transaction, JoinsTransaction,
mssql_copy_parallel_writers, threads)`; atomics for rows sent / confirmed /
batches; `pool_handle`, `transaction_pinned`, `reset_on_release` captured on
the client thread in `GetGlobalSinkState` (#178). Local state: a
`BulkLoadSession` for an own writer and the `may_claim` flag, exactly COPY's
loop (`TryStart` → `Started` / `GateClosed` / `Unavailable`).

Opening the stream (first threshold crossing, under the global mutex):
`ConnectionProvider::GetConnection` — pinned in a transaction, else a pool
connection. In autocommit the session sends `BEGIN TRANSACTION` before its
first `INSERT BULK` (§ 0.2); on the pinned connection it sends nothing, the
DuckDB transaction owns it. Own writers do the same on their pool connections.

Ending: `Combine` finishes a thread's own stream (`DONE`, confirmation) and
**moves the session into the global state** — local sink states are destroyed
before `Finalize`, and the connection must stay open until the commit.
`Finalize` finishes the shared stream, then commits the load's server
transaction on every autocommit connection and releases them; the pinned
connection stays pinned. Two pieces, shaped for spec 066 as much as for this
(§ 2.9): `BulkLoadSession::Finish()` splits into `CloseStream()` — the last
DONE and the server's confirmation, the connection kept — `Commit()` and
`Release()`, with `Finish()` staying the three for COPY and CTAS; and the
transaction is a small helper, `LoadTransaction` (in `copy/`), that wraps ONE
connection: `Begin()` sends `BEGIN TRANSACTION` unless the connection is
pinned (then the DuckDB transaction owns it and every call is a no-op),
captures the transaction descriptor the server answers with — every later
request on the connection must carry it, error 3989 otherwise — `Commit()` /
`Rollback()` send theirs and clear it, and its destructor rolls back whatever
was begun and not committed — from a worker thread, without a
`ClientContext`, the #178 rule. The statement executors hold one directly
(W1c). A bulk session holds one for the connection it owns, on a params
flag (`own_transaction`): a parallel writer acquires its connection and
sends its first `INSERT BULK` in one step, so the bracket has to go there,
and the shared session takes the same flag so the two are treated alike;
COPY and CTAS leave it off, which is their per-batch-commit contract.

**Parallel writers, only where their locks cannot conflict.** COPY's writers
commit every batch; an INSERT's hold their locks until `Finalize` commits
them together, and two writers whose locks conflict deadlock CLIENT-SIDE:
writer B waits on a lock A's uncommitted rows hold, so the server stops
reading B's stream, so B's thread blocks in `send()`; A's commit is in
`Finalize`, which waits for B's `Combine`. The server sees no deadlock (one
side is a client) and nothing times out — measured as a hang past ten
minutes on a heap on row locks, and a 30 s BCP read timeout on a clustered
rowstore. Two shapes let concurrent transactional bulk loads coexist: a
**bare** heap under TABLOCK (BU locks are mutually compatible, nothing
escalates) and a clustered columnstore without it (each session fills its own
rowgroups) — measured 1M rows in 0.49 s and 0.80 s at four writers against
1.78 s at one. Everything else — a clustered rowstore index, a heap on row
locks, a columnstore under a table lock — gets one writer. Under
`mssql_copy_tablock = auto` that is: bare heaps and columnstores fan out,
clustered rowstore tables do not.

"Bare" is the whole of it, on BOTH arms, and the base structure cannot answer
it (review of this PR, job 1632). SQL Server hands concurrent bulk loaders
compatible locks only when the table has **no indexes at all**;
`MSSQLIndexKind` comes from queries that filter `index_id <= 1`, which is
exactly the rows a nonclustered index is not, so a `PRIMARY KEY NONCLUSTERED`
heap reported HEAP and fanned out. Measured under TABLOCK, in a transaction,
reading `sys.dm_tran_locks` for the target (§ 6.5): a bare heap takes **BU**,
the same heap carrying a nonclustered PK takes **Sch-M** — the lock nothing
else is compatible with, including another Sch-M. `QueryTableShape` therefore
returns a `TableLoadShape` carrying `has_nonclustered` beside `kind`, from a
second scalar subquery on the same round trip.

The review left the columnstore arm open, and measuring it settles it the
other way from the heap: a clustered columnstore carrying one nonclustered
index does not stall at four writers, it **fails** — a writer times out
reading its BCP response after 30 s, which is an error rather than a failed
claim, so the whole INSERT rolls back and lands nothing (30.8 s and 0 rows,
against 0.99 s and 400000 on one writer). So `has_nonclustered` gates both
arms: fan-out needs a target with no index on it.

The shape this rule reads is queried LIVE when the stream opens
(`TargetResolver::QueryTableShape`, one `sys.indexes` lookup on the load's
own connection), not taken from the catalog cache: a table cached as a heap
and given a clustered index since — through `mssql_exec`, which does not
invalidate by default, or by another client — would otherwise fan out into
exactly the deadlock above (self-review). The TABLOCK hint stays the one
the plan built from the cached shape; with one writer it serialises nothing.

**Statement semantics on the bulk wire.** A bulk load ignores CHECK
constraints, does not fire triggers, and writes a column's DEFAULT where the
stream says NULL — bcp's contract, and COPY's (measured: a 1000-row INSERT
with a CHECK violation in row 950 loaded all 1000 rows). An INSERT is a
statement, so its `INSERT BULK` carries `CHECK_CONSTRAINTS, FIRE_TRIGGERS,
KEEP_NULLS` (`InsertBulkHints::StatementSemantics()` in
`BuildInsertBulkSql`); COPY's hint set is unchanged. Between the first and the last `COMMIT` of a multi-writer load there is
a window in which a failed commit leaves the earlier writers' rows in place —
the usual two-phase gap of any multi-connection load; it is named in the docs,
and it does not exist for one writer, which is every load inside a
transaction and every load under `mssql_copy_parallel_writers = 1`.

Failure anywhere: the throwing path rolls back its own session (`ROLLBACK
TRANSACTION` if the connection is still usable, else `Abandon()` — a closed
connection rolls back on the server); every other session is rolled back by
`~BulkLoadSession` → `ReleaseBcpConnectionOnError` when its state is
destroyed. Inside a DuckDB transaction the rows sent so far sit in the open
server transaction and go with its `ROLLBACK`, which is what DuckDB requires
after a failed statement anyway.

Result: the count of rows the server confirmed, as today.

### W2a — the threshold buffer

The "buffer until N rows, then decide" step of W1 is its own small type,
`StagedRows` (a `ColumnDataCollection` allocated from the context, the row
count, and `Exceeds(threshold)`), because spec 066 makes the same decision
one operator later: a handful of rowids go as one parameterised statement,
more go through a `#temp` fill. One buffer type, one decision shape.

### W3 — COLMETADATA from the catalog

`BCPColumnMetadata::FromServerColumn(name, type_name, max_length, precision,
scale, nullable, collation_name)` — the body of the loop in
`GetExistingTableColumnMetadata`, including the UTF-8 varchar retarget of
spec 060 and the `tinyint` → `UTINYINT` rule. The resolver calls it per row;
`PlanInsert` calls it per `MSSQLColumnInfo` of an inserted column. The column
mapping handed to the writer is `insert_column_indices` (target position →
chunk column), so the chunk's unnamed columns are never read.

`FromServerColumn` is also what declares the columns of a `#temp` staging
table in spec 066 — rowid columns plus the updated ones, typed exactly as the
target's — so it takes the seven fields, never a `BCPCopyTarget`.

### W4 — `is_identity` in the catalog (#327, metadata half)

`c.is_identity` joins the select list of the four column-metadata queries
(single table, per schema, all schemas) and rides on `MSSQLColumnInfo`; the
`MSSQLInsertColumn` built in `PlanInsert` stops hard-coding it to false, and
the CTAS `has_identity_column` vocabulary that exists for this is populated
the same way. Used by W1's routing rule. `duckdb_columns()` is unchanged —
DuckDB has no column-level identity concept to report it through.

### W5 — error attribution

A server error arrives at a batch boundary (`FlushBatch`) or at the final
`DONE`, after the batch was sent. The message names what the text path named:
which batch (`INSERT BULK batch K, rows A–B of the load`), the server's own
text, and what happened to the rows before it — `rolled back` in autocommit
(W2 guarantees it), `part of the open transaction; ROLLBACK discards them`
inside one. The spec 060 length guard still fires client-side before the batch
is sent, naming the column and the row. `MSSQLInsertError` gains a `bulk`
variant rather than a second exception type, so `FormatMessage` stays the one
place messages are rendered (#344).

### W6 — tests

- `test/sql/insert/insert_bcp_paths.test`: the path observed through the
  statement path's test lever (`mssql_test_fail_parse_after_tokens` lives in
  those loops only) — bulk for a plain `INSERT … SELECT` above the
  threshold, statements for `RETURNING`, an explicit identity column, a
  view, a column of a type the bulk wire cannot carry (hierarchyid), and
  under `mssql_insert_use_bcp = false`.
- the threshold, in `insert_bcp_paths.test`: N = threshold rows → the
  statement path; N = threshold + 1 → bulk; a lower threshold moves the
  line; zero rows sends nothing.
- `insert_statement_plans.test` (W1b): after `DBCC FREEPROCCACHE`, 20
  distinct 900-row inserts into a 3-column HEAP — 60 statements, two
  shapes — leave two `Prepared` plans and no `Adhoc`
  (`sys.dm_exec_cached_plans` joined to `sys.dm_exec_sql_text`, § 0.5 as an
  assertion); five `RETURNING` inserts leave five `Adhoc` plans, pinning
  that `OUTPUT` is never parameterised.
- `insert_bcp_types.test`: every type family through both paths into the same
  table, compared row by row — non-ASCII into nvarchar and into a UTF-8
  varchar, NULLs, `datetime2(3)`/`(7)` via `TIMESTAMP_MS`/`_NS`, decimal at
  the 38-digit edge, `uniqueidentifier`, `varbinary(max)` (acceptance 3;
  `both_paths_agree.test` is the model).
- `insert_bcp_transaction.test`: `BEGIN; INSERT … SELECT (> threshold);
  ROLLBACK` leaves nothing; the same with `COMMIT` keeps all; an INSERT that
  reads the same catalog inside the transaction (spec 075 W3 covers the sink).
- `insert_bcp_atomic.test`: autocommit, `mssql_copy_flush_rows = 100`, a
  source of 1000 rows whose row 950 violates a CHECK constraint → the error
  names batch 10 and `rolled back`, and the table holds 0 rows — the #344
  class, closed for the bulk path.
- `dml_statement_atomic.test` (W1c): the same shape on the statement path —
  a 1000-row `INSERT … RETURNING` whose row 950 fails, an `UPDATE` and a
  `DELETE` over 1200 rowids whose last batch fails — each leaves the table
  exactly as it was, and each message says `rolled back`; inside `BEGIN …
  ROLLBACK` the same statements leave nothing behind and the message says
  the rows are in the open transaction.
- `insert_bcp_parallel.test`: `SET threads = 4`, a 400k-row load fans out
  on a heap (`connections_created` grows by the extra writers), loads a
  clustered columnstore with four threads, stays on one writer against a
  clustered rowstore and under `mssql_copy_parallel_writers = 1`, loads a
  heap carrying a `PRIMARY KEY NONCLUSTERED` (whose regression signature is
  a 30 s stall, not a wrong count) and a clustered columnstore carrying one
  (whose signature is a failed statement), and — the shape read live at stream
  open, not from the cache — loads a table that was a heap when the catalog
  cached it and has a clustered index now, without the client-side deadlock
  a stale shape would have caused; inside a transaction exactly 1
  (`insert_bcp_transaction.test`).
- `insert_bcp_semantics.test`: CHECK constraints enforced, triggers fired,
  explicit NULLs kept over a DEFAULT, omitted columns defaulted — on both
  paths.
- `insert_server_defaults.test` and `bcp_identity_column.test` keep passing
  unchanged (identity, defaults).
- C++: `FromServerColumn` on the type table (one case per family, the UTF-8
  retarget, `tinyint`).

### W7 — docs

`README.md` (INSERT section: BCP by default, when it is not), website
`writing/dml.md` (the paths, atomicity, the two-phase window, the error
shape), `reference/settings.md` (the two new settings; `mssql_insert_batch_size`
says the 1000-constant cap sits under it; the three `mssql_copy_*` rows say
they govern INSERT too), `docs/transactions.md` (autocommit INSERT is
one server transaction), `DATAMODEL.md` (the write-path layer gains the INSERT
consumer of `BulkLoadSession`), `CLAUDE.md` settings table, CHANGELOG. #327
gets the finding on the binder half.

### W8 — measurement

`test/bench/bench_live_server.sh` gets an `insert` group beside the `copy`
one: the same fixture and families, `INSERT INTO db.dbo.t SELECT c FROM
src.syn`, run with `mssql_insert_use_bcp` false and true alternating in ONE
session (the A/B is a setting, so the harness rule about same-session
interleaving is met by construction). Reported: wall and client CPU per
family at 1M rows; the crossover — wall at 10 / 100 / 1000 / 10000 rows both
ways — which sets the default of `mssql_insert_bcp_threshold`; and the statement
path before and after W1b at 1M rows with `RETURNING` (the one large insert
that never leaves it). Also
checked once, by `sys.dm_db_column_store_row_group_physical_stats`: that a
transaction-wrapped load into a clustered columnstore still lands compressed
rowgroups per batch (the assumption behind reusing `mssql_copy_flush_rows`).

### 2.9 The seams spec 066 takes over

UPDATE and DELETE via a `#temp` staging table (spec 065 recon, spec 066) are
the next consumer of everything above, and the shapes here are chosen so that
066 adds an operator and not a second copy:

| 066 needs | built here as |
| --- | --- |
| fill a `#temp` on the connection that will run the UPDATE — the transaction's pinned one, or the statement's own pool connection — then run ONE statement on that same connection | `BulkLoadSession::Adopt` + `CloseStream()` without `Release()` (W0, W2); the `#temp` policy is already `max_writers = 1` (`MSSQLResolveLoadPolicy(target_is_session_scoped = true)`) |
| the fill and the statement in one server transaction in autocommit, rolled back together on failure | `LoadTransaction` on the owner's connection (W2), already bracketing the three statement executors (W1c) |
| declare the `#temp`'s columns exactly as the target's | `BCPColumnMetadata::FromServerColumn` from the catalog (W3) and `BuildInsertBulkSql` (spec 063) |
| a few rowids as one statement, many as a fill | `StagedRows` (W2a); the statement itself sized under the 1000-constant line of § 0.5, which is a rule 066 inherits rather than code — a `#temp` fill starts where auto-parameterisation stops |
| name the failing batch and what happened to the rows before it | the `bulk` variant of the error (W5) |

Not pre-built: the `CREATE TABLE #tmp` text, the UPDATE/DELETE statement
shapes, the rowid mapping — 066's own, with its own measurements.

## 3. Not proposed

- **`RETURNING` over BCP.** No rows come back from `INSERT BULK`; the
  `OUTPUT INSERTED` rewrite stays, on the text path, byte-identical.
- **Changing COPY's per-batch commit.** `COPY … (FORMAT 'bcp')` keeps `bcp`
  semantics (each batch commits in autocommit); a user who wants an atomic
  COPY has `BEGIN … COMMIT`. The transaction wrapper is INSERT's because an
  SQL `INSERT` is atomic by definition.
- **Unifying the three operators' accounting** (spec 063 D5). W0 moves the
  writer, not the counters; each operator keeps its rows/batches bookkeeping.
- **The binder half of #327.** Upstream.
- **UPDATE / DELETE.** Spec 066: `#temp` staging via this same session plus
  one server statement.
- **A separate TABLOCK or batch-size setting for INSERT.** F7.

## 4. Risks

- **Lock duration.** Under `mssql_copy_tablock = auto` a heap load holds a
  BU lock until the commit — for the whole INSERT now, since it is one
  transaction. A plain SQL Server `INSERT … SELECT` of the same size escalates
  to a table X lock after 5000 row locks and holds it just as long, so this is
  not new for the size of load where it matters; documented, and `false`
  restores row locks.
- **Type coverage of the encoder for catalog-typed chunks.** The chunk's types
  are the catalog's, not a user's source types: `TIMESTAMP_NS`,
  `MSSQL_VARCHAR(n)` with a collation, `UTINYINT`. COPY from a scan of the
  same table already sends these (`columnar_encode_all_families.test`); W6's
  type test pins it for INSERT.
- **The two-phase window** (W2). Named; one writer has none.
- **A stale catalog column list** (F5). Same failure as the text path, same
  fix (`mssql_invalidate_cache`).
- **W0 regresses COPY or CTAS.** The most used write paths, moved before
  anything new is built on them. Contained by the commit rule (a move commit
  changes no test and must pass every COPY and CTAS test as it stands,
  `parallel_writers*.test` and `copy_failed_bcp_releases_connection.test`
  included, before the next commit) and by the bench's write families, run
  once before and after the move on the same server.

## 5. Acceptance

1. `INSERT … RETURNING` and an INSERT naming an identity column stay on the
   statement path (same operator, same statement text); so does any INSERT
   of at most `mssql_insert_bcp_threshold` rows. The one change on that path
   is W1b: no statement carries more than 1000 constants.
2. A workload of N inserts of one shape into one heap, each under the
   threshold, leaves ONE cached plan on the server, not N
   (`sys.dm_exec_cached_plans`); `RETURNING` and clustered targets past
   ~250 rows a statement compile per statement, at the cheap end (§ 0.5).
3. Above the threshold, the rows the server holds are identical to the text
   path's, row by row, for every type family, non-ASCII and NULLs included.
4. `ROLLBACK` discards the rows; a failed INSERT leaves **no** rows in
   autocommit and names the failing batch — on the bulk path and on the
   statement path alike, and the same for a failed UPDATE or DELETE (W1c).
5. Measured in one interleaved session against the text path: wall and client
   CPU at 1M rows per family, and the small-row crossover that set the
   threshold default.
6. After W0, COPY and CTAS pass their suites unchanged and their bench write
   families are within noise of the pre-move run.

## 6. Measurements

### 6.1 W0 — COPY/CTAS shared writer on the session (2026-09-14)

`bench_wide_write.sh` (CTAS 500k × 20 columns), pre-move binary (076 head
`55f36de`) against the W0 tree, interleaved, two rounds, min per cell:

| cell | wall before | wall after | ratio |
| --- | --- | --- | --- |
| threads=1 plain nonulls | 5.13 s | 4.71 s | 0.92 |
| threads=1 sized nonulls | 2.18 s | 2.18 s | 1.00 |
| threads=1 plain nulls | 4.49 s | 4.55 s | 1.01 |
| threads=1 sized nulls | 2.19 s | 2.17 s | 0.99 |
| threads=4 (four cells) | 1.06–2.63 s | 1.09–2.67 s | 0.71–1.47 |

The single-writer cells are the move's own path and sit within noise; the
four-writer cells swing both ways on the shared docker server, as every
earlier A/B of that matrix did (spec 076 § 6: ±1.7×), and need six pairs to
say anything. Client CPU per statement is 0.07–0.14 s on every cell, below
what two rounds can rank.

### 6.2 The crossover, and the threshold default (2026-09-14)

Local docker, 3-column heap, 20 statements per cell with fresh values, the
two paths interleaved per cell, two rounds (40 timings); statements are
`mssql_insert_use_bcp = false`, bulk is `mssql_insert_bcp_threshold = 1`.

| rows per INSERT | statements, min / median | bulk, min / median |
| --- | --- | --- |
| 10 | 1 / 2 ms | 3 / 4 ms |
| 100 | 2 / 3 ms | 3 / 4 ms |
| 1000 | 12 / 14 ms | 4 / 5 ms |
| 10000 | 113 / 134 ms | 13 / 15 ms |

The bulk path's fixed cost is about 2 ms here — the two extra round trips
(`INSERT BULK`, `DONE`) at a local RTT — and the statement path costs about
12 µs a row on top of its own round trip per 333 rows. They cross near 300
rows locally; at a 20 ms RTT the two extra round trips cost 40 ms and the
crossing moves to about 1000 rows. The default stays **1000**: at or below
it an INSERT is one to three statements, and the bulk path never loses by
more than a round trip or two. `mssql_insert_bcp_threshold` moves it.

### 6.3 Per family, 1M rows — 500k × 2 iterations (2026-09-14)

`bench_live_server.sh` group `insert`: `INSERT INTO db.dbo.t SELECT c FROM
src.syn`, one column per family, 500k rows × 2 iterations, the two paths
interleaved per family in one run (the A/B is a setting). The statement path
is the W1b one — one column, so 1000 rows a statement, every statement
auto-parameterised: its best case.

| family | bulk wall | statements wall | ratio | bulk client CPU | statements client CPU |
| --- | --- | --- | --- | --- | --- |
| bigint | 0.70 s | 5.53 s | 7.9 | 0.04 s | 0.24 s |
| int | 0.64 s | 5.44 s | 8.5 | 0.04 s | 0.29 s |
| double | 0.67 s | 6.84 s | 10.3 | 0.05 s | 0.44 s |
| decimal(18,2) | 0.74 s | 6.58 s | 8.9 | 0.05 s | 0.34 s |
| decimal(38,10) | 0.77 s | 7.14 s | 9.3 | 0.05 s | 0.62 s |
| bit | 0.67 s | 5.31 s | 7.9 | 0.03 s | 0.13 s |
| date | 0.67 s | 6.96 s | 10.5 | 0.03 s | 0.45 s |
| datetime2 | 0.68 s | 12.13 s | 17.9 | 0.04 s | 0.77 s |
| uniqueidentifier | 0.72 s | 7.16 s | 9.9 | 0.04 s | 0.27 s |
| varbinary(max) | 0.70 s | 6.57 s | 9.3 | 0.04 s | 0.51 s |
| nvarchar(4) / (16) / (200) | 0.67 / 0.76 / 1.88 s | 6.35 / 7.52 / 11.01 s | 9.4 / 10.0 / 5.9 | 0.04 / 0.09 / 0.26 s | 0.49 / 1.52 / 2.01 s |
| nvarchar(max), 16-char values | 2.48 s | 7.25 s | 2.9 | 0.06 s | 0.27 s |
| varchar(16) / (200) UTF-8 | 0.73 / 1.30 s | 6.80 / 10.26 s | 9.3 / 7.9 | 0.05 / 0.11 s | 0.57 / 1.92 s |
| varchar(max) UTF-8 | 2.50 s | 7.61 s | 3.0 | 0.06 s | 0.70 s |
| nvarchar(16), NULLs | 0.70 s | 5.88 s | 8.4 | 0.05 s | 0.36 s |

Three to eighteen times on wall, six to seventeen on client CPU. The MAX
string families are the narrow end (3×): the bulk wire sends them as PLP
chunks, the statement path as inline literals, and both are bound by the
server for those. The multi-column case of § 0.1 — where the statement path
had left the auto-parameterisation line — is the wide end: 74 s against
1.8 s, 40×.

### 6.5 The writer rule's premise, measured (2026-09-15)

The rule rests on which table lock `INSERT BULK ... WITH (TABLOCK)` takes.
Read from `sys.dm_tran_locks` for the target while the load's transaction was
still open, 5000 rows into each:

| target | lock granted |
| --- | --- |
| heap, no indexes | **BU** (bulk update — mutually compatible) |
| heap + `PRIMARY KEY NONCLUSTERED` | **Sch-M** (schema modification — compatible with nothing) |

Two Sch-M requests cannot both be granted, so the second writer's own
`INSERT BULK` blocks behind the first writer's still-open transaction. What
that costs, 400k rows into a heap with a nonclustered PK, `threads = 4`,
`mssql_copy_parallel_writers = 4`:

| gate | wall | writers used |
| --- | --- | --- |
| before (blind to the nonclustered index) | 30.97 s | 1 of 4 |
| after (`has_nonclustered`) | **0.55 s** | 1 of 1 |

56×. The damage is bounded rather than the unbounded hang of a heap on row
locks, and by luck: each extra writer blocks inside `TryStart`, whose 30 s
read timeout then reports a failed claim — and a failed claim is not an error
("a load must not fail because it could not go faster"), so the load
completes on the shared writer after paying 30 s of dead time and silently
losing the parallelism it asked for.

The columnstore arm, the same 400k rows at four writers, is worse — there the
extra writer gets *past* its `INSERT BULK` and times out mid-stream, which is
an error:

| clustered columnstore target | wall | rows landed |
| --- | --- | --- |
| no nonclustered index | 0.63 s (4 writers) | 400000 |
| + one nonclustered index, before | 30.81 s | **0 — the INSERT failed and rolled back** |
| + one nonclustered index, after | 0.99 s (1 writer) | 400000 |

### 6.6 `RETURNING` at 1M rows, before and after W1b (2026-09-14)

`INSERT INTO t SELECT * FROM src RETURNING id`, 1M rows × 3 columns, the
pre-062 binary (1000-row statements) against this branch (333-row
statements): **69.8 s → 70.2 s**. The `OUTPUT INSERTED` statement is not
auto-parameterised at any size, and its cost is per row (~70 µs) rather than
per statement, so the cap is neutral for it. `RETURNING` stays the one large
INSERT nothing here speeds up; an `OUTPUT`-free bulk load followed by a read
is the way to get rows back fast.

### 6.8 The whole PR against main, end to end (2026-09-16)

One fixture, both binaries, interleaved A/B in the same session, two rounds,
min per cell. A = main `ddec241` (before this PR), B = this branch with
oluies' review fixes and #349 merged in. `SET threads = 4`, a bare heap
target for the INSERT cells, 1M rows × 3 columns unless the name says
otherwise. No settings are touched by the script — this is what the same
statement costs a user before and after.

| cell | A wall | B wall | B/A | A client CPU | B client CPU |
| --- | --- | --- | --- | --- | --- |
| `INSERT … SELECT`, 1M rows | 74.71 s | **0.51 s** | **0.01** | 1.604 s | 0.051 s |
| 20 × `INSERT … SELECT` of 500 rows (under the threshold) | 0.729 s | **0.219 s** | 0.30 | 0.170 s | 0.072 s |
| `INSERT … RETURNING`, 100k rows | 7.44 s | 7.15 s | 0.96 | 0.254 s | 0.226 s |
| `COPY … (FORMAT 'bcp')`, 1M rows | 0.739 s | 0.520 s | 0.70 | 0.214 s | 0.087 s |
| `CREATE TABLE … AS SELECT`, 1M rows | 0.855 s | 0.745 s | 0.87 | 0.109 s | 0.091 s |
| `UPDATE` of 5000 rows by rowid | 0.318 s | 0.312 s | 0.98 | 0.003 s | 0.004 s |
| `DELETE` of 5000 rows by rowid | 0.214 s | 0.223 s | 1.04 | 0.003 s | 0.003 s |

Reading it:

- **145× on the headline**, and 31× on client CPU — more than the 40× of
  § 0.1 because this cell fans out to four writers on a bare heap where that
  one measured a single writer.
- **3.3× on small inserts**, which is W1b alone: 500 rows × 3 columns is
  1500 constants, so before the cap every one of those twenty statements
  compiled its own ad-hoc plan.
- `RETURNING` is a wash (§ 6.6 says why), as designed.
- COPY and CTAS are the W0 move, and they must not have changed: 0.70 and
  0.87 are within the band this fixture swings over two rounds on a shared
  docker server (§ 6.1 saw ±1.5× on the four-writer cells), and nothing in
  the move touches the wire. Not claimed as an improvement.
- **UPDATE and DELETE carry W1c's new server transaction, and it costs
  nothing measurable** — 0.98 and 1.04, the latter ~9 ms over 5000 rows,
  which is the two extra round trips (`BEGIN` and `COMMIT`) at a local RTT.
  At a 20 ms RTT expect ~40 ms per statement, once, whatever its size. That
  is the price of the statement no longer being applied in part (#344).

### 6.7 Before the INSERT work

§ 0.1 and § 0.4 are the baseline: a DuckDB table of 1M rows into an existing
3-column heap costs the text path 73.7 / 74.2 / 74.9 s across three cold runs
and COPY 0.51–0.78 s; the text path's client CPU is 1.5–2.0 s against COPY's
0.12 s. W8 repeats this inside the bench harness, per family, after the work.

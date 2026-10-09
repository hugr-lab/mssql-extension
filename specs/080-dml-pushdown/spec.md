# Spec 080: DML on the server — pushed statements, a staged fallback, RETURNING, MERGE

**Status:** revised 2026-10-08 after the reconnaissance in
`recon-2026-10-08.md`, merged with oluies' design-review revision (#421, 21
findings, then 12 more from its branch review in `e2940db`). Its decisions are
folded in and marked "(#421)". That file holds the code inventory, the DuckDB API at
`4fbae437b22` and the Fabric/Synapse matrix, each with sources. This spec
cites it as "R§n". The first draft (2026-09-17, PR #364) is superseded; its
measured ground still stands in `../065-dml-pushdown-recon/` and is cited
from there. Revised again 2026-10-09 (owner): MERGE always on the server or
refused (D4), and no batching of a staged statement (D3).

**Goal**
- A DML statement the writer can express runs on the server as **one
  statement**, and no rowids travel.
- A DML statement the writer cannot express runs as a **staged JOIN** keyed
  by the best key the table has, or by all its columns if it has none.
- RETURNING works where the platform can return rows and is refused by name
  where it cannot. It never ends in an InternalException.
- MERGE INTO a catalog table runs on the server, always: pushed when the
  source is on the same server, through a `#src` filled by BCP otherwise, and
  refused by name when its conditions cannot be written in T-SQL (D4, owner
  2026-10-09).
- **Closes #140 on SQL Server / Azure SQL in PR 2.** It was reopened on
  2026-10-08: #364 closed it with text, not code. **Fabric** (the Fabric DML
  PR, owner 2026-10-09): the stage is filled by INSERT BULK -- Fabric's BCP
  API, in preview, which COPY and CTAS already use there -- and the staged
  statement selects its rows through `WHERE EXISTS` / a correlated subquery
  (D3), so keyed and keyless UPDATE / DELETE run there. With no live
  warehouse at hand, the forms run against SQL Server emulating Fabric
  (`mssql_test_dml_platform`); `fabric-probe/` p19 / p20 are to confirm them
  on a warehouse. Synapse keeps today's refusal. A keyless
  statement with a volatile function, or with a predicate the scan does not
  push, is refused by name on every platform.

**Not the goal**
- Strict string semantics. DML is native (owner, 2026-09-17; 079 D4).
- Table-valued parameters (owner, 2026-10-08: no `CREATE TYPE` rights can be
  assumed, and Fabric/Synapse must keep working).
- ON CONFLICT, which keeps today's behaviour (Open).

**Platforms**
- SQL Server 2019+ and Azure SQL (Database, Managed Instance): everything
  below.
- Fabric Warehouse: per the platform rule in D0.
- Synapse dedicated: no test environment. Today's DML path is kept, with no
  pushdown and no stage, and documented as untested, with two changes:
  - UPDATE / DELETE … RETURNING is refused by name instead of ending in the
    InternalException (PR 1);
  - **a rowid key must be enforced** (review of #422). Synapse dedicated
    accepts PRIMARY KEY / UNIQUE only as `NOT ENFORCED`, so such a key can
    match several rows and a keyed UPDATE / DELETE can hit rows the statement
    did not select. `ChooseRowIdKey` gains "the key is not enforced" beside
    "disabled" and "filtered" (`sys.indexes` / `sys.key_constraints` carry
    it), so on Synapse every UPDATE / DELETE through the catalog is refused by
    name. That is a behaviour change on a platform we cannot test, chosen over
    a wrong-rows hazard (PR 2);
  - **MERGE is refused by name** (D4, review of #424): its MERGE is in
    preview, and after PR 4 the catalog has no native MERGE path to keep.

  The host test tells Synapse apart (D0).
- Synapse serverless: no DML at all.

---

## D0: what the platform can do

DML forms differ by platform (R§4), so the catalog resolves a
`DmlCapabilities` once per ATTACH, next to `is_fabric_endpoint` and the
Synapse flag.

**Detection** is the existing host test, which already separates the three
platform families that matter here (review of #422; an earlier revision quoted
a stale comment of the function rather than its behaviour):
- `IsFabricEndpoint`: `.datawarehouse.fabric.microsoft.com` and
  `.pbidedicated.windows.net` (`azure_fedauth.cpp:114`);
- `IsSynapseEndpoint`: the whole `.sql.azuresynapse.net` domain
  (`ContainsIgnoreCase`, `azure_fedauth.cpp:126`). Dedicated and serverless
  are not told apart, and need not be: both keep today's DML path, and on
  serverless the server refuses DML itself;
- everything else is SQL Server / Azure SQL.

`SERVERPROPERTY('EngineEdition')` and `ProductMajorVersion` are added to the
ATTACH collation query for one thing only: `null_safe` (below). They are
**not** a platform switch. Fabric Warehouse is commonly reported with
EngineEdition 11, the value Synapse serverless reports too, so an edition test
would have classified Fabric as "no DML". A custom DNS name in front of Fabric
or Synapse defeats the host test, as it already does for the row-count pass;
that case is in Open. `fabric-probe/p17_server_properties` reads
`EngineEdition`, `ProductMajorVersion`, `@@VERSION` and `DB_NAME()` on a live
warehouse, so that the value is recorded rather than assumed.

Both properties ride the collation query as `SNAPSHOT_STATE_COLUMN` did
(#331), so there is no extra round trip, and they are cached on the catalog.
An unreadable property, an unknown edition, or a version below 16 takes the
INTERSECT form. That form is correct everywhere; the only cost of erring
that way is speed (#421).

| capability | SQL Server / Azure SQL | Fabric |
|---|---|---|
| `output_bare` (`… OUTPUT inserted.c`, p01) | yes, unless the target has an enabled trigger | until the probe: no |
| `output_into_table` (`… OUTPUT … INTO <table>`, p02; what RETURNING uses: `#out` since PR 2b) | yes | until the probe: no |
| `exact_count` (`ROWCOUNT_BIG()` into an RPC OUTPUT parameter) | yes | until the probe: no (the DONE count is used) |
| `update_from_join` (`UPDATE … FROM … JOIN`, `DELETE … FROM … JOIN`) | yes | **no** (documented) |
| `merge` | yes | yes (GA) |
| `stage_bulk` (`INSERT BULK` into `#stage`) | yes | yes: the BCP API (preview); COPY / CTAS use it already; a session `#temp` target to be confirmed by p19 |
| `null_safe` | `IS NOT DISTINCT FROM` when `ProductMajorVersion >= 16` or `EngineEdition IN (5, 8)` (Azure SQL DB / MI report version 12 while having the operator), otherwise `EXISTS (SELECT t.c… INTERSECT SELECT s.c…)` | the INTERSECT form until the probe confirms the operator |

Synapse (the host test) gets none of these. Its DML takes today's path
unchanged (§ Platforms). Its documented abilities (FROM … JOIN
with a bare target, MERGE in preview, no DDL inside a transaction) are recorded
in R§4 for later.

When the probe (`fabric-probe/`, R§4) shows a Fabric "until the probe" row
working, that row flips to yes. That is a one-line change and a test, not a
spec revision.

## D1: whole-statement DML through the rewriter

`SupportsPushdown(const QueryNode &)` gains `UpdateQueryNode`,
`DeleteQueryNode`, `InsertQueryNode` and `MergeQueryNode` cases. **The whole
decision is made there, in the dry run** (R§3: `RemoteExecute` cannot hand a
DML node back). It answers yes only when all of these hold:

- the node is not the body of a CTE. The hook carries no position: it is
  asked for every node whose result is one remote catalog (opt:315) and for
  nested nodes (opt:205), the same way for a statement-top DML and for
  `WITH c AS (UPDATE ms.t … RETURNING *) SELECT * FROM c` (review of #422).
  **The rule is: a DML node with a RETURNING list is not pushed** (review of
  #422). DuckDB accepts a DML as a CTE body only with RETURNING (a checked
  claim, W2), so this vetoes every CTE-body DML without needing to know the
  node's position, and without an identity key that a plan copy (079's
  inlined CTEs) could break. The cost: a top-level `UPDATE … RETURNING` is not
  pushed either; it runs on D3, with RETURNING through `OUTPUT … INTO @o` from
  PR 2. `mssql_dml_returning`, the row vehicle, is therefore kept in the
  design but **not built in PR 3**; it comes back with a position mechanism
  (079's per-rewrite thread state, keyed by something a copy preserves) when
  one is measured to work. The expected
  behaviour of the CTE case is then the shipped path: the CTE's DML runs on
  D3, RETURNING through `OUTPUT … INTO @o` where the platform has it, refused
  by name where not. An `EXPLAIN` wraps the statement and the DML inside is
  still pushed: `EXPLAIN` binds the vehicle without running it, and `EXPLAIN
  ANALYZE` runs it, as it runs a native DML;
- the catalog is not read-only;
- no `$n` parameter appears;
- every name is resolvable under the search-path rule 079 applies;
- every expression renders under 079's writer with no `value_divergence` and
  no `approximate` flag (a DML writes the value);
- a SET value, and each column an `INSERT … SELECT` fills, has the target
  column's DuckDB type, or a cast the vocabulary defines. DuckDB casts to the
  target type (DOUBLE → INT rounds), while T-SQL's implicit conversion
  truncates;
- the target is a table, not a view. The fallback refuses views (`GetRowIdColumns`),
  and a pushed DML through an updatable view would succeed where the same
  statement fails unpushed;
- the target has no INSTEAD OF trigger for the action: the count, and the
  rows OUTPUT returns, would be the trigger's. The table's metadata does not
  read triggers today; W2 adds an `INSTEAD OF` flag to the object row (084's
  first result set, no extra round trip);
- the platform has the form that is needed (D0).

**Vetoed:**
- `INSERT … VALUES`, `DEFAULT VALUES`, ON CONFLICT, BY NAME;
- an `INSERT … SELECT` whose column list omits a column that carries a
  **server DEFAULT**. The server would apply its default where the shipped
  path inserts DuckDB's NULL. `has_default` (`sys.columns.default_object_id
  <> 0`) rides the column metadata query (#421);
- `DEFAULT` in a SET;
- `UPDATE … FROM` / `DELETE … USING` unless each target row can match at
  most one source row: the join equates a unique key of the source. T-SQL
  updates from an arbitrary one of several matches, and DuckDB's own
  semantics for that case are pinned by a W8 test, not assumed;
- RETURNING without `output_into_table`.

A SET of a key column is **not** vetoed on the pushed path: T-SQL updates key
columns. The ban exists only on the fallback, where the key identifies the
row (D3).

No gain threshold applies. A DML either goes whole or plans as today.

| DuckDB | T-SQL (SQL Server / Azure) | Fabric |
|---|---|---|
| `UPDATE t SET c = e WHERE p` | `UPDATE [r1] SET [c] = e FROM [s].[t] AS [r1] WHERE p` | `UPDATE [s].[t] SET [c] = e WHERE p` (single-table form) |
| `UPDATE t SET … FROM u WHERE …` | `UPDATE [r1] SET … FROM [s].[t] AS [r1] JOIN …` | veto (no FROM) |
| `DELETE FROM t WHERE p` | `DELETE [r1] FROM [s].[t] AS [r1] WHERE p` | `DELETE FROM [s].[t] WHERE p` |
| `DELETE FROM t USING u WHERE …` | `DELETE [r1] FROM … JOIN …` | `DELETE FROM [s].[t] WHERE EXISTS (…)` (a semi-join, so no duplicate rows) |
| `INSERT INTO t (cols) SELECT …` (both remote) | `INSERT INTO [s].[t] ([cols]) SELECT …`; a named identity column is bracketed with IDENTITY_INSERT (077 W2) | same |
| `MERGE INTO t USING s ON … WHEN …` (both remote) | `MERGE [s].[t] AS [r1] USING … ON … WHEN …;` (D4) | same (GA) |
| `MERGE INTO t USING <anything else> …` | not the rewriter's: `MSSQLOptimizer` stages the source in `#src` and sends `MERGE … USING #src` (D4) | same |
| CTAS from remote to remote | our CREATE, then the pushed `INSERT … SELECT` (D6, the last PR) | same |

### The vehicles

`RemoteExecute` returns a ref to one of two table functions. Both bind with
no describe and no execution (081's bind, R§3), so `EXPLAIN` and `PREPARE`
change nothing.

**At least once: the vehicles keep a DML's eagerness (#421).** The DML binders
set `ResultEagerness::FORCED` (`bind_update.cpp:288`, `bind_delete.cpp:130`,
`bind_insert.cpp:722`). A statement bound as a SELECT gets `AUTO`
(`bind_select.cpp:10`), which is streamable. A pushed UPDATE opened as a
stream and closed unconsumed (JDBC `executeQuery` + `close`, the C API stream
fetch) could then never run, where the shipped path always completes inside
the call. The extension restores both properties itself, with no upstream
change:
- **`FORCED`.** `Bind(SelectStatement)` sets `AUTO` *before* it binds the node
  (`bind_select.cpp:9-11`). The vehicle's bind runs inside that, later, and
  writes `input.binder->GetStatementProperties().result_eagerness = FORCED`.
  `GetStatementProperties()` is the global binder state shared by every child
  binder (`binder.cpp:234`), and the planner copies it after binding
  (`planner.cpp:250`). `input.binder` is an `optional_ptr`: when it is absent
  the vehicle's bind **refuses** the statement, by name, rather than run
  without `FORCED` and without `RegisterDBModify` (review of #422). W8 checks
  it is present on every path the rewriter reaches (statement, EXPLAIN,
  PREPARE / EXECUTE).
- **`CHANGED_ROWS`.** `Bind(SelectStatement)` already gives a bare table-function
  passthrough the function's own `call_return_type` (`bind_select.cpp:14-19`).
  `SELECT * FROM <ref>` is exactly the shape the rewriter builds
  (`WrapRemoteRef`), so the count form declares `call_return_type =
  CHANGED_ROWS`.

Only `StatementType` stays `SELECT`. W8 asserts at-least-once from the
outside: open a stream over a pushed UPDATE, close it without consuming a
row, and assert the rows changed. **PR 3 does not merge if that assertion
fails.** It is the one finding that could veto the pushed half.

**Read-only** is refused at two points (#421, corrected in the review of #422):
- in the dry run, by a plain `IsReadOnly()` test that answers **false**, so
  the statement takes the shipped path and its hook refuses it with the
  shipped message. Not `CheckWriteAccess`: that throws
  (`mssql_catalog.cpp:1150`), which would abort the rewrite instead of
  declining it, with a message naming the wrong operation;
- in the vehicle's bind, through `CheckWriteAccess`, so no route around the
  dry run can execute a write.

There is one contract, not two. A database **opened** read-only
(`duckdb --readonly`) propagates `AccessMode::READ_ONLY` into the ATTACH, so
the catalog is already `IsReadOnly()`. `SET access_mode` cannot make one at
all, because `AccessModeSetting::OnGlobalSet` throws while the database runs.
W8 therefore opens the database read-only. `RegisterDBModify` in the vehicle's
bind adds DuckDB's one-writable-database-per-transaction rule.

**Where it executes: InitGlobal** (review of #422), as the 075 read path
does. Not at bind (EXPLAIN / PREPARE), not lazily on the first scan call: the
at-least-once gate, exactly-once and the pool-of-one behaviour all rest on one
known point. On a pool of one the vehicle takes the connection there and
gives it back before its result is returned.

**Exactly once (#421), enforced.** A vehicle has one global init and no
parallel local state. It is not a scan: there is no range to split and
nothing to re-initialise. A table function DuckDB initialised twice would run
the UPDATE twice, so the argument is backed by a **run-once latch** in the
bind data: a second InitGlobal of the same bound vehicle throws a named
InternalException instead of writing twice (review of #422). A future DuckDB
change that copies or re-initialises the plan then fails loudly. A `PREPARE`d
pushed DML executed twice affects its rows twice, as the shipped path does:
each EXECUTE rebinds, so each has its own latch.

**What still changes for a client (#421).** The statement's
`StatementType` stays `SELECT`, since the rewriter wraps the vehicle in a
SELECT. A binding keyed on the statement type rather than on the
return type would see a query. This is documented next to 079's
"result types change for a pushed statement", with `mssql_dml_pushdown`
(D5) as the way back.

**Constants are parameters** (review of #422). The writer renders a DML's
constants as `@pN`, as it does a pushed SELECT's (079 through 076/083's
`SqlParamSet`), and the vehicle sends the statement as `sp_executesql` over
RPC, with `@rc bigint OUTPUT` added when `exact_count` holds. One plan per
statement shape, which matters most on the write path, where one shape
repeats.

**A server error inside a transaction** (review of #422) takes the path a
shipped DML batch's error takes today: `MSSQLStatementConnection::Fail` on the
pinned connection does not roll back (the DuckDB transaction owns that) and
does not close it; the statement's exception aborts the DuckDB transaction,
and its ROLLBACK rolls the server transaction back. A server that already
doomed or ended the transaction (3960 update conflict under SNAPSHOT, a
deadlock victim) is the case #331 handled for ROLLBACK: the stale descriptor is
tolerated and the isolation level restored as its own statement.
`mssql_reset_connection = false` changes nothing here: it governs what happens
when the connection goes back to the pool, after the transaction. W8 forces a
3960 conflict and a constraint violation inside a transaction and asserts the
connection serves the next transaction.

**The vehicles are registered** through `mssql::RegisterDocumentedFunction`
(`mssql_function_docs.test` fails otherwise). They are callable, so they are
documented as internal: the rewriter's targets, not an API, with no
compatibility promise on their arguments.

- **`mssql_dml(context, statement, …)`**, the count form.
  - Its result is one BIGINT named `Count`.
  - It runs on a `MSSQLStatementConnection`: the pinned connection in a
    transaction, otherwise one connection under the `LoadTransaction`
    bracket, so the statement is atomic.
  - The count:
    - With `exact_count`, the statement is sent as `sp_executesql` over RPC
      with `@rc bigint OUTPUT` and `SET @rc = ROWCOUNT_BIG()` immediately
      after the DML. This needs the RETURNVALUE token parsed, which spec 083
      deferred (083 D5). A trigger's DONE and IDENTITY_INSERT's DONE can no
      longer replace the count. (A trailing `SELECT ROWCOUNT_BIG()` would
      avoid the RETURNVALUE code, but it is a result set an AFTER trigger's own
      result set could precede; the OUTPUT parameter is unambiguous.)
    - Without `exact_count`, the count comes from the DONE token of the DML
      statement itself, the last DONE with a count before the batch's final
      DONE. No platform without `exact_count` has triggers.
- **`mssql_dml_returning(context, statement, columns := {…})`**, the row
  form, only with `output_into_table`.
  - The statement is
    `DECLARE @o TABLE (…); <DML> OUTPUT inserted.… | deleted.… INTO @o …; SELECT … FROM @o`.
  - `INTO @o`, not a bare OUTPUT: a bare OUTPUT is refused while the target
    has an enabled trigger (R§4). The shipped INSERT … RETURNING still uses a
    bare OUTPUT and moves to `INTO @o` with it (W1).
  - `@o`'s column declarations follow the target's columns, with these
    exceptions:
    - a `rowversion` is declared `binary(8)`, because one cannot be inserted;
    - the native string types keep their length and collation;
    - TIMESTAMP_NS is `datetime2(7)`.
  - The rows are the post-image for UPDATE and the pre-image for DELETE
    (`inserted.*` / `deleted.*`), as DuckDB's RETURNING has them.
  - `RETURNING rowid` is vetoed.
  - The shape is the RETURNING list typed by the writer, read through the
    081 trusted-shape check.

**After a pushed DML** the target's cached row count and statistics entry are
invalidated. How depends on the transaction (#421):
- **In autocommit:** the shared entry is invalidated, as COPY and CTAS do
  today.
- **In an explicit transaction:** the change is uncommitted on the pinned
  connection, so the shared cache must not learn it. The path is
  `NoteTransactionChange(context, schema, table)`, plus
  `NoteTransactionChangeLocally` where the transaction's own later reads must
  stop trusting the shared entry. That is the route the catalog's own DDL
  takes: COMMIT publishes, ROLLBACK forgets (#380, #383). W8 asserts that a
  rolled-back pushed DML leaves the shared cache exactly as it was.

The table's column metadata is not touched: no epoch bump, describe cache
kept (R§3).

## D2: what a veto leads to

A vetoed statement plans as today, through `PlanUpdate` / `PlanDelete` /
`PlanInsert` / `PlanMergeInto`, on the path D3 describes.

**No DML predicate is ever partially pushed.** A DML's own WHERE and SET
either run on the server as one statement or run on the client whole. Extra
rows *written* are not harmless, so a DML predicate is never pushed in a
relaxed form (065 research § 2).

This does not cover a vetoed `INSERT INTO remote SELECT … FROM remote`: the
rewriter still pushes its **SELECT** alone (`push_select_only`), and the
shipped INSERT writes the rows. That is core behaviour, and it is safe,
because the pushed half only reads (#421).

## D3: the fallback, one path keyed by the ladder

The key is resolved per table at plan time:

1. **The primary key.**
2. **A usable unique index.** This is spec 077's `ChooseRowIdKey`, merged in
   #350. Its `RowIdRefusal` names why a key is unusable.
3. **All columns, NULL-safe.** The keyless base case, 067 § 1's argument: for
   a deterministic WHERE and SET, matching by value updates exactly the set
   DuckDB would.
   - Refused by name when:
     - a VOLATILE function appears in WHERE or SET;
     - the WHERE is not fully pushed to the scan (067 § 1: both sides must
       compare under the same semantics; first draft § D3).
   - **The key is every column of the table, or a refusal (#421).** A column
     that cannot be in the key does not shrink it; it refuses the statement.
     A key over a subset is sound only if the **target** is unique on that
     subset. The stage cannot show that: it holds the selected rows, not the
     table, and proving it costs a round trip and still races. Every refusal
     is answerable at plan time, from the column metadata and the statement's
     shape. The test is **round-trip fidelity**, not server comparability:
     the staged value is read into a DuckDB type and written back. There are
     two reasons, and the refusal names which one applies:
     - **"the catalog does not read its value":**
       - `is_cast_required` columns (read as NVARCHAR(MAX): `sql_variant`,
         `hierarchyid`, CLR UDT);
       - `xml` and `image` by name, because `is_cast_required` is false for
         both (`IsKnownSQLServerType` lists them);
       - `is_geometry` columns (`geometry`, `geography`);
       - `text` / `ntext`.
     - **"its value does not survive the round trip":**
       - `datetime` (1/300 s ticks);
       - `time(7)` and `datetimeoffset(7)` (100 ns on the server, µs in
         DuckDB).

       These are exactly what `IsLiteralMismatch` already refuses on rung 2
       (#358). Mis-keyed, they would match nothing and report **0 rows with
       no error**.

     Rungs 2 and 3 ask one shared predicate, `IsRoundTripExactForKey`,
     factored where `IsLiteralMismatch` lives, so a column rung 2 refuses by
     name can never be silently mis-keyed by rung 3. The MAX types compare
     exactly and stay in the key.
   - **`UPDATE … FROM` / `DELETE … USING` on rung 3 are refused by name.**
     Their join selects rows on the client, which is outside "the WHERE is
     fully pushed". Such a table needs a key.
   - **On rung 3 the stage carries the old values (the match) and the new
     values (the SET).** So a SET of any column is allowed there: the "no SET
     of a key column" rule exists for rungs 1–2 only.
   - On Fabric's MERGE form the stage is DISTINCT over the key, which is
     every column: a deterministic SET over equal rows gives equal new values,
     so the DISTINCT loses nothing, and without it MERGE fails with 8672 on
     duplicate stage rows. SQL Server's `UPDATE … JOIN` / `DELETE … JOIN`
     take duplicate stage rows as they are (PR 2).
   - **A string key column is matched by its bytes as well** (found in PR
     2's review): under the column's collation `'Ab'` equals `'ab'` and `'a'`
     equals `'a '`, so each such row would match the other's stage row -- an
     UPDATE could write one row's new value into the other, and a DELETE
     whose pushed LIKE told them apart would take both. The join adds
     `CAST(t.c AS varbinary(max)) = CAST(s.c AS varbinary(max))` beside `=`
     (kept for the seek); stage and target share type and collation.
   - **No concurrent writer** (review of #425): the scan and the JOIN are two
     steps, in autocommit two server transactions, and the key is the row's
     value, so a row another session inserts equal to a staged one in between
     is written too, and a selected row changed in between is not found. The
     latter, and a value that did not come back as read, are caught: the
     statement fails before its commit when the JOIN finds no row (rung 3) or
     fewer rows than distinct staged keys (rungs 1-2). The former is the
     user's to prevent: SNAPSHOT / REPEATABLE READ (`transaction_isolation`).
   - **How the key reaches the operator** (PR 2): DuckDB's binder appends
     the ids `GetRowIdColumns()` names and looks each up in
     `GetVirtualColumns()`, which takes virtual ids only, so the entry
     exposes one hidden virtual column per physical column
     (`MSSQL_KEYLESS_KEY_START + i`, named like the column, so a name
     resolves to the column and the hidden ones cannot be referenced), and the
     scan reads each as its column.
   - **Duplicates move together.** Target rows identical in every column are
     one match, so they are updated or deleted together. A row
     DuckDB's plan selected once can therefore count twice. This is the
     documented semantics of a keyless table, stated in the docs (W9) and in
     acceptance 3, not compared by the agreement harness.
   - The `rowid` pseudo-column stays refused on a keyless table (#421; 067 §
     5): the pseudo-column is the key's, and rung 3 has none to expose. Of
     the three bind-time `RowIdRefusal` throws in `mssql_table_entry.cpp`,
     two move to plan time (`BindUpdateConstraints`, `GetRowIdColumns`). The
     one raised for `"rowid"` stays. The staged path never materialises a
     rowid.

**Delivery.** Two forms:
- **Rungs 1–2, up to `mssql_dml_stage_threshold` rows** (BIGINT, default
  1000, the shape of `mssql_insert_bcp_threshold`; rows counted as they
  arrive, never estimated; W3 measures the crossover locally and at a 20 ms
  RTT): today's `VALUES`-join statements. Those statements are
  `UPDATE … FROM … JOIN (VALUES …)`, which Fabric does not have, so on Fabric
  every rung stages.
- **Everything else**: a session-local `#stage_<uuid>` on the statement's own
  connection. A `#` name, not `##`: the stage is filled on the statement's
  own connection, so no other session needs to see it.
  - A `#` name is **caught** by spec 063 D1 (`target_is_session_scoped` sets
    `policy.max_writers = 1`), so the stage fill is single-writer. That is
    what this path wants, since the rows go down one connection anyway, and
    it is a throughput ceiling W3 measures (#421).
  - The connection is taken at the **first `Sink`**, never at init
    (`BulkLoadSession::DeferAdoption` / `AdoptDeferred`, as COPY and CTAS
    do). On a pool of one, the source scan has given the connection back by
    then (#421).
  - The stage is filled fully first, then the JOIN runs (first draft § D3:
    no scan ∥ DML pipelining; rung 3 cannot seek).
  - The stage is dropped on the way out, including on failure (review of
    #422). A statement that throws mid-fill unwinds through destructors on
    worker threads with no `ClientContext` (the #178 / #191 contract), as a
    failed bulk load does: the cleanup follows `ReleaseBcpConnectionOnError`,
    with its `reset_on_release` parameter carried and never defaulted. An Idle
    connection gets `DROP TABLE #stage_<uuid>`; one left mid-response is
    closed rather than pooled, unless it is pinned, where the transaction's end
    takes it. Without that, `mssql_reset_connection = false` would let
    repeated failures accumulate `#stage_*` tables in a pooled session.

**Batching (#421; owner, 2026-10-09):**
- The `VALUES`-join statements keep today's sizing (`mssql_dml_batch_size`,
  capped by `mssql_dml_max_parameters`).
- **A staged statement is one statement over the whole stage**, on every rung
  and for MERGE (D4). How much one statement writes is the business of whoever
  writes it, and the extension does not split it. Splitting is also not
  equivalent in general:
  - on rung 3 the key is every compared column, and any SET rewrites part of
    it. Keyless `(a, b)` holds `(1, 1)` and `(2, 2)`, the statement is
    `UPDATE t SET a = a + 1`; batch 1 turns `(1, 1)` into `(2, 1)`, and batch
    2's key `(2, …)` matches it again;
  - for MERGE, `WHEN NOT MATCHED BY SOURCE` would take every target row
    outside the batch, a target row matched from two batches would be written
    twice instead of refused, and a SET of an ON column moves a row into a
    later batch's match.

  One statement matches against the target as it stood before it. The stage
  fill itself still goes in `mssql_copy_flush_rows` bulk-load batches; that is
  the wire, not the statement.

**#358, a proposal measured in W3** (until then `IsRoundTripExactForKey`
refuses these columns on both rungs, as rung 2 does today):
- **The cause.** A `datetime` key is read as a microsecond TIMESTAMP and sent
  back as a `datetime2(7)` value. The server compares `datetime` with
  `datetime2` exactly, and the two never meet.
- **The proposed fix:** convert the key to **the column's own type** before
  the comparison.
  - The `VALUES` literal is rendered `CAST(… AS datetime)`.
  - The stage column is declared `datetime`; BCP sends it as `datetime2` and
    the server rounds it to the tick (write_column_ops.cpp:145).
  - Either way it should land on the stored value's 1/300 s tick.
  - **If W3 measures that exact** across the tick values, `datetime` leaves
    the refusal list on both rungs and #358 closes for it.
- **`time(7)` and `datetimeoffset(7)`** lose their 100 ns digit on read; no
  conversion recovers it. They stay refused.
- **`datetime2(7)` in range is lossless** (it reads as TIMESTAMP_NS). Out of
  range it reads as NULL, and the refusal covers it.

| step | SQL Server / Azure | Fabric |
|---|---|---|
| fill | `INSERT BULK`, adopted at the first `Sink` (`DeferAdoption` / `AdoptDeferred`) | the same (BCP API) |
| UPDATE | `UPDATE [t] SET … FROM [s].[t] AS [t] JOIN #stage AS s ON <key>` | `UPDATE [s].[t] SET [c] = (SELECT TOP (1) stg.[n] FROM #stage AS stg WHERE <key>) … WHERE EXISTS (SELECT 1 FROM #stage AS stg WHERE <key>)` |
| DELETE | `DELETE [t] FROM … JOIN #stage …` | `DELETE FROM [s].[t] WHERE EXISTS (SELECT 1 FROM #stage AS stg WHERE <key>)` |
| rows | up to `mssql_dml_stage_threshold` as VALUES-join statements, past it the stage | always the stage: the VALUES join is an `UPDATE … FROM` too |

**Not MERGE on Fabric** (the Fabric DML PR): MERGE refuses a target row matched
twice (8672), so it needs a deduplicated stage, and a `DISTINCT` under the
column's collation folds rows the byte-exact match keeps apart: `'a'` and
`'a '` even under Fabric's default `Latin1_General_100_BIN2_UTF8` (BIN2 still
ignores trailing spaces), and `'Ab'` / `'ab'` too on a warehouse created
case-insensitive (`…_CI_AS_KS_WS_SC_UTF8`) -- one stage row would match only
one of them, leaving the other unwritten. The subquery forms take duplicate stage rows as they are: `EXISTS`
does not count them, and stage rows matching one target row are equal in the
key; where they differ (DuckDB stages one rowid twice under `UPDATE … FROM`
with two matching source rows) every SET column's `TOP (1)` takes the same
stage row, under one total order over the new values, as the JOIN form takes
one row for the whole target row. A native MERGE's UPDATE / DELETE actions are refused on Fabric until
PR 4 (they run the VALUES join).
| in a transaction | stage created on the pinned connection | same |

The `<key>` comparison on rung 3 uses `null_safe` (D0).

**RETURNING on the fallback**, with `output_into_table` -- its own PR right
after PR 2 (owner, 2026-10-09: DuckDB's RETURNING chunk carries the old image
as well under `capture_old_rows`, and a DELETE's carries the virtual columns,
on rung 3 one per column):
- Every RETURNING statement stages, whatever the row count, and the JOIN
  statement carries `OUTPUT inserted.… / deleted.… INTO #out`; `#out` is read
  back as a stream.
- The operator returns DuckDB's RETURNING chunk: every non-generated column,
  in table order, at DuckDB's types (binder.cpp:571-580). That is the
  post-image for UPDATE and the pre-image for DELETE.
- `#out` is created like the stage (`SELECT … INTO … UNION ALL`), from the
  scan's own read expressions (`BuildReadExpression`: spatial as WKB, legacy
  LOBs and CLR / alias types cast; rowversion as `binary(8)`), and OUTPUT
  writes the same expressions over `inserted.` / `deleted.`: tempdb never has
  to know a user type, and `#out` is read back bare (review of 2b). A
  DELETE's chunk appends the table's virtual columns (the rowid, or rung 3's
  hidden key columns), and its key is taken where the plan's row-id
  expressions put it (`WHERE rowid = …` binds the rowid first).
- The price is the stage's round trips even for one row: create the stage,
  INSERT BULK, create `#out`, the JOIN, the read, the DROP.

Without `output_into_table`, RETURNING is refused by name at plan time:
`PlanUpdate` / `PlanDelete` read `op.return_chunk`. That is the
InternalException fix (R§2).

**The 066 remainder.** `CollectSinkCatalogs` counts `LOGICAL_UPDATE` /
`LOGICAL_DELETE` (and `LOGICAL_MERGE_INTO`). So the scans feeding a DML
materialise at init inside a transaction and on a pool of one, and
`defer_execution_` with its buffers goes (no per-value path).
`BindUpdateConstraints` / `GetRowIdColumns` / `GetRowIdType` stop refusing a
keyless table at bind: the refusal moves to plan time and names rung 3's
guards. "Fully pushed" means that the scan applied every filter DuckDB handed
it on the server. A refused filter runs in the scan's own net
(`table_scan.cpp`), not as a PhysicalFilter, so `PlanUpdate` asks the scan's
bind data, not the plan shape.

Materialising the scan that feeds a large UPDATE inside a transaction holds
its rows on the client. That is the price of sharing the one pinned
connection; it spills like any `ColumnDataCollection` (Risks).

**One token loop.** The four DML loops (UPDATE, DELETE, INSERT and INSERT
RETURNING's parser) become one `ExecuteDmlBatch`, which the new vehicles call
too. `MSSQLSimpleQuery` stays as it is. It reads `query_timeout` instead of a hard-coded
30 s, keeps the `mssql_test_fail_parse_after_tokens` lever, and reports a
desync naming what the server executed (#323).

## D4: MERGE INTO

Owner's decision, 2026-10-09: **a MERGE into a catalog table runs on the
server, always, or is refused by name** -- as DuckLake does. There is no
native MERGE path for the catalog after PR 4.

Why not the native path (DuckDB's `Catalog::PlanMergeInto`, each action
through our `PlanUpdate` / `PlanDelete` / `PlanInsert`):
- each action is its own operator with its own `MSSQLStatementConnection`, so
  in autocommit a failing action leaves the others committed, the actions take
  turns on a pool of one, and UPDATE / DELETE actions hold their rows until
  Finalize (found in PR 1, #423);
- DuckDB tells a matched row from an unmatched one by its first row-id column
  (`PhysicalMergeInto::ComputeMatches`: "the first row-ID component is also
  the target-presence marker"). On a keyless table (rung 3) that is the value
  of the first column, NULL whenever the column is, so a WHEN NOT MATCHED
  INSERT or an ERROR action would fire on a matched row (found in PR 2).

**Two forms, one T-SQL writer:**
- **The source is on the same server** (a table of the catalog, or a query
  over them that the writer renders): the rewriter hands the `MergeQueryNode`
  over whole (duckdb#24854), and the vehicle sends
  `MERGE [s].[t] AS t USING (<query>) AS s ON … WHEN …;`.
- **Any other source** (a DuckDB table, a file, another catalog, a VALUES
  list): the rewriter sees two catalogs and does not offer the node
  (`RemotePushdownOptimizer::RewriteNode(MergeQueryNode)` merges to Unknown).
  `MSSQLOptimizer` takes the `LogicalMergeInto` instead, before physical
  planning (`PlanMergeInto` receives the source already joined to the target,
  plan_merge_into.cpp:351-356, too late):
  - the source subtree stays as the operator's child; the join and the
    target's scan go;
  - every expression over source columns only (`s.a * 2`, any DuckDB
    function) is computed by DuckDB into a `#src` column, so only what touches
    the target is written in T-SQL: the ON condition, the WHEN conditions, and
    SET / INSERT values that read the target;
  - **What the rewrite takes apart** (review of #424). The binder builds the
    match below `LogicalMergeInto`: a projection over a join of source and
    target (bind_merge_into.cpp:445-447), the join type chosen from the
    actions present (:314-320; INNER, LEFT, RIGHT, FULL), the sides inverted
    for a RIGHT join (:341), and for WHEN NOT MATCHED BY SOURCE a
    `source_marker` column added by that projection (:381-402). Every action
    expression is bound against that projection. The rewrite removes the
    projection, the join and the target's scan; the join type is not kept
    (T-SQL's MERGE has its own); and each action expression's column
    references are remapped through the deleted projection onto the target
    table's columns (`t.[col]`) or the `#src` columns (`s.[cN]`), the
    inverted case included. A reference the remapping cannot place (the
    `source_marker`, a row id) is a refusal, never a guess;
  - `#src` columns are typed with the source's DuckDB types through the CTAS
    mapping, **except a column the ON condition compares with a target
    column** (review of #424): that one is declared with the target column's
    own SQL Server type and collation, as rung 3's stage is, so the server
    compares like with like (`datetime` against `datetime`, #358; a string
    under one collation, else error 468). A target column the ON condition
    reads must pass `IsRoundTripExactForKey`, the predicate rungs 2-3 use,
    or the MERGE is refused by name: a mismatch there is worse than in an
    UPDATE -- the unmatched row makes WHEN NOT MATCHED INSERT fire and
    duplicates the target row;
  - `#src` is D3's stage in every other respect, by reference: the fill per
    platform (INSERT BULK on the statement's connection, the first `Sink`,
    `DeferAdoption`; `INSERT … VALUES` batches on Fabric until `stage_bulk`),
    created inside the statement's server transaction so a failure's ROLLBACK
    drops it, a connection left mid-response closed rather than pooled, one
    writer (063 D1); then one `MERGE [s].[t] AS t USING #src AS s ON … WHEN
    …;`, then dropped;
  - the bound expressions render through `ExpressionVocabulary`, the atoms the
    scan path and the 079 writer share, with a resolver for two relations:
    a target binding to `t.[col]`, a source binding to `s.[cN]`.

**Refused by name** (the whole statement, nothing sent):
- a condition or value touching the target that the vocabulary cannot write;
- a shape T-SQL does not have: more than two WHEN MATCHED clauses (one UPDATE
  and one DELETE, the first conditional), more than one WHEN NOT MATCHED [BY
  TARGET], more than two WHEN NOT MATCHED BY SOURCE; DuckDB allows any number,
  with first-match semantics;
- DO NOTHING anywhere but last of its kind (omitting it there changes
  nothing; elsewhere a later clause with an overlapping condition would take
  its rows); ERROR; `UPDATE SET *`; RETURNING (DuckDB refuses it itself);
- an INSERT action naming the identity column (no IDENTITY_INSERT bracket
  inside MERGE in this spec);
- Synapse (MERGE in preview there; D0).

**Semantics.** The source is evaluated by DuckDB, the match and the conditions
by the server, under the column collations (D4 of 079: native). A target row
matched by two source rows is an error in T-SQL (8672) as in DuckDB: same
outcome. One statement, no batches (D3), atomic: one connection and one server
transaction, committed with the statement's `MSSQLTransaction` in autocommit.
No key is needed, so keyed and keyless targets are the same case. The count is
`exact_count`'s where the platform has it, else the MERGE's DONE count.

**Until PR 4** the native path stays for keyed tables, with PR 1's fixes
(`PlanMergeInto` names the INSERT action's unnamed columns; actions defer to
Finalize, since in autocommit on a larger pool they hold separate server
transactions and would deadlock through the client otherwise). A keyless
target is refused by name from PR 2.

The same `#src` mechanism serves `UPDATE t … FROM <local>` and
`DELETE FROM t USING <local>` (`UPDATE t SET … FROM [s].[t] AS t JOIN #src
AS s ON …`), today a client-side join that rung 3 refuses; it follows PR 4 as
its own step.

## D5: the DML switch (#421)

`mssql_dml_pushdown` (BOOLEAN, default **true**, `SetScope::GLOBAL`) is read
in `SupportsPushdown` for the DML and MERGE nodes, and in
`SupportsPushdown(const SQLStatement &)` for CTAS (D6).

It governs the **pushed** form only (review of #424). A MERGE whose source the
setting keeps from being pushed takes the staged form (`#src`, D4), which
`MSSQLOptimizer` applies whatever the setting: after PR 4 a MERGE into the
catalog has no other way to run, so the setting cannot send it "down D3's
path" -- D3's ladder is UPDATE / DELETE's.

`mssql_remote_pushdown` cannot be the DML lever. 079 D6 reads it once at
ATTACH: it also answers `IS_REMOTE`, so after attaching it is not a switch,
and throwing it would give up the read path too. The DML switch is read per
rewrite, so it can be flipped at any time, and when it is off every pushed
SELECT is left alone.

It ships **true** (owner, 2026-10-08). The agreement suite (W8) is the gate:
it must be green on every pushed shape before the PR that turns pushed DML on
merges. The switch is the escape hatch for the first risk, not a rollout
stage. There is no per-session form (079 D6's reason).

## D6: pushed CTAS (#421; last PR, droppable)

`CREATE TABLE remote AS SELECT … FROM remote` becomes our CREATE (table kind,
collation, lengths: the WITH options keep their meaning) followed by a pushed
`INSERT … SELECT`, run on one connection in one server transaction, so a
failed load leaves no table behind.

- It needs `EXECUTE_STATEMENT`, a **catalog-wide** claim, fixed at ATTACH
  like `IS_REMOTE` (079 D6), so `mssql_dml_pushdown` cannot withdraw it
  (#421).
  - It is claimed only from the PR that adds pushed CTAS (PR 5), and only
    when the catalog's ATTACH-time `remote_pushdown` answer is on. PRs 1–4
    change no DDL routing.
  - Once it is claimed, `mssql_dml_pushdown = false` restores pre-080
    **behaviour**, not pre-080 **routing**: DDL still passes through the
    rewriter's DDL path, is vetoed there, and lands on the shipped path
    unchanged.
  `SupportsPushdown(const SQLStatement &)` answers false for every other
  shape: DROP, ALTER, CREATE SCHEMA and CREATE VIEW keep the shipped path,
  and W8 asserts that.
- **The column types** come from `sp_describe_first_result_set` on the
  rendered SELECT, at execution, on the statement's connection. The rewriter
  runs before binding, so no bound types exist yet. `SELECT INTO` would let
  the server pick the types.
  - The types then go through the shipped CTAS type mapping
    (`mssql_ctas_text_type`, `mssql_default_string_length`,
    `mssql_utf8_collation`, `mssql_default_table_kind`).
  - A describe the server cannot answer **fails the statement at execution**,
    never 075's run-for-the-shape fallback. It is not a veto in this spec's
    sense (decline and let the shipped path run): by then there is no shipped
    path left (see the kept regression below).
- `OR REPLACE` / `IF NOT EXISTS` / `WITH (table_kind = …)` behave as they do
  on the shipped CTAS.
- After it, the catalog is invalidated as the shipped CTAS invalidates it
  (`InvalidateSchemaTableSet` + `NoteTransactionChange`). The "no epoch bump"
  rule of D1 is for DML only.
- The ref is lazy, so `EXPLAIN` / `PREPARE` create nothing.
- When CTAS is vetoed, the rewriter still pushes its query alone, as today.
- **Droppable.** It is the one shape that needs a round trip before it can
  decide. If it is dropped, nothing in the other PRs changes.
- **A known regression, kept (#421).** Over a SELECT the server cannot
  describe, pushed CTAS fails where the shipped path succeeds (075's F1
  runs such a batch at bind). By describe time the CREATE has already been
  replaced, so there is no shipped path left to hand it back to. Moving the
  describe earlier would cost a round trip at optimize time on every CTAS,
  `EXPLAIN` included. `mssql_dml_pushdown = false` is the way back, and a W8
  case covers it.

## Work

### W1: the vehicles and the count
- `mssql_dml` / `mssql_dml_returning` (bind with no describe, `RegisterDBModify`,
  `MSSQLStatementConnection`).
- RETURNVALUE parsing for the `exact_count` OUTPUT parameter.
- `ExecuteDmlBatch`, with the four DML loops folded into it.
- Statistics invalidation after a pushed DML (the staged / `VALUES` path gets
  its own in PR 2, W3).

### W2: the writer's DML nodes
- `WriteUpdate` / `WriteDelete` / `WriteInsertSelect` / `WriteMerge`, with
  the platform forms of D1.
- **The DML path bypasses `SQLWriter::PushesMoreThanScan`** and the join /
  `mssql_pushdown_min_rows` thresholds (R§3): they veto every non-SELECT, and
  a DML is decided on renderability alone (review of #422).
- The RETURNING veto (D1), and a test pinning the claim it rests on: DuckDB
  refuses a DML CTE body without RETURNING.
- Columns resolved against the entries 079 D1 recorded for this rewrite, on
  this thread (#421). Never a fresh lookup by name: without a `ClientContext`
  that lookup reads the shared cache and misses the transaction's own layer
  (#380).
- `has_default` and an `INSTEAD OF` flag added to the metadata queries
  (084's result sets, no extra round trip).
- The dry-run vetoes of D1 and the top-node rule.
- The unique-source rule for `UPDATE … FROM`.

### W3: the ladder and the stage
- Row-count / statistics invalidation after a `VALUES` or staged DML, landing
  with the staged path in PR 2 rather than with the pushed path in PR 3, so
  the planner never reads a stale count between the two (review of #422).
- The enforced-key requirement in `ChooseRowIdKey` (Synapse `NOT ENFORCED`).
- `IsRoundTripExactForKey`, shared by rungs 2 and 3, with the two named
  refusals (#421).
- `mssql_test_force_intersect_join_form`: a registered test-only setting,
  off by default, in the style of `mssql_test_fail_metadata_after_rows`.
  The integration lane is a release build against SQL Server 2022, which
  takes the operator, so without it the INTERSECT form (the default, and the
  only form correct everywhere) would never run in CI (#421).
- The stage-fill rate of a large keyless statement (single-writer by 063 D1).
- Rung 3.
- `#stage` with columns typed as the target's, both fills, the JOIN / MERGE
  forms per platform, `null_safe`, the volatile and unpushed-predicate
  guards, LOB exclusion.
- The #358 measurement (datetime round trip; the microsecond range for the
  7-digit types).

### W4: the 066 remainder and the bind-time refusals
- `CollectSinkCatalogs` += UPDATE / DELETE / MERGE.
- `defer_execution_` removed.
- The bind-time keyless refusal moved to plan time.
- RETURNING on the fallback, or refused by name.

### W5: D0
- `DmlCapabilities` at ATTACH, with `EngineEdition` in the collation query.
- `fabric-probe/` run, and the rows it settles flipped.

### W6: cleanups
- `mssql_dml_use_prepared`: registered and read by nothing. Deprecated as a
  documented no-op for one minor release (the 047 precedent), then removed.
  Unregistering it now would make `SET mssql_dml_use_prepared = …` throw and
  kill the rest of a `.duckdbrc`.
- `EnsurePKLoaded` deleted (a `D_ASSERT` since 084).

### W7: MERGE
- The T-SQL MERGE writer with D4's shape vetoes, shared by both forms.
- The pushed form (the source on the same server) through the vehicles.
- The staged form: the `LogicalMergeInto` rewrite in `MSSQLOptimizer`, `#src`
  (types, collations, source-only expressions computed by DuckDB), the
  two-relation resolver over `ExpressionVocabulary`.
- The native path retired for the catalog; every MERGE test runs on the
  server or is a named refusal.

### W8: tests
From the first draft:
- the bite test (065 § 4.1): an unmapped function in WHERE takes the
  fallback, asserted by the `remote_pushdown` counter and new `dml_staged` /
  `dml_values_join` counters under `MSSQL_COUNTERS`;
- pushed correctness against expected row sets: multi-column SET, CASE in
  SET, functions in WHERE, empty match, full table, `UPDATE … FROM` a remote
  join; transactions (BEGIN / COMMIT / ROLLBACK, mixed with reads and a
  sink);
- the ladder: rungs 1 / 2 / 3 asserted through the counters; keyless
  duplicates (both move); NULL-bearing keys in both join forms; the volatile
  guard; #140's reproduction end to end;
- `INSERT … SELECT` both remote: the rows never reach the client (counter and
  `mssql_pool_stats` bytes), the identity bracket;
- 065's acceptance 1 as a bench (1M matching rows, the wide fixture);
- pool of one with a short `mssql_acquire_timeout`: UPDATE / DELETE pushed and
  staged, autocommit and transaction (extends
  `transaction_single_connection_pool.test`);
- rung 3 under load: a keyless DELETE of 200k rows with a concurrent reader on
  another session completes with no 1205.

From #421:
- the agreement harness: the same DSN attached twice, once with the ATTACH
  option `remote_pushdown false`. For each pushed shape: seed the table, run
  the statement through the pushed alias, read the state and `Count`;
  re-seed, run it through the other alias, read again; compare. The final
  state is read through the `remote_pushdown false` alias both times, so the
  reader itself is never pushed (review of #422);
- exactly once (`threads = 4`; `PREPARE` executed twice);
- rung 3's hazards: the `(1, 1)` / `(2, 2)` `SET a = a + 1` shape on a stage
  of more than one fill batch; keyless tables with an `xml` / a
  `geometry` column;
- both join forms (the INTERSECT form forced by
  `mssql_test_force_intersect_join_form`, in the release-build integration
  lane);
- DROP / ALTER / CREATE SCHEMA / CREATE VIEW unchanged after the
  `EXECUTE_STATEMENT` claim;
- `mssql_dml_pushdown = false`;
- the read-only refusals: a read-only catalog and a database opened
  read-only, both guards, one message;
- **at least once**: a pushed UPDATE opened as a stream and closed unconsumed
  changed its rows (PR 3's gate);
- a rolled-back pushed DML leaves the shared cache as it was;
- keyless tables with an `xml`, a `geometry`, a `text`, a `time(7)` and a
  `datetime` column: each refused by name with its reason, never 0 rows;
- pushed CTAS over an undescribable SELECT: the documented failure.

And these:
- `UPDATE` / `DELETE … RETURNING` on both paths: rows match DuckDB's own
  semantics, and the InternalException reproduction is now a pass or a named
  refusal.
- `EXPLAIN` / `PREPARE` of a pushed DML change nothing, and `ATTACH … READ_ONLY`
  refuses through `RegisterDBModify`. A DML into two attached catalogs in one
  transaction is refused by DuckDB's own rule.
- An AFTER trigger on the target: the `exact_count` count is the
  statement's. A target with an INSTEAD OF trigger is not pushed.
- MERGE: each action kind, keyed and keyless targets, a source on the same
  server and a DuckDB source (a table, VALUES, a file); a target row matched
  twice errors; each refusal of D4 by name; atomic in autocommit (a failing
  action leaves nothing); ON over a `datetime` column (every row matched, no
  duplicate inserted) and over a `time(7)` column (refused by name); a RIGHT
  join shape (WHEN NOT MATCHED only) and WHEN NOT MATCHED BY SOURCE (the
  `source_marker` case).
- Pool of one: UPDATE, DELETE and MERGE in autocommit and in a transaction,
  pushed and staged (extends `transaction_single_connection_pool.test`).
- The `fabric-probe/` files stay probes (`# group: [fabric_probe]`, a
  placeholder answer that prints the server's): they are run by hand per
  their README, never in the `[fabric]` lane. What a probe settles becomes an
  ordinary `[fabric]` test in the PR that flips the D0 row.

### W9: docs
**Each PR documents what it ships** (review of #422): its settings, its
refusal messages, its semantics (rung 3's "duplicates move together" with PR
2, the deprecation line with PR 1), in CHANGELOG, CLAUDE.md, DATAMODEL and the
website. PR 5 adds only pushed CTAS's part and the final pass. The whole, when
done:

README / website DML page:
- what runs on the server;
- the ladder;
- RETURNING;
- MERGE;
- the platform table.

Also:
- DATAMODEL: the DML flow (rewriter → one statement | plan → ladder →
  stage);
- the CLAUDE.md DML line and the new settings (`mssql_dml_pushdown`,
  `mssql_dml_stage_threshold`, and the test-only
  `mssql_test_force_intersect_join_form`, documented as the other
  `mssql_test_*` options are);
- CHANGELOG;
- "what a pushed DML changes for a client" (#421).

### PR plan (#421)

Not one PR: spec 079 moved away from one large PR because "the size is the
risk", and this spec is no smaller. Each PR merges before the next opens
(never stacked). The order follows the dependencies:

| PR | contents | stands on its own because |
|---|---|---|
| **1** | W4 (the 066 remainder: sinks, defer removed, pool of one); RETURNING on UPDATE / DELETE **refused by name** at plan time (the InternalException fix); W6 | fixes UPDATE / DELETE on a pool of one and the crash, behind no setting |
| **2** | W3 (the ladder, rung 3, `#stage`, `mssql_dml_stage_threshold`, #358) and W5's capability read (`EngineEdition` / `ProductMajorVersion`); the `fabric-probe/` run if the warehouse answers by then (the rows it settles flip, one line each) | closes #140 and #358 with no rewriter involved |
| **2b** | RETURNING on the fallback (`OUTPUT … INTO #out`), split from PR 2 (owner, 2026-10-09); the capability is `output_into_table` (it was named after `@t`) | UPDATE / DELETE … RETURNING return rows instead of a named refusal |
| **3** | W1 + W2 (the count vehicle, the run-once latch, the exact count, `ExecuteDmlBatch`, the writer's UPDATE / DELETE / INSERT … SELECT, `mssql_dml_pushdown`); the agreement harness | pushed DML end to end, PR 2's path under every veto |
| **4** | W7: MERGE on the server -- pushed when the source is on the same server, through `#src` otherwise, refused by name when not writable in T-SQL; the native path retired for the catalog | MERGE on top of PRs 2–3 (PR 1 made the native MERGE run in a transaction and on a pool of one, #423; it stays until this PR) |
| **5** | D6 (pushed CTAS) and its docs; W9's final pass | droppable without touching PRs 1–4: every earlier PR carries its own docs |

PRs 1 and 2 change the shipped path only and are live from the moment they
merge, which is why they come first.

## Not proposed

- `%%physloc%%` as a row identifier (067 § 5).
- A strict-string DML mode (065 § 8.5 keeps the measured form).
- TVPs (owner, 2026-10-08).
- Pushing a relaxed predicate for a DML.
- ON CONFLICT through `MERGE` (Open).
- A per-session pushdown switch, for the DML half or the read half (079 D6:
  `Supports` takes no `ClientContext`).
- **A reduced rung-3 key** (#421): dropping a column for cost or because it
  cannot be read, and keeping the statement. It is sound only if the target
  is unique on the rest, which the stage cannot establish. Earlier drafts
  carried it, including this revision's "re-apply the WHERE" form; it is
  withdrawn for the simpler all-or-refusal rule.
- Batching a staged statement or a MERGE, by a setting or otherwise (owner,
  2026-10-09: the volume of one statement is its author's business; D3 names
  why batches are not equivalent).
- A native MERGE fallback for a statement the T-SQL writer refuses (owner,
  2026-10-09; DuckLake refuses the same way).

## Risks

- **A pushed DELETE removes the server's set**, padded and case-variant rows
  included (079 D4). That is today's behaviour on the rowid path too.
- **Fabric's table-level write conflicts.** Two writers to one table conflict
  even on different rows (24556 / 24706), and MERGE always conflicts. That is
  the platform's behaviour, so it is documented, not worked around.
- **Rung 3 locks.** A JOIN that cannot seek escalates toward a table lock.
  Stage first, then join; RCSI is the user's lever.
- **`exact_count` depends on RETURNVALUE parsing**, new TDS code. Its
  fallback (the DONE count) is today's behaviour.
- **MERGE on SQL Server has a history of plan bugs** with filtered indexes
  and indexed views. Today's metadata does not tell us about either (077's
  query reads unique indexes only), so the pushed MERGE carries that risk as
  documented, until the metadata reads them.
- **The pushed and fallback paths can diverge.** D1's vetoes close the view
  and implicit-conversion differences. One difference is deliberate: a SET of
  a key column is pushed, while on rungs 1–2 the fallback refuses it. The
  agreement harness (W8: the same DSN attached twice, once with the ATTACH
  option `remote_pushdown false`) states that case instead of comparing it,
  and catches the rest.
- **`INSERT … SELECT` and the identity seed.** SQL Server's identity seed is
  not transactional (077): a rolled-back pushed insert still advances it, as
  a rolled-back statement insert does (#421).
- **The remainder's blast radius.** `CollectSinkCatalogs` is on the planner's
  path for every DML; the transaction suite is the guard (#421).
- **A large UPDATE in a transaction materialises its source scan.** Its rows
  are held on the client, spilling as `ColumnDataCollection` does.
- **MERGE refuses more than it used to.** A keyed MERGE whose WHEN condition
  uses a DuckDB-only function over a target column ran natively until PR 4 and
  is refused from it; the user rewrites the condition or sends the statement
  with `mssql_exec`.
- **One large staged statement** (an UPDATE / DELETE through `#stage`, a MERGE
  through `#src`) holds its stage in tempdb and its changes in one
  transaction's log; nothing splits it (D3).
- **The join-form probe** errs toward the INTERSECT form on an unknown
  edition. That form is correct everywhere and only slower on 2022+.
- **Rung 3 on a table with an unusable column** (`xml`, `geometry`,
  `sql_variant`, `hierarchyid`, `text` / `ntext` / `image`, `datetime`,
  `time(7)`, `datetimeoffset(7)`) has no rung 3 and is refused by name. That
  is a real table shape this spec does not make writable; the fix for the
  user is a unique index (#421).
- **At least once** depends on the vehicle setting `FORCED` from its bind.
  W8's assertion proves it, and it gates PR 3 (#421).

## Open, decided later

- ON CONFLICT (DuckDB's upsert) → T-SQL `MERGE`: a D1 row once D4 is in.
- `UPDATE … FROM` on Fabric through `MERGE`: once the Fabric forms are
  measured.
- `UPDATE … FROM` / `DELETE … USING` a DuckDB source through `#src` (D4's
  mechanism): after PR 4, as its own step.
- Fabric or Synapse behind a custom DNS name, which the host test misses (as
  it misses it for the row-count pass). An ATTACH option naming the platform
  is the likely answer; EngineEdition alone cannot tell Fabric from Synapse
  serverless.

## Acceptance

1. `UPDATE t SET x = 1 WHERE <pushable>` on 1M matching rows has no scan round
   trip and runs in the server's statement time. Measured before and after on
   the wide fixture.
2. #140's reproduction passes on all three rungs. The unpushable keyless
   statement with a volatile function is refused by name.
3. The count a pushed statement reports equals the count the shipped path
   reports on the same data, an AFTER trigger included (INSTEAD OF is not
   pushed). The column is `Count` BIGINT on both paths. The full DML suite is
   green through both ATTACH aliases of the agreement harness (#421). Stated
   rather than compared: on rung 3, duplicate rows move together, so the count
   can exceed the number of rows DuckDB's plan selected.
4. No DML defers inside a transaction. On a pool of one, UPDATE, DELETE and
   MERGE run in autocommit and in a transaction, on both paths.
5. `UPDATE` / `DELETE … RETURNING` return the rows, or are refused by name.
   None ends in an InternalException.
6. MERGE into a catalog table runs on the server as one statement, for keyed
   and keyless targets alike: pushed when the source is on the same server,
   through `#src` otherwise. A MERGE the writer cannot express in T-SQL is
   refused by name, and nothing is sent.
7. The four DML token loops are one. `mssql_dml_use_prepared` is a
   registered no-op with its deprecation line in the CHANGELOG.
8. A pushed DML executes exactly once per execution, and twice for two
   `EXECUTE`s of one `PREPARE`. `EXPLAIN` / `PREPARE` change nothing (#421).
9. A read-only catalog and a read-only database each refuse every pushed
   write, through both guards, with the shipped path's message (#421).
10. `mssql_dml_pushdown = false` leaves pushed SELECTs pushed and sends every
    UPDATE / DELETE / INSERT … SELECT down D3's path, and every MERGE through
    `#src` (D4; it still runs on the server or is refused). Once
    `EXECUTE_STATEMENT` is claimed, it restores behaviour, not routing
    (#421).
11. A keyless UPDATE or DELETE over a `time(7)` or `datetimeoffset(7)`
    column is refused by name and never reports 0 rows; rungs 2 and 3 answer
    through the same predicate (#421). The same holds for `datetime`
    **unless W3 measured the column-type conversion exact**, in which case a
    `datetime` key matches on both rungs and #358 closes for it (review of
    #422).
12. A pushed DML opened as a streaming result and closed unconsumed still
    changed its rows (#421).

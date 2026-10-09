# Spec 080: DML on the server — pushed statements, a staged fallback, RETURNING, MERGE

**Status:** revised 2026-10-08 after the reconnaissance in
`recon-2026-10-08.md`, merged with oluies' design-review revision (#421, 21
findings). Its decisions are folded in and marked "(#421)". That file holds the code inventory, the DuckDB API at
`4fbae437b22` and the Fabric/Synapse matrix, each with sources. This spec
cites it as "R§n". The first draft (2026-09-17, PR #364) is superseded; its
measured ground still stands in `../065-dml-pushdown-recon/` and is cited
from there.

**Goal**
- A DML statement the writer can express runs on the server as **one
  statement**, and no rowids travel.
- A DML statement the writer cannot express runs as a **staged JOIN** keyed
  by the best key the table has, or by all its columns if it has none.
- RETURNING works where the platform can return rows and is refused by name
  where it cannot. It never ends in an InternalException.
- MERGE INTO works, natively, and pushed when both sides are remote.
- **Closes #140 on SQL Server / Azure SQL.** It was reopened on 2026-10-08:
  #364 closed it with text, not code. Fabric follows once the probe settles
  `stage_bulk` / `merge`. Synapse dedicated keeps today's refusal. A keyless
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
  pushdown and no stage, and documented as untested. The one change there:
  UPDATE / DELETE … RETURNING is refused by name instead of ending in the
  InternalException. Telling it apart
  from Azure SQL needs `EngineEdition` (D0).
- Synapse serverless: no DML at all.

---

## D0: what the platform can do

DML forms differ by platform (R§4), so the catalog resolves a
`DmlCapabilities` once per ATTACH, next to `is_fabric_endpoint` and the
Synapse flag.

**Detection** reads `SERVERPROPERTY('EngineEdition')` and
`SERVERPROPERTY('ProductMajorVersion')`, both added to the ATTACH collation
query. The host test cannot do it:
- `IsSynapseEndpoint` matches only `*-ondemand` (serverless), so a dedicated
  pool classifies as Azure SQL today and would get OUTPUT, table variables
  and aliased targets.
- 6 is Synapse dedicated.
- 11 is serverless.
- Fabric Warehouse's value is to be read by the probe.

The host test stays as a fallback when the query cannot run.

Both properties ride the collation query as `SNAPSHOT_STATE_COLUMN` did
(#331), so there is no extra round trip, and they are cached on the catalog.
An unreadable property, an unknown edition, or a version below 16 takes the
INTERSECT form. That form is correct everywhere; the only cost of erring
that way is speed (#421).

| capability | SQL Server / Azure SQL | Fabric |
|---|---|---|
| `output_clause` (RETURNING rows; OUTPUT INTO a table variable) | yes | until the probe: no |
| `exact_count` (`ROWCOUNT_BIG()` into an RPC OUTPUT parameter) | yes | until the probe: no (the DONE count is used) |
| `update_from_join` (`UPDATE … FROM … JOIN`, `DELETE … FROM … JOIN`) | yes | **no** (documented) |
| `merge` | yes | yes (GA) |
| `stage_bulk` (`INSERT BULK` into `#stage`) | yes | until the probe: no (stage filled with `INSERT … VALUES` batches) |
| `null_safe` | `IS NOT DISTINCT FROM` when `ProductMajorVersion >= 16` or `EngineEdition IN (5, 8)` (Azure SQL DB / MI report version 12 while having the operator), otherwise `EXISTS (SELECT t.c… INTERSECT SELECT s.c…)` | the INTERSECT form until the probe confirms the operator |

Synapse dedicated (EngineEdition 6) gets none of these. Its DML takes
today's path unchanged (§ Platforms). Its documented abilities (FROM … JOIN
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

- the node is a whole statement (or an INSERT's / CTAS's query). DuckDB
  calls `FinishPushdown` for nothing else (opt:256 / 458 / 627 / 702), and
  the writer vetoes a DML node that appears as a CTE body. An `EXPLAIN` wraps
  the statement and the DML inside is still pushed: `EXPLAIN` binds the
  vehicle without running it, and `EXPLAIN ANALYZE` runs it, as it runs a
  native DML;
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
- RETURNING without `output_clause`.

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
| CTAS from remote to remote | our CREATE, then the pushed `INSERT … SELECT` (D6, the last PR) | same |

### The vehicles

`RemoteExecute` returns a ref to one of two table functions. Both bind with
no describe and no execution (081's bind, R§3), so `EXPLAIN` and `PREPARE`
change nothing. Both set the statement properties a native DML has
(`result_eagerness = FORCED`, and `CHANGED_ROWS` for the count form;
bind_update.cpp:287-289). Without them, a streaming client would not finish
the DML until it fetched, and row-count APIs would change. Both also call
`RegisterDBModify` on the binder (R§1).

Read-only is refused with the shipped path's class and message, through
`MSSQLCatalog::CheckWriteAccess` (#421), at two points:
- in the dry run, so the statement takes the shipped path, whose hook
  refuses it;
- in the vehicle's bind, so no route around the dry run can execute a write.

`RegisterDBModify` adds what `CheckWriteAccess` cannot: DuckDB's own refusal
for a read-only *database* (`SET access_mode = read_only`) and its
one-writable-database-per-transaction rule.

**Exactly once (#421).** A vehicle has one global init and no parallel local
state. It is not a scan: there is no range to split and nothing to
re-initialise. A table function DuckDB initialised twice would run the UPDATE
twice, so this invariant is load-bearing. A `PREPARE`d pushed DML executed
twice affects its rows twice, as the shipped path does.

**What still changes for a client (#421).** The statement's
`StatementType` stays `SELECT`, since the rewriter wraps the vehicle in a
SELECT. A binding keyed on the statement type rather than on the
`CHANGED_ROWS` property would see a query. This is documented next to 079's
"result types change for a pushed statement", with `mssql_dml_pushdown`
(D5) as the way back.

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
  form, only with `output_clause`.
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

After a pushed DML, the target table's row count is invalidated in the
statistics provider (`InvalidateTable`). The table's metadata is not touched:
no epoch bump, describe cache kept (R§3).

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
   - **Which columns match.** Every column the server can compare exactly.
     Left out by type:
     - `text`, `ntext`, `image`, `xml`, `geometry`, `geography`, which cannot
       be in an `=`;
     - `cast_required` columns (`sql_variant`, `hierarchyid`, CLR types),
       which read lossily.

     The MAX types (`varchar(max)`, `nvarchar(max)`, `varbinary(max)`) **are**
     comparable and stay in the key. They may be left out for cost only when
     neither the SET nor the WHERE reads them (#421; W3 measures the cost).
   - **The WHERE is re-applied on the target side of the JOIN.** Rung 3
     requires it to be fully pushed. That is what makes leaving columns out
     sound: a target row that the WHERE did not select can never match, even
     when its compared columns equal a selected row's.
   - **`UPDATE … FROM` / `DELETE … USING` on rung 3 are refused by name.**
     The join condition is not a scan filter, so it cannot be re-applied on
     the target side. Such a table needs a key.
   - A SET expression may not read a column left out by type. Two rows equal on the
     compared columns would then take different new values, and the server's
     `UPDATE … FROM … JOIN` would take an arbitrary one of them without
     raising anything (#421). Refused by name, naming the column and its type
     ("`t.payload` is `xml`: the server cannot compare it, and the SET reads
     it — add a unique index").
   - A `time(7)` / `datetimeoffset(7)` / out-of-range `datetime2(7)` column
     reads lossily. It is compared by the microsecond range below; an
     out-of-range one reads as NULL and is left out.
   - **On rung 3 the stage carries the old values (the match) and the new
     values (the SET).** So a SET of any column is allowed there: the "no SET
     of a key column" rule exists for rungs 1–2 only.
   - The stage is DISTINCT over the old values of the compared columns
     together with the new values. Because the SET reads only compared
     columns and is deterministic, two staged rows with equal old values carry
     equal new values. Without the DISTINCT, Fabric's MERGE fallback fails
     with 8672 on duplicate stage rows.
   - **Duplicates move together.** Target rows identical in every compared
     column are one match, so they are updated or deleted together. A row
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
  own connection, so no other session needs to see it, and a `#` name stays
  clear of spec 063 D1's refusal of a second bulk-load writer against a
  session-scoped target (#421).
  - The connection is taken at the **first `Sink`**, never at init
    (`BulkLoadSession::DeferAdoption` / `AdoptDeferred`, as COPY and CTAS
    do). On a pool of one, the source scan has given the connection back by
    then (#421).
  - The stage is filled fully first, then the JOIN runs (first draft § D3:
    no scan ∥ DML pipelining; rung 3 cannot seek).
  - The stage is dropped on the way out.

**Batching (#421):**
- A DELETE batches at about 100k staged rows: a deleted row cannot match a
  later batch.
- An UPDATE on rungs 1–2 batches too. A SET of a rowid-key column is refused
  there, so no batch can move a row into another batch's key.
- **An UPDATE on rung 3 is one statement over the whole stage.** There the key
  is every compared column, and any SET rewrites part of it. In batches it
  corrupts silently:
  - keyless `(a, b)` holds `(1, 1)` and `(2, 2)`, and the statement is
    `UPDATE t SET a = a + 1`;
  - batch 1 turns `(1, 1)` into `(2, 1)`;
  - batch 2's key `(2, …)` then matches it again.

  One statement matches against the target as it stood before the statement.
  If one statement ever proves too much for the log, the way back is a
  key-stable generation guard in the stage, not batching as it stands.

**#358** (a `datetime` key that never matches):
- **The cause.** The key is read as a microsecond TIMESTAMP and sent back as
  a `datetime2(7)` value. The server compares `datetime` with `datetime2`
  exactly, and the two never meet.
- **The fix, on both delivery forms.** The key is converted to **the column's
  own type** before the comparison:
  - the `VALUES` literal is rendered `CAST(… AS datetime)`;
  - the stage column is declared `datetime`. BCP sends it as `datetime2`, and
    the server rounds it to the tick (write_column_ops.cpp:145).
  - Either way, it lands on the same 1/300 s tick as the stored value.
- **`time(7)` and `datetimeoffset(7)`** lose their 100 ns digit on read; an
  out-of-range `datetime2(7)` reads as NULL. These match by the microsecond
  range: `t.c >= s.c AND t.c < DATEADD(microsecond, 1, s.c)`. As a rowid key
  they stay refused on rungs 1–2 (077), where a range could match two rows.
- **`datetime2(7)` in range is lossless** (it reads as TIMESTAMP_NS) and needs
  nothing.
- W3 measures all of this before #358 is closed.

| step | SQL Server / Azure | Fabric |
|---|---|---|
| fill | `INSERT BULK`, adopted at the first `Sink` (`DeferAdoption` / `AdoptDeferred`) | `INSERT … VALUES` batches (1000-constant rule) until `stage_bulk` |
| UPDATE | `UPDATE [t] SET … FROM [s].[t] AS [t] JOIN #stage AS s ON <key>` | `MERGE [s].[t] AS t USING #stage AS s ON <key> WHEN MATCHED THEN UPDATE SET …` |
| DELETE | `DELETE [t] FROM … JOIN #stage …` | `MERGE … WHEN MATCHED THEN DELETE` |
| in a transaction | stage created on the pinned connection | same |

The `<key>` comparison on rung 3 uses `null_safe` (D0).

**RETURNING on the fallback**, with `output_clause`:
- The JOIN / VALUES statement carries `OUTPUT … INTO @o`.
- The operator returns DuckDB's RETURNING chunk: every non-generated column,
  in table order, at DuckDB's types (binder.cpp:571-580). That is the
  post-image for UPDATE and the pre-image for DELETE.
- `@o` is declared under the rules of the row vehicle (D1).

Without `output_clause`, RETURNING is refused by name at plan time:
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

- **Native.** DuckDB plans each action through our hooks (R§1), so MERGE
  rides D3. DuckDB's binder needs `GetRowIdColumns`, and on a keyless table
  that now answers with rung 3's key. RETURNING in MERGE is refused by DuckDB
  itself. Tests pin what works today and what rung 3 adds.
  - **Known limit on rung 3:** identical duplicate target rows share one
    composite rowid. DuckDB's duplicate check (physical_merge_into.cpp:398)
    then reports "the same target row more than once" for rows that are in
    fact two. Documented; a key is the fix.
- **One statement connection for all actions** (found in PR 1, #423). DuckDB
  plans each action as its own operator. Each has its own
  `MSSQLStatementConnection`, and they are fed from several threads. PR 1
  makes them send at their own Finalize, under the pinned connection's lock.
  The rest is PR 4:
  - in autocommit the actions are separate server transactions, so a failing
    action leaves the others committed (as on `main` before PR 1);
  - on a pool of one they take turns, bounded by `mssql_acquire_timeout`;
  - UPDATE / DELETE actions hold their rows in memory until Finalize.

  The fix: the actions share one connection and one server transaction for
  the operator, in autocommit committed when the statement's own
  `MSSQLTransaction` commits. The staged path (D3) then replaces their
  per-value buffers.
- **IDENTITY in a MERGE INSERT action** (found in PR 1). DuckDB plans the
  action full-width, so the identity column arrives as an explicit NULL and
  the INSERT is refused by name: "names the identity column … but supplies no
  value". PR 4 drops an identity column from a MERGE action's list when the
  MERGE's own INSERT clause does not name it.
- **Pushed** (both sides in the catalog): `MergeQueryNode` → T-SQL `MERGE`
  with `merge` (D0).
  - Only shapes T-SQL can express are pushed; anything else is vetoed:
    - at most two WHEN MATCHED clauses, one UPDATE and one DELETE, the first
      of them conditional;
    - one WHEN NOT MATCHED [BY TARGET];
    - at most two WHEN NOT MATCHED BY SOURCE.

    DuckDB allows any number, with first-match semantics.
  - **DO NOTHING** is pushed only as the last clause of its kind, where
    omitting it changes nothing. Elsewhere it is vetoed: a later clause with
    an overlapping condition would otherwise take its rows.
  - Vetoed: ERROR, `UPDATE SET *`, `INSERT *` / BY NAME, an INSERT action
    naming the identity column (no IDENTITY_INSERT bracket inside MERGE in
    this spec), and a source that is not a table of the same catalog or a
    pushable query over them.
  - T-SQL refuses a target row matched twice (it errors), and DuckDB refuses
    it too: same outcome, kept.
  - The count is `exact_count`'s where the platform has it, otherwise the
    MERGE's DONE count.
- **Source in DuckDB, target remote** (`MERGE INTO ms.t USING local_df …`):
  this **moves to `after-0.3.0`**. `PlanMergeInto` receives the already-planned
  physical join of source and target (plan_merge_into.cpp:351-356), so the
  source cannot be taken from there. Staging it would need a logical rewrite
  in `MSSQLOptimizer`, which is a design of its own. Until then such a MERGE
  takes the native path: per-action rowid batches through D3.

## D5: the DML switch (#421)

`mssql_dml_pushdown` (BOOLEAN, default **true**, `SetScope::GLOBAL`) is read
in `SupportsPushdown` for the DML and MERGE nodes, and in
`SupportsPushdown(const SQLStatement &)` for CTAS (D6).

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

- It needs `EXECUTE_STATEMENT`, a **catalog-wide** claim.
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
  - A describe the server cannot answer is a **veto at execution**, never
    075's run-for-the-shape fallback.
- `OR REPLACE` / `IF NOT EXISTS` / `WITH (table_kind = …)` behave as they do
  on the shipped CTAS.
- After it, the catalog is invalidated as the shipped CTAS invalidates it
  (`InvalidateSchemaTableSet` + `NoteTransactionChange`). The "no epoch bump"
  rule of D1 is for DML only.
- The ref is lazy, so `EXPLAIN` / `PREPARE` create nothing.
- When CTAS is vetoed, the rewriter still pushes its query alone, as today.
- **Droppable.** It is the one shape that needs a round trip before it can
  decide. If it is dropped, nothing in the other PRs changes.

## Work

### W1: the vehicles and the count
- `mssql_dml` / `mssql_dml_returning` (bind with no describe, `RegisterDBModify`,
  `MSSQLStatementConnection`).
- RETURNVALUE parsing for the `exact_count` OUTPUT parameter.
- `ExecuteDmlBatch`, with the four DML loops folded into it.
- Statistics invalidation after a DML on both paths (today's path does none,
  R§3).

### W2: the writer's DML nodes
- `WriteUpdate` / `WriteDelete` / `WriteInsertSelect` / `WriteMerge`, with
  the platform forms of D1.
- Columns resolved against the entries 079 D1 recorded for this rewrite, on
  this thread (#421). Never a fresh lookup by name: without a `ClientContext`
  that lookup reads the shared cache and misses the transaction's own layer
  (#380).
- `has_default` and an `INSTEAD OF` flag added to the metadata queries
  (084's result sets, no extra round trip).
- The dry-run vetoes of D1 and the top-node rule.
- The unique-source rule for `UPDATE … FROM`.

### W3: the ladder and the stage
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
- Native tests, including the rung-3 duplicate limit.
- The pushed MERGE (W2), with the shape vetoes of D4.

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
  option `remote_pushdown false`, every pushed shape compared on table state
  and `Count`;
- exactly once (`threads = 4`; `PREPARE` executed twice);
- rung 3's hazards: the `(1, 1)` / `(2, 2)` `SET a = a + 1` shape forced past
  the threshold and the batch size; keyless tables with an `xml` / a
  `geometry` column;
- both join forms (the INTERSECT form forced in a debug build);
- DROP / ALTER / CREATE SCHEMA / CREATE VIEW unchanged after the
  `EXECUTE_STATEMENT` claim;
- `mssql_dml_pushdown = false`;
- the read-only refusals: catalog, `access_mode`, both guards.

And these:
- `UPDATE` / `DELETE … RETURNING` on both paths: rows match DuckDB's own
  semantics, and the InternalException reproduction is now a pass or a named
  refusal.
- `EXPLAIN` / `PREPARE` of a pushed DML change nothing, and `ATTACH … READ_ONLY`
  refuses through `RegisterDBModify`. A DML into two attached catalogs in one
  transaction is refused by DuckDB's own rule.
- An AFTER trigger on the target: the `exact_count` count is the
  statement's. A target with an INSTEAD OF trigger is not pushed.
- MERGE: native on rungs 1–3 (with the duplicate limit); pushed with each
  action kind; a target row matched twice errors.
- Pool of one: UPDATE, DELETE and MERGE in autocommit and in a transaction,
  pushed and staged (extends `transaction_single_connection_pool.test`).
- The `fabric-probe/` files, kept runnable under `[fabric]`.

### W9: docs
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
  `mssql_dml_stage_threshold`);
- CHANGELOG;
- "what a pushed DML changes for a client" (#421).

### PR plan (#421)

Not one PR: spec 079 moved away from one large PR because "the size is the
risk", and this spec is no smaller. Each PR merges before the next opens
(never stacked). The order follows the dependencies:

| PR | contents | stands on its own because |
|---|---|---|
| **1** | W4 (the 066 remainder: sinks, defer removed, pool of one); RETURNING on UPDATE / DELETE **refused by name** at plan time (the InternalException fix); W6 | fixes UPDATE / DELETE on a pool of one and the crash, behind no setting |
| **2** | W3 (the ladder, rung 3, `#stage`, `mssql_dml_stage_threshold`, #358) and W5's capability read (`EngineEdition` / `ProductMajorVersion`); RETURNING through `OUTPUT … INTO @o` on the fallback where `output_clause` holds; the `fabric-probe/` run if the warehouse answers by then (the rows it settles flip, one line each) | closes #140 and #358 with no rewriter involved |
| **3** | W1 + W2 (the vehicles, the exact count, `ExecuteDmlBatch`, the writer's UPDATE / DELETE / INSERT … SELECT, `mssql_dml_pushdown`); the agreement harness | pushed DML end to end, PR 2's path under every veto |
| **4** | W7: one statement connection for a MERGE's actions (atomic in autocommit), IDENTITY in an INSERT action, pushed MERGE | MERGE on top of PRs 2–3 (PR 1 already made the native MERGE run in a transaction and on a pool of one, #423) |
| **5** | D6 (pushed CTAS) and W9 (docs) | droppable without touching PRs 1–4 |

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
- Batching the rung-3 UPDATE behind a generation guard (D3 names the hazard;
  one statement is the decision until a measurement makes it untenable).

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
- **Native MERGE on a keyless table** with identical duplicate target rows
  reports a false duplicate match (D4).
- **The join-form probe** errs toward the INTERSECT form on an unknown
  edition. That form is correct everywhere and only slower on 2022+.
- **Rung 3 on a table with a column that cannot compare**, when the SET reads
  that column, has no rung 3 and is refused by name. Such a table needs a
  unique index (#421).

## Open, decided later

- ON CONFLICT (DuckDB's upsert) → T-SQL `MERGE`: a D1 row once D4 is in.
- `UPDATE … FROM` on Fabric through `MERGE`: once the Fabric forms are
  measured.
- MERGE with a source in DuckDB through `#stage` (D4): a logical rewrite in
  `MSSQLOptimizer`, after 0.3.0.

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
6. MERGE works natively on keyed and keyless tables (with D4's documented
   duplicate limit), and pushed when both sides are remote and the shape is
   expressible in T-SQL.
7. The four DML token loops are one. `mssql_dml_use_prepared` is a
   registered no-op with its deprecation line in the CHANGELOG.
8. A pushed DML executes exactly once per execution, and twice for two
   `EXECUTE`s of one `PREPARE`. `EXPLAIN` / `PREPARE` change nothing (#421).
9. A read-only catalog and a read-only database each refuse every pushed
   write, through both guards, with the shipped path's message (#421).
10. `mssql_dml_pushdown = false` leaves pushed SELECTs pushed and sends every
    DML down D3's path (#421).

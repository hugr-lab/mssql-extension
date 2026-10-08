# Spec 080: DML on the server — pushed statements, a staged fallback, RETURNING, MERGE

**Status:** revised 2026-10-08 after the reconnaissance in
`recon-2026-10-08.md`. That file holds the code inventory, the DuckDB API at
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
- ON CONFLICT, which keeps today's behaviour; § 5.

**Platforms**
- SQL Server 2019+ and Azure SQL (Database, Managed Instance): everything
  below.
- Fabric Warehouse: per the platform rule in D0.
- Synapse dedicated: no test environment. Today's DML path is kept unchanged,
  with no pushdown and no stage, and documented as untested. Telling it apart
  from Azure SQL needs `EngineEdition` (D0).
- Synapse serverless: no DML at all.

---

## D0: what the platform can do

DML forms differ by platform (R§4), so the catalog resolves a
`DmlCapabilities` once per ATTACH, next to `is_fabric_endpoint` and the
Synapse flag.

**Detection** is `SERVERPROPERTY('EngineEdition')`, added to the ATTACH
collation query; the host test cannot do it:
- `IsSynapseEndpoint` matches only `*-ondemand` (serverless), so a dedicated
  pool classifies as Azure SQL today and would get OUTPUT, table variables
  and aliased targets.
- 6 is Synapse dedicated.
- 11 is serverless.
- Fabric Warehouse's value is to be read by the probe.

The host test stays as a fallback when the query cannot run.

| capability | SQL Server / Azure SQL | Fabric |
|---|---|---|
| `output_clause` (RETURNING rows; OUTPUT INTO a table variable) | yes | until the probe: no |
| `exact_count` (`ROWCOUNT_BIG()` into an RPC OUTPUT parameter) | yes | until the probe: no (the DONE count is used) |
| `update_from_join` (`UPDATE … FROM … JOIN`, `DELETE … FROM … JOIN`) | yes | **no** (documented) |
| `merge` | yes | yes (GA) |
| `stage_bulk` (`INSERT BULK` into `#stage`) | yes | until the probe: no (stage filled with `INSERT … VALUES` batches) |
| `null_safe` | `IS NOT DISTINCT FROM` on 2022+ and Azure, otherwise `EXISTS (SELECT a INTERSECT SELECT b)` | `IS NOT DISTINCT FROM` |

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
| CTAS from remote to remote | only the query is pushed, as today | same |

**CTAS is not claimed.** `EXECUTE_STATEMENT` is per catalog (R§1): claiming
it would route every CREATE / DROP / ALTER of the catalog through
`SupportsPushdown(SQLStatement)` for one shape. The rewriter already pushes a
CTAS's query on its own. A server-side `INSERT … SELECT` for CTAS is left to
`after-0.3.0`.

### The vehicles

`RemoteExecute` returns a ref to one of two table functions. Both bind with
no describe and no execution (081's bind, R§3), so `EXPLAIN` and `PREPARE`
change nothing. Both set the statement properties a native DML has
(`result_eagerness = FORCED`, and `CHANGED_ROWS` for the count form;
bind_update.cpp:287-289). Without them, a streaming client would not finish
the DML until it fetched, and row-count APIs would change. Both also call
`RegisterDBModify` on the binder (R§1). DuckDB
then refuses a READ_ONLY attach itself and keeps the
one-writable-database-per-transaction rule. That replaces the first draft's
two hand-written guards.

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
`PlanInsert` / `PlanMergeInto`, on the path D3 describes. Nothing is pushed
in half. Extra rows *written* are not harmless, so a DML predicate is never
pushed in a relaxed form (065 research § 2).

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
   - **Which columns match.** Every column that compares exactly. Left out:
     - `text`, `ntext`, `image`, `xml`, `geometry`, `geography`, which cannot
       be in an `=`;
     - `cast_required` columns (`sql_variant`, `hierarchyid`), which read
       lossily;
     - `varchar(max)` / `nvarchar(max)` / `varbinary(max)`, by choice, for
       the JOIN's cost.
   - **The WHERE is re-applied on the target side of the JOIN.** Rung 3
     requires it to be fully pushed. That is what makes leaving columns out
     sound: a target row that the WHERE did not select can never match, even
     when its compared columns equal a selected row's.
   - A SET expression may not read a left-out column. Two rows equal on the
     compared columns would then take different new values. Refused by name.
   - A `time(7)` / `datetimeoffset(7)` / out-of-range `datetime2(7)` column
     reads lossily. It is compared by the microsecond range below; an
     out-of-range one reads as NULL and is left out.
   - **On rung 3 the stage carries the old values (the match) and the new
     values (the SET).** So a SET of any column is allowed there: the "no SET
     of a key column" rule exists for rungs 1–2 only.
   - The stage is DISTINCT over the compared columns. Without it, Fabric's
     MERGE fallback fails with 8672 on duplicate stage rows.
   - Keyless tables expose a `rowid` virtual column (all compared columns)
     once rung 3 exists. It is user-visible and documented.

**Delivery.** Two forms:
- Rungs 1–2 below the small-result threshold: today's `VALUES`-join
  statements, on SQL Server / Azure and Synapse.
- Everything else: a **session-local `#stage_<uuid>`** on the statement's own
  connection. The stage is filled fully first, then the JOIN runs (first
  draft § D3: no scan ∥ DML pipelining; rung 3 cannot seek). The stage is
  dropped on the way out.

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
| fill | `INSERT BULK` (`BulkLoadSession::Adopt`; on a pool of one `DeferAdoption`) | `INSERT … VALUES` batches (1000-constant rule) until `stage_bulk` |
| UPDATE | `UPDATE [t] SET … FROM [s].[t] AS [t] JOIN #stage AS s ON <key>` | `MERGE [s].[t] AS t USING #stage AS s ON <key> WHEN MATCHED THEN UPDATE SET …` |
| DELETE | `DELETE [t] FROM … JOIN #stage …` | `MERGE … WHEN MATCHED THEN DELETE` |
| in a transaction | stage created on the pinned connection | same |

The `<key>` comparison on rung 3 uses `null_safe` (D0).

**RETURNING on the fallback**, with `output_clause`:
- The JOIN / VALUES statement carries `OUTPUT … INTO @o`.
- The operator returns DuckDB's RETURNING chunk: every non-generated column,
  in table order, at DuckDB's types (binder.cpp:571-580). That is the
  post-image for UPDATE and the pre-image for DELETE.
- `@o` is declared under the rules of the row vehicle (D1). Without `output_clause` it is refused by
name at plan time: `PlanUpdate` / `PlanDelete` read `op.return_chunk`. That is
the InternalException fix (R§2).

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
connection; it spills like any `ColumnDataCollection` (§ 4).

**One token loop.** UPDATE, DELETE, INSERT, RETURNING and the new vehicles
call one `ExecuteDmlBatch`. These are the four DML loops; `MSSQLSimpleQuery`
stays as it is. It reads `query_timeout` instead of a hard-coded
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

## 2. Work

### W1: the vehicles and the count
- `mssql_dml` / `mssql_dml_returning` (bind with no describe, `RegisterDBModify`,
  `MSSQLStatementConnection`).
- RETURNVALUE parsing for the `exact_count` OUTPUT parameter.
- `ExecuteDmlBatch`, with the five loops folded into it.
- Statistics invalidation after a DML on both paths (today's path does none,
  R§3).

### W2: the writer's DML nodes
- `WriteUpdate` / `WriteDelete` / `WriteInsertSelect` / `WriteMerge`, with
  the platform forms of D1.
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
- `EnsurePKLoaded` deleted (a `D_ASSERT` since 084).

### W7: MERGE
- Native tests, including the rung-3 duplicate limit.
- The pushed MERGE (W2), with the shape vetoes of D4.

### W8: tests
Everything the first draft's W5 listed, plus:
- `UPDATE` / `DELETE … RETURNING` on both paths: rows match DuckDB's own
  semantics, and the InternalException reproduction is now a pass or a named
  refusal.
- `EXPLAIN` / `PREPARE` of a pushed DML change nothing, and `ATTACH … READ_ONLY`
  refuses through `RegisterDBModify`. A DML into two attached catalogs in one
  transaction is refused by DuckDB's own rule.
- An AFTER trigger on the target: the `exact_count` count is the
  statement's. A target with an INSTEAD OF trigger is not pushed.
- MERGE: native on rungs 1–3; pushed with each action kind; a local source
  staged; a target row matched twice errors.
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

Also: DATAMODEL (the DML flow: rewriter → one statement | plan → ladder →
stage), the CLAUDE.md DML line, CHANGELOG.

One PR after this revision is approved; W1 → W9 as commits.

## 3. Not proposed

- `%%physloc%%` as a row identifier (067 § 5).
- A strict-string DML mode (065 § 8.5 keeps the measured form).
- TVPs (owner, 2026-10-08).
- Pushing a relaxed predicate for a DML.
- ON CONFLICT through `MERGE` (§ 5).

## 4. Risks

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
- **The pushed and fallback paths can diverge.** Views, implicit conversions
  and key SETs differ between them. The vetoes in D1 close the known
  differences; the agreement tests (W8: every pushed shape also run with
  `mssql_remote_pushdown = false`) catch the rest.
- **A large UPDATE in a transaction materialises its source scan.** Its rows
  are held on the client, spilling as `ColumnDataCollection` does.
- **Native MERGE on a keyless table** with identical duplicate target rows
  reports a false duplicate match (D4).

## 5. Open, decided later

- ON CONFLICT (DuckDB's upsert) → T-SQL `MERGE`: a D1 row once D4 is in.
- `UPDATE … FROM` on Fabric through `MERGE`: once the Fabric forms are
  measured.
- MERGE with a source in DuckDB through `#stage` (D4): a logical rewrite in
  `MSSQLOptimizer`, after 0.3.0.

## 6. Acceptance

1. `UPDATE t SET x = 1 WHERE <pushable>` on 1M matching rows has no scan round
   trip and runs in the server's statement time. Measured before and after on
   the wide fixture.
2. #140's reproduction passes on all three rungs. The unpushable keyless
   statement with a volatile function is refused by name.
3. Counts agree with the server's affected rows on both paths, a trigger
   included (AFTER triggers; INSTEAD OF is not pushed). The full DML suite is
   green with `mssql_remote_pushdown` on and off.
4. No DML defers inside a transaction. On a pool of one, UPDATE, DELETE and
   MERGE run in autocommit and in a transaction, on both paths.
5. `UPDATE` / `DELETE … RETURNING` return the rows, or are refused by name.
   None ends in an InternalException.
6. MERGE works natively on keyed and keyless tables (with D4's documented
   duplicate limit), and pushed when both sides are remote and the shape is
   expressible in T-SQL.
7. The four DML token loops are one.

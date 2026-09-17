# Spec 080 — UPDATE / DELETE / INSERT … SELECT through the writer, and the staged fallback

**Status:** Draft, 2026-09-17, on `spec/065-067-revalidation` (PR #364),
after spec 079 (the writer it uses). Absorbs and closes the 065
reconnaissance's DML half: spec 065 (direct UPDATE/DELETE — its goal, on
the core mechanism instead of our own plan hooks), the 066 remainder
(scans feeding a DML materialise in a transaction), and spec 067 (the
match-key ladder, so DML works without a primary key). The research is
`../065-dml-pushdown-recon/revalidation-2026-09-17.md` § 3, § 4, § 9.4 and
`research.md` there; `../067-dml-staging/spec.md` § 1–3 hold the ladder's
argument and are pointed at, not repeated.
**Goal:** a DML statement the writer can express runs on the server as
**one statement** — no rowids travel; one that it cannot express runs on a
staged JOIN keyed by the best key the table has, all columns if it has
none. **Closes #140 fully** (065 closed it for pushable statements only;
067 for the rest).
**Not the goal:** RETURNING through the rewriter (an `OUTPUT` mapping is a
later row), MERGE (later), strict string semantics (owner, 2026-09-17: a
SQL Server user expects SQL Server's DELETE).
**Depends on:** spec 079 (the writer, `mssql_remote_pushdown`, the agreement
suite pattern), #350 (spec 077: `ChooseRowIdKey` is rung 2 of the ladder,
the `IDENTITY_INSERT` bracket), spec 062 W0 (`BulkLoadSession::Adopt`) and
W1c (the autocommit bracket).

---

## 0. Measured ground (in the research record)

| what | where | the fact |
|---|---|---|
| the rewriter and DML | § 5, § 9.1 | `GetNodeFromStatement` yields a node for INSERT, DELETE, UPDATE and MERGE INTO; the INSERT path pushes the SELECT alone when the target is local or the INSERT is vetoed; `RemoteExecute(QueryNode)` returns a table ref whose result stands in for the statement's |
| today's DML | § 3 (065 claims table) | UPDATE/DELETE scan rowids to the client and ship them back as `VALUES`-join batches of 500; in a transaction they defer to Finalize with the rows buffered; the scan's pushed WHERE is already the server's (native) |
| the sink set | § 3 | `CollectSinkCatalogs` counts INSERT and COPY, not UPDATE/DELETE — the 066 remainder |
| the key ladder | § 4, 067 § 1–2, 077 | rung 2 is `ChooseRowIdKey` (PK, else a usable unique index; datetime / sql_variant keys unusable, #358); rung 3 is value matching over all columns, exact for deterministic statements |
| strings in DML | § 8.3 → § 9.3 | native: the rowid path pushes the WHERE already and DuckDB does not re-check it, so `DELETE … WHERE a = 'ab'` removes the rows `SELECT … WHERE a = 'ab'` shows; a strict rewrite would remove fewer than the SELECT displays |
| autocommit atomicity | spec 062 W1c | one statement connection, batches bracketed in a server transaction, the ENVCHANGE descriptor carried |

## 1. Design

### D1 — whole-statement DML through the rewriter

`Supports(EXECUTE_QUERY_NODE)` already claimed by 079 covers the DML nodes;
`SupportsPushdown(const QueryNode &)` grows `UpdateQueryNode`,
`DeleteQueryNode` and `InsertQueryNode` cases, and the writer renders:

| DuckDB | T-SQL | rule |
|---|---|---|
| `UPDATE t SET c = e, … [FROM u …] WHERE p` | `UPDATE t SET [c] = e, … FROM [s].[t] AS t [JOIN …] WHERE p` | every SET expression and the WHERE through 079's expression writer under the same rules; SET of a rowid-key column is refused as today; `DEFAULT` in SET → veto (server DEFAULT ≠ DuckDB's NULL default, 065 D2.5) |
| `DELETE FROM t [USING u …] WHERE p` | `DELETE t FROM [s].[t] AS t [JOIN …] WHERE p` | same |
| `INSERT INTO t [(cols)] SELECT …` (both sides remote) | `INSERT INTO [s].[t] ([cols]) SELECT …` | the SELECT through 079; an explicitly named identity column brackets the statement in `SET IDENTITY_INSERT` (077 W2); `INSERT … VALUES` stays on the shipped path (it carries no remote SELECT to win by) |
| `INSERT INTO local SELECT … FROM remote` | the SELECT alone | the rewriter does this by itself (`push_select_only`); nothing to add |
| `CREATE TABLE remote AS SELECT … FROM remote` | our CREATE (table kind, collation, lengths — the WITH options keep meaning), then the pushed `INSERT … SELECT` | not `SELECT INTO`; needs `EXECUTE_STATEMENT` claimed for CREATE TABLE AS only, and `SupportsPushdown(const SQLStatement &)` saying yes for that shape alone |
| `RETURNING`, `MERGE INTO`, `ON CONFLICT` | — | veto; the shipped path handles what it handles today |

Execution: `RemoteExecute` returns a ref to the DML form of the vehicle —
`mssql_scan_params` with a **count result**: the statement runs through
`MSSQLStatementConnection` as every DML does (the pinned connection inside a
transaction; in autocommit one connection bracketed by spec 062 W1c so the
statement is atomic), the affected count comes from the DONE token and is
returned as the single BIGINT row DuckDB expects of a DML. Errors surface
with the server's message and number, as `mssql_exec` reports them. The
plan is one statement; nothing is buffered on the client; in a transaction
nothing defers.

Strings: **native** (D4 of 079 applies unchanged). What the pushed
statement's WHERE selects is what a pushed SELECT with that WHERE shows.

### D2 — what is vetoed, and what happens then

Everything 079's writer vetoes (an unmapped function, a parameter, a local
table in FROM / USING, a construct outside the table) vetoes the statement,
and the rewriter leaves it to the binder: the statement plans as today,
through `PlanUpdate` / `PlanDelete` / `PlanInsert`, on the path D3 describes.
Nothing is half-pushed: a DML either runs as one server statement or takes
the client path whole (research § 2: extra rows *written* are not
harmless, so no relaxed pushdown of a DML predicate, ever).

### D3 — the fallback: one path, keyed by the ladder

The shipped rowid path generalised by 067's ladder, resolved per table at
plan time:

1. **Primary key** — today's join key.
2. **A usable unique index** — spec 077's `ChooseRowIdKey`, already the
   rowid source since #350; its refusals (`RowIdRefusal`) name why a key
   is unusable (datetime / sql_variant, #358; a cast-required type, #354).
3. **All columns, NULL-safe** — the keyless base case (067 § 1's argument:
   for a deterministic WHERE and SET, matching by value updates exactly
   the set DuckDB would): `IS NOT DISTINCT FROM` on SQL Server 2022+, the
   `EXISTS (SELECT … INTERSECT SELECT …)` form below; the stage is
   DISTINCT over the key; LOB columns excluded from the key when the rest
   is unique in the staged set. A VOLATILE function in WHERE or SET on
   rung 3 is refused by name ("add a unique index, or make the expression
   deterministic").

Delivery: rungs 1–2 keep the `VALUES`-join statements below the small-result
threshold and stage above it; rung 3 always stages. The stage is a
`##stage_<uuid>` scratch table filled by a `BulkLoadSession` (the writer
spec 062 W0 built, `Adopt`ed onto the statement's own connection — the
pinned one in a transaction), the DML is
`UPDATE t SET t.c = s.c__new … FROM target t JOIN ##stage s ON <key>` /
`DELETE t FROM target t JOIN ##stage s ON <key>` per ~100k-row batch, and
the stage is dropped on the way out (067 D2/D3). The `vector<vector<Value>>`
buffer and the defer machinery go (no per-value path).

The **066 remainder** lands here because this is where it stops being
theoretical: `CollectSinkCatalogs` counts `LOGICAL_UPDATE` and
`LOGICAL_DELETE` beside INSERT and COPY, so the scans feeding a DML
materialise at init inside a transaction (spec 075 W3) and the executors'
`defer_execution_` retires — the sink and the scan feeding it share the one
pinned connection by the same rule INSERT already follows.

`BindUpdateConstraints` / `GetRowIdColumns` stop refusing a keyless table at
bind (065 D4): the refusal moves to plan time and names the only thing left
to refuse — the volatile guard on rung 3.

### D4 — MERGE, later

The rewriter carries `MergeQueryNode`, and T-SQL has `MERGE`; the mapping is
a D1 row when asked for. Until then MERGE keeps its bind-time PK requirement
(DuckDB's MERGE binder needs rowid for match classification — upstream).

## 2. Work

### W1 — the DML writer and the count vehicle

`UpdateQueryNode` / `DeleteQueryNode` / `InsertQueryNode` rendering in
`SQLWriter`; the `IDENTITY_INSERT` bracket reused from 077 W2; the count
form of `mssql_scan_params` over `MSSQLStatementConnection`; the CTAS shape
(`EXECUTE_STATEMENT` for CREATE TABLE AS only). The DML token loop exists
once (065 D3): the three shipped executors and the new path call one
`ExecuteDmlBatch`.

### W2 — the 066 remainder

`CollectSinkCatalogs` += `LOGICAL_UPDATE` / `LOGICAL_DELETE`;
`defer_execution_` and its buffers removed from the update/delete
executors; the transaction suite proves a scan feeding an UPDATE inside
`BEGIN … COMMIT` materialises and the connection is Idle for the sink.

### W3 — the ladder

Rung 3 in `ChooseRowIdKey`'s caller (the plan-time resolution), the two
NULL-safe join forms by server version, the volatile guard, the staged
delivery (`##stage` + `Adopt` + batched JOIN DML), the DISTINCT stage and
the LOB exclusion; bind-time refusal removed.

### W4 — riding cleanups from 065 D5

`mssql_dml_use_prepared` retired (loaded, read by nothing since inception);
`EnsurePKLoaded` no longer degrades a discovery error to "no key" — since
#350 it records `discovery_error` and the refusal names it, so with D3 a
hiccup cannot silently change which path a statement takes.

### W5 — tests

- The bite test (065 § 4.1): a statement with an unmapped function in WHERE
  takes the fallback — asserted by the `remote_pushdown` counter and a
  `dml_staged` / `dml_values_join` counter under `MSSQL_COUNTERS`.
- Pushed correctness: filtered UPDATE / DELETE against expected row sets
  (multi-column SET, CASE in SET, functions in WHERE, empty match, full
  table, `UPDATE … FROM` a remote join); count fidelity on both paths;
  transactions (pushed statement inside BEGIN / COMMIT / ROLLBACK on the
  pinned connection, mixed with reads and a sink).
- The ladder: PK → rung 1, PK-less with a unique index → rung 2, keyless →
  rung 3, asserted through counters; keyless UPDATE / DELETE with
  duplicates (both move); NULL-bearing keys in both join forms; the
  volatile guard's refusal on a keyless table and its absence on a keyed
  one; #140's reproduction end to end.
- `INSERT … SELECT` both remote: the rows never reach the client (counter +
  `mssql_pool_stats` bytes), identity bracket when the list names the
  column; `INSERT INTO local SELECT remote` pushed by the rewriter alone.
- The agreement pattern from 079 W5: every pushed DML shape also run with
  `mssql_remote_pushdown = false`, the table state compared after.
- 065's acceptance 1 as a bench: `UPDATE t SET x = 1 WHERE <pushable>` on
  1M matching rows before / after on the wide fixture.

### W6 — docs

README / website DML page ("what runs on the server", the ladder, the
volatile guard, the count), DATAMODEL (the DML flow diagram: rewriter →
one statement | plan → ladder → stage), CLAUDE.md DML line, CHANGELOG.

One PR after 079's merge; W1 → W6 as commits.

## 3. Not proposed

- `%%physloc%%` as a row identifier (undocumented, rows move) — 067 § 5.
- A strict-string DML mode: the measured form (`DATALENGTH` pair, § 8.5)
  stays in the record; native is the decision.
- RETURNING through `OUTPUT` — a later D1 row; the shipped path keeps its
  RETURNING behaviour, and a statement with RETURNING is not pushed.
- Pushing a relaxed predicate for a DML (research § 2).

## 4. Risks

- **A pushed DELETE removes the server's set** — the padded and
  case-variant rows a local `=` would not match. This is today's behaviour
  on the rowid path too; documented in one place with the SELECT rule.
- **`INSERT … SELECT` and identity seed** — SQL Server's identity seed is
  not transactional (077); a rolled-back pushed insert still advances it,
  as a rolled-back statement insert does.
- **Rung 3 on a wide table** — the stage carries every column; LOB columns
  make the JOIN expensive (067 § 2's note): measured in W3, LOBs excluded
  from the key when the rest is unique.
- **The remainder's blast radius** — `CollectSinkCatalogs` is on the
  planner's path for every DML; the transaction suite is the guard.

## 5. Acceptance

1. `UPDATE t SET x = 1 WHERE <pushable>` on 1M matching rows: no scan round
   trip, the counter shows the rewriter, wall time collapses to the
   server's statement time (before / after on the wide fixture).
2. #140's reproduction passes on all three rungs; the unpushable keyless
   statement with a volatile function is refused by name.
3. Counts agree with the server's affected rows on both paths; the full DML
   suite is green with the setting on and off.
4. Inside a transaction no DML defers: the connection is Idle after each
   statement, the transaction suite proves it.
5. The token loop exists once; `mssql_dml_use_prepared` is gone.

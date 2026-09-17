# Specs 065 / 066 / 067 — revalidation against the tree of 2026-09-17

**Why.** The DML reconnaissance (`research.md`) and the three specs it produced
are dated 2026-08-05/06. Since then the tree shipped spec 062 (INSERT via BCP,
one connection per statement, atomic DML), spec 075 (read path off the pinned
connection, materialisation before a sink), spec 076 (parameterised filters),
spec 077 (rowid from a unique index, `IDENTITY_INSERT`), and the DuckDB pin
moved from v1.5.5 to `v2.0-cyanoptera`. Every load-bearing claim below was
re-checked against `main` at `3a48795` plus PR #350; nothing here is from
memory of the August tree.

**Verdict in one line.** 065's mechanism is superseded by DuckDB 2.0's own
remote pushdown optimizer, which ships whole UPDATE/DELETE statements too (§ 5
— the finding that reorders the plan); 066 is superseded by spec 075 W3 except
for one small remainder worth its own PR; 067 stands, and the seam it was
waiting for has been built.

---

## 1. Claims re-checked

| Claim (research.md / spec) | Today | Evidence | Consequence |
|---|---|---|---|
| §1.1 — the 3-arg `PlanUpdate`/`PlanDelete` overloads exist and the generator calls them; we override only the 4-arg forms | **holds** on the v2.0 pin | `duckdb/src/include/duckdb/catalog/catalog.hpp:365-370`; `plan_update.cpp:30`, `plan_delete.cpp:41` call the 3-arg form; base 3-arg plans the child and forwards (`:24`, `:34`); ours: `mssql_catalog.hpp:135,138` (4-arg only) | 065 D1 as written |
| §1.2 — `PlanMergeInto` hook present | **holds** | `catalog.hpp:371` | MERGE phase 1 still reachable |
| §1.3 — the staging seam lacks "a single-writer bulk-load session on a GIVEN held/pinned connection" | **built** by spec 062 W0 | `BulkLoadSession::Adopt` — "takes a connection the operator already holds — the transaction's pinned one, or the pool" (`bulk_load_session.hpp:157-159`) | 067 D3 has its vehicle; research §1.3's "build it once" is done |
| §1.4 — #140 breaks at bind (`BindUpdateConstraints` / `GetRowIdColumns` throw) | **holds, with a different message and a wider key** | spec 077 W1: the throw is now `RowIdKeyInfo::RowIdRefusal`, and "no PK" became "no usable rowid key" — a PK-less table with a usable unique index no longer fails at all | 065 D4 must be rewritten against the 077 refusal, not the old text; #140's own reproduction (a table with no key at all) is still a bind error and still needs 065/067 |
| §2 — the scan silently DROPS filters it cannot encode | **no longer true** | since the 2.0 migration a refused filter runs client-side: `ApplyClientFilters` / `ClientTableFilter` (`table_scan.cpp:56-80, 832`) | 065 D2's bail-out rule stands, but for a different reason: an unencodable filter now means DuckDB-side work exists in the plan, which the direct statement cannot reproduce |
| §2 — SET expressions could go through `FilterEncoder::EncodeExpression` | **holds**, and the encoder is now parameterised | spec 076: constants travel as `@pN` through `SqlParamSet` (`filter_encoder.hpp:97,173-176,185`) | 065 D3's direct statement should be an `sp_executesql` batch with the same parameter set, not inline literals — free plan reuse, and the same `DeclarationForColumn` typing that keeps index seeks |
| §2 — the raw token loop is copy-pasted three times | **holds** — four, counting the RETURNING parser | `mssql_insert_executor.cpp:194`, `mssql_update_executor.cpp:281`, `mssql_delete_executor.cpp:252`, `mssql_returning_parser.cpp:153` | 065 D3's consolidation is still the right riding cleanup |
| §2 — "in explicit transactions everything defers to Finalize (rows buffered unbounded)" | **holds for UPDATE/DELETE** | `defer_execution_ = true` when `IsInTransaction` (`mssql_update_executor.cpp:58`, `mssql_delete_executor.cpp:62`); `vector<vector<Value>>` buffers (`:177`, `:153`) | see 066 below — the mechanism that retires this shipped for INSERT and COPY only |
| §2 — INSERT RETURNING keeps only the last batch | **fixed** | spec 062: "One result chunk per statement this input chunk completes" (`mssql_insert_executor.cpp`, `ExecuteWithReturning`) | drop from 065 §3 |
| §2 — `EnsurePKLoaded` swallows discovery errors as "no PK" | **fixed differently** by #350's self-review | `RowIdKeyInfo::discovery_error`; the refusal says the indexes could not be read | 065 D5's "rethrow instead" is moot; drop it |
| §2 — `mssql_dml_use_prepared` is loaded and never read | **still dead** | loaded in `mssql_dml_config.cpp:41`, `use_prepared` consumed nowhere | 065 D5 keeps this one |
| §3.3 / 066 — materialise own-catalog scans before a sink | **shipped** as spec 075 W3, catalog-scoped and transaction-gated exactly as 066 D1/D2 proposed | `MaterializeSharedConnectionScans` (`mssql_optimizer.cpp:747-775`): inside a transaction, a catalog with a sink or with ≥2 scans has its catalog scans materialised | 066 is superseded — with the remainder in § 3 |
| 066 D5 — the vehicle (`##stage` on a second connection, pipelined modes) | **not shipped** and not needed for what 075 covers | 075 materialises into a `ColumnDataCollection` (buffer-manager bounded, spills) | 066 D5's design carries over to 067 D3 unchanged, which is where it was always going to be consumed |
| 065 test 1 — assert the path through `MSSQL_COUNTERS` | counters exist for CTAS/COPY only | `CountersEnabled()` used in `mssql_ctas_*`; nothing in update/delete | the `direct_dml` / `rowid_dml` counters are new work, as the spec already says |
| §7 — native server semantics are the contract | **holds and is now written into shipped code** | spec 076's parameters are declared from the column (`DeclarationForColumn`), so a pushed comparison follows the column's collation; spec 077's rowid ladder already excludes keys a literal cannot match (#358, #354) | 067 rung 3's "comparison semantics are the server's" is consistent; the varchar-key caveat is #337's, not 067's |

## 2. Spec 065 — the amendments it would need, recorded before § 5 retired its mechanism

1. **D2, the reason for the bail-out.** Rewrite: "any table filter the encoder
   refuses is now applied client-side by the scan (`ClientTableFilter`); a
   plan that carries one has DuckDB-side work the direct statement cannot
   reproduce, so it is a hard bail-out to the rowid path." Same rule, true
   reason. Also: `complex_filter_where_clause` now comes with
   `complex_filter_params` (`mssql_functions.hpp:138-142`), and the direct
   statement must carry those parameters rather than re-render literals.
2. **D3, the statement shape.** `sp_executesql` with the scan's `SqlParamSet`
   plus the SET constants registered through `EncodeConstantValue`, not an
   inline-literal batch: one plan per statement shape, and no second
   rendering of a value the encoder has already typed. The count still comes
   from the DONE token; the consolidated loop (D3's real content) is unchanged.
3. **D4, the guard.** The bind-time throw is `RowIdRefusal` since 077 and it
   already names every rejected index. 065 does not replace it; it makes it
   *conditional*: `GetRowIdColumns` binds zero rowid columns when there is no
   usable key, plan time takes the direct path if fully pushed, and otherwise
   raises the 077 refusal **with one more sentence** — "a fully pushable WHERE
   would not need a key; this statement is not: <first unencodable
   filter/SET>". One message, both outs, nothing the user has already been
   told lost. MERGE keeps the bind-time requirement (upstream).

   **D5** shrinks to the dead setting. **§3** drops the RETURNING loss (fixed).
   Everything else — D1, D6, the tests, the acceptance criteria — as written.

**File overlap with #350** (why this waits for its merge): D4 edits
`mssql_table_entry.cpp` (`BindUpdateConstraints`, `GetRowIdColumns`) and
`mssql_catalog.cpp` (`PlanUpdate`/`PlanDelete`), both touched by 077 W1/W5b.

## 3. Spec 066 — superseded by 075 W3, with one remainder

What 066 asked for is in the tree: a plan-time walk that flags own-catalog
scans for materialisation, only inside a transaction, only for the catalog a
sink writes to (or when two scans of one catalog would share the connection).
`ColumnDataCollection` is the vehicle; 066 D5's server-side `##stage` is not
needed for this purpose.

**The remainder.** `CollectSinkCatalogs` counts `LOGICAL_COPY_TO_FILE` and
`LOGICAL_INSERT` as sinks (`mssql_optimizer.cpp:728-736`) — **not**
`LOGICAL_UPDATE` / `LOGICAL_DELETE`. So an UPDATE or DELETE inside a
transaction still runs its scan on the pinned connection *while* it needs
that connection for statements, which is exactly why the executors still
carry `defer_execution_` and buffer every row as `Value`s until Finalize. Add
the two operator types to the sink set and the scan drains first; the
executors can then send batches as they arrive in a transaction too, and the
defer machinery — the code 066's rationale wanted retired — goes. Small,
planner-only, its own PR; it also removes the "unbounded memory in a
transaction" caveat from 067 D3 before 067 exists. The one thing to measure
first: that `MaterializeSharedConnectionScans` sees `LogicalUpdate`'s scan
through the projection the binder inserts.

Status of the 066 file: kept for its problem statement and D5's design (which
067 D3 reuses); marked superseded in its header.

## 4. Spec 067 — stands; its seam exists

- **D1 rung 2 is spec 077's key choice.** "Any UNIQUE index over NOT NULL
  columns" is `ChooseRowIdKey` with its usability criteria — including the
  two 077 added that 067 did not know about: a filtered or disabled index is
  out, and so is a key column a literal cannot match (`datetime`, #358;
  `sql_variant`/`hierarchyid`, #354). 067 should call the same function
  rather than restate the rule; rung 3 begins where `RowIdKeyInfo::exists` is
  false.
- **D3 has its vehicle**: `BulkLoadSession::Adopt` on the statement's own
  connection (`MSSQLStatementConnection` from 062 W1c) is the "single-writer
  session on a held connection" research §1.3 said was missing. Stage fill
  and the JOIN then run on one connection inside one server transaction —
  which is also what makes the whole statement atomic (#344's contract).
- **Rung 3 and string keys.** The value-match ladder inherits the server's
  comparison semantics by design (§7). #337 — a `VARCHAR` primary-key
  DELETE failing intermittently on linux with a byte-sequence error — is a
  literal-rendering defect on today's VALUES path, not a semantics question;
  067's staged join sends those values through the BCP wire instead of as
  literals, which sidesteps the rendering but must be checked against #337's
  reproduction when the time comes.
- The volatile-function guard, the NULL-safe join forms and the tests stand.

## 5. The finding that reorders everything: DuckDB 2.0's remote pushdown

Raised by the owner during this revalidation; verified in the pin.

**What the core now has.** `Catalog` grew a remote-execution surface
(`catalog.hpp:98-103, 398-409`): `Supports(RemoteCapability)` with
`IS_REMOTE`, `EXECUTE_QUERY_NODE`, `EXECUTE_STATEMENT`, `CONNECT`;
`RemoteExecute(context, QueryNode | SQLStatement | string) → TableRef`; and
four `SupportsPushdown(...)` vetoes (expression, table ref, query node,
statement) that default to **true** — a catalog opts in by claiming a
capability and then vetoes what its dialect cannot carry. Driving it is
`RemotePushdownOptimizer` (`optimizer/remote_pushdown_optimizer.hpp`), run
from `Optimizer::OptimizeStatement` on the **parsed** statement, before
binding, whenever any attached catalog claims `IS_REMOTE`
(`database_manager.cpp:251`; `optimizer.cpp:216-226`). It walks the tree,
folds constants, and if every table reference resolves to ONE remote catalog
(`CatalogReferenceType::SINGLE_REMOTE_CATALOG`) and nothing is vetoed, it
strips the catalog qualifier, hands the node to `RemoteExecute`, and replaces
the whole statement with `SELECT * FROM <the TableRef it got back>`
(`FinishPushdown`, `WrapRemoteRef`). `SET disabled_optimizers =
'remote_pushdown'` turns it off; the CTE-in-parent case is a known FIXME
that blocks pushdown conservatively. History: "initial support for remote
pushdown to MySQL catalogs" (2026-06-04), DDL added 2026-08-18.

**It is not only SELECT.** `GetNodeFromStatement` yields a node for SELECT,
INSERT, DELETE, UPDATE and MERGE INTO (`:1826-1835`), and the optimizer has
`RewriteNode` for each — so a whole `UPDATE ms.t SET … WHERE …` whose every
piece the catalog accepts is shipped as one statement. **That is spec 065's
goal, delivered by the core**, with the dialect emitter as our part. CREATE /
DROP / ALTER go through `RemoteExecute(SQLStatement)`.

**What the reference implementation looks like** (duckdb-mysql,
`src/storage/mysql_catalog.cpp`, read 2026-09-17): `RemoteExecute(QueryNode)`
renders the node with its own dialect writer (`MySQLSQLWriter::MySQLToString`)
and forwards to `RemoteExecute(string)`, which returns a `TableFunctionRef` to
its own `mysql_query(catalog, sql, …)` — i.e. the existing raw-query table
function IS the transport. `SupportsPushdown` is a long veto list: TRY_CAST
and unsupported CASTs, VARCHAR literals under non-binary collation, FILTER /
ORDER-in-aggregate, DISTINCT aggregates beyond count/sum/min/max, window
frame kinds, STAR with EXCLUDE/REPLACE, QUALIFY, GROUP BY ALL, ROLLUP/CUBE,
SAMPLE, UNION BY NAME, SEMI/ANTI/FULL joins, TABLESAMPLE, AT clauses, VALUES,
and every INSERT. query.farm's ADBC extension does the same with an Arrow
stream as transport and reports 10-11× on grouped aggregates and selective
join+aggregate at 5M rows, with `query_pushdown false` as the ATTACH-level
switch and `EXPLAIN` showing the generated `Remote SQL`.

**What it means for us.**

- The transport exists: `mssql_scan(catalog, sql)` already describes at bind
  and streams at init (spec 075). `RemoteExecute(string)` is a
  `TableFunctionRef` to it.
- The investment is a **T-SQL writer over DuckDB's parsed tree** — the
  `MSSQLSQLWriter` analogue — plus the veto list. `QueryNode::ToString()`
  renders DuckDB's dialect and `dialect_extension.hpp` is a parser hook, not a
  renderer, so there is no core facility to reuse; the mysql extension wrote
  its own. Ours has known translation points from `FilterEncoder` already
  (function mapping, literal rendering, identifier quoting) and the hard
  parts are T-SQL's: `TOP`/`OFFSET…FETCH` needs an `ORDER BY`, no `LIMIT`
  without it; `||` → `+`/`CONCAT`; no `ILIKE`; no boolean-typed select
  expressions; `GROUP BY ALL`; `DISTINCT ON`; integer division; `AVG(int)`
  and `SUM(int)` overflow/truncation (research §7's numeric wrappers);
  collation semantics (§7 — the mysql veto on non-binary-collation string
  literals is the same question, answered the other way; ours is *native
  by default, strict opt-in*).
- It decides on the **parsed** tree: no types, no catalog metadata beyond
  "does this entry exist" (`EntryExistsInLocalCatalog`). Vetoes have to be
  decidable syntactically, which is why the mysql list is long and coarse.
  Where a decision needs a column type — `datetime` vs `datetime2`
  comparison, a `sql_variant` column — the veto must be conservative or the
  writer must fetch metadata itself.
- It requires the WHOLE statement to be one remote catalog. A DuckLake-style
  UPDATE joined to local data, or any mixed-catalog statement, falls back to
  today's plan — which is exactly where 067's staged JOIN remains necessary.
- Our own `MSSQLOptimizer` (specs 039/075/076: filter, ORDER BY, TOP-N into
  the catalog scan) runs on the *logical* plan, after binding. When the
  remote rewrite fires it never sees a catalog scan; when it does not, it
  applies as today. They compose; nothing to reconcile.

**Consequence for 065.** Its D1/D2 (3-arg plan hooks, a plan-time dry run of
the filter encoder over pushed table filters) is a second, narrower
implementation of what the core rewriter does on the parsed tree — and the
core one also delivers joins, aggregates and DDL for the same writer. 065 as
written should not be built. What survives of it: D3's token-loop
consolidation (a cleanup on its own), D5's dead setting, and the *tests* —
count fidelity, transactions, the #140 shapes — which apply to the rewrite
path unchanged.

## 6. Order, now

1. **Spec 078 (new): T-SQL writer + remote pushdown.** Claim `IS_REMOTE` +
   `EXECUTE_QUERY_NODE`; `RemoteExecute(string)` over `mssql_scan`; the
   writer for SELECT first (joins, aggregates, ORDER/TOP), UPDATE/DELETE
   next, with a veto list written from T-SQL's grammar and the §7 semantic
   contract; DDL last. An ATTACH option to disable, mirroring
   `query_pushdown false`. This is the join/agg phase AND 065's goal in one
   piece of work, on a core mechanism instead of our own. Spec-only first;
   the writer is the bulk of it and deserves its own measured design (which
   constructs, in what order, with `EXPLAIN` showing the remote SQL as the
   test lever).

   **Decided by the owner, 2026-09-17:** SELECT first, UPDATE/DELETE as the
   second milestone. And one more item goes into 078's scope: **the fate of
   `MSSQLOptimizer`** (specs 039/075/076 — filter, ORDER BY and TOP-N pushed
   into the catalog scan on the *logical* plan). For a single-catalog
   statement the rewriter makes it redundant; it stays meaningful only for
   mixed-catalog plans, where a per-scan TOP-N still saves a transfer. Two
   translators for one dialect — `FilterEncoder` for scan predicates, the
   new writer for whole statements — is the real cost, so 078 should decide
   whether the logical optimizer is rewritten to render its scan subtree
   through the same writer, kept as is for the mixed case only, or retired
   with the mixed case left to the scan's own filter pushdown. The owner's
   expectation is that it is rewritten or retired in favour of the rewriter;
   078 measures which. **Settled in § 9.1 after reading the rewriter's
   granularity:** kept, as the fallback for mixed plans, local views and
   subqueries under a non-pushable outer node — which the rewriter cannot
   reach — with the vocabulary shared (one function table, one collation
   predicate, one literal/parameter spelling); the two translators walk
   different trees (bound vs parsed), so that is the sharing there is.
   The delivery vehicle is `RemoteExecute(QueryNode)` → `mssql_query`, not
   the `RemoteExecute(string)` over `mssql_scan` first sketched here.
2. **066-remainder** — unchanged, small, independent: UPDATE/DELETE join the
   sink set so the executors stop deferring in transactions. After #350.
3. **067** — the staged JOIN for what the rewriter cannot take: mixed
   catalogs, unsupported constructs, keyless tables. Consumes
   `ChooseRowIdKey` and `Adopt`. Its rung 1-2 keyed path is today's VALUES
   join until then.
4. MERGE: the rewriter has `MergeQueryNode`; the composition phase (research
   §3.2) becomes the fallback for what it vetoes.

## 7. Decisions and open questions

Decided (owner, 2026-09-17): 065 is retired in favour of spec 078 on the core
rewriter; the first writer is SELECT only, UPDATE/DELETE second; 078 also
settles what becomes of `MSSQLOptimizer`'s TOP-N/ORDER BY pushdown.

Answered since, in § 9: default **on** behind `mssql_remote_pushdown`
(BOOLEAN, a setting rather than the ATTACH option § 6 first suggested, so it
can be flipped per session for a diagnosis) once the agreement suite is
green (§ 9.1); `MSSQLOptimizer` kept (§ 6 item 1); string semantics in
§ 9.3, the aggregate and function vocabulary in § 9.2, `vector` deferred past
v0.3.0 (§ 9.5, #363).

Still open:

- 066-remainder as its own small PR before 067, or folded into 067?
- The final word on § 9.3's string rule (proposed there; the reviewer was
  part of the original `<>` discussion, so it is asked in the PR).

## 8. Strings on the server — measured, for 078, 061 and 076

Raised by the owner: with whole statements on the server, ORDER BY, GROUP BY,
DISTINCT, join keys, MIN/MAX and every comparison on a string column run
under the column's collation, and most SQL Server columns are not UTF-8.
Spec 061 already holds the measured half of this (the `COLLATE …_BIN2` added
predicate keeps the seek; padding is an operator rule no collation turns off;
negations cannot be made exact by adding predicates). Two more measurements
were needed, and both change something. Live SQL Server 2025, 2026-09-17.

### 8.1 A `_BIN2` collation on a code-page column does not give DuckDB's order

`varchar(20) COLLATE SQL_Latin1_General_CP1_CI_AS` holding
`z é Š a € A ab␣ ab`:

| ordered by | result |
|---|---|
| the column's own collation | `€ Š a A ab␣ ab é z` — linguistic |
| `COLLATE Latin1_General_BIN2` (what 061 emits) | `A a ab␣ ab z € Š é` — **the code page's bytes**: € 0x80 < Š 0x8A < é 0xE9 |
| `COLLATE Latin1_General_100_BIN2_UTF8` | `A a ab␣ ab z é Š €` — **UTF-8 bytes** |
| DuckDB | `A a ab ab␣ z é Š €` |

`_BIN2` on a varchar column compares the bytes it is stored in; only a
`_UTF8` binary collation transcodes first, and then byte order is code-point
order for every plane — including above the BMP, which is 061's own caveat
for `nvarchar` (UTF-16 code units) and which `_BIN2_UTF8` does not fix there
(a UTF-8 collation applies to `varchar`; an `nvarchar` under it stays
UTF-16). The one residual is `ab` / `ab␣`: SQL Server calls them equal
(padding), so their relative order is a tie the server breaks as it likes,
where DuckDB puts the shorter first.

**Consequence for 061:** the forced collation for a `varchar` key is
`Latin1_General_100_BIN2_UTF8`, not `Latin1_General_BIN2`; for an `nvarchar`
key `_BIN2` stays (code units, BMP-exact). UTF-8 collations exist from SQL
Server 2019; on an older server there is no exact form for a non-ASCII
`varchar` key at all — refuse to push, or push native and say so. 061's
§ 4.3 numbers hold for the UTF-8 form: `TOP 10 … ORDER BY a COLLATE
…_100_BIN2_UTF8` plans as Index Scan → `Sort(TOP N)`, the bounded sort; the
native form is an ordered index scan with no sort.

### 8.2 The seek depends on the collation FAMILY and the literal's type

Same table, index on `a`, 20 000 rows, `SET SHOWPLAN_TEXT`:

| column collation | predicate | plan |
|---|---|---|
| `SQL_Latin1_General_CP1_CI_AS` (the installation default) | `a = 'f1500'` | **Index Seek** |
| `SQL_Latin1_General_CP1_CI_AS` | `a = N'f1500'` | **Index Scan**, `CONVERT_IMPLICIT(nvarchar(20), a)` on the column side |
| `Latin1_General_CI_AS` (Windows) | `a = 'f1500'` | Index Seek |
| `Latin1_General_CI_AS` (Windows) | `a = N'f1500'` | Index Seek through `GetRangeThroughConvert` |
| `SQL_…CP1_CI_AS` | `a = 'x' AND a = 'x' COLLATE …_100_BIN2_UTF8` | **Index Seek** + residual — 061 § 3.2's pair holds for the UTF-8 form |
| `SQL_…CP1_CI_AS` | `a = 'x' COLLATE …_100_BIN2_UTF8` alone | Index Scan |

And the exact form spec 076 emits today, under `SET STATISTICS PROFILE`:

| `sp_executesql` declaration | plan |
|---|---|
| `@p0 nvarchar(20)` = `N'ñu'` | **Index Scan** with the convert on the column |
| `@p0 varchar(20)` = `'ñu'` | **Index Seek** |

**A live defect in spec 076, on the default install.** `DeclarationForColumn`
(`filter_encoder.cpp`) declares any **non-ASCII** constant as `nvarchar`
"whatever the column's collation", for a documented reason that is right in
its own case — a `varchar` variable takes the *database's* code page, so a
Cyrillic constant against a UTF-8 column arrived as `?` (#321) — and costs
the index seek in the common one: column and database both on
`SQL_Latin1_General_CP1_CI_AS`, a Latin-1 constant such as `'ñu'`. Windows
collations are unaffected (the optimizer derives a range through the
convert); SQL_ collations are exactly the installation default. The remedy is
to declare `varchar` when the constant is representable in BOTH the column's
and the database's code page — for the Latin-1 range on CP1252 that is a
table lookup, and it is the case that matters — and `nvarchar` otherwise, as
now. Worth its own issue and small PR; it is independent of 078.

### 8.3 What this means for the T-SQL writer (spec 078)

1. **Literals are spelled per column.** A `varchar` literal for a `varchar`
   column when every character is representable in its code page (the seek
   survives on every collation family), `N'…'` otherwise (correct, and a scan
   on SQL_ collations — the same trade 076 makes today, but only when it is
   forced). Never a `varchar` literal that would transcode lossily. This needs
   the column's collation and code page, which the writer has: parsed table
   refs carry `catalog.schema.table`, and our metadata cache resolves columns
   without DuckDB binding.
2. **The decision point is `SupportsPushdown(const QueryNode &)`.** It sees
   the whole SELECT with its FROM, so it can resolve every column reference
   through our cache and decide per string column and per surface; a
   reference it cannot resolve (an alias through a subquery it did not walk)
   is a conservative veto. `RemoteExecute` cannot decline — the optimizer has
   already replaced the statement — so nothing may be left for it to find.
3. **Two semantics, one knob, chosen per surface exactly as 061 § 4.4 does:**
   - **native** — the server's answer: ORDER BY as an ordered index scan,
     GROUP BY / DISTINCT / joins under the column's collation, `=`/`LIKE`
     exact and seekable. This is the shipped contract for pushed filters
     (owner's decision on #275; research § 7), it is what SSMS shows the same
     user, and it is the fast path.
   - **strict** — DuckDB's answer: string ORDER BY keys get `COLLATE
     …_100_BIN2_UTF8` (`_BIN2` on `nvarchar`), a bounded sort under TOP;
     equality and prefix predicates get the added `COLLATE` predicate and
     keep the seek; GROUP BY / DISTINCT / join keys get the collation on the
     key expression (an index scan, never a seek); `<>` / `NOT LIKE` are
     rendered exactly under the forced collation (a scan) — there is no
     client re-check behind the rewriter, so the superset argument of 061
     § 3.3 does not apply and exactness is the only option; padding stays,
     and is documented as the one residual (061's sentinel is available for
     equality). Above-BMP characters on `nvarchar` keys stay a documented
     divergence.
   - **DML through the rewriter is always strict** (061 § 4.2's reasoning
     applies unchanged: a write nothing can re-check must match what DuckDB
     would have written), whatever the knob says for reads.
4. **String functions are a value question, not a collation one** — 061
   § 4.5's list (`LEN` counts UTF-16 units and drops trailing spaces; `/`,
   `week`, `dayofweek`) carries over to the writer's function table as
   vetoes or exact forms; the mysql reference vetoes the same way.

### 8.4 Decided (owner, 2026-09-17), with one premise corrected by measurement

**Padding: push symmetrically and document.** The owner's premise was that
trailing spaces never reach DuckDB because the server stores `varchar` without
them. Measured: it stores them and returns them. `'ab  '` has `DATALENGTH` 4 in
`varchar`, 8 in `nvarchar`, 4 under `Latin1_General_100_BIN2_UTF8`; our read
path delivers `[ab  ]` with `length() = 4`; DuckDB's `a = 'ab'` is **false** for
that row while the server's is **true** — under every collation, the binary
UTF-8 one included, because padding is an operator rule. So the two sides do
disagree on that row, and the decision stands on the native contract rather
than on equivalence: a pushed `=` returns the server's answer (both rows), the
same predicate evaluated in DuckDB returns one, and the documentation says so
in so many words (this is #272's shape; 061 § 4.1's arithmetic). Symmetric on
ORDER BY: the two values are a tie the server breaks as it likes.

**ORDER BY / TOP: push only when the key's collation already orders the way
DuckDB does; otherwise no pushdown, as today.** No `COLLATE` forcing at all —
simpler than 061 § 4.3, keeps the ordered-index path, needs no version gate.
The criterion is narrower than "a UTF-8 collation": a linguistic `_UTF8`
collation (`…_CI_AS_SC_UTF8`) sorts linguistically, and a `_BIN2` on a
non-UTF-8 `varchar` sorts code-page bytes (§ 8.1). It is **binary AND
UTF-8-coded**: `varchar` under a `_BIN2_UTF8` collation, or `nvarchar` under a
`_BIN2` one (UTF-16 code units — exact through the BMP, the above-BMP
divergence documented). Non-string keys as now. This also corrects 061 § 2,
whose "`_BIN2` or non-string" admitted code-page `_BIN2` columns. The
extension's own CTAS/COPY default, `Latin1_General_100_BIN2_UTF8`, qualifies,
so tables the extension created push; a database's `_CI_AS` columns do not,
and the query runs as today. The `UTF8SUPPORT` acknowledgement at ATTACH
(#225) says the server is 2019+ and can carry such collations; the decision
itself is per column, from the collation name the catalog already holds
(`is_utf8`, plus a `_BIN2` test to add beside it).

**Filters: native, the shipped contract.** Exact predicate, seekable, the
server's answer; `<>` / `NOT LIKE` on strings stay un-pushed as 061 § 4.1
decided — **measured later that they are pushed today, see § 9.3**, which
proposes native for every string predicate. **DML through the rewriter:
strict**, as § 8.3 — **revised to native in § 9.3** (owner, the same day).

Two things stand on their own and are filed separately: the 061 correction
(`_BIN2` → binary-and-UTF-8-coded as the agreement test) and the 076 seek
defect (§ 8.2).

### 8.5 Trailing spaces: no server-side setting, and what the strict forms cost

Asked after § 8.4: is there a setting that changes the server's treatment of
trailing spaces, and what does each way of getting DuckDB's answer cost.
Measured on the docker `2025-latest`, database collation
`SQL_Latin1_General_CP1_CI_AS`; `dbo.pad_probe` holds `ab`, `ab␣`, `ab␣␣`,
`abc` in a `varchar(10)`, a `char(10)`, an `nvarchar(10)` and a `varchar(10)`
under `Latin1_General_100_BIN2_UTF8`.

**There is no setting.** Padding is the comparison rule of the engine
(ANSI SQL-92 § 8.2: the shorter operand is padded with spaces before `=`, `<`,
`>`), not a property of a collation — no SQL Server collation is `NO PAD`,
the `_BIN2_UTF8` column pads like the rest. The one knob with "padding" in
its name, `SET ANSI_PADDING`, governs **storage** only: a table created under
`OFF` stores `'ab␣␣'` as 2 bytes (`char(10)` too), and its `v = 'ab␣␣␣␣'`
is still true. It is deprecated besides. So the server cannot be told to
compare the way DuckDB does; a predicate has to say it.

| form, value `ab` vs `ab␣` | server says | DuckDB says |
|---|---|---|
| `v = 'ab'` | equal | not equal |
| `v LIKE 'ab'` (varchar, any collation incl. BIN2_UTF8) | **match** — value-side spaces ignored, pattern-side significant (`LIKE 'ab␣'` misses `ab`) | — |
| `n LIKE N'ab'` (nvarchar) | no match — strict, unlike varchar | — |
| `v = 'ab' AND DATALENGTH(v) = DATALENGTH('ab')` | not equal | not equal |
| `v + '~' = 'ab' + '~'` (061's sentinel) | not equal | not equal |
| `GROUP BY v` / `DISTINCT v` on the four rows | 2 groups; the representative kept for the `ab` group was `ab␣` (arbitrary) | 4 |
| self-join on `v` | 10 rows (3×3 + 1) | 4 |
| `UNIQUE (v)` | `ab␣` is a **duplicate** of `ab` | — |
| `char(10)` = `'ab'` | equal (stored padded to 10) | not equal: DuckDB receives `[ab        ]`, `length() = 10` |

The last row is the practical case: `char(n)` codes arrive padded, so any
DuckDB-side `=`, join or `GROUP BY` on them misses what the pushed predicate
finds. Demonstrated end to end through the extension: `WHERE v = 'ab'` pushed
→ 3 rows, the same rows grouped locally → 4 groups; `WHERE c = 'ab'` on the
`char(10)` pushed → 3, locally → 0.

**What strictness costs on the server** — `dbo.pad_seek`, 200 000 rows,
nonclustered index on each column, `sp_executesql` parameters as spec 076
sends them, `STATISTICS PROFILE` / `IO`:

| predicate | plan | logical reads |
|---|---|---|
| `v = @p` (shipped) | Index Seek | 3 |
| `v = @p AND DATALENGTH(v) = DATALENGTH(@p)` | Index Seek + Filter | 3 |
| `v = @p AND v + '~' = @p + '~'` (061 § 3.2 pair) | Index Seek, residual inside | 3 |
| `v + '~' = @p + '~'` alone | Index **Scan** | 519 |
| `CAST(v AS varbinary(20)) = CAST(@p AS varbinary(20))` alone | Index Scan | 519 |
| `RTRIM(v) = @p` | Index Scan | 519 |
| `v LIKE @p` / `n LIKE @p` | dynamic seek (`LikeRangeStart` + Index Seek) | 3 |
| `n = @p AND DATALENGTH(n) = DATALENGTH(@p)` (nvarchar) | Index Seek + Filter | 3 |
| `c = @p`, `@p varchar(10)` (char column, 076's declaration) | Index Seek, 4 rows | 3 |
| `c = @p AND DATALENGTH(c) = DATALENGTH(@p)` | Index Seek + Filter, **0 rows** — a padded `char` never equals a shorter constant, which is DuckDB's own answer for the padded value | 3 |
| `v > @p OR (v = @p AND DATALENGTH(v) > DATALENGTH(@p))` (DuckDB's `>`) | two seeks + Concatenation + Distinct Sort | 6 (vs 3) |

So a strict predicate is free exactly when it is **paired** with the native
one — the seek finds the padded-equal rows, the residual drops the rest — and
costs a full index scan when it replaces it. `DATALENGTH` is the simpler
pair: no concatenation, works for `nvarchar` (byte length on both sides), and
nothing in it depends on the collation. `LIKE` is not a strict `=`: it ignores
value-side spaces on `varchar` and honours them on `nvarchar`, and its
pattern needs `%`, `_`, `[` escaped.

Beyond the seek, `STATISTICS TIME`, three runs each: `GROUP BY c` 23 ms vs
`GROUP BY c, DATALENGTH(c)` 24–25 ms; hash join `a.v = b.v` 37–50 ms vs the
`DATALENGTH` pair 47–55 ms (a residual on the probe, about +10 ms on 200 000
matches); `ORDER BY n` 120 ms serial vs `ORDER BY n, DATALENGTH(n)` 51 ms
elapsed / 253 ms CPU — the extra key made the optimizer pick a parallel sort,
so it is not the expression that costs but the plan change it invites.

**What this adds to § 8.4.** The decision (native `=`, documented) stands; the
measurement says the strict alternative would cost the server nothing on a
seek and little elsewhere, so if a strict mode is ever wanted for spec 078's
rewritten DML — where § 8.3 already asks for it — the `DATALENGTH` pair is
the form, not the sentinel and not `LIKE`. And the `char(n)` row is worth a
line in the documentation next to the padding note: a `char` column's values
reach DuckDB padded, so a local comparison against a shorter constant misses
them where the pushed one matches (`rtrim()` on the DuckDB side is the user's
tool, as it is for any client of `char(n)`).

## 9. Proposal: what we push down, and how (the shape of spec 078)

Written from § 5 (the mechanism), § 8 (strings, measured) and a second read
of `remote_pushdown_optimizer.cpp` on the pinned DuckDB for its granularity.
**The aim is not 100 % pushdown** (owner, 2026-09-17): one expression writer
shared by SELECT list, WHERE, ON, HAVING and ORDER BY, a table-driven
vocabulary where every entry is a proven exact form, veto by default, and
the scan path as the fallback — logical and maintainable before complete.

### 9.1 Mechanism and its granularity

The core rewriter does the walking; we supply three things: `Supports`
(`IS_REMOTE`, `EXECUTE_QUERY_NODE`, later `EXECUTE_STATEMENT`), the
**policy** in the four `SupportsPushdown` overloads (expression, table ref,
query node, statement — every veto lives there, `RemoteExecute` cannot
decline), and the **T-SQL writer** behind `RemoteExecute`, which returns a
`TableFunctionRef` to a `mssql_query(catalog, sql)` on spec 075's machinery:
shape from `sp_describe_first_result_set` at bind, execution at InitGlobal,
materialised at init inside a transaction under `MaterializeMutex` on the
pinned connection. One describe round trip per planned statement is the
fixed cost; the result's types are the server's for the pushed shape.

Where the rewriter can and cannot split a statement (read from the source,
not assumed): it pushes the **whole statement**, an INSERT's **SELECT alone**
when the target is local or the INSERT is vetoed, **each child of a set
operation** on its own, and a **CTAS query** whose table is local. It does
**not** push a subquery sitting under a non-pushable outer node
(`Rewrite(SubqueryRef&)` never calls `FinishPushdown`), a **local view or
macro** over remote tables blocks it (`EntryExistsInLocalCatalog`), a
statement mixing catalogs blocks it, and a **parameter** (`$1`) reaches
`SupportsPushdown(ParsedExpression)` for us to veto. So the two logical-level
mechanisms we ship — the scan's filter pushdown and `MSSQLOptimizer`'s
ORDER BY / TOP — **stay** as the fallback for mixed plans and views; neither
is retired by 078. What they share with the writer is one collation
predicate and one function table, so a query cannot answer differently
depending on which path it took.

Constants travel as `sp_executesql` parameters exactly as spec 076 does —
declared from the column when the comparison is column-vs-constant (§ 8.3.1,
with the 076 seek fix), from the value otherwise — so the server keeps one
plan per query shape. Kill switch: DuckDB's own
`SET disabled_optimizers = 'remote_pushdown'` is global across every remote
catalog; we add `mssql_remote_pushdown` (BOOLEAN, default **true** once the
agreement suite is green) gating `Supports`, and `EXPLAIN` shows the T-SQL.

### 9.2 SELECT — what pushes in phase A

| construct | T-SQL | rule |
|---|---|---|
| FROM base tables, remote views | `[schema].[name]` | one catalog; a remote view pushes, the server expands it |
| projections, WHERE, HAVING | expression writer | column refs, constants, `+ - *` (overflow errors on both sides), `/` → `CAST(a AS float) / NULLIF(b, 0)` (DuckDB: double, NULL on zero), `//` and `%` on integers, CASE, CAST to a mapped type (no TRY_CAST), COALESCE/NULLIF, BETWEEN, IN list, IS [NOT] NULL, AND/OR/NOT, comparisons, LIKE with `[`, `%`, `_` escaped and an ESCAPE clause, the verified scalar-function table |
| scalar functions | per-function exact form or veto; the writer starts from the scan's `function_mapping.hpp` (`lower`, `upper`, `trim`, `ltrim`, `rtrim`, `year`, `month`, `day`, `hour`, `minute`, `second`, `+ - * %`, `negate`, `prefix` / `suffix` / `contains` → `LIKE`), measured live: `year(dt) = 2024`, `upper(s) = …`, `LIKE`, `i % 2` reach the server; `length`, `abs`, `round`, `date_trunc`, `coalesce`, `||` and a comparison DuckDB wrapped in a CAST (`datetime2(3)` vs a TIMESTAMP constant) are refused and run in DuckDB | vetoes as duckdb-mysql's and 061 § 4.5: `length` (LEN drops trailing spaces, counts UTF-16 units), `upper`/`lower` (simple case mapping only), `sqrt`/`ln`/`log` (server errors where DuckDB gives NaN), `power`/`exp` (overflow errors), `week`/`dayofweek` numbering; date parts, `abs`, `floor`/`ceil`, `round` (half away from zero on both), `substring`, `concat`, `trim` family verified one by one |
| GROUP BY + aggregates | plain group lists, positional and alias references **expanded** (T-SQL has neither: errors 164 / 207); `COUNT_BIG(*)` / `COUNT_BIG(x)` / `COUNT_BIG(DISTINCT x)` (`COUNT` is `int`), `SUM(CAST(int AS bigint))` (`SUM(int)` overflows with 8115 where DuckDB widens to HUGEINT; bigint overflow still errors — documented), `AVG(CAST(x AS float))` (`AVG(int)` truncates: 1 for 1 and 2; `AVG(decimal)` is decimal scale 6 where DuckDB says DOUBLE), `MIN`/`MAX` (`bit` → `CAST(b AS tinyint)`, error 8117 otherwise; string keys only under a binary UTF-8-coded collation, § 9.3), `STRING_AGG(CAST(x AS nvarchar(max)), <literal>) WITHIN GROUP (ORDER BY …)` (the separator must be a literal, 8733; without the LOB cast the result errors past 8000 bytes, 9829), `STDEV` / `STDEVP` / `VAR` / `VARP` for `stddev_samp` / `stddev_pop` / `var_samp` / `var_pop` (equal to the last digit or one ulp: 2.857738033247041 vs 2.8577380332470415 — the suite compares with a tolerance), `agg(x) FILTER (WHERE c)` → `agg(CASE WHEN c THEN x END)` | veto `first` / `any_value` (`ANY_VALUE` is not a function on `2025-latest`), `arg_min` / `arg_max`, `median` / `quantile` / `mode`, `approx_count_distinct` (`APPROX_COUNT_DISTINCT` exists but is a different estimator), `list` / `array_agg` / `histogram`, `bit_*`, `corr` / `covar_*` / `regr_*`, a non-literal `string_agg` separator, GROUP BY ALL / ROLLUP / CUBE / GROUPING SETS in phase A (T-SQL has the last three) |
| JOIN | INNER / LEFT / RIGHT / FULL / CROSS with ON; USING expanded; the ON condition is any expression the writer renders — non-equi, OR, functions, subqueries — under the same rules and the same string policy as WHERE, nothing join-specific | veto NATURAL, SEMI / ANTI (EXISTS rewrite later), ASOF, POSITIONAL, LATERAL (CROSS APPLY later) |
| ORDER BY | `ORDER BY` with NULL placement emulated as `MSSQLOptimizer` does today | string keys only under a binary, UTF-8-coded collation (§ 8.4); otherwise the node is vetoed and the query runs as today |
| LIMIT / OFFSET | `TOP n`; `OFFSET … FETCH` (needs ORDER BY — `ORDER BY (SELECT NULL)` when there is none, both sides are arbitrary then) | |
| DISTINCT, set operations | `DISTINCT`; UNION / UNION ALL / EXCEPT / INTERSECT | veto DISTINCT ON, `EXCEPT ALL` / `INTERSECT ALL`, UNION BY NAME |
| CTEs, subqueries | `WITH`; scalar / EXISTS / IN / quantified (`= ANY`) subqueries, correlated included | recursive CTEs in phase B (UNION ALL only, no RECURSIVE keyword) |
| window functions | ROW_NUMBER / RANK / DENSE_RANK / NTILE / LAG / LEAD / FIRST_VALUE / LAST_VALUE / aggregates OVER, PARTITION BY, ORDER BY, ROWS frames and the default RANGE frame | veto RANGE with offsets, GROUPS, EXCLUDE, QUALIFY |
| `SELECT * EXCLUDE / REPLACE` | expanded from the catalog's column list | `COLUMNS(...)`, PIVOT / UNPIVOT, TABLESAMPLE, parameters: veto |

### 9.3 Strings, the § 8.4 policy applied to a whole query

- **Equality-class operations are the server's**: `=`, `IN`, GROUP BY,
  DISTINCT, join keys, set-operation deduplication — under the column's
  collation (a `_CI` collation merges case variants) and with padding
  (`ab` = `ab␣`). This is the shipped contract for filters, and the only
  workable one: the installation default is `SQL_Latin1_General_CP1_CI_AS`,
  so vetoing case-insensitive keys would leave the rewriter dead on most
  databases. One documentation paragraph, with § 8.5's `char(n)` note.
- **Orderings must be DuckDB's**: ORDER BY, `MIN`/`MAX` on strings, window
  ORDER BY push only under a binary, UTF-8-coded collation; otherwise the
  node is vetoed. Range predicates (`name > 'M'`) are pushed natively today
  by the scan and stay so. Rule of thumb: a *set* the server produces is the
  server's set, an *order* the server produces must be DuckDB's order.
- **Negative string predicates.** The recollection was that `<>` / `NOT LIKE`
  / `NOT IN` stay un-pushed (061 § 4.1, the discussion with the reviewer:
  they narrow the server's answer and DuckDB cannot widen it back). Measured
  on the shipped scan, five rows `ab`, `ab␣`, `AB`, `abc`, `x`: `v = 'ab'` → 3,
  `v <> 'ab'` → **2** (server; DuckDB would say 4), `v NOT LIKE 'ab%'` → **1**
  (server; DuckDB 2), `v > 'ab'` → 2 (server; DuckDB 3), and only
  `v NOT IN ('ab','x')` → **3** (DuckDB; the server would say 1) — because the
  encoder has no `COMPARE_NOT_IN` case, not by policy. So `<>` and `NOT LIKE`
  are already native, as are the ranges, which narrow just the same under a
  linguistic collation (`'ñu'` between `n` and `o`, the `like_pushdown_collation`
  test). Native on both sides is also the only rule under which `=` and `<>`
  partition the table (3 + 2 = 5; with a strict `<>` it is 3 + 4). Proposal:
  native for every string predicate in both paths, and `NOT IN` joins the
  encoder so it stops being the one strict exception.
- **Partial pushdown of a WHERE** exists today and is what the rewriter falls
  back to: DuckDB splits a conjunction, each conjunct is pushed when the
  encoder renders it and evaluated client-side otherwise (a `LogicalFilter`
  for what the combiner keeps, the spec 069 net for a refused table filter);
  a disjunction is pushed whole or not at all. The rewriter itself is
  all-or-nothing per statement by construction, and it should **not** add a
  DuckDB-side re-check of its own (`RemoteExecute` could return a subquery
  ref with a WHERE): a re-check only helps where the server returned a
  superset (positive equality), never for `<>` or ranges; it needs the
  predicate's columns in the output and nothing aggregated or joined above
  them, so it would make the semantics depend on the query's shape
  (`SELECT a … WHERE a = 'ab'` → 1 row, `SELECT count(*) … WHERE a = 'ab'` →
  3); and it contradicts native DML. Not even for the simple cases.
- **DML is native** (owner, 2026-09-17: a SQL Server user expects SQL Server's
  DELETE; a revision of § 8.3's "always strict"). Today's
  rowid path is already native: the scan pushes the WHERE, DuckDB does not
  re-check a pushed filter, so `DELETE … WHERE a = 'ab'` removes the three
  padded/case-variant rows that `SELECT … WHERE a = 'ab'` shows. A strict
  rewritten DELETE would remove one and leave two the same SELECT displays —
  the asymmetry the symmetric decision exists to prevent. If strict DML is
  wanted after all, § 8.5's `DATALENGTH` pair is the form (seek kept), plus
  `COLLATE …_BIN2` paired for case — never the sentinel, never `LIKE`.
- Literals spelled per column (§ 8.3.1), the 076 seek defect fixed first.

### 9.4 Phase B — DML and DDL through the same writer

- **UPDATE / DELETE** fully expressible (§ 9.2's rules on WHERE and SET,
  `UPDATE … FROM` joins) → one statement on the pinned connection inside a
  transaction, bracketed as spec 062 W1c in autocommit; the count from DONE.
  `RETURNING` vetoed until `OUTPUT` is mapped. This supersedes 065's D1/D2/D6
  on a core mechanism; D3's token-loop consolidation and D5's dead setting
  still stand on their own.
- **INSERT … SELECT** with both sides remote → server-side, the 077
  `IDENTITY_INSERT` bracket when the list names the identity column;
  `INSERT INTO local SELECT … FROM remote` gets its SELECT pushed by the
  rewriter with no work on our side.
- **CTAS** with remote source and target → our own CREATE (table kind,
  collation, lengths as today) followed by a pushed `INSERT … SELECT`, not
  `SELECT INTO`, so the WITH options keep meaning.
- **MERGE** later; recursive CTEs here.
- Everything not expressible stays on the staging path (066-remainder, 067).

### 9.5 `vector` (SQL Server 2025) — measured, and what a pushdown would be

On `2025-latest`: `vector(3)` is a user type over `varbinary` (`sys.columns`:
system type `varbinary`, user type `vector`, `max_length` 20 = 8-byte header +
3 × float32); `sp_describe_first_result_set` reports it as **`varchar(max)`** —
the server hands clients without native vector support a JSON array
(`[1.0000000e+000,0.0000000e+000,0.0000000e+000]`); `VECTOR_DISTANCE('cosine',
emb, CAST('[1,0,0]' AS vector(3)))` works in ORDER BY. **Today the column is
unreadable through the catalog**: the type resolves to BLOB from the
`varbinary` system type and the read fails with "declared BLOB in the catalog
but SQL Server returned TDS type 0xA7" — the same class as #296's
`rowversion`, to be filed. What a spec would do: read `vector(n)` as
`FLOAT[n]` (the JSON text parsed, or the native binary form behind the TDS
`VECTORSUPPORT` feature extension — to investigate), write a `FLOAT[n]` as a
JSON literal cast to `vector(n)`, and map `array_cosine_distance` /
`array_distance` / `array_inner_product` (`array_negative_inner_product`) with
a constant array to `VECTOR_DISTANCE('cosine' | 'euclidean' | 'dot', col,
CAST(@p AS vector(n)))` — so `ORDER BY array_cosine_distance(emb, $q) LIMIT k`
pushes as an exact kNN (`TOP k … ORDER BY VECTOR_DISTANCE(…)`, no collation
question on floats). The approximate search (`VECTOR_SEARCH` table-valued
function over a DiskANN `VECTOR INDEX`) has no DuckDB query shape to be
recognised from and would be its own table function.

`SELECT DISTINCT` is the server's deduplication (§ 9.3: sets are the
server's); `DISTINCT ON` is vetoed (a ROW_NUMBER rewrite is phase B material).

### 9.6 Tests and order

An **agreement suite**: every pushed shape runs with `mssql_remote_pushdown`
on and off and the results are compared, with the string cases of § 9.3 given
explicit expectations instead; a `remote_pushdown` statement counter under
`MSSQL_COUNTERS` (spec 063's lesson: an SQL-invisible path needs a counter or
the suite goes vacuous), and `EXPLAIN` assertions on the emitted T-SQL. Two
PRs: **078-A** SELECT (writer, vetoes, `mssql_query`, setting, docs, suite),
then **078-B** DML/DDL. The 076 seek fix and the 061 correction go first as
their own small PRs.

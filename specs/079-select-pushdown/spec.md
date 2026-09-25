# Spec 079 — SELECT pushdown through DuckDB 2.0's remote-pushdown rewriter

**Status:** Draft, 2026-09-17, on `spec/065-067-revalidation` (PR #364).
**Revised 2026-09-23** from a reconnaissance against main `48c9e0d` and the pin
`62ee922db3` (§ 0.1): the rewriter is not reached by two-part names, the dry
run has no `ClientContext`, `IS_REMOTE` has side effects, and the work ships as
five sequential PRs (§ 2) instead of one.
Supersedes the read half of the 065 reconnaissance (`join-agg-pushdown.md`)
and closes specs 065/066/067 together with spec 080, which is the DML half on
the same writer. Written from the revalidation record,
`../065-dml-pushdown-recon/revalidation-2026-09-17.md` — § 5 (the
mechanism), § 8 (strings, measured), § 9 (the proposal) — which stays as
the research; nothing measured there is repeated here, it is pointed at.
Pinned DuckDB: `v2.0-cyanoptera` at `d673cf9ab4` when written; `62ee922db3`
at the revision (§ 0.1 was measured there).
**Goal:** a statement that reads one attached SQL Server catalog and uses
only constructs the writer renders runs on the server as **one T-SQL
statement** — joins, aggregates, DISTINCT, ORDER BY / TOP included — so that
rows a query does not need are never transferred. The server's answer where
the server's answer is the contract (sets), DuckDB's where DuckDB's differs
and is cheap to keep (orders).
**Not the goal (owner, 2026-09-17):** 100 % pushdown. One writer, a
table-driven vocabulary of proven-exact forms, veto by default, the scan
path as the fallback — logical and maintainable before complete.
**Depends on:** #350 (spec 077, `ChooseRowIdKey` is not needed here but the
branch order is), #361 (the 076 seek fix — the writer spells literals the
same way), #362 (the collation predicate this spec shares).

---

## 0. Measured ground (in the research record)

| what | where | the fact the design stands on |
|---|---|---|
| the rewriter | § 5, § 9.1 | `RemotePushdownOptimizer` runs in `Optimizer::OptimizeStatement` on the **parsed** statement when a remote catalog is attached; the catalog supplies `Supports`, four `SupportsPushdown` overloads and `RemoteExecute`; `RemoteExecute` cannot decline |
| its granularity | § 9.1 | pushes the whole statement, an INSERT's SELECT alone, each set-operation child, a CTAS query; **not** a subquery under a non-pushable outer node; a local view or macro blocks; a parameter (`$1`) is offered to `SupportsPushdown`; every expression node is offered; constant subtrees are folded locally first |
| string sets | § 8.4, § 8.5, § 9.3 | the server's `=` is its collation's and padded; no server setting changes that; `<>`, `NOT LIKE`, ranges are pushed natively by the shipped scan already; only `NOT IN` runs client-side, because the encoder lacks the case |
| string orders | § 8.1, #362 | a `_BIN2` on a code-page `varchar` orders code-page bytes; DuckDB's order is a binary **UTF-8-coded** collation's |
| the seek | § 8.2, #361 | a `varchar` literal keeps the seek on every collation family; `nvarchar` against a `SQL_` collation is a scan |
| aggregates, functions | § 9.2 | `COUNT` is `int`, `SUM(int)` overflows, `AVG(int)` truncates, `MAX(bit)` is invalid, `STRING_AGG` needs a literal separator and a LOB cast, `STDEV`/`VAR` match to an ulp, no positional or alias GROUP BY; today's function table and its refusals |

### 0.1 Reconnaissance, 2026-09-23 (the revision's evidence)

Measured with a throwaway catalog that answered `Supports(IS_REMOTE |
EXECUTE_QUERY_NODE)`, said yes to every `SupportsPushdown` and logged every call.

| what | measured | consequence |
|---|---|---|
| three-part names | `SELECT … FROM db.dbo.t` reaches `RemoteExecute` with the catalog stripped (`dbo.t`); joins, WHERE, ORDER BY, LIMIT arrive whole; every expression, constants and the LIMIT count included, is offered to `SupportsPushdown(expr)` | as § 9.1 said |
| **two-part names, `USE`** | `db.t` and `USE db; … FROM t` produce **no call at all**. `RemotePushdownOptimizer::LookupEntry` fills a missing schema with the hard-coded `DEFAULT_SCHEMA` (`main`), not `Catalog::GetDefaultSchema()`; SQL Server has no `main`, the lookup returns null and the table is treated as local | the most common spelling would never push. The extension answers a lookup of schema `main` with its default schema when the server has none of that name (W1, first commit), since patching DuckDB is not an option |
| **the dry run's context** | the four `SupportsPushdown` overloads take **no `ClientContext`**; but the rewriter resolves every base table first through `Catalog::GetEntry(binder.context, …)`, i.e. our `MSSQLTableSet::GetEntry(context, …)` — on the **same thread**, before `SupportsPushdown(ref)`, in autocommit and inside a transaction (checked with `threads = 4`) | D1's column resolution reads the entries `GetEntry` resolved for this rewrite, recorded thread-locally — never a fresh lookup by name, which without a context would read the shared cache and miss the transaction's own layer (#380: a table the transaction created or altered) |
| table functions | `range(3)` in the FROM is offered to `SupportsPushdown(ref)` | vetoed (D1 already says so) |
| reference implementation | none in the DuckDB tree — only the base `Catalog` stubs | nothing to crib from |
| `IS_REMOTE` side effects | `Catalog::CheckAmbiguousCatalogOrSchema` is skipped for a remote catalog (name resolution of `db.x`), and `DatabaseManager::GetRemoteCatalogCount() > 0` runs the rewriter on **every** statement in the process | D6 revised: `IS_REMOTE` follows the setting, read once at ATTACH |

Premises that moved since the draft: `NOT IN` reaches the server since #367
(AC-4 and D5's "NOT IN added" are done); #361 is merged (#368); #362 is still
open and `OrdersLikeDuckDB` does not exist — and `MSSQLOptimizer` has **no**
collation check at all, so with `mssql_order_pushdown` on, a string key under a
linguistic collation came back in the server's order with DuckDB's sort removed
(fixed in PR A); the identifier quoter had eight copies and six unescaped
sites, not three; `collation_name` from the describe was read by nothing.

## 1. Design

### D1 — one vocabulary: `SupportsPushdown` is the writer's dry run

`RemoteExecute` cannot decline, so every veto must already have happened in
`SupportsPushdown`. Two lists — one of vetoes, one of renderings — drift (the
070 lesson: `pushdown_expression` was made to dry-run the same encoder as
`pushdown_complex_filter` for exactly this reason). So there is **one**:
`SupportsPushdown(x)` renders `x` with the writer into a scratch buffer and
answers whether it succeeded. A construct is supported iff the writer has an
exact form for it; adding a construct is adding a rendering, nothing else.

The four overloads:

- `SupportsPushdown(const ParsedExpression &)` — the expression writer
  (D2); a parameter, an unknown function, a cast to an unmapped type, a
  subquery shape T-SQL lacks → false.
- `SupportsPushdown(const TableRef &)` — base table (must resolve in the
  metadata cache: table or view), subquery, join (types in D2), table
  function → false (no T-SQL form for `mssql_scan` inside a rewrite),
  `TABLESAMPLE`, positional/ASOF/lateral joins → false.
- `SupportsPushdown(const QueryNode &)` — the node writer over a
  `SelectNode` or `SetOperationNode` with its CTE map; this is where the
  string-order rule (D4) and the aggregate rules (D2) are applied, because
  only here are the column references resolvable through the FROM.
- `SupportsPushdown(const SQLStatement &)` — false in this spec
  (`EXECUTE_STATEMENT` is not claimed; DDL is spec 080's question).

Resolution: column references are resolved against the metadata cache
(spec 076 already has every column's type, length and collation, and the
rewriter has verified the table exists) without a connection and without
binding; a reference the writer cannot resolve — an alias through a subquery
it did not walk, a `COLUMNS(...)` expansion — is a veto, never a guess.

### D2 — the T-SQL writer

Input: the parsed tree with the catalog name already stripped by the
rewriter; output: the statement text and its parameter list. Every constant
becomes an `sp_executesql` parameter `@pN`, declared **from the column** when
the constant is compared with one (the 076 rule, with #361's fix: `varchar`
when the constant is representable in both the column's and the database's
code page, `nvarchar` otherwise) and from the value otherwise
(`DeclarationForValue`); `mssql_scan_parameterize_filters = false` renders
literals instead, for plan comparison. Identifiers are bracket-quoted; the
schema is the one the catalog resolved (the default schema is the catalog's,
#322 when it lands).

The vocabulary, from § 9.2 — each row is a rendering the agreement suite
(W5) exercises:

| construct | T-SQL | rule |
|---|---|---|
| FROM base table, remote view | `[schema].[name]`; every column in the SELECT list is rendered through **`MSSQLColumnInfo::BuildReadExpression`** — the read expression the catalog scan and INSERT's OUTPUT list already share: `.STAsBinary()` for geometry / geography, `CAST(… AS NVARCHAR(MAX))` for a cast-required type (hierarchyid, sql_variant, CLR UDT), `CAST(… AS NVARCHAR(n))` for a CHAR / VARCHAR / TEXT column under a non-UTF-8 collation, `NVARCHAR(MAX)` / `VARBINARY(MAX)` for ntext / image; a `*` is expanded from the catalog's column list so each column gets its expression. Without it a non-UTF-8 column (the installation default) arrives as code-page bytes and a geometry column fails the describe at bind. In WHERE / ON / GROUP BY the bare `[c]` is rendered — comparison semantics are the column's, as the scan's filters do today | one catalog; a view pushes, the server expands it |
| projections, WHERE, HAVING, ON | the expression writer | column refs, constants, `+ - *` (overflow errors on both sides), `/` → `CAST(a AS float) / NULLIF(b, 0)` — DuckDB gives `inf` on a zero divisor (`ieee_floating_point_ops`, measured on the pin), SQL Server's float has no infinity and a bare `/` raises 8134, so `NULLIF` is the least-wrong form and a **recorded divergence** (NULL where DuckDB says `inf`) with its own W5 exemption, `//` and `%` on integers, CASE, CAST to a mapped type (no TRY_CAST), COALESCE / NULLIF, BETWEEN, IN list, IS [NOT] NULL, AND / OR / NOT with parentheses (three-valued logic is the same), comparisons, LIKE with `[`, `%`, `_` escaped and an ESCAPE clause |
| scalar functions | one exact form per name | starts from the scan's `function_mapping.hpp` (`lower`, `upper`, `trim`, `ltrim`, `rtrim`, `year` … `second`, `+ - * %`) keyed on **parsed** names — LIKE arrives as `~~` / `!~~` (ILIKE `~~*` vetoed), unary minus as `-` with one argument (its own entry beside the binary one); `negate`, `prefix` / `suffix` / `contains` are bound-tree names DuckDB's optimizer rules hand the scan's encoder and never reach the writer — grows one proven form at a time; vetoes as duckdb-mysql's and 061 § 4.5: `length` (`LEN` drops trailing spaces and counts UTF-16 units), `upper` / `lower` on non-ASCII (simple case mapping only — accepted as-is for the ASCII range: measured pushed today), `sqrt` / `ln` / `log` (server errors where DuckDB gives NaN), `power` / `exp` (overflow errors), `week` / `dayofweek` numbering |
| GROUP BY + aggregates | positional and alias references **expanded** (T-SQL has neither: errors 164 / 207); `COUNT_BIG(*)` / `COUNT_BIG(x)` / `COUNT_BIG(DISTINCT x)`; `SUM(CAST(int AS bigint))`; `AVG(CAST(x AS float))`; `MIN` / `MAX` (`bit` → `CAST(b AS tinyint)`; strings under D4); `STRING_AGG(CAST(x AS nvarchar(max)), <literal>) WITHIN GROUP (ORDER BY …)`; `STDEV` / `STDEVP` / `VAR` / `VARP`; `agg(x) FILTER (WHERE c)` → `agg(CASE WHEN c THEN x END)`, `COUNT(*) FILTER (WHERE c)` → `COUNT_BIG(CASE WHEN c THEN 1 END)` | veto `first` / `any_value`, `arg_*`, `median` / `quantile` / `mode`, `approx_count_distinct` (a different estimator), `list` / `array_agg` / `histogram`, `bit_*`, `corr` / `covar_*` / `regr_*`, a non-literal `string_agg` separator, GROUP BY ALL / ROLLUP / CUBE / GROUPING SETS |
| JOIN | INNER / LEFT / RIGHT / FULL / CROSS with ON; USING expanded to ON | the ON condition is any expression the writer renders — non-equi, OR, functions, subqueries — nothing join-specific; veto NATURAL, SEMI / ANTI, ASOF, POSITIONAL, LATERAL |
| ORDER BY | `ORDER BY`; NULL placement: SQL Server has no NULLS FIRST / LAST and sorts NULL lowest (ASC → first, DESC → last) while DuckDB's default is NULLS LAST for both, and `MSSQLOptimizer` **refuses** the mismatch today (`IsNullOrderCompatible`: NOT NULL column, or the requested placement already the server's — a bare `ORDER BY nullable_col` never pushes). The writer **emulates** it — `CASE WHEN x IS NULL THEN 1 ELSE 0 END, x` (0 / 1 by the requested placement) when the column is nullable and the placements differ — new work (W2), shared back into `MSSQLOptimizer` (W4) so both paths push the same shapes. In `MSSQLOptimizer` (PR A) only under a LIMIT: the leading CASE key defeats an index, so a plain ORDER BY is cheaper sorted by DuckDB; and a TOP N is not pushed while a non-optional filter stays client-side (it would be applied after the server's cut) | string keys under D4 only; an ORDER BY **inside** a subquery, derived table or CTE renders only with a LIMIT / OFFSET (`TOP` / `OFFSET-FETCH`) — without one it is vetoed: SQL Server rejects it (1033) and DuckDB promises no order there either. The dry run checks renderability per node; this positional rule is the node writer's, which knows its nesting |
| LIMIT / OFFSET | `TOP n`; `OFFSET … FETCH` (`ORDER BY (SELECT NULL)` when there is no ORDER BY — both sides are arbitrary then) | |
| DISTINCT, set operations | `DISTINCT`; UNION / UNION ALL / EXCEPT / INTERSECT | veto DISTINCT ON, `EXCEPT ALL` / `INTERSECT ALL`, UNION BY NAME |
| CTEs, subqueries | `WITH`; scalar / EXISTS / IN / quantified (`= ANY`) subqueries, correlated included | veto recursive CTEs (spec 080 material: UNION ALL only, no RECURSIVE keyword) |
| window functions | ROW_NUMBER / RANK / DENSE_RANK / NTILE / LAG / LEAD / FIRST_VALUE / LAST_VALUE / aggregates OVER, PARTITION BY, ORDER BY, ROWS frames and the default RANGE frame | veto RANGE with offsets, GROUPS, EXCLUDE, QUALIFY |
| `SELECT * EXCLUDE / REPLACE` | expanded from the catalog's column list | veto `COLUMNS(...)`, PIVOT / UNPIVOT, TABLESAMPLE, parameters |

Types: the result's types are the **server's** for the pushed shape
(`SUM` → bigint where DuckDB says HUGEINT, `AVG` → float, `COUNT` → bigint),
read from `sp_describe_first_result_set` as every `mssql_scan` is. Documented
under "what changes when a query is pushed", with the settings that turn it
off.

### D3 — execution: `mssql_scan_params` is the vehicle

`RemoteExecute(context, QueryNode)` returns a `TableFunctionRef` to
**`mssql_scan_params(catalog, sql, {params}, declarations)`** — the function
spec 075 W5 already ships: shape from `sp_describe_first_result_set` at
bind, execution at InitGlobal, one server plan per statement text through
`sp_executesql`, and inside a transaction the rows materialised at init
under `MaterializeMutex` on the pinned connection. No new execution
machinery; `EXPLAIN` shows the call with the T-SQL as its argument, which is
the review lever. The one describe round trip per planned statement is the
fixed cost (W3 measures it; a cache keyed on the statement text inside the
catalog is the lever if it matters).

The ref is **lazy**: `RemoteExecute` builds the function ref and touches the
server only when the plan runs, so `EXPLAIN` and `PREPARE` of a pushed
statement execute nothing (the rewriter descends into both).

**Native types from the describe.** The vehicle's bind reports
`MSSQL_VARCHAR(n)` / `MSSQL_NVARCHAR(n)` with the collation from the
describe's `system_type_name` and `collation_name` when
`mssql_catalog_native_types` is on — the describe returns both, today only
the type name is read (`DescribedTypeToTdsMetadata`) — so a pushed
`SELECT *` has the catalog path's column types. This is load-bearing for
CTAS: the rewriter pushes a CTAS's **query** whenever it cannot push the
CREATE (`RewriteCreateInfo`), 079 claims no `EXECUTE_STATEMENT`, and no hook
stops it — so `CREATE TABLE ms.dst AS SELECT * FROM ms.src` reads `ms.src`
through the vehicle, and without native types it would create `nvarchar(max)`
where the catalog path creates `varchar(50)` with the source collation. W3
closes it in the same PR; the interim window is zero.

**Column resolution and the cache.** The dry run resolves columns against
the metadata cache and **loads a table's metadata on first touch** as any
catalog access does (spec 076: one round trip per fresh table, on the pinned
connection inside a transaction); a miss is not a veto — vetoing would
silently disable pushdown for the first statement against every table.

### D4 — strings (the § 9.3 rule; asked in PR #364, the reviewer was in the original `<>` discussion)

- **Sets are the server's.** `=`, `<>`, `IN`, `NOT IN`, `LIKE`, `NOT LIKE`,
  ranges, GROUP BY, DISTINCT, join keys, set-operation deduplication — under
  the column's collation (a `_CI` collation merges case variants) and with
  padding (`ab` = `ab␣`), no client re-check, the same in the scan path, in
  this writer and in spec 080's DML. Native on both sides is the only rule
  under which `=` and `<>` partition a table (3 + 2 = 5 on the § 9.3 probe;
  a strict `<>` gives 3 + 4), and the installation default is
  `SQL_Latin1_General_CP1_CI_AS`, so a veto on case-insensitive keys would
  leave the rewriter dead on most databases. One documentation paragraph,
  with § 8.5's `char(n)` note (values arrive padded; a local comparison with
  a shorter constant misses what the pushed one finds).
- **Orders are DuckDB's.** ORDER BY, `MIN` / `MAX` on strings, window ORDER
  BY push only when the key's collation is **binary AND UTF-8-coded** (#362:
  `varchar` under `_BIN2_UTF8`); otherwise the node is vetoed and the query
  runs as today. `nvarchar` under `_BIN2` is vetoed too (PR A review): its
  UTF-16 order puts a character above the BMP before U+E000–U+FFFF, DuckDB
  after — a wrong answer, not a documentable nuance. The predicate is an
  allow-list (numeric, `bit`, date/time but not `datetime2(7)`, whose values
  past 2262 DuckDB reads as NULL; binary / `json` / uniqueidentifier out), a
  string-valued function of a key is never pushed, nor a date part of a
  `datetimeoffset`. As TEXT even a `_BIN2_UTF8` column pads with spaces (`ab`
  = `ab `, `ab`+TAB before `ab`, measured), so a UTF-8 `varchar` is ordered
  by its BYTES, `CAST(col AS varbinary(n))` — DuckDB's order, measured,
  trailing NUL the one tie — and, that key not being sargable, only under a
  LIMIT (owner, PR A review); a bounded `varchar` only (`char(n)` is read
  trimmed, so its stored bytes are not DuckDB's values). The same rule
  applies to filters: no string function is pushed (`UPPER('ß')` is `ß` on
  the server, `ẞ` in DuckDB — a lost row), nor a date part of a
  `datetimeoffset`. No `COLLATE` forcing (owner, § 8.4).
- Literals spelled per column (§ 8.3.1, #361).
- No strict forms, no re-check: § 8.5's `DATALENGTH` pair stays in the
  record as the measured form should strictness ever be asked for.

### D5 — the fallback stays, the vocabulary is shared

The scan's filter pushdown (`FilterEncoder`, `pushdown_complex_filter`) and
`MSSQLOptimizer`'s ORDER BY / TOP pushdown are **kept**: they are the only
mechanisms that reach a mixed-catalog plan, a subquery under a non-pushable
node, or a local view — the rewriter cannot (§ 9.1). They walk bound trees,
the writer walks parsed ones, so they cannot share a renderer; what they
share is the vocabulary: **one** function table (`function_mapping.hpp`
consulted by both, keyed on parsed names with the scan's bound-name aliases
beside them, `NOT IN` added so it stops being the scan's one strict
exception), **one** collation predicate (`OrdersLikeDuckDB`, #362, consulted
by `MSSQLOptimizer` and D4), **one** literal/parameter spelling (#361),
**one** column read expression (`MSSQLColumnInfo::BuildReadExpression`,
already shared by the scan and INSERT's OUTPUT list — D2's base-table row),
**one** NULL-order emulation (D2's ORDER BY row, back-ported into
`MSSQLOptimizer`), and **one** identifier quoter — three exist today
(`mssql_value_serializer.cpp`, `mssql_ddl_translator.cpp`,
`filter_encoder.cpp`, all doubling `]`); W4 makes them one and the writer
quotes every identifier through it, aliases included (`SELECT 1 AS "x]y"`). A query cannot answer differently depending on which path took it.

### D6 — setting and switches

**Revised 2026-09-23.** `mssql_remote_pushdown` (BOOLEAN) is read **once per
catalog, at ATTACH**, and fixes that catalog's answer to **both** `IS_REMOTE`
and `EXECUTE_QUERY_NODE` for its life: the counter `DatabaseManager` keeps from
`IS_REMOTE` stays consistent because the answer never changes under it, and a
catalog attached with the setting off has none of `IS_REMOTE`'s side effects
(§ 0.1). Default **false** until PR E, which flips it. The text below is the
draft's, kept for the reasoning about scopes:

`mssql_remote_pushdown` (BOOLEAN, default **true at merge** — W1–W6 ship as
one PR): **instance-wide, not per session**. `Supports(RemoteCapability)
const` takes no `ClientContext`, and a SESSION-scoped extension option is
invisible to `DBConfig::TryGetCurrentSetting` (the database-level store this
repo reads its settings from, `mssql_table_entry.cpp`), so the option is
registered `SetScope::GLOBAL` and read there; there is no per-session switch
and the docs say so. It gates **`EXECUTE_QUERY_NODE` only — never
`IS_REMOTE`**: `DatabaseManager` counts remote catalogs from the live answer
of `Supports(IS_REMOTE)` at ATTACH and DETACH (`remote_catalog_count`, a
checked integer), so a toggle between the two would leave the counter high
forever (the rewriter running on every statement in the process) or throw
out of DETACH; `IS_REMOTE` is a constant `true`. The rewriter consults
`EXECUTE_QUERY_NODE` at every per-catalog decision, so gating it alone loses
nothing. DuckDB's own `SET disabled_optimizers = 'remote_pushdown'` is the
other instance-wide switch and needs nothing from us. `MSSQL_COUNTERS` gains a `remote_pushdown` statement counter
(spec 063's lesson: an SQL-invisible path needs a counter or its suite goes
vacuous).

## 2. Work

### W1 — the writer, expressions and a single table

`src/pushdown/mssql_sql_writer.{hpp,cpp}` (namespace `duckdb::mssql`,
`SQLWriter`): expression rendering per the D2 table, column resolution
against the metadata cache, parameter collection; `SupportsPushdown` on
`MSSQLCatalog` as the dry run (D1); `Supports(IS_REMOTE | EXECUTE_QUERY_NODE)`
gated by D6. Base-table `SelectNode` with projections, WHERE, ORDER BY,
LIMIT / OFFSET — the shapes the scan path pushes today (filters; ORDER BY on
a NOT NULL key or a placement the server already has; LIMIT), so W5 compares
the two paths on the same statements from the first commit; the nullable
ORDER BY shapes have no old-path counterpart and get expected-row assertions
instead.

### W2 — joins, aggregates, the rest of the node writer

JOIN forms, GROUP BY / HAVING with the aggregate table, DISTINCT, set
operations, CTEs, subqueries, window functions, `EXCLUDE` / `REPLACE`
expansion, positional / alias expansion, NULL-order emulation.

### W3 — the vehicle and the levers

`RemoteExecute` → a lazy `mssql_scan_params` ref (D3); native types from
the describe (`system_type_name` + `collation_name` → `MSSQL_VARCHAR(n)` with
collation under `mssql_catalog_native_types`), which also fixes the CTAS
window D3 names; `EXPLAIN` output; the counter; the describe cost measured
on the wide fixture (one statement planned N times) and the cache decided
from the number.

### W4 — shared vocabulary

`function_mapping.hpp` as the one table (the scan's encoder and the writer
both consult it; `NOT IN` added), `OrdersLikeDuckDB` shared with
`MSSQLOptimizer`, the #361 spelling shared with `DeclarationForColumn`, the
NULL-order emulation shared with `MSSQLOptimizer` (which then pushes the
nullable ORDER BY it refuses today), one identifier quoter replacing the
three.

### W5 — the agreement suite

`test/sql/pushdown/`: every D2 row as a statement run **twice** —
`mssql_remote_pushdown = true` and `false` — with the results compared,
except the D4 string cases, which get explicit expectations for both paths;
`EXPLAIN` assertions on the emitted T-SQL (the join, the aggregate, the
`TOP`); counter assertions that the rewriter took the statement; the
vetoes, one statement each, asserting the counter did NOT move and the
result is right; transactions (a pushed join inside BEGIN … COMMIT on the
pinned connection, interleaved with a sink); C++ unit tests for the
expression writer without a server (`test/cpp/test_sql_writer.cpp`).

Fixtures the comparison must include, or "run twice and compare" passes
while a divergence ships: a `varchar` column under a non-UTF-8 collation
holding non-ASCII data (the encoding, not the order), a `geometry` column,
an `ntext` column, a `sql_variant` / `hierarchyid` column (the cast-required
read expression), a nullable ORDER BY key, a same-catalog CTAS asserting the
target's length and collation. Exemptions, each with its expected rows on
both paths: the D4 string cases and division by zero.

### W6 — docs

README (one paragraph + the setting), `website/docs/…/pushdown.md` (the D2
table in user terms, the D4 paragraph, "types of a pushed query are the
server's"), DATAMODEL (the rewriter as a layer between planner and scan,
the shared-vocabulary invariant), CLAUDE.md (setting row, key concept),
CHANGELOG.

**Revised 2026-09-23: five sequential PRs**, each merged before the next is
opened (never stacked, never two open at once), because the whole is 7–8
thousand lines and the size is the risk § 4 names. The setting stays off until
the last, so every intermediate main is safe:

| PR | contents | stands on its own because |
|---|---|---|
| **A** | this revision; the shared vocabulary without the rewriter — `MSSQLColumnInfo::OrdersLikeDuckDB` (#362) in `MSSQLOptimizer`, the NULL-order emulation there, one identifier quoter (`mssql::QuoteIdentifier`), native types and collations from the describe for `mssql_scan` / `mssql_scan_params` | fixes a wrong-order bug under `mssql_order_pushdown`, pushes nullable ORDER BY keys, fixes names with `]` in COPY / INSERT BULK / DELETE, and gives `CREATE TABLE … AS SELECT * FROM mssql_scan(…)` the source's types |
| **B** | W1's skeleton and W3's vehicle: `Supports` / `RemoteExecute`, the `main` schema answer, the thread-local resolution, the single-table writer (projections through `BuildReadExpression`, simple WHERE, ORDER BY, LIMIT), the counter, `EXPLAIN`, the agreement-suite harness | the mechanism end to end on the simplest shapes, behind the setting |
| **C** | the expression vocabulary: the function table, CASE / CAST / COALESCE / LIKE / IN / BETWEEN, division, parameters declared from the column | real WHERE clauses and projections |
| **D** | JOIN, GROUP BY / HAVING, the aggregate table, DISTINCT | acceptance 1 |
| **E** | set operations, CTEs, subqueries, window functions, EXCLUDE / REPLACE; the describe-cost and per-statement-rewrite measurements; the setting on by default; W6's docs | the feature |

## 3. Not proposed

- Partial pushdown of a subquery under a non-pushable node — upstream
  granularity, and "no DuckDB patches" (owner); the scan path covers it.
- `COLLATE` forcing for non-qualifying ORDER BY keys — owner's decision
  (§ 8.4); § 8.5 records that it would dominate no-pushdown if ever wanted.
- A DuckDB-side re-check behind the rewriter — § 9.3: helps only where the
  server returned a superset, needs the predicate's columns in the output
  and nothing aggregated above them, and would make the semantics depend
  on the query's shape.
- Parameters (`$1`) in pushed statements, recursive CTEs, DISTINCT ON,
  QUALIFY, LATERAL / CROSS APPLY, SEMI / ANTI → EXISTS — each a later row
  in the D2 table when asked for, none a design change.
- `vector` search (#363, its own spec after v0.3.0).
- A per-session switch — not implementable through `Supports` (D6); the
  instance-wide setting is what there is.

## 4. Risks

- **Size.** W1 + W2 is the bulk of the code this spec adds; the D1 rule
  (one vocabulary, veto by default) is what keeps it reviewable, and W5's
  suite is what keeps it honest. If the PR grows past review, the split is
  by D2 rows, not by mechanism.
- **Result types change for a pushed statement** (D2, documented); a user
  who materialises `SUM(int)` into a table gets BIGINT instead of HUGEINT.
  The setting restores the old types.
- **Server errors where DuckDB gives values** — `SUM` overflow past bigint,
  `sqrt(-1)` and friends are vetoed; what is pushed errors instead of
  returning a value, never silently.
- **Local views block the rewriter** — a common way to wrap remote tables;
  documented, and the scan path still pushes filters through the view.
- **Materialisation in transactions** — a pushed statement's whole result is
  materialised at init inside a transaction (spec 075's rule), as any
  `mssql_scan` result is; a pushed join that returns millions of rows
  inside a transaction costs that memory, as it does today.
- **The describe round trip** — measured in W3 before the cache is decided.

## 5. Acceptance

1. A join of two remote tables with a GROUP BY runs as one T-SQL statement:
   `EXPLAIN` shows it, the counter moves, the server's `sys.dm_exec_requests`
   shows one request, and the rows transferred are the aggregate's, not the
   tables' (wide fixture, measured before/after).
2. The agreement suite is green with the setting on and off; the D4 string
   cases pass with their explicit expectations on both paths.
3. Every existing suite is unchanged with the setting off, and green with
   it on (the shapes the rewriter takes return the same rows — the string
   cases excepted and listed).
4. ~~`NOT IN` on a string column reaches the server through the scan path~~
   — done by #367 before the revision.
5. Docs name what a pushed query changes: types, string sets, the switch.

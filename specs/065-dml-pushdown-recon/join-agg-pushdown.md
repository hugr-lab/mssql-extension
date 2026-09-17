# JOIN / aggregate pushdown — reconnaissance

**Date:** 2026-08-05. Three parallel investigations (plan-rewrite surface,
semantic divergence catalog, ship-vs-pull heuristics with federated prior
art) plus live-server probes run during synthesis. Companion to
`research.md` (the DML rework recon) — the staging seam, the semantic
contract directive (§7 there) and the spec ordering all carry over.

---

## 0. Step zero, valuable on its own: the cardinality hole

**Every MSSQL scan is estimated at 1 row today.** `mssql_catalog_scan` sets
neither `cardinality` nor `statistics` callbacks (`table_scan.cpp:992-1027`),
so `LogicalGet::EstimateCardinality` falls through to `return 1` — verified
empirically: a 5M-row table EXPLAINs as `~1 row`. DuckDB's join-order
optimizer and build/probe-side choices around remote tables are therefore
essentially arbitrary — TODAY, without any pushdown in the picture. The
number already exists client-side (`MSSQLTableEntry::GetStorageInfo` → DMV
row count, TTL-cached, spec-049 `approx_row_count_` fallback); nothing feeds
it to the planner. A `function.cardinality` callback is a small standalone
fix that improves every join plan immediately and is the prerequisite for
every heuristic below. **Do it first, possibly before the pushdown phase
entirely.**

## 1. Semantic contract (directive from research.md §7, applied)

**Native server semantics by default; strictness is an opt-in annotation.**
Divergences split into two classes that must not be conflated:

- **Class 1 — comparison semantics (collation, trailing-space padding).**
  What the server's own users see; pushed WHERE has worked this way since
  spec 026 (`collation_filter.test` asserts it). GROUP BY / JOIN / DISTINCT
  on bare string keys push by default — index seeks intact, which is the
  point. The strict opt-in (`col::MSSQL_VARCHAR_STRICT`-style annotation in
  the spirit of spec 060 types) buys byte-exact DuckDB semantics via the
  verified rewrites: `GROUP BY x, DATALENGTH(x)` / varbinary-cast DISTINCT /
  `DATALENGTH` join conjunct — at the cost of server-side seek strategies.
  Verified live: padding applies EVEN under `_BIN2` (`'a' = 'a '` TRUE,
  GROUP BY merges; LIKE does not pad), and the varbinary form restores
  byte-exact grouping/joining (3 groups for {'a','a ','A'}; 0-row join for
  'a'×'a ').
- **Class 2 — engine mechanics (result types, overflow errors, truncation,
  server-setting dependence).** These are wrong NUMBERS for a DuckDB query,
  not "server semantics anyone expects" — wrappers are MANDATORY regardless
  of the directive:

| construct | mandatory form | verified |
|---|---|---|
| `count(*)`/`count(x)` | `COUNT_BIG(...)` (T-SQL COUNT is int) | doc |
| `sum(int family)` | `SUM(CAST(x AS decimal(38,0)))` + client cast → HUGEINT; residual gap (10^38..1.7e38) physically unreachable | live: naked SUM(int) → error 8115 |
| `avg(int/bigint/decimal)` | **decompose**: push `SUM(...)` + `COUNT_BIG(...)`, divide in DuckDB — bit-exact, sidesteps every T-SQL result-type rule at once | live: AVG(int) over {1,2} = **1** |
| `sum(decimal)` | native — identical type, values, overflow | src+doc |
| `min/max(bool)` | `MIN/MAX(CAST(b AS tinyint))` (T-SQL rejects bit) | doc |
| `bool_and/or`, `count_if`, `FILTER` clause | MIN/MAX(CAST), SUM(CASE), AGG(CASE WHEN p THEN x END) — all exact | doc |
| NEVER push | zero-reachable divisors (DuckDB NULL vs server error), float `%` (server rejects), `avg(timestamp/date)`, xml keys, mark joins, `median/quantile/mode/arg_min/product/bit_agg/list`, `string_agg(DISTINCT)` | catalog |

Semi/anti joins → `EXISTS`/`NOT EXISTS`: safe unconditionally.
`stddev/var` family: name-map, float-noise only. Full 23-row verdict table
with per-row rationale is in the agent report (session transcript);
remaining live probes are enumerated there (P2, P3, P5, P7-P16 — the
critical P1/P4/P8/P16-class cells were settled live during synthesis).

**Pre-existing bugs this recon exposed in TODAY'S pushed filters** (file as
an issue; gate these mappings out of pushed group keys/join conditions until
fixed): `length`→`LEN` (LEN ignores trailing spaces and counts UTF-16 units
— `length(x)=n` already returns wrong rows), `dayofweek`/`week` →
`DATEPART` (depends on the server's `@@DATEFIRST`; DuckDB is fixed
Sunday=0/ISO), divide-by-zero (DuckDB NULL, server hard error).

## 2. The rewrite surface

- **Architecture: post-optimizer subtree substitution** extending the
  spec-039 `OptimizerExtension` (runs after filter pushdown — WHERE texts
  already exist; after join-order; TopN already formed and composable on
  top). The 039 trick (mutate bind data, splice binding-neutral nodes)
  does NOT extend to joins/aggregates — they own table indexes. The viable
  mechanism, per the submodule: build a new `LogicalGet`
  (`binder.GenerateTableIndex()`) over an internal **`mssql_query_scan`**
  table function (same executor/stream; + serialize/deserialize, +
  cardinality callback, + the 039 order/top fields so those patterns
  compose), rewire ancestors with `ColumnBindingReplacer`, and interpose a
  cast `LogicalProjection` where T-SQL cannot produce the bound type —
  mandatory for `sum(int)`→HUGEINT (TDS has no int128 wire).
- Existing `mssql_scan` is almost right but executes the query at bind time
  and has no serialize — hence the dedicated internal function.
- Bail-outs: grouping sets/`GROUPING()`, aggregate `filter`/`order_bys`
  (initially), DELIM/ASOF/MARK joins, rowid projected through the join,
  `LogicalAnyJoin` conditions beyond encoder coverage.
- Traps verified: debug-build `Verify` runs immediately after the extension
  (binding mistakes throw before tests run — develop in debug); generated
  SQL must reuse `BuildColumnExpression` (collation/LOB casts) and the
  scans' already-textualized filters; `JoinFilterPushdownInfo` /
  `dynamic_filters` pointing at replaced Gets needs an audit.

## 3. The ladder (ship-vs-pull), and the safety line

**Reduction vs relocation is the semantic boundary.** Semi-join REDUCTION
(remote side only gets filtered; DuckDB still executes the join) is correct
under EVERY collation: coarser server equality only widens the returned
superset, and DuckDB re-applies the exact predicate. Full RELOCATION (server
executes the join) inherits server comparison semantics — which under the
§1 directive is the accepted default, with strict as opt-in. Prior art
(Teiid dependent joins, SAP HANA SDA two-tier, Denodo data movement, Trino
dynamic filtering) validates the shape; **no DuckDB remote-DB extension has
any of this** — first-mover territory.

| tier | mechanism | safety | notes |
|---|---|---|---|
| 0 | encode DuckDB's join-derived dynamic filters (min/max, IN) — today explicitly DROPPED at `filter_encoder.cpp:256-262` | always | days of work; Trino's "domain compaction" degenerate form |
| 1 | keys ≤ ~1000: `IN (...)` / `JOIN (VALUES ...)` into the remote WHERE | always (reduction) | literal machinery exists |
| 2a | BCP DISTINCT keys → `#temp`; remote scan gains `WHERE key IN (SELECT ... FROM #temp)` | always (reduction) | no literal cap |
| 2b | BCP whole small side → `#temp`; server executes JOIN [+GROUP BY]; stream only the result | native semantics (default); strict via opt-in rewrites | the prize: index seeks driven by #temp — server may never scan the big table |
| 3 | pull remote (today) | — | fallback |

**Break-even** collapses to value counts (`c_bcp/c_read ≈ 1.3`):
relocate when `R_s·C_s + R_out·C_out ≪ R_r·C_r`, safety factor ~10.
**The decisive insight: the go/no-go for tier 2 happens AFTER the small side
is materialized** (BCP needs full materialization anyway, like a hash-join
build) — exact counts, not estimates. The universal prior-art failure mode
(mis-estimation ships a huge side) is closed by construction; a runtime cap
(`Abandon()` mid-ship + error naming the setting) is the backstop.

**Settings surface:** `mssql_join_pushdown = off | reduction | full`
(reduction default once tested; full opt-in initially),
`mssql_join_inlist_max_keys` (1000), `mssql_join_ship_max_rows` (100k),
`mssql_join_ship_max_bytes` (64 MB), `mssql_join_min_reduction` (10).

**Mechanics:** one held connection for CREATE `#temp` → BCP → streaming
SELECT → DROP; the result stream already holds its connection with
`reset_on_release` captured (`mssql_result_stream.hpp:54-57,164-178`); load
policy needs no new rule; the single-writer-on-given-connection seam is the
SAME gap specs 062/066 need — this is its fourth consumer. Transactions:
the sequence is serial (fill DONE before SELECT), so legal on the pinned
connection; v1 disables tiers 2a/2b inside explicit transactions until #239
materialization lands (a small side containing a same-catalog scan is
exactly that problem). Permissions: `#temp` needs no grants — works for the
read-only persona of issue #189.

## 4. Sequencing proposal (extends research.md §4's table, rows 7+)

| step | content | ships value |
|---|---|---|
| 7.0 | **cardinality callback** (§0) | better join plans immediately; could even ride an earlier spec |
| 7.1 | spec 061 re-examined under the native-semantics directive (default may flip to on; padding caveat added) + one coherent knob (`mssql_pushdown_semantics = native | strict`) | ORDER BY pushdown |
| 7.2 | tiers 0+1 (dynamic filters, IN-list reduction) | always-safe reduction |
| 7.3 | tier 2a (temp-key reduction) — first consumer of the staging seam on the read path | unbounded reduction |
| 7.4 | aggregate pushdown over single scans (COUNT/SUM/MIN/MAX/GROUP BY with §1 wrappers; AVG decomposition) | the classic analytics win |
| 7.5 | tier 2b relocation + join/agg over joined subtrees (`mssql_query_scan` rewrite machinery) | full federation |

Each rung independently shippable; Trino is the precedent that stopping at
reduction is defensible. 7.4 and 7.5 order can swap depending on demand —
7.4 has no shipping machinery at all (pure rewrite over one scan).

## 5. Open questions carried to the specs

1. Exact build-side key capture for tiers 1/2 (DuckDB dynamic filters
   deliver min/max, not key sets — needs our own capture point).
2. Outer-join scope for v1 (inner+semi first; ON/WHERE placement fidelity
   rules recorded in the catalog agent's report).
3. Group-count estimation for relocated aggregates (conservative default).
4. EXPLAIN visibility of the decision + per-query override surface (prior
   art unanimously provides one; DuckDB has no hint syntax — settings or a
   scan option).
5. The strict-annotation surface: new marker type vs collation-parameter on
   existing MSSQL_VARCHAR — decide alongside 061's knob.

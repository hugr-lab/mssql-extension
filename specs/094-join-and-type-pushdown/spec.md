# Spec 094: pushdown for joins and the new types

Status: draft, 2026-11-24; DQ-1 and DQ-2 decided by the owner the same day. From #515 (the umbrella): its runtime join filters (level 0),
the vector pushdown (#363) and the spatial items, with the owner's direction of 2026-11-20 ("research
what can be pushed, and filter hints such as a bbox to help spatial joins"). Measured: `measured/X1-X3.md`
(SQL Server 2025, a throwaway container) and `research/duckdb-side.md` (DuckDB at the pin, `4569c0a1eeb`).

## Why

- A join of a DuckDB / Parquet / DuckLake table with an SQL Server table goes through the catalog scan,
  which drops the keys DuckDB hands it: with 3 or 40 build keys the scan streams all 150k rows; only a
  one-key join reaches the server (#515, measured 2026-10-23).
- `ORDER BY array_cosine_distance(v, $q) LIMIT k` reads the whole table into DuckDB; SQL Server 2025
  answers the same kNN from `TOP (k) … ORDER BY VECTOR_DISTANCE(…)`.
- A spatial filter or join reads the whole table: only `IS NULL` of a spatial column is pushed.

## Scope

In: P0 level-0 join filters; P1 vector distances (ORDER BY … LIMIT, projections, conditions); P2 spatial
predicates with a constant (`geometry`); P3 a bbox hint for spatial joins; P4 spatial functions in the
079 writer where the server's answer is DuckDB's.

Out: `VECTOR_SEARCH` (a DiskANN index: no DuckDB query shape; an `mssql_vector_search` table function,
its own task if wanted); more than 50 join keys (`mssql_keyed_scan` via `#keys`, its own spec after P0
is measured); geography predicates (X3: geodesic on the server, planar in DuckDB); any DuckDB patch.

## Decisions

### D1 (P0): DuckDB's join filters rendered, never a narrower answer

What a hash join pushes into the probe scan at the pin (`JoinFilterPushdownInfo::FinalizeFilters`,
research Q1): min == max as a plain comparison; an IN list (1 < build rows ≤ `dynamic_or_filter_threshold`,
50) wrapped `__internal_tablefilter_optional`; a prefix-range filter (small-span keys: then nothing else
for the key); min/max (only with no IN and no prefix range) wrapped `optional` (equality joins) or
`selectivity_optional`; a bloom filter, deferred, `selectivity_optional`. The build side always finishes
before the probe Get's `init_global`, where the catalog scan sends its SQL (the pipeline schedule; the
scan keeps `INITIALIZE_ON_EXECUTE`).

- FilterEncoder unwraps the two optional wrappers (`ExpressionFilter::GetOptionalFilterChild`, already
  used in `mssql_entry_name_filter.cpp`) in its BOUND_FUNCTION arm and renders the child through the
  existing encoder: the per-type rules (datetime ticks, the truncated reads, parameters typed from the
  column) apply as to a written filter.
- An IN list on any type the encoder takes: it can only widen the result, and the join re-checks every
  match. A `>=` / `<=` bound only where `OrdersLikeDuckDB()` holds.
- Anything the encoder does not take (bloom, prefix range, a string range, an IN of an unencodable
  type) is dropped, never armed client-side (DuckDB evaluates the wrapper as all-true).
- **Open, to test first (research Q1):** the unwrapped min == max comparison a range join (NLJ, IEJoin,
  PWMJ) pushes -- `s < 'b'` from a one-row build -- is rendered today like a user's filter. On a string
  under a non-binary collation the server's `<` is not DuckDB's: a row the join matches can be dropped.
  If measured, a filter from a join (`input.op` tells static from dynamic) on such a key keeps only `=`.

### D2 (P1): vector distances by VECTOR_DISTANCE (SQL Server 2025)

Measured (X1, 2000 `vector(3)` values exact in float32):

| DuckDB | SQL Server | values |
|---|---|---|
| `array_distance(v, q)` | `VECTOR_DISTANCE('euclidean', v, q)` | bit-equal, 2000 / 2000 |
| `array_negative_inner_product(v, q)` | `VECTOR_DISTANCE('dot', v, q)` | bit-equal, 2000 / 2000 |
| `array_cosine_distance(v, q)` | `VECTOR_DISTANCE('cosine', v, q)` | 1665 bit-equal, the rest within 2.4e-7 (1-2 float32 ulps); the 2000 rows' order the same |

The server returns `float` (a float32 widened); DuckDB `FLOAT`. A zero vector: cosine NaN in DuckDB
(ordered last), 1.0 on the server; NULL first on the server, last in DuckDB. `VECTOR_NORM(v, 'norm2')`
tells a zero vector on the server.

- Pushed: a distance of a `vector(n)` column and a constant `FLOAT[n]` (`CAST(@q AS vector(n))`, a
  parameter), or of two columns of one table, as a result column, an ORDER BY key (with `TOP`) and a
  condition. `array_inner_product` as `-VECTOR_DISTANCE('dot', …)`.
- NULL and NaN placement emulated: `ORDER BY CASE WHEN v IS NULL THEN 1 ELSE 0 END, CASE WHEN
  VECTOR_NORM(v, 'norm2') = 0 THEN 1 ELSE 0 END, VECTOR_DISTANCE(…)` for cosine (the NaN ones after the
  others, before NULL: DuckDB's order); a constant zero vector not pushed.
- Where: the catalog-scan TOP N path (`MSSQLOptimizer::TryPushTopN`: a two-argument key over the column
  and a constant; it is behind `mssql_order_pushdown`, opt-in) and the 079 writer's vocabulary (the
  default path).
- **DQ-1 (owner, 2026-11-24): cosine is pushed everywhere**, though it differs in its last bits -- under
  the floating-point aggregates' rule (recorded, vetoed in a condition / ORDER BY) it would never be
  pushed in a kNN, the case it is for. The difference is recorded (`docs/type-mapping.md`): a kNN's
  membership can change only where two distances are within 2 ulps at its boundary; euclidean and dot
  are exact.
- **DQ-2 (owner, 2026-11-24): with ORDER BY … LIMIT k pushed, DuckDB's TopN is removed for euclidean /
  dot** (bit-equal: the server's order, D2's emulation) **and kept for cosine**, over `TOP (k)`: DuckDB
  orders the k rows the server chose in its own floats.

### D3 (P2): spatial predicates with a constant, a superset DuckDB re-checks (geometry only)

Measured (X2, 100k points and other shapes, a GEOMETRY_AUTO_GRID index):

- The spatial index is used by `g.STIntersects(@c) = 1` (either operand order), `STWithin`,
  `STDistance(@c) <= d`, `g.Filter(@c) = 1` (an index-only superset of STIntersects) and
  `g.STIntersects(@c1.STUnion(@c2)) = 1`; not by an OR of two, nor by `g.STEnvelope().STIntersects(@c)`
  (DuckDB's `&&`, bbox meets bbox), nor under a `CASE`.
- An invalid geometry (a bow tie) fails every exact method -- error 6522 / 24144, the statement ended --
  when it is a candidate; `STIsValid()` does not fail. A row the index drops is never evaluated, so an
  unguarded pushed predicate fails or not by where the invalid rows lie.
- `&&` is exactly `g.STEnvelope().STIntersects(@c)` (2004 / 2004 rows); `Filter` is a superset of
  `STIntersects`, not of `&&` (the index holds a geometry's cells, not its envelope).
- A curve: the server sees it, DuckDB reads NULL (`geometry_as 'geometry'`).

So:
- A pushed spatial predicate is a superset; DuckDB keeps its own filter (the complex filter pushdown
  erases only what it encodes). Never consumed: a curve, an invalid geometry and the index's coarseness
  are the server's.
- With the spatial extension's `ST_Intersects` / `ST_Within` / `ST_Contains` / `ST_DWithin` and a
  constant: `CASE WHEN g.STIsValid() = 1 THEN g.STIntersects(@c) ELSE 1 END = 1` where the column has no
  spatial index (a full scan on the server, less on the wire); where it has one (the catalog reads
  `sys.spatial_indexes`), `g.Filter(@c) = 1` with the index forced (`WITH (INDEX(…))`) if X4 shows a
  forced index never evaluates `Filter` exactly -- else the guarded form.
- `&&` / `ST_Intersects_Extent` (core) with a constant: the guarded envelope form, no index.
- The encoder sees through a CRS-only cast (a geography's `CAST(g AS GEOMETRY)`: research Q5) -- but
  geography is not pushed (X3).

### D4 (P3): a bbox hint for a spatial join

A join on `&&` / `ST_Intersects` binds as a LogicalAnyJoin, planned as a blockwise nested loop, and
DuckDB pushes nothing into the probe scan (research Q2). The extension adds the hint itself, as spec
080's staged MERGE replaces `LogicalMergeInto` in `PreOptimizeRoot` (research Q3, option B):

- MSSQLOptimizer recognises `X ⋈ Get(mssql catalog table)` on a spatial predicate of a `geometry` column
  and puts an extension operator on X's side: a pass-through sink that computes X's extent (or up to N
  boxes, one STUnion: X2 shows one geometry keeps the index) and, at Finalize, pushes into the probe
  Get's `DynamicTableFilterSet` an optional-wrapped `st_intersects_extent(column, envelope)`;
  `LogicalGet::dynamic_filters` is copied to the physical scan (`plan_get.cpp`), the schedule orders the
  build before the probe's init.
- The scan renders it by D3 (a superset; the join re-checks).
- Not taken: rewriting the join into range joins on extent columns (option A: one box only, needs
  virtual extent columns and functions core lacks) and an in-out table function staging the build in
  `#stage` (option C: the right shape for many boxes or exact matches -- later, as tier 1 of the keys).

### D5 (P4): spatial functions in the 079 writer

Mapped only where the server's answer is DuckDB's (X5 to measure each, planar `geometry` only):
`ST_X` / `ST_Y` (`.STX` / `.STY`), `ST_Area`, `ST_Length`, `ST_NumPoints`, `ST_IsEmpty`, `ST_GeometryType`
(names differ: mapped), `ST_SRID`; each guarded against an invalid geometry as D3. What never maps is
listed in `docs/type-mapping.md`.

## Experiments still to run

| # | for | what |
|---|---|---|
| X4 | D3 | `WITH (INDEX(six))` and `Filter`: is a candidate ever evaluated exactly (an invalid one fails)? plans and logical reads, indexed vs guarded, 100k / 1M rows |
| X5 | D5 | each mapped function against DuckDB's (spatial extension) on X2's shapes |
| X6 | D1 | the range-join min == max string filter against a server: rows lost or not |
| X7 | D4 | DuckDB at the pin: a `LogicalExtensionOperator` pass-through sink before a blockwise NL join; the dynamic filter reaching the catalog scan's `init_global` |
| X8 | D2 | kNN on 100k / 1M `vector(1536)`: wire, server CPU, wall, against today's full read |

## PRs (each from main)

| PR | content |
|---|---|
| P0 | D1: FilterEncoder unwraps the optional join filters; IN on any type, ranges where `OrdersLikeDuckDB`; X6 and its fix; tests with a local table ⋈ a server table (1, 3, 40, 100 keys) |
| P1 | D2: VECTOR_DISTANCE in the 079 vocabulary and the TOP N path; NULL / NaN emulation; X8 |
| P2 | D3: spatial predicates with a constant; `sys.spatial_indexes` in the catalog; X4 |
| P3 | D4: the bbox hint operator; X7 |
| P4 | D5: the writer's spatial functions; X5 |

The spatial extension is needed for P2's `ST_Intersects` forms and P4's tests: installed in CI (a
download the owner approves), skipped by `require spatial` locally where it is absent.

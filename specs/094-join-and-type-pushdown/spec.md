# Spec 094: pushdown for joins and the new types

Status: draft, 2026-11-24; DQ-1 and DQ-2 decided by the owner the same day for cosine, both revisited
after X1b (review of #622: euclidean and dot are not exact either; for the owner to confirm), DQ-3 open. From #515 (the umbrella): its runtime join filters (level 0),
the vector pushdown (#363) and the spatial items, with the owner's direction of 2026-11-20 ("research
what can be pushed, and filter hints such as a bbox to help spatial joins"). Measured: `measured/X1-X3.md`
and `measured/X1b-X3b.md` (SQL Server 2025, a throwaway container) and `research/duckdb-side.md` (DuckDB at
the pin, `4569c0a1eeb`).

The rule every decision below keeps:
- **A filter pushed into a scan is DuckDB's answer or a superset of it**, and DuckDB re-checks it. A
  narrower one loses rows silently, because the rows it drops never reach the re-check.
- **A value or a condition in a statement pushed whole** (079's writer) has no re-check, so it must be
  DuckDB's.

The one exception is DQ-1: a vector distance as a result column and as a kNN's ORDER BY key differs from
DuckDB's in its last bits, recorded.

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
is measured); the server's geodesic geography methods (X3b: not DuckDB's answer; geography is pushed in
DuckDB's planar form, D3); any DuckDB patch.

## Decisions

### D1 (P0): DuckDB's join filters rendered, never a narrower answer

What a hash join pushes into the probe scan at the pin (`JoinFilterPushdownInfo::FinalizeFilters`,
research Q1, `physical_hash_join.cpp`): min == max as a plain comparison; an IN list (an equality, 1 < build
rows ≤ `dynamic_or_filter_threshold`, 50, and the values neither holding a NULL nor a dense integer range:
`PushInFilter`, `FilterCombiner::IsDenseRange`) wrapped `__internal_tablefilter_optional`; a prefix-range
filter when the key's span is small (then no min/max for the key); min/max (only with no IN and no prefix
range) wrapped `optional` (equality joins) or `selectivity_optional`; a bloom filter, deferred,
`selectivity_optional`. The build side always finishes
before the probe Get's `init_global`, where the catalog scan sends its SQL (the pipeline schedule; the
scan keeps `INITIALIZE_ON_EXECUTE`).

- FilterEncoder unwraps the two optional wrappers (`ExpressionFilter::GetOptionalFilterChild`, already
  used in `mssql_entry_name_filter.cpp`) in its BOUND_FUNCTION arm and renders the child through the
  existing encoder: the per-type rules (datetime ticks, the truncated reads, parameters typed from the
  column) apply as to a written filter.
- An IN list on any type the encoder takes: it can only widen the result, and the join re-checks every
  match. A `>=` / `<=` bound only where `OrdersLikeDuckDB()` holds.
- Anything the encoder does not take (bloom, prefix range, a string range, an IN of an unencodable
  type) is dropped, never armed client-side. Dropping is correct for both wrappers: DuckDB evaluates
  `optional` as all-true, and `selectivity_optional` it may skip itself.
- **Dense keys.** A build of consecutive integers (3 or 40 ids, a `range(n)`), or one holding a NULL (a
  RIGHT join keeps them), gets no IN list; when the span is small it gets a prefix-range filter and no
  min/max, so nothing the encoder takes, and the probe scan streams the whole table. `PrefixRangeFilter`
  has no accessor for its bounds, but `LookupRange(lo, hi)` answers `FILTER_ALWAYS_FALSE` when no key can
  lie in `[lo, hi]`: a binary search over the key type's range (about 64 calls a bound) gives a lower and
  an upper bound, a `>=` / `<=` superset, rendered where `OrdersLikeDuckDB()` holds. Taken once X6 shows
  `LookupRange` never answers false for a range holding a key; else the gap is documented.
- An IN list stays literal (spec 076: IN lists are not parameters), so each key set compiles a plan of
  its own.
- **Open, to test first (research Q1):** the unwrapped min == max comparison a range join (NLJ, IEJoin,
  PWMJ) pushes -- `s < 'b'` from a one-row build -- is rendered today like a user's filter. On a string
  under a non-binary collation the server's `<` is not DuckDB's: a row the join matches can be dropped.
  If measured, a filter from a join (`input.op` tells static from dynamic) on such a key keeps only `=`.

### D2 (P1): vector distances by VECTOR_DISTANCE (SQL Server 2025)

**Measured** on random float32 vectors (X1b: 2000 of 384 and of 1536 dimensions, three runs). X1's vectors
were multiples of 1/64, which made every sum exact and so hid the summation order.

| DuckDB | SQL Server | values |
|---|---|---|
| `array_distance(v, q)` | `VECTOR_DISTANCE('euclidean', v, q)` | 12-26% bit-equal; max relative error 1.1e-6 (5-9 float32 ulps) |
| `array_negative_inner_product(v, q)` | `VECTOR_DISTANCE('dot', v, q)` | 3-9% bit-equal; max absolute error 1.1e-5 (384) and 5.7e-5 (1536), about 15-30 ulps at the typical magnitudes, with no relative bound near 0 |
| `array_cosine_distance(v, q)` | `VECTOR_DISTANCE('cosine', v, q)` | 79-82% bit-equal (values near 1.0, where an ulp is coarse); max absolute error 1.2e-7 |

**Order.** The rows ordered by a distance disagree only between distances that close: 0-32 rank
disagreements of 2000, the most for euclidean at 1536 dimensions. The top-10 and top-100 sets were the same in
the two runs that measured them.

**Types and edge cases.**
- The server returns `float`, a float32 widened in every row (X1b); DuckDB returns `FLOAT`. The 079
  vehicle's `column_types` reads it as `FLOAT`, which is exact.
- Cosine distance lies in [0, 2] (2 for opposite vectors). A zero vector: NaN in DuckDB (the largest
  value in its order, `NaN > 2` is true) and 1.0 on the server.
- NULL sorts first on the server and last in DuckDB, ascending or descending.
- `VECTOR_NORM(v, 'norm2')` tells a zero vector on the server.

- **What is pushed.** A distance of a `vector(n)` column and a constant `FLOAT[n]` (`CAST(@q AS vector(n))`,
  a parameter), or of two columns of one table: as a result column, as an ORDER BY key (with `TOP`), and
  as a condition in the catalog scan only (below). `array_inner_product` is pushed as
  `-VECTOR_DISTANCE('dot', …)`.
- **NULL and NaN placement, emulated.**
  - ASC: `ORDER BY CASE WHEN v IS NULL THEN 1 ELSE 0 END, CASE WHEN VECTOR_NORM(v, 'norm2') = 0 THEN 1
    ELSE 0 END, VECTOR_DISTANCE(…)` for cosine: the NaN ones after the others, before NULL, which is
    DuckDB's order.
  - DESC: the NaN ones first and NULL still last.
  - An explicit NULLS FIRST / LAST moves the NULL term; each direction keeps NaN as the largest value.
  - A constant zero vector is not pushed. For two columns the zero test is on either norm.
- **A zero vector outside ORDER BY (cosine).**
  - A condition `>` / `>=` adds `OR VECTOR_NORM(v, 'norm2') = 0`: DuckDB keeps the row for any threshold,
    the server's 1.0 drops it for one of 1 or more. `<` / `<=` need nothing: the server keeps a row DuckDB
    drops, and DuckDB re-checks.
  - A result column in the 079 writer: T-SQL `float` holds no NaN, so the distance is sent as `CASE WHEN
    <zero> THEN NULL ELSE VECTOR_DISTANCE(…) END` with a hidden bit column for `<zero>`, and a projection
    over the vehicle restores `'nan'::FLOAT` (as the HUGEINT `sum` is cast back over it).
  - Measured by X1c before P1.
- **A condition (`array_distance(v, q) < r`) is a superset in the catalog scan, never consumed.** The
  threshold is widened by an a-priori float32 rounding bound, doubled for the two implementations, with
  γ_k = k·2^-24 / (1 - k·2^-24) and n the dimension:
  - euclidean: relative 2γ_(n+2);
  - dot: absolute 2γ_(n+1)·‖v‖·‖q‖, where ‖v‖ is `VECTOR_NORM(v, 'norm2')` on the server (Cauchy-Schwarz
    bounds the sum of |products|);
  - cosine: absolute 4γ_(n+2).

  At 1536 dimensions these are 1.8e-4 relative, 1.8e-4·‖v‖·‖q‖ and 3.7e-4: loose against X1b's errors,
  and sound. In a statement pushed whole a distance condition is vetoed: there is no re-check.
- **Where.**
  - The catalog-scan TOP N path (`MSSQLOptimizer::TryPushTopN`: a two-argument key over the column and a
    constant). It is behind `mssql_order_pushdown`, which is opt-in.
  - The catalog scan's filters.
  - The 079 writer's vocabulary (the default path), for result columns and ORDER BY keys.
- **DQ-1 (owner, 2026-11-24, for cosine; extended to euclidean and dot after X1b, for the owner to
  confirm): a distance is pushed as a result column and a kNN's ORDER BY key**, though it differs in its
  last bits. Under the floating-point aggregates' rule (recorded, vetoed in a condition / ORDER BY) a
  distance would never be pushed in a kNN, the case it is for.
  - The difference is recorded in `docs/type-mapping.md`.
  - A kNN's membership can change only where two distances at its boundary are within the errors above.
- **DQ-2, revisited after X1b (owner's call; was: TopN removed for euclidean / dot).** With ORDER BY …
  LIMIT k pushed, DuckDB's TopN is kept for all three, over `TOP (k)`: DuckDB orders the k rows the
  server chose by its own floats. The server's order is not DuckDB's (X1b: up to 32 rank disagreements
  in 2000).
  - That is the catalog scan's TOP N path. In the 079 writer (a statement pushed whole) there is no
    TopN: the rows come in the server's order with the server's distances, so the result is ordered by
    the values it returns (owner, 2026-11-25). The writer's veto of an approximate ORDER BY key is lifted
    for a distance by DQ-1.

### D3 (P2): spatial predicates with a constant, a superset DuckDB re-checks

**Measured** (X2: 100k points and other shapes, a GEOMETRY_AUTO_GRID index; X2b, X3b).

*The spatial index:*
- Used by `g.STIntersects(@c) = 1` (either operand order), `STWithin`, `STDistance(@c) <= d`,
  `g.Filter(@c) = 1` (an index-only superset of the server's STIntersects) and
  `g.STIntersects(@c1.STUnion(@c2)) = 1`.
- Not used by an OR with anything, nor by `g.STEnvelope().STIntersects(@c)`, nor under a `CASE`.

*An invalid geometry (a bow tie):*
- It fails every exact method, `STEnvelope` included -- error 6522 / 24144, the statement ended -- when it
  is a candidate. `STIsValid()` does not fail.
- A row the index drops is never evaluated.

*`&&` (bbox meets bbox):*
- It is `g.STEnvelope().STIntersects(@c.STEnvelope())`: 441 / 441 against DuckDB on a diagonal line's box
  (X2b).
- Against the constant itself the form is narrower: 21 of 441 rows (the first draft's form, wrong).
- `Filter` is a superset of the server's `STIntersects`, not of `&&`: the index holds a geometry's cells,
  not its envelope.

*SRIDs (X2b):*
- A method over two SRIDs returns NULL, so a constant of SRID 0 drops every row of SRID 4326: 0 of their
  43 matches.
- DuckDB reads no SRID (spec 093 DQ-45), so its answer is SRID-blind.
- A constant rebuilt in each row's SRID (`geometry::STGeomFromWKB(@wkb, g.STSrid)`) is exact (4048 /
  4048), but it uses no index, and neither does an OR with `g.STSrid <> @s`.

*Not measured:* the server's exact methods (`STIntersects`, `STWithin`, `STContains`) on boundary cases
against DuckDB's spatial extension. X3 saw the server round a boundary point.

So every spatial predicate with a constant has **one form, the envelopes'**. It is a superset of every
predicate that implies the two shapes meet, and it does not depend on the server's exact methods or on
the constant being valid:

```
CASE WHEN G.STIsValid() = 1
     THEN G.STEnvelope().STIntersects(geometry::STGeomFromWKB(@env, G.STSrid)) ELSE 1 END = 1
```

- **`G`** is the row's geometry: `g` for a `geometry` column, and for a `geography` one
  `geometry::STGeomFromWKB(g.STAsBinary(), g.STSrid)` (below). The guard tests `G`, the value the
  methods run on.
- **`@env`** is the constant's extent, computed on the client (`Geometry::GetExtent`) and sent as the WKB
  of a 2D POLYGON. It is widened on each side by m = 1e-9·max(1, the extent's largest |coordinate|), so:
  - a rounding at the rectangle's edge never drops a row;
  - a point's or an axis-parallel line's extent is never degenerate (an invalid zero-area polygon);
  - the constant's own validity, Z and M do not matter.

  It is rebuilt in each row's SRID: SRID-blind, as DuckDB is.
- **The predicates:**
  - `&&` / `ST_Intersects_Extent` (core), and from the spatial extension `ST_Intersects`, `ST_Within`,
    `ST_Contains`, `ST_Covers`, `ST_CoveredBy`, `ST_Touches`, `ST_Overlaps`, `ST_Crosses` and `ST_Equals`,
    in either argument order: each implies the envelopes meet.
  - `ST_DWithin(g, c, d)`: the same form with `@env` grown by `d` (+ m). The distance between two shapes is
    at least the distance between their envelopes, which is at least the L∞ gap.
  - Never `ST_Disjoint` or a negation: their superset is everything.
- **Never consumed.** DuckDB keeps its own filter (the complex filter pushdown erases only what it
  encodes): the envelope test, an invalid geometry let through and a curve the server sees are the
  server's.
- **A scan filter only.** In a statement pushed whole (079) a spatial condition is vetoed: the form is a
  superset, and there is no re-check.
- **The index: DQ-3 (open, the owner's call).** The envelope form uses no index (X2). An indexed form
  would be the server's exact predicate or `Filter` against the constant itself, in the constant's SRID,
  `UNION ALL` the envelope form for `g.STSrid <> @s`. It is safe only once X4 / X5 show the indexed
  predicate is a superset of DuckDB's on boundary cases, and that a forced index never evaluates `Filter`
  exactly (an invalid candidate fails unguarded). Options:
  - (a) P2 ships the envelope form alone: less on the wire, the server's work a full scan;
  - (b) the indexed branch, added for `ST_Intersects` / `ST_Within` / `ST_Contains` / `ST_DWithin` after
    X4 / X5. It never applies to `&&`: `Filter` against an envelope is narrower for a non-point row, e.g.
    the line (0,0)-(10,10) against the box [8,20] x [0,2], whose boxes meet.
- **geography (owner, 2026-11-24): DuckDB's answer.** DuckDB reads a geography as `GEOMETRY('OGC:CRS84')`
  and compares it in the plane, so `G` converts the value to a planar geometry, longitude / latitude as
  DuckDB's (X3b: the server's planar form answers ids 1, 3, 5 as DuckDB, where its geodesic
  `STIntersects` answers 2, 3). A geography's spatial index is geodesic, so none is used. The encoder sees
  through the CRS-only cast DuckDB plans (research Q5).

### D4 (P3): a bbox hint for a spatial join

A join on `&&` / `ST_Intersects` binds as a LogicalAnyJoin, planned as a blockwise nested loop, and
DuckDB pushes nothing into the probe scan (research Q2). The extension adds the hint itself in
MSSQLOptimizer's post-optimize pass, which runs after DuckDB's join-order optimizer has fixed each join's
sides (research Q3, option B). Not in `PreOptimizeRoot`, where spec 080's staged MERGE goes: the sides are
not fixed there.

- MSSQLOptimizer recognises `X ⋈ Get(mssql catalog table)` on a spatial predicate of a `geometry` or
  `geography` column and puts an extension operator on X's side. It is a pass-through sink that computes
  X's extent and, at Finalize, pushes into the probe Get's `DynamicTableFilterSet` an optional-wrapped
  `st_intersects_extent(column, envelope)`.
  - `LogicalGet::dynamic_filters` is copied to the physical scan (`plan_get.cpp`), and the schedule
    orders the build before the probe's init.
  - The scan renders it by D3's envelope form (a superset; the join re-checks), so one box. Up to N boxes
    would travel as one `ExpressionFilter`, an OR of N `st_intersects_extent` calls, rendered as an OR of
    envelope tests (X7 to measure; no index either way).
- **The predicates** are the ones whose matches imply the boxes meet (D3's list). For `ST_DWithin` X's
  extent is grown by `d`. No `ST_Disjoint` and no negation.
- **The join types** are as DuckDB's own join filters allow: the Get is the probe child of an INNER,
  SEMI or RIGHT join. Not LEFT, OUTER, ANTI, MARK or SINGLE, where a probe row the hint removes is one
  the join must return (as unmatched, or for NOT EXISTS).
  - The check is made on the plan as DuckDB's join-order optimizer leaves it, which may flip a join's
    sides.
  - A Get on the build side gets no hint: its scan initialises before X's extent is known.
  - A Get on the non-preserved side of a LEFT or ANTI join could be hinted too: later.
- **Between the join and the Get** only operators that commute with a filter: projections and filters.
  No LIMIT / TOP_N, aggregate or window.
- Not taken:
  - rewriting the join into range joins on extent columns (option A): one box only, and it needs virtual
    extent columns and functions core lacks;
  - an in-out table function staging the build in `#stage` (option C): the right shape for many boxes or
    exact matches, later, as tier 1 of the keys.

### D5 (P4): spatial functions in the 079 writer

A function's value in a statement pushed whole has no re-check, and D3's superset does not carry over.
So a function maps only where X5 shows the server's value equals DuckDB's on every one of X2's shapes:
- **Invalid geometries.** The mapped form is guarded on `G.STIsValid()`, and a guard that answers NULL
  where DuckDB answers a value is a different column: such a function does not map.
- **Curves and FullGlobe**, which DuckDB reads as NULL under `geometry_as 'geometry'`. The mapped form
  NULLs them (`CASE WHEN G.STGeometryType() IN ('CircularString', 'CompoundCurve', 'CurvePolygon',
  'FullGlobe') THEN NULL ELSE … END`), and maps only under that read.
- **Planar only.** A geography goes through `G` as in D3, and its geodesic methods never map.

The candidates are `ST_X` / `ST_Y` (`.STX` / `.STY`), `ST_Area`, `ST_Length`, `ST_NumPoints`,
`ST_IsEmpty` and `ST_GeometryType` (whose names differ: mapped). `ST_SRID` never maps: DuckDB reads no
SRID. What does not map is listed in `docs/type-mapping.md`.

## Experiments still to run

| # | for | what |
|---|---|---|
| X4 | D3 | the envelope form against DuckDB's spatial extension on boundary cases (points on a box's edges and corners, on a diagonal edge, touching shapes) and its cost (plans, logical reads, 100k / 1M rows); for DQ-3 (b): the exact methods and `Filter` on the same cases, whether `WITH (INDEX(six))` ever evaluates `Filter` exactly, whether the `UNION ALL`'s first branch keeps the index, the parameterised `@wkb` form's plan |
| X5 | D5 | each mapped function against DuckDB's (spatial extension) on X2's shapes, invalid ones and curves included |
| X1c | D2 | cosine with zero-vector rows (the column, the constant, two columns) under `<`, `>`, ASC, DESC and NULLS FIRST, against DuckDB |
| X6 | D1 | the range-join min == max string filter against a server: rows lost or not; the dense-key build's prefix-range filter, its bounds by `LookupRange`, never false for a range holding a key |
| X7 | D4 | DuckDB at the pin: a `LogicalExtensionOperator` pass-through sink before a blockwise NL join; the dynamic filter reaching the catalog scan's `init_global`; a LEFT and an ANTI spatial join left without a hint, their answers equal to the unhinted plan's |
| X8 | D2 | kNN on 100k / 1M `vector(1536)`: wire, server CPU, wall, against today's full read |

## PRs (each from main; P3 after P2, whose encoder arm for `st_intersects_extent` it renders through)

| PR | content |
|---|---|
| P0 | D1: FilterEncoder unwraps the optional join filters; IN on any type, ranges where `OrdersLikeDuckDB`; the dense keys' bounds; X6 and its fix; tests with a local table ⋈ a server table (sparse keys: 1, 3, 40, 100; and consecutive ids, the dense case) |
| P1 | D2: VECTOR_DISTANCE in the 079 vocabulary and the TOP N path; NULL / NaN emulation, the zero vector in conditions and result columns; X1c, X8 |
| P2 | D3: spatial predicates with a constant, SRID-blind, geography in its planar form; `sys.spatial_indexes` in the catalog; X4, DQ-3 |
| P3 | D4: the bbox hint operator; X7, with a LEFT and an ANTI spatial join left unhinted |
| P4 | D5: the writer's spatial functions; X5 |

The spatial extension is needed for P2's `ST_Intersects` forms and P4's tests: installed in CI (a
download the owner approves), skipped by `require spatial` locally where it is absent.

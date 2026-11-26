# Spec 094: pushdown for joins and the new types

Status: draft, 2026-11-24, revised 2026-11-26 on the third review of #622 (D0, the superset mechanism; float32
only; the TOP N path's parameters; the EMPTY / degenerate spatial constants; off switches). DQ-1 and DQ-2
were decided by the owner on 2026-11-24 for cosine. Both were revisited after X1b (euclidean and dot are
not exact either) and still await the owner's confirmation for those two. DQ-2's 079 path (no TopN: the
server's order and values) is the owner's of 2026-11-25. DQ-3 is open. From #515 (the umbrella): its runtime join filters (level 0),
the vector pushdown (#363) and the spatial items, with the owner's direction of 2026-11-20 ("research
what can be pushed, and filter hints such as a bbox to help spatial joins"). Measured: `measured/X1-X3.md`
and `measured/X1b-X3b.md` (SQL Server 2025, a throwaway container) and `research/duckdb-side.md` (DuckDB at
the pin, `4569c0a1eeb`).

The rule every decision below keeps:
- **A filter pushed into a scan is DuckDB's answer or a superset of it**, and DuckDB re-checks it. A
  narrower one loses rows silently, because the rows it drops never reach the re-check. A superset is
  its own kind of encoded filter (D0), never one taken as exact.
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

In:
- P0: level-0 join filters;
- P1: vector distances of float32 columns (ORDER BY … LIMIT, projections, conditions);
- P2: spatial predicates with a constant (`geometry`, and `geography` in its planar form);
- P3: a bbox hint for spatial joins;
- P4: spatial functions in the 079 writer, where the server's answer is DuckDB's.

Out: `VECTOR_SEARCH` (a DiskANN index: no DuckDB query shape; an `mssql_vector_search` table function,
its own task if wanted); more than 50 join keys (`mssql_keyed_scan` via `#keys`, its own spec after P0
is measured); the server's geodesic geography methods (X3b: not DuckDB's answer; geography is pushed in
DuckDB's planar form, D3); any DuckDB patch.

## Decisions

### D0 (P1, P2, P3): a superset is its own kind of encoded filter (review of #622, N1)

Today encoding a filter is erasing it. `ComplexFilterPushdown` (`table_scan_pushdown.cpp`) moves every
expression `FilterEncoder` renders into `encoded_conditions` and removes it from the plan. Other callers
also take an encoded filter as exact:
- `MSSQLPushdownExpression`: DuckDB then makes the predicate a TableFilter and never checks it again;
- the staged MERGE's `ON` / `WHEN` conditions (`FilterEncoder::EncodeSearchCondition`);
- `BuildWhereClause` at init, for the TableFilters, the dynamic ones a join pushes included.

So the "superset, re-checked by DuckDB" rule needs a mechanism of its own. It is relied on by D2's
distance conditions, D3's spatial predicates, D4's hint and D1's `LookupRange` bounds.

- **A third encode result, `superset`,** next to exact and refused. Only the D2 / D3 arms and D1's dense
  bounds return it.
- **How it combines** (`FilterEncoder`'s arms; today a partial AND already returns its rendered part
  with `supported = false`, and every caller drops it):
  - an AND of exact and superset children is a superset;
  - an OR with a superset child is a superset (an OR with a refused child stays refused);
  - NOT, a CASE, or a comparison of a boolean over a superset is refused, because NOT of a superset is
    narrower and loses rows;
  - the partial-AND shape stays as it is: refused for the expression paths, its conjuncts taken one by
    one where the caller splits them.
- **Where a superset is rendered:**
  - **In `ComplexFilterPushdown`, the expression stays in the plan,** so DuckDB applies it again.
    DuckDB calls `ComplexFilterPushdown` more than once per scan, each time with the filters left.
    Since #623 the extension appends each pass to `complex_filter_where_clause` instead of replacing it.
    Supersets therefore go in a bind-data slot of their own (`superset_where_clause` and its parameters),
    de-duplicated per expression, so a second pass does not add them again. The slot is serialised with
    the rest of the bind data, as `complex_filter_where_clause` is (load-bearing, `table_scan.cpp`).
    - The cardinality callback's `DEFAULT_SELECTIVITY` is not applied to that slot, because DuckDB
      applies its own selectivity to the kept filter.
  - **In `BuildWhereClause`** (the TableFilter path), only under an `optional` /
    `selectivity_optional` root: a join's filter (D1, D4), which the join re-checks. Anywhere else a
    superset there is unhandled and armed in the client-side net.
- **Every caller that takes a result as exact treats a superset as refused:**
  - `MSSQLPushdownExpression` answers no, so a superset never becomes a mandatory TableFilter;
  - the staged MERGE refuses the condition by name.
- **Consequence:** a superset kept by `ComplexFilterPushdown` stays in the plan as a filter above the Get.
  - That already blocks TOP N: `FindMSSQLScan` matches only `[Projection ->] Get`.
  - It already fails the keyless-DML check (`KeylessPlanRefusal`: "selected on the client").
  - `ScanHasClientSideFilters` reads only `table_filters`, where no mandatory superset can be, so neither
    check needs a change.
  - So `WHERE <distance or spatial predicate> … ORDER BY … LIMIT k` never pushes TOP. A kNN with a
    distance condition is pushed as a filter only.
- **Tests (P1 and P2 each):**
  - the predicate as a MERGE condition (refused) and in a keyless DELETE (refused);
  - a materialised CTE read twice, whose scan's pushdown runs twice (the superset is sent once);
  - EXPLAIN shows the kept filter above the scan;
  - NOT over the predicate, and an OR with an unencodable child: not pushed.
- **It lands with whichever of P1 / P2 goes first;** the other builds on it.

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
- **A non-equality filter from a join is rendered only where `OrdersLikeDuckDB()` holds,** wrapped or
  not (review of #622). `FilterEncoder` has no ordering gate of its own: it renders `<` / `<=` / `>` /
  `>=` for any column that is not `comparison_differs`. `OrdersLikeDuckDB()` excludes more than strings
  under a non-binary collation: uniqueidentifier, binary / varbinary and datetime2(7) as well.
  - The unwrapped min == max filter a range join pushes is such a filter.
  - The static case, a written `WHERE`, is #639: uniqueidentifier ranges and binary comparisons returned
    other rows than DuckDB, and DML on a keyless table touched extra rows. Fixed in #642 (open), which
    runs a uniqueidentifier's ranges in DuckDB and sends a binary's comparisons with the lengths
    deciding a tie.
  - X6 is the regression test of the join case. If it shows rows lost today, that is shipped code and
    gets its own issue ahead of P0.
- Anything the encoder does not take (bloom, prefix range, a string range, an IN of an unencodable
  type) is dropped, never armed client-side. Dropping is correct for both wrappers: DuckDB evaluates
  `optional` as all-true, and `selectivity_optional` it may skip itself.
- **The drop works per conjunct.** `GetFinalTableFilters` puts a column's static filter and the join's
  dynamic one into one ExpressionFilter, `AND(static, optional(…))` (`physical_table_scan.cpp`,
  `table_filter_set.cpp`).
  - The encoder does not unwrap `optional` today. So that AND comes back partial (`supported = false`),
    and `Encode` arms the whole AND client-side.
  - That is likely a shipped loss of pushdown, read from the code and not run: a written
    `WHERE t.id > 100` on a join key stops reaching the server once the join pushes min/max. X6 measures
    it, and a confirmed loss gets its own issue.
  - In P0 an AND whose only refused children are optional is fully supported: its required children are
    rendered and its optional ones dropped. A test covers a static filter and a join filter on one column.
- **Dense keys.** A build of consecutive integers (3 or 40 ids, a `range(n)`), or one holding a NULL (a
  RIGHT join keeps them), gets no IN list.
  - **When the prefix-range filter is chosen.** `CanUsePrefixRangeFilter` (`physical_hash_join.cpp`)
    needs three things: a small span, exactly one equality condition, and a build-to-probe ratio of at
    most 1.0, taken from the probe's estimated cardinality. Then there is no min/max, so nothing the
    encoder takes, and the probe scan streams the whole table.
  - **Otherwise** -- a view, an unknown count, `mssql_enable_statistics = false` (no estimate), or a
    composite key -- the dense keys fall through to the optional min/max, which the encoder takes.
    P0's tests pin which branch each case exercises, with one test per branch and a two-key case.
  - **The bounds.** `PrefixRangeFilter` has no accessor for them, but `LookupRange(lo, hi)` answers
    `FILTER_ALWAYS_FALSE` when no key can lie in `[lo, hi]`. A binary search over the key type's range
    (about 64 calls a bound) gives a lower and an upper bound, a `>=` / `<=` superset, rendered where
    `OrdersLikeDuckDB()` holds.
  - **The key type.** `LookupRange` reads its values as the filter's key type, which is the build-side
    condition's type. After an integral cast that type can be wider than the column. So the probes are
    built in that key type, and the bounds are clamped to the column's range before they are rendered.
  - Taken once X6 shows `LookupRange` never answers false for a range holding a key; else the gap is
    documented. A pin bump re-runs X6.
- An IN list stays literal (spec 076: IN lists are not parameters), so each key set compiles a plan of
  its own.
- **The range join's filter (research Q1).** The unwrapped min == max comparison a range join (NLJ,
  IEJoin, PWMJ) pushes -- `s < 'b'` from a one-row build -- is rendered today like a user's filter. On a
  key that does not order like DuckDB (above), a row the join matches can be dropped.
  - P0 renders a non-equality filter from a join (`input.op` tells static from dynamic) only where
    `OrdersLikeDuckDB()` holds, whatever X6 shows. X6 measures the rows lost today.

### D2 (P1): vector distances by VECTOR_DISTANCE (SQL Server 2025)

**Measured** on random float32 vectors (X1b: 2000 of 384 and of 1536 dimensions, three runs). X1's vectors
were multiples of 1/64, which made every sum exact and so hid the summation order.

The numbers below are **this data's**, not properties of the functions. The components are iid uniform
in [-1, 1], so every vector is nearly orthogonal to q and the distances concentrate. Normalised or
clustered embeddings and near-duplicates, where a top-k boundary is contested, are not represented. The
equal top-10 / top-100 sets come from two unseeded runs of 2000 rows on one CPU pairing. No decision
rests on these maxima: the bounds below are a priori, and the TopN is kept. X8 uses real embeddings and
reports the top-k overlap.

| DuckDB | SQL Server | values |
|---|---|---|
| `array_distance(v, q)` | `VECTOR_DISTANCE('euclidean', v, q)` | 12-26% bit-equal; max relative error 1.1e-6 (5-9 float32 ulps) |
| `array_negative_inner_product(v, q)` | `VECTOR_DISTANCE('dot', v, q)` | 3-9% bit-equal; max absolute error 1.1e-5 (384) and 5.7e-5 (1536), about 15-30 ulps at the typical magnitudes, with no relative bound near 0 |
| `array_cosine_distance(v, q)` | `VECTOR_DISTANCE('cosine', v, q)` | 79-82% bit-equal (values near 1.0, where an ulp is coarse); max absolute error 1.2e-7 |

**Order.** The rows ordered by a distance disagree only between distances that close: 0-32 rank
disagreements of 2000, the most for euclidean at 1536 dimensions. The top-10 and top-100 sets were the same in
the two runs that measured them.

**Types and edge cases.**
- The server returns `float`, a float32 widened in every row (X1b); DuckDB returns `FLOAT`.
  - `column_types` cannot narrow it today: `ColumnTypeFits` (`mssql_functions.cpp`) refuses a described
    `float` (DOUBLE) read as FLOAT, in the unsafe vehicle too (review of #622, N5).
  - So the T-SQL sends `CAST(VECTOR_DISTANCE(…) AS real)`, and the column describes as `real` = FLOAT.
    On X1b's data every value was a widened float32, which the cast keeps exactly. Where the server
    computes wider, the cast is one more rounding (half a float32 ulp), within DQ-1's last bits. This is
    P1 work, with a test that a distance column types as FLOAT on both paths.
  - T-SQL `float` holds no infinity: a huge-component vector's distance is an error or an unknown on the
    server, where DuckDB answers inf. X1c covers it.
- **float32 columns only.** The catalog reads `vector(n, float16)` as `FLOAT[n]` too
  (`mssql_column_info.cpp`), and the server refuses to assign a float32 vector to a float16 one (42238,
  measured in the MERGE rewrite). So `VECTOR_DISTANCE(…, v16, CAST(@q AS vector(n)))` may fail (not
  run), and a q cast to float16 moves by about 1e-3 relative, which breaks the float32 bounds below and
  DQ-1's "last bits".
  - A float16 column is not pushed until X1d measures it and gives it a rule of its own.
- Cosine distance lies in [0, 2] (2 for opposite vectors). A zero vector: NaN in DuckDB (the largest
  value in its order, `NaN > 2` is true) and 1.0 on the server.
- NULL sorts first on the server and last in DuckDB, ascending or descending.
- `VECTOR_NORM(v, 'norm2')` tells a zero vector on the server.

- **What is pushed.** A distance of a float32 `vector(n)` column and a constant `FLOAT[n]`, or of two
  columns of one table. A binder-cast `CAST(v AS DOUBLE[n])` against a `DOUBLE[]` constant is refused:
  its result is DOUBLE, and DQ-1's "last bits" no longer applies. It is pushed in three places:
  - as a result column;
  - as an ORDER BY key, with `TOP`;
  - as a condition, in the catalog scan only (below).
  
  `array_inner_product` is pushed as `-VECTOR_DISTANCE('dot', …)`.
- **How q travels: to measure (X1c) before P1.**
  - The plan is a parameter, `CAST(@q AS vector(n))`.
  - The first candidate is its float32 bytes as `varbinary`. But V6's precedent does not carry over: V6
    is the bulk wire under a declared `vector(n)` (TYPE_INFO 0xA5), and V6 case 6 shows varbinary refused
    where it is the declared type (206, "varbinary is incompatible with vector"). Whether the CAST accepts
    a varbinary RPC parameter has not been run; X1 used only a `vector(3)` variable.
  - The fallbacks: the JSON text as `nvarchar` (9 significant digits round-trip a float32), or a 0xF5
    parameter under VECTORSUPPORT.
  - The TOP N path has no parameter channel today. `bind_data.order_by_clause` is a plain string, appended
    at init and serialised, and the path always removes the TopN, so keeping it over `TOP (k)` (DQ-2) is a
    new mode as well.
  - P1 gives the path parameters of its own, serialised with the clause. A 1536-dimension literal in the
    text would compile a plan per query vector.
- **A prepared kNN is out of scope** (review of #622, N6). `ORDER BY array_cosine_distance(v, $q) LIMIT k`
  is pushed by neither path:
  - the catalog scan's arm needs a `BoundConstantExpression`, and a `$q` is never folded
    (`BoundParameterExpression::IsFoldable()` is false at the pin);
  - the 079 writer pushes no `$n` parameter.
  
  It is the usual client form, so P1 pins the non-push with a test, and a later step may resolve the
  constant at init. X8 runs that shape too, to show what it costs.
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
  - **Only ordered comparisons are pushed as a condition:** `<`, `<=`, `>`, `>=`, and BETWEEN as two of
    them. `=`, `<>` and a negated condition never are. `<> 1.0` is true for DuckDB's NaN and false for
    the server's 1.0, and no superset form exists for them.
  - **Overflow and underflow.** A vector whose float32 squared norm underflows to 0 is NaN in DuckDB.
    But if the server's `VECTOR_NORM` is not 0 for it, the `OR VECTOR_NORM(v, 'norm2') = 0` rescue does
    not fire. X1c covers denormal-scale and huge-component rows as well as exact zero vectors.
  - Measured by X1c before P1.
- **A condition (`array_distance(v, q) < r`) is a superset in the catalog scan (D0), never consumed.** The
  threshold is widened by an a-priori float32 rounding bound, doubled for the two implementations, with
  γ_k = k·2^-24 / (1 - k·2^-24) and n the dimension:
  - euclidean: on the squared distance, absolute 2γ_(n+2)·(‖v‖ + ‖q‖)². This holds however the server
    sums: by differences, or as ‖v‖² + ‖q‖² − 2v·q. A relative bound would fail near zero distance for
    the second form, which is exactly near-duplicates and small-r conditions. X1c includes near-duplicate
    rows;
  - dot: absolute 2γ_(n+1)·‖v‖·‖q‖, where ‖v‖ is `VECTOR_NORM(v, 'norm2')` on the server (Cauchy-Schwarz
    bounds the sum of |products|);
  - cosine: absolute 4γ_(n+2).

  At 1536 dimensions these are 1.8e-4·(‖v‖ + ‖q‖)², 1.8e-4·‖v‖·‖q‖ and 3.7e-4: loose against X1b's
  errors, and sound. The cosine bound assumes the server computes dot / (norm · norm). X1c checks it on
  near-parallel vectors. In a statement pushed whole a distance condition is vetoed: there is no re-check.
- **Where.**
  - The catalog-scan TOP N path (`MSSQLOptimizer::TryPushTopN`: a two-argument key over the column and a
    constant). It is behind `mssql_order_pushdown`, which is opt-in. A distance condition on the same
    scan keeps TOP off (D0).
  - The catalog scan's filters.
  - The 079 writer's vocabulary (the default path), for result columns and ORDER BY keys.
- **DQ-1 (owner, 2026-11-24, for cosine; extended to euclidean and dot after X1b, for the owner to
  confirm): a distance is pushed as a result column and a kNN's ORDER BY key**, though it differs in its
  last bits. Under the floating-point aggregates' rule (recorded, vetoed in a condition / ORDER BY) a
  distance would never be pushed in a kNN, the case it is for.
  - The difference is recorded in `docs/type-mapping.md`.
  - A kNN's membership can change only where two distances at its boundary are within the a-priori bounds
    above (the γ terms). X1b's measured maxima only illustrate them on its data.
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

- **`G`** is the row's geometry:
  - for a `geometry` column, `g` itself;
  - for a `geography` one, `geometry::STGeomFromWKB(g.STAsBinary(), g.STSrid)` (below).
  
  The guard tests `G`, the value the methods run on.
  - **A geography that cannot become a planar geometry is let through (1), not converted.** That covers
    a FullGlobe, which has no WKB, and an invalid instance: `g.STAsBinary()` runs before the guard. So
    for a geography the CASE first tests `g.STIsValid() = 1 AND g.STGeometryType() <> 'FullGlobe'`.
    Only then is `G` built and tested.
  - D5 NULLs a FullGlobe in a value. Here it is kept for DuckDB to judge.
  - X4 runs a FullGlobe and an invalid geography through the whole form.
- **`@env`** is the constant's extent, computed on the client (`Geometry::GetExtent`) and sent as the WKB
  of a 2D POLYGON. It is widened on each side by a margin m, so:
  - a rounding at the rectangle's edge never drops a row;
  - the constant's own validity, Z and M do not matter.

  It is rebuilt in each row's SRID: SRID-blind, as DuckDB is.
  - **The margin is X4's output, not a guess** (review of #622, N4). A margin that scales with the
    constant's coordinates alone is too small for rows far from it. A point at the origin would get a
    2e-9 square against rows spanning 1e6, and X3 saw the server round a boundary point.
  - **No margin ships before X4 sets it.** The same rounding argument applies to every extent: the
    server plausibly snaps to a grid sized by both operands' extents (not checked). X4 is part of P2, so
    P2's envelope form takes the margin X4 gives, whether from the constant's and the rows' magnitudes or
    from the column's extent.
  - A degenerate extent (a POINT, an axis-parallel LINESTRING) is a polygon only through that margin. The
    server's exact `STIntersects` is no superset either (X3 saw it round a boundary point). So it is
    pushed only if X4's margin makes it one, and not otherwise.
- **Never pushed** (nothing is sent; DuckDB filters):
  - a constant that is EMPTY, or whose extent is not finite. It has no extent, and "each implies the
    envelopes meet" fails for `ST_Equals(g, EMPTY)` if the spatial extension follows GEOS (not checked);
  - `ST_DWithin` with a negative, NaN or infinite `d`.
- **The whole form is run as one statement by X4:** the CASE guard, the per-row
  `STGeomFromWKB(@env, G.STSrid)`, the widening, and the geography conversion. X2b and X3b ran its parts
  with literals.
- **The predicates:**
  - `&&` / `ST_Intersects_Extent` (core), and from the spatial extension `ST_Intersects`, `ST_Within`,
    `ST_Contains`, `ST_Covers`, `ST_CoveredBy`, `ST_Touches`, `ST_Overlaps`, `ST_Crosses` and `ST_Equals`,
    in either argument order: each implies the envelopes meet.
  - `ST_DWithin(g, c, d)`: the same form with `@env` grown by `d` (+ m). The distance between two shapes is
    at least the distance between their envelopes, which is at least the L∞ gap.
  - Never `ST_Disjoint` or a negation: their superset is everything.
- **Never consumed: a superset (D0).** DuckDB keeps its own filter. The envelope test, an invalid
  geometry let through and a curve the server sees are the server's; DuckDB decides the rest.
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
    - Only (b) needs `sys.spatial_indexes` in the catalog, to know which column has an index. That is a
      change to spec 084's metadata batch, so it belongs to (b), not to P2.
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
- **The operator's lifecycle.** `DynamicTableFilterSet::PushFilter` ANDs into what is already stored for
  the operator, and DuckDB's joins call `ClearFilters(op)` at the start of each execution.
  - The hint operator clears its filter at the start of each execution too. Otherwise a prepared plan
    run again with another X would AND the previous execution's box with the new one, and lose rows. X7
    covers EXECUTE twice with different parameters.
  - An empty X, or one of only NULL / EMPTY geometries, has no extent: nothing is pushed.
  - No gain rule: the table's own extent is unknown at plan time, so a box covering the table is pushed
    all the same. Its cost is one envelope test a row, which X7 measures.
- **The join types** are as DuckDB's own join filters allow: the Get is the probe child of an INNER,
  SEMI or RIGHT join. Not LEFT, OUTER, ANTI, MARK or SINGLE, where a probe row the hint removes is one
  the join must return (as unmatched, or for NOT EXISTS).
  - The check is made on the plan as DuckDB's join-order optimizer leaves it, which may flip a join's
    sides.
  - A Get on the build side gets no hint: its scan initialises before X's extent is known.
- **Between the join and the Get** only operators that commute with a filter: projections and filters.
  No LIMIT / TOP_N, aggregate or window.
- Not taken:
  - rewriting the join into range joins on extent columns (option A): one box only, and it needs virtual
    extent columns and functions core lacks;
  - an in-out table function staging the build in `#stage` (option C): the right shape for many boxes or
    exact matches, later, as tier 1 of the keys.

### D5 (P4): spatial functions in the 079 writer

A function's value in a statement pushed whole has no re-check, and D3's superset does not carry over.
So a function maps only where X5 shows the server's value equals DuckDB's on every shape it runs: X2's
shapes, multi-segment ones with random coordinates, and an EMPTY one of each type. DuckDB reads POINT
EMPTY as a NaN point (`geometry_as`), so `ST_X` / `ST_Y` give NaN where `.STX` gives NULL; such a case
does not map, and `ST_IsEmpty` is the candidate to test it with.
- **Invalid geometries.** The mapped form is guarded on `G.STIsValid()`, and a guard that answers NULL
  where DuckDB answers a value is a different column: such a function does not map.
- **Curves and FullGlobe**, which DuckDB reads as NULL under `geometry_as 'geometry'`. The mapped form
  NULLs them (`CASE WHEN G.STGeometryType() IN ('CircularString', 'CompoundCurve', 'CurvePolygon',
  'FullGlobe') THEN NULL ELSE … END`), and maps only under that read.
- **Planar only.** A geography goes through `G` as in D3, and its geodesic methods never map.

The candidates are `ST_X` / `ST_Y` (`.STX` / `.STY`), `ST_NumPoints`, `ST_IsEmpty` and `ST_GeometryType`
(whose names differ: mapped). `ST_SRID` never maps: DuckDB reads no SRID. What does not map is listed in
`docs/type-mapping.md`.

- **`ST_Area` and `ST_Length` are not candidates** (review of #622, N3). They fail D5's own rule:
  - X1-X3.md shows an invalid geometry fails `STArea`, so it would need the guard, which answers NULL
    where DuckDB answers a value;
  - they are floating-point sums whose order is the implementation's, so they would fall under the
    float-aggregate rule anyway (recorded; vetoed in a condition, ORDER BY and DISTINCT).
  
  X2's shapes cannot show otherwise: integer coordinates, one segment or a square, exact in any summation
  order. They return to the list only if X5 compares them on multi-segment shapes with random
  coordinates, and DQ-1's kind of recorded difference is accepted for them.

## Experiments still to run

| # | for | what |
|---|---|---|
| X4 | D3 | the whole envelope form as one statement (guard, per-row `STGeomFromWKB(@env, G.STSrid)`, widening, the geography conversion) against DuckDB's spatial extension on boundary cases (points on a box's edges and corners, on a diagonal edge, touching shapes), rows far from the constant (the margin against the rows' magnitude), a FullGlobe and an invalid geography; its cost (plans, logical reads, 100k / 1M rows); for DQ-3 (b): the exact methods and `Filter` on the same cases, whether `WITH (INDEX(six))` ever evaluates `Filter` exactly, whether the `UNION ALL`'s first branch keeps the index, the parameterised `@wkb` form's plan |
| X5 | D5 | each mapped function against DuckDB's (spatial extension) on X2's shapes and on multi-segment shapes with random coordinates, invalid ones and curves included |
| X1c | D2 | how q travels (a `varbinary` parameter under `CAST(@q AS vector(n))`, else JSON text or 0xF5); cosine with zero-vector rows (the column, the constant, two columns), denormal-scale and huge-component rows, near-duplicate and near-parallel vectors (the euclidean and cosine bounds), under `<`, `>`, BETWEEN, ASC, DESC and NULLS FIRST, against DuckDB |
| X1d | D2 | a float16 `vector(n)` column: `VECTOR_DISTANCE` against a float32 and a float16 q, the error against DuckDB's float32 over the read values |
| X6 | D1 | the range-join min == max filter against a server, on a string under a non-binary collation and on a uniqueidentifier: rows lost or not today; a static filter beside a join filter on one column (`AND(static, optional)`): pushed today or not; the dense-key build's branches (prefix range vs min/max: no estimate, a composite key), the prefix-range bounds by `LookupRange`, never false for a range holding a key, built in the key type |
| X7 | D4 | DuckDB at the pin: a `LogicalExtensionOperator` pass-through sink before a blockwise NL join; the dynamic filter reaching the catalog scan's `init_global`; EXECUTE twice with different parameters, and an empty build; a LEFT and an ANTI spatial join left without a hint, their answers equal to the unhinted plan's; the cost of a box covering the table |
| X8 | D2 | kNN on 100k / 1M `vector(1536)` real embeddings: wire, server CPU, wall against today's full read, the top-k overlap with DuckDB's; the prepared `$q` form's cost |

## PRs (each from main; P3 after P2, whose encoder arm for `st_intersects_extent` it renders through)

| PR | content |
|---|---|
| P0 | D1: FilterEncoder unwraps the optional join filters, per conjunct; IN on any type, non-equality filters from a join only where `OrdersLikeDuckDB`; the dense keys' bounds; X6; tests with a local table ⋈ a server table (sparse keys: 1, 3, 40, 100; consecutive ids, each dense branch, a two-key join) |
| P1 | D0 and D2: the superset result; VECTOR_DISTANCE in the 079 vocabulary (cast to `real`) and the TOP N path with its own parameters; NULL / NaN emulation, the zero vector in conditions and result columns; float32 only; the prepared `$q` pinned as not pushed; X1c, X1d, X8 |
| P2 | D3: spatial predicates with a constant as supersets (D0), SRID-blind, geography in its planar form, the EMPTY / degenerate / non-finite cases not pushed; X4; DQ-3 (and `sys.spatial_indexes` only with its option b) |
| P3 | D4: the bbox hint operator, cleared each execution; X7, with a LEFT and an ANTI spatial join left unhinted |
| P4 | D5: the writer's spatial functions; X5 |

**Off switches.** Every shipped pushdown has one (`mssql_order_pushdown`, `mssql_remote_pushdown`,
`mssql_dml_pushdown`, `mssql_scan_parameterize_filters`), and so does each part here. The neighbours are
read three ways, so each switch states its own:

| Setting | Scope | Read | Notes |
|---|---|---|---|
| `mssql_join_filter_pushdown` (P0) | GLOBAL | per statement, at the scan's init, as `mssql_scan_parameterize_filters` is | — |
| `mssql_vector_pushdown` (P1) | GLOBAL | per statement | both paths: the 079 writer reads it in the dry run, through the context that resolved the tables, as `mssql_dml_pushdown` is (the hooks get none); the scan path at optimize, as `mssql_order_pushdown` is |
| `mssql_spatial_pushdown` (P2, P3, P4) | GLOBAL | per statement | the same two reads |

None has an ATTACH option in the first cut. One is added, as for `remote_pushdown`, if DuckLake's
`METADATA_PARAMETERS` needs to turn a part off per catalog. All default true. P1 on the 079 path changes
result values (DQ-1's last bits), so its switch is the way back to DuckDB's own floats.

**The spatial extension at the pin** (review of #622, N9). The pin is past v2.0.0 and builds as `-dev`.
A `-dev` build looks for extensions under its source id, where nothing is published for a release
(CLAUDE.md, Versioning). So whether a `spatial` binary exists for the pin is not known, and nothing in
#622 loaded one (X2b: "no spatial extension: core && only"). This is settled before P2 is scheduled:
- either the spatial extension is built from source in CI against the pin (a download the owner
  approves), and skipped by `require spatial` locally where it is absent;
- or P2 ships core `&&` / `ST_Intersects_Extent` alone, tested without the extension. Its `ST_*` forms,
  X4's spatial-extension comparisons, X5 and P4 then wait for the pin to be a release tag.

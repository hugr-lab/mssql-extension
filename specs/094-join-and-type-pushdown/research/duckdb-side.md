# Spec 094 research: the DuckDB side (pin `4569c0a1eeb`, v2.0.0-dev70230)

Sources: `duckdb/` submodule at `4569c0a1eeb27c69f7799d9a40453a4fc7c0299d`, the extension at `main` (`e604c797`).
Plans come from `build/release/duckdb` (source id `4569c0a1ee`) run on local tables. No extension was loaded,
so the MSSQL-side behaviour below is read from the source and was not run against a server. The scripts are
`q*.sql` in this directory.

Paths: `D/` = `duckdb/src/`, `X/` = the extension's `src/`.

---

## Q1. Level 0: the filters a hash join pushes, and when they arrive

### Answer

The background in `specs/079-select-pushdown/after-0.3.0.md` § "Runtime join filters" is **correct**. It needs
these additions:

1. **Shapes, per equality key, in the hash join's `FinalizeFilters`** (`D/execution/operator/join/physical_hash_join.cpp:1876-2006`):
   - **min == max**: a plain comparison that is **not wrapped**:
     `make_uniq<ExpressionFilter>(BoundComparison(cmp, BoundReference(0), constant))` (:1931-1940). It uses
     the join's own comparison, so it is `=` for an equality join and `<`, `<=`, ... for a range join.
   - **IN list**: pushed when `1 < ht.Count() <= dynamic_or_filter_threshold` (default 50; :1472-1476,
     `settings.hpp:1216-1222`). The build values must contain no NULL and must not form a dense range.
     Shape: `ExpressionFilter(CreateOptionalFilterExpression(ExpressionFilter::CreateInExpression(BoundReference(0), values)))` (:1784-1834).
   - **Prefix-range filter (PRF)**: for a key whose span is small enough (`TryComputeSpan`). When one is
     registered, the code `continue`s (:1963-1981), so **no min/max and no bloom filter follow**. Measured:
     an integer key with 100 or 100 000 build rows gets only `optional: k IN PRF(k)`. A VARCHAR key with 100
     build rows (`'x'||i`) also gets only `s IN PRF(s)`.
     `PrefixRangeFilter` has no min/max accessor (`D/include/duckdb/planner/filter/table_filter_functions.hpp:166-200`),
     so the extension cannot recover a range from it.
   - **min/max**: pushed only when no IN list was pushed and no PRF was registered (:1984-1987).
     It is `k >= min` and `k <= max`, each wrapped by itself:
     - equality joins use `__internal_tablefilter_optional` (`CreateDynamicMinMaxFilter(..., selectivity_optional=false)`, :1728-1744);
     - non-equality joins use `__internal_tablefilter_selectivity_optional` (:1941-1947).
   - **Bloom filter**: deferred and published later (`PublishDeferredRuntimeFilters`, :1698-1726). It sits
     inside a selectivity-optional wrapper, as
     `BoundFunctionExpression(__internal_tablefilter_bloom_filter, [BoundReference(0)], BloomFilterFunctionData)`.

2. **Representation.** Each wrapper is an `ExpressionFilter` whose `expr` is
   `BoundFunctionExpression(<wrapper fn>, [BoundReference(col_type, 0)], bind_info)`
   (`D/planner/filter/table_filter_functions.cpp:67-91`).
   - The real predicate lives in the bind data: `OptionalFilterFunctionData::child_filter_expr` or
     `SelectivityOptionalFilterFunctionData::child_filter_expr`.
   - The public accessor is `ExpressionFilter::GetOptionalFilterChild(expr)` (`D/planner/filter/expression_filter.cpp:940-955`).
     Other helpers: `IsRootOptionalExpression`, `IsOptionalExpression` (:932-938), `IsRootOptionalFilter` (:962).
   - Function names (`D/include/duckdb/function/scalar/tablefilter_functions.hpp:19-59`):
     `__internal_tablefilter_optional`, `__internal_tablefilter_selectivity_optional`,
     `__internal_tablefilter_bloom_filter`, `__internal_tablefilter_prefix_range`, `__internal_tablefilter_dynamic`.
   - `OptionalFilterSelect` sets every row true (`table_filter_optional_function.cpp:50-57`). So the
     extension's client-side net running an unencoded optional filter is a no-op. A selectivity-optional
     filter evaluates its child (bloom / range).
   - A dynamic filter is ANDed into the column's existing filter (`TableFilterSet::PushFilter`,
     `D/planner/table_filter_set.cpp:406-425`). The extension therefore sees one filter per column, e.g.
     `AND(static, optional(IN …), selectivity_optional(PRF))`. EXPLAIN ANALYZE shows it as
     `optional: (k IN (37, …, 333)) AND optional: k IN PRF(k)` (`q2b.sql`).
   - Input casts: when the probe key went through an integral or VARIANT cast, the child is rebuilt over
     `BoundReference(0)` with `CAST` / `TRY_CAST` (`D/optimizer/runtime_filter_cast.cpp:67-84`).

3. **Not only hash joins** (addition to the background). `PhysicalPiecewiseMergeJoin`, `PhysicalIEJoin` (through
   `PhysicalRangeJoin`, `physical_range_join.cpp:341-365`) and `PhysicalNestedLoopJoin`
   (`physical_nested_loop_join.cpp:186-313`) also take a `JoinFilterPushdownInfo`. They push **min/max only**
   (no hash table, so no IN, bloom or PRF), wrapped selectivity-optional. When min == max they push the
   **unwrapped one-sided comparison**.
   - Measured: `big ⋈ small ON b.k < s.k` gives `Dynamic Filters: optional: (k <= 333)` (`q2b.sql`).
   - A one-row build gives `ON b.s < o.s` → NLJ, `Dynamic Filters: s < 'B'`, unwrapped (`q1d.sql`).

4. **When the filters are generated.** `JoinFilterPushdownOptimizer` runs on `LOGICAL_COMPARISON_JOIN` only
   (`D/optimizer/join_filter_pushdown_optimizer.cpp:437-443`).
   - Accepted comparisons: `= < <= > >=` (:352-364).
   - The probe-side expression must be a bare column ref, an integral cast, a VARIANT cast or `variant_normalize` (:51-109).
   - Nested types are refused (:53-56), so FLOAT[n] gets nothing; INTERVAL is refused too.
   - The target must be a `LogicalGet` with `function.filter_pushdown` (:237-243). It is reached through
     projections, filters, the probe side of other joins, inner ANY joins, unions and groups (:162-314), but
     not through LIMIT or TOP_N (:167-171).
   - Join types excluded: MARK, SINGLE, LEFT, OUTER, ANTI, RIGHT_ANTI (:129-145).
   - The optimizer runs late, after `STATISTICS_PROPAGATION` (`D/optimizer/optimizer.cpp:531-535`), and
     before the extension's post-optimize hook (:595-602).

5. **When the filters reach `init_global`.** `TableScanGlobalSourceState`'s constructor builds the merged set
   (`dynamic_filters->GetFinalTableFilters(op, op.table_filters)`) and then calls `init_global` with it
   (`D/execution/operator/scan/physical_table_scan.cpp:34-50`). That constructor runs from `Pipeline::Schedule → Reset → ResetSource(false)`
   (`D/parallel/pipeline.cpp:317-325, 401-404, 445-455`).

### Is the build guaranteed to be finished first?

Yes, by the schedule. Each link:

- **Build before probe.** The build side becomes a child MetaPipeline and the probe pipeline depends on it
  (`MetaPipeline::CreateChildMetaPipeline` → `current.AddDependency`, `D/parallel/meta_pipeline.cpp:108-117`;
  `PhysicalJoin::BuildJoinPipelines`, `physical_join.cpp:47-58`).
- **The dependency is the complete event.** The schedule makes the probe's EXECUTE stage depend on the
  dependency's COMPLETE stage (`D/parallel/pipeline_schedule.cpp:196-205`). `PipelineEvent::Schedule` →
  `Pipeline::Schedule` (`pipeline_event.cpp:9-20`) only runs once the event's dependencies have finished.
- **Late hash-join filters are published before that.** Bloom and PRF filters built during the hash join's
  finalize are published in the finalize tasks' `FinishTasks` (`physical_hash_join.cpp:1297-1303`). The
  finalize event is inserted ahead of the build's complete event (`ScheduleFinalize` / `InsertEvent`).
- **Unions inherit it.** A union branch inherits all of `current`'s dependencies (`meta_pipeline.cpp:253-269`).

Caveats:

- A table function with `global_initialization = INITIALIZE_ON_SCHEDULE` is initialised while the schedule is
  built (`pipeline_schedule.cpp:122-129, 177-183`; `executor.cpp:104-106`), which is **before** the build.
  `mssql_catalog_scan` keeps the default `INITIALIZE_ON_EXECUTE` (`D/include/duckdb/function/table_function.hpp:580`;
  not set in `X/table_scan/table_scan.cpp:1585-1630`). It must stay that way.
- `ResetSource(false)` creates the source state only if there is none, so nothing initialises it earlier on the normal path.
- An empty build sends nothing (`ht.Count() > 0`, :2096), and the join may return `NO_OUTPUT_POSSIBLE` (:2134).
- The extension runs its SQL in `TableScanInitGlobal` (`X/table_scan/table_scan.cpp:180-…`). So in autocommit,
  in a transaction (materialised at init) and under the shared-connection materialisation alike, the SQL is
  built after the filters are known.

### Where FilterEncoder would hook in the extension

- `TableScanInitGlobal` hands `input.filters` (static plus dynamic, merged) to
  `FilterEncoder::Encode(...)` (`X/table_scan/table_scan.cpp:424-477`).
  - The refused filters go to `encode_result.unhandled`. They are armed client-side through
    `ToExpression(BoundReference(0))` plus an `ExpressionExecutor` (:445-476).
  - `FilterEncoder::Encode` → `EncodeFilter` → `EXPRESSION_FILTER` → `EncodeExpressionFilter` →
    `EncodeSearchCondition` → `EncodeExpressionImpl` (`X/table_scan/filter_encoder.cpp:604-718, 724-753, 889-915, 938-1033`).
  - The wrapper reaches `EncodeExpressionImpl`'s `BOUND_FUNCTION` arm. Today it falls through to
    `EncodeFunctionExpression`, where `ExpressionVocabulary::FunctionFor` has no mapping, so it is refused
    (:1035-1060).
  - Under an AND (`EncodeConjunctionExpression`, :1533-1576) the refusal sets `all_supported = false`, so the
    whole column filter goes to the client net, static part included.
- **Hook.** In `EncodeExpressionImpl`'s `BOUND_FUNCTION` arm, before the cast and comparison checks, add:
  `if (auto child = ExpressionFilter::GetOptionalFilterChild(expr)) { … }`.
  - Encode `*child` with the same `ctx`. `ctx.filter_column` is set by the `EXPRESSION_FILTER` arm, and the
    child uses `BoundReference(0)`.
  - If the child does not encode — bloom, PRF, `__internal_tablefilter_dynamic`, TRY_CAST, or a range the
    rules refuse — return `{"", true}`, i.e. "nothing to send, nothing to re-check". The conjunction arm
    already treats `supported && sql.empty()` as "contributes nothing" without clearing `all_supported`.
- **Precedents in the extension.**
  - `X/catalog/mssql_entry_name_filter.cpp:37` already unwraps with `IsRootOptionalExpression` and `GetOptionalFilterChild`.
  - `ScanHasClientSideFilters` (`X/table_scan/mssql_optimizer.cpp:596-626`) already tells optional from
    mandatory filters with `IsRootOptionalFilter`.
- **Telling dynamic filters from static ones.** `TableFunctionInitInput::op` is the `PhysicalTableScan`
  (`D/include/duckdb/function/table_function.hpp:137-166`; passed at `physical_table_scan.cpp:43-44`). Its
  `table_filters` (static) and `dynamic_filters` (`physical_table_scan.hpp:57`) are separate, so the encoder
  can learn which filters came from a join.

### What it implies for the spec

- **Level 0 is a FilterEncoder change only.** No DuckDB API is missing.
- **Superset rule.** A join filter may widen the result, because the join re-checks every match.
  - An IN list is safe whenever server equality is no stricter than DuckDB's (true for CI collations and trailing blanks).
  - A `>=` / `<=` bound is safe only where `OrdersLikeDuckDB()` holds.
- **Open, possibly a live issue.** The **unwrapped** min == max comparison of a range join (NLJ / PWMJ / IEJoin,
  e.g. `s < 'B'`) is a hard filter. FilterEncoder pushes it today like a user filter (server collation). For a
  string key under a non-binary collation it can **drop** rows the DuckDB join would match: the server orders
  `'C' > 'b'` where DuckDB has `'C' < 'b'`.
  - The after-0.3.0 note's "the unwrapped min == max comparison keeps today's path" is only safe for `=`.
  - The spec should apply the range rule to a non-equality hard dynamic filter as well, recognised through `input.op`.
  - Not reproduced against a server: needs a test.
- **More than 50 keys** leaves nothing renderable for small-span integer **and string** keys (PRF only). That
  confirms the after-0.3.0 tier 1 / 2a item (`mssql_keyed_scan` and `#keys`).

---

## Q2. Joins that are not equalities: `&&`, `ST_Intersects_Extent`, distance

### Answer

- **Plan.** `ON a.g && b.g`, `ON ST_Intersects_Extent(a.g, b.g)` and `ON array_distance(a.v, b.v) < 0.1`
  (both sides inside the function) bind as a **`LogicalAnyJoin`**. It is planned as **`BLOCKWISE_NL_JOIN`**
  (`D/execution/physical_plan/plan_any_join.cpp:15`).
- **No pushdown.** `JoinFilterPushdownOptimizer` only visits comparison joins, and `PhysicalBlockwiseNLJoin`
  has no `filter_pushdown` (0 occurrences in `physical_blockwise_nl_join.cpp`). So **nothing** is pushed into
  the probe Get.
- **Mixed conditions.** When an equality sits beside the spatial condition (`id = id AND g && g`), the plan is a
  hash join with `g && g` as a residual condition. Filters come from the equality only.
- **The range-join choice** (`plan_comparison_join.cpp:217-283`):
  1. a hash join if there is an equality;
  2. otherwise IEJoin when there are at least 2 range conditions and both sides have at least
     `merge_join_threshold` (1000) rows;
  3. otherwise PWMJ when there is a range condition;
  4. NLJ when either side has fewer than `nested_loop_join_threshold` (5) rows. NLJ needs at least one
     comparison and accepts other, non-comparison conditions (`physical_nested_loop_join.cpp:133-158`);
  5. BNL otherwise.
- **No GEOMETRY-specific join filter exists at the pin.** "GEOMETRY" appears nowhere in
  `join_filter_pushdown_optimizer.cpp`, `physical_hash_join.cpp`, `runtime_filter_cast.cpp` or `planner/filter/*.cpp`.
  - GEOMETRY extent statistics are used only for row-group pruning of a **constant** predicate
    (`IntersectsExtentFilterPrune`, `D/function/scalar/geometry/geometry_functions.cpp:146-209`: exactly one
    operand must be a constant).
  - An equality join on GEOMETRY would get the generic min/max on bytes. It is irrelevant here, since the
    extension marks geometry as `comparison_differs`.
- **Static, plan-time join filters** (`Filters: k <= 333` in the plans) come from
  `StatisticsPropagator::CreateFilterFromJoinStats` (`D/optimizer/statistics/operator/propagate_join.cpp:429-475`).
  It handles integral column refs whose statistics both sides know. An MSSQL scan has no column statistics,
  so it never gets one.

### Evidence (EXPLAIN on local tables, `q2.sql`, `q2b.sql`, `q2c.sql`)

```
Any Join  Condition: g && g                              (optimized logical plan)
Blockwise Nl Join  Condition: g && g
  Seq Scan big   Projections: g         (no Dynamic Filters, 200,000 rows read)
Blockwise Nl Join  Condition: st_intersects_extent(g, g)
Blockwise Nl Join  Condition: array_distance(v, v) < 0.1
Hash Join  Conditions: id = id, g && g     (equality + residual)
Piecewise Merge Join  Conditions: k < k    Dynamic Filters: optional: (k <= 333)
```

### What it implies for the spec

- A spatial or distance join of a local table with an MSSQL table gets **no** runtime filter at this pin. Anything
  better has to come from the extension (Q3).
- A residual condition on a comparison join (hash / PWMJ / IEJoin / NLJ) is allowed. So once a comparison exists
  in the join, the `&&` can stay as a residual and DuckDB keeps the exact check (see Q3, option A).

---

## Q3. An optimizer-extension rewrite: a small build side `X` joined on `&&` with an MSSQL scan

The plan the hooks see:

- `MSSQLOptimizer::PreOptimizeRoot` runs before every built-in optimizer (`D/optimizer/optimizer.cpp:582-590`);
  today it only does `RewriteMerges` (`X/table_scan/mssql_optimizer.cpp:1029-1044`).
- `OptimizeRoot` runs after all of them, `JOIN_FILTER_PUSHDOWN` (:531) included (:595-602).
- At either point the join is a `LogicalAnyJoin(condition = st_intersects_extent(colref_probe, colref_build))`.

Three options exist at this pin.

### Option A: let DuckDB's own min/max machinery compute the extent (no new operator)

Add redundant range conditions over extent columns. Then the planner picks NLJ / PWMJ / IEJoin with
`filter_pushdown` instead of BNL, and `&&` stays as a residual.

Verified on local tables with plain x/y columns (`q3.sql`):
`ON b.x >= s.x0 AND b.x <= s.x1 AND b.y >= s.y0 AND b.y <= s.y1 AND b.g && s.g` gives a PWMJ with
`Conditions: x >= x0, x <= x1, y <= y1, g && g`, and the probe Get gets
`Dynamic Filters: optional: (x >= 0.0) AND optional: (x <= 14.0), y <= 5.0`. That is the build side's union
bounding box, computed at execution time and present at the probe's `init_global`.

Notes on that run:
- `y >= y0` was proven always true from statistics and dropped.
- `y <= 5.0` is unwrapped because min == max.

What it needs:
1. **Probe-side extent as bare column refs on the MSSQL Get.** Join filters take only a column ref or an
   integral cast (`join_filter_pushdown_optimizer.cpp:51-109`).
   - The way in is virtual columns (`get_virtual_columns`, already used for `rowid` and the keyless key columns,
     `X/table_scan/table_scan.cpp:1399-1440`), e.g. `_mssql_xmin(g)`. The optimizer would add them to the Get's
     column ids.
   - FilterEncoder refuses virtual columns today (`filter_encoder.cpp:650-657`; `EncodeColumnRef` :1494-1499).
     A new arm would render them. Best is to combine the four bounds of one geometry column into **one**
     envelope predicate at init instead of four non-sargable `STEnvelope()` expressions.
2. **Build-side extent expressions.** The right-hand side may be any expression: the min/max aggregates are
   bound over `cond.GetRHS()` (:410-427).
   - Core has no `ST_XMin` etc. at the pin. Its geometry functions are `st_geomfromwkb`, `st_aswkb`, `st_astext`,
     `st_intersects_extent`, `st_crs`, `st_setcrs` and `vertex_extract` (points only), plus geojson.
   - So the extension would register its own scalar functions on `Geometry::GetExtent`
     (`D/include/duckdb/common/types/geometry.hpp:304-305`, `GeometryExtent` :100).
3. **Where.**
   - In **PreOptimizeRoot**, before `JOIN_ORDER` (:355) and `JOIN_FILTER_PUSHDOWN` (:531), add the four
     conjuncts and the virtual columns. Join ordering then makes a `LogicalComparisonJoin`, and
     `JoinFilterPushdownOptimizer` handles the rest.
   - In OptimizeRoot it is too late. Only `JoinFilterPushdownOptimizer::VisitOperator` is public;
     `GenerateJoinFilters` is private (`join_filter_pushdown_optimizer.hpp:21-37`). Re-running the visitor over
     the whole plan is untested.

Constraints:
- It gives **one** global box (the union of the build extents), not one box per build row.
- The bounds arrive as four separate per-column filters: selectivity-optional when min != max, a **hard**
  unwrapped comparison when min == max. Doubles order like DuckDB, so both are safe to push.
- IEJoin needs both sides of at least 1000 rows; a small build gets PWMJ or NLJ, which also push.
- A LIMIT or TOP N over the probe side blocks it (:167-171).
- Correctness: `bbox(a) ∩ bbox(b) ≠ ∅` holds exactly when the four inequalities hold, so the filter is a
  superset of `&&`. Empty geometries have NULL extents, and a NULL min/max is skipped (`physical_hash_join.cpp:1916-1921`).
- Geography: DuckDB's `&&` on `GEOMETRY('OGC:CRS84')` is a planar lon/lat box. SQL Server geography has no
  `STEnvelope`. Open: owner's call.

### Option B: an extension operator fills the scan's `DynamicTableFilterSet`

The APIs at the pin:
- `LogicalGet::dynamic_filters` is a public `shared_ptr<DynamicTableFilterSet>` (`D/include/duckdb/planner/operator/logical_get.hpp:77`).
- It is copied onto the `PhysicalTableScan` (`D/execution/physical_plan/plan_get.cpp:236`, on the
  `projection_pushdown` path, which the catalog scan takes).
- `DynamicTableFilterSet::PushFilter(const PhysicalOperator &op, ProjectionIndex column, unique_ptr<TableFilter>)`
  is public and takes a lock (`D/include/duckdb/planner/table_filter_set.hpp:116-128`; `table_filter_set.cpp:457-470`).
- The set is merged in `TableScanGlobalSourceState` (Q1).

How the operator would work:
- **Operator.** A `LogicalExtensionOperator` → `CreatePlan` → a custom `PhysicalOperator` that is both sink and
  source. It is placed on the join's **build** side (`children[1]`) as a pass-through materialiser:
  - Sink: collect the rows and fold their extents.
  - Finalize: push the filter into the probe Get's `DynamicTableFilterSet`. The optimizer creates the set on the
    Get and hands the `shared_ptr` to the operator.
  - Source: emit the rows unchanged.
- **Why the order holds.** The operator is inside the join's build child MetaPipeline, so its pipeline completes
  before the join's build. That completes before the probe's EXECUTE (Q1).
- **What it may push.** Any `ExpressionFilter` the extension's encoder knows, on the geometry column's position.
  Wrapping it in `CreateOptionalFilterExpression` (`table_filter_functions.cpp:76-81`) means DuckDB, and the
  client net, treat it as droppable. Example: `st_intersects_extent(BoundReference(0), <envelope constant>)`.
- **Precedent.** Spec 080 PR 4b's `MSSQLLogicalStagedMerge` (`X/include/dml/merge/mssql_logical_staged_merge.hpp:24, 49`)
  plans `MSSQLPhysicalStagedMerge` (`X/include/dml/merge/mssql_physical_staged_merge.hpp:20-45`, a sink with
  `Finalize`). It replaces the binder's `LogicalMergeInto` in `PreOptimizeRoot` (`X/table_scan/mssql_optimizer.cpp:1029-1044`).
  A post-optimize insertion also works, since join order and build/probe sides are already fixed there.

Constraints:
- The build side is materialised one extra time; it is small by assumption.
- The filter is still one box unless the extension sends a list of envelopes, which is its own encoding.
- The operator must find the probe Get's binding through projections itself.

### Option C: a table in-out function, the server-side join or keys vehicle

- **APIs.** `TableFunction::in_out_function` / `in_out_function_final`, planned as `PhysicalTableInOutFunction`
  (`D/execution/operator/projection/physical_tableinout_function.cpp:164-173`).
- **Shape.** The same shape as the after-0.3.0 tier-1 `mssql_keyed_scan`: collect the build geometries, stage
  them by INSERT BULK into a `#stage`, run `… JOIN #stage s ON t.g.STIntersects(s.g) = 1` (or `.Filter`) at final,
  and keep the join in DuckDB.
- **Constraints.**
  - `ParallelOperator()` is true, and `FinalExecute` runs per pipeline executor
    (`physical_tableinout_function.hpp:35-44`). "Run once after all input" needs a global-state count (the last
    thread runs the query) or a single-threaded input.
  - The function owns the probe stream.
  - It is the most work, and the only option that sends per-row geometry rather than one box.

### What it implies for the spec

- Option A uses only DuckDB's machinery and was demonstrated on local tables. Its cost is extension extent
  functions, extent virtual columns, a pre-optimize rewrite, and a combined encoder arm.
- Option B is more direct and keeps the plan shape, at the price of a custom operator. Spec 080 already does this.
- Option C is a separate tier.
- All three are server-side superset filters, with `&&` re-checked in DuckDB.

---

## Q4. Vector kNN

### Answer

**What reaches the extension's optimizer.** With no late materialisation, the logical plan is
`LOGICAL_TOP_N(orders: [BOUND_COLUMN_REF → projection col 1]) → LOGICAL_PROJECTION(id, array_cosine_distance(v, [0.1,0.2,0.3])) → LOGICAL_GET`
(`q4b.sql`).
- The TOP N key **is** visible as an expression: through the projection, it is a `BoundFunctionExpression` with
  children `[BoundColumnRef v, BoundConstant FLOAT[3]]`.
- DuckDB's own tables get late materialisation (a SEMI join on rowid with the Top N inside, `q45.sql`).
  `mssql_catalog_scan` sets no `late_materialization` (`D/optimizer/late_materialization.cpp:228`), so its shape
  stays TopN → Projection → Get.
- TopN's own dynamic filter is pushed only for a bare column key (`D/optimizer/topn_optimizer.cpp:64-157`), so a
  distance key gets none.

**Where ORDER BY / TOP N pushdown is decided today.**
- **Catalog-scan path:** `MSSQLOptimizer::Optimize` → `TryPushTopN` (`X/table_scan/mssql_optimizer.cpp:714-776`),
  then `TryPushLimitOrderBy` / `TryPushOrderBy`.
  - This path is **opt-in**: `mssql_order_pushdown`, default false (`X/connection/mssql_settings.cpp:610`;
    `IsOrderPushdownEnabled` :70-97).
  - `FindMSSQLScan` accepts a Get, or a Projection over a Get (:109-131).
  - `ProcessOrderByNodes` → `ResolveOrderExpression` (:177-301) resolves through the projection. A
    `BOUND_FUNCTION` is pushed only through `GetFunctionMapping` with **one argument**, which must be a column
    (:237-268). So `array_cosine_distance(v, const)` is refused at the argument-count check (:246).
  - Then `OrderKeyFragment` / `OrderTerm` apply. For a vector column, `comparison_differs` is true
    (`MSSQLColumnInfo::ComparisonDiffersFor`, `X/catalog/mssql_column_info.cpp:130-146`; vector is not on the
    allow-list), and `OrdersLikeDuckDB` decides keys.
  - A full push sets `bind_data.order_by_clause` and `top_n` and **removes the TopN** (:766-771).
  - `ScanHasClientSideFilters` blocks the push when a mandatory filter stays client-side (:596-626).
- **Remote-pushdown path (spec 079), on by default:** DuckDB's `RemotePushdownOptimizer` runs in
  `Optimizer::OptimizeStatement` on the **parsed** statement (`D/optimizer/optimizer.cpp:216-226`).
  - The writer works on `SelectNode` / `ParsedExpression`. ORDER BY is `NodeWriter::WriteOrder`
    (`X/pushdown/mssql_node_writer_select.cpp:645-761`).
  - An expression key goes through `WriteValue`, then needs `computed.orderable` (:690-742).
  - `array_*` distance functions are not in `ExpressionVocabulary` (no match in `X/pushdown/`). So the statement
    is vetoed and falls back to the catalog-scan path above.

**`WHERE array_distance(v, c) < 0.5`.**
- On a DuckDB table it becomes a TableFilter: `Filters: array_distance(v, [0.1, 0.2, 0.3]) < 0.5` on the Seq Scan
  (`q45.sql`), through `TryPushdownGenericExpression` (`D/optimizer/filter_combiner.cpp:447-488`), as an
  `ExpressionFilter` over `BoundReference(0)`.
- On the MSSQL scan it **stays a `LogicalFilter`**:
  1. `PushdownGet` offers it first to `pushdown_complex_filter`, i.e. `ComplexFilterPushdown`
     (`D/optimizer/pushdown/pushdown_get.cpp:125-147`; `X/table_scan/table_scan.cpp:1334-1390`). The encoder refuses
     it: the function is unmapped, and `EncodeColumnRef` refuses a `comparison_differs` column (`filter_encoder.cpp:1501-1508`).
  2. The combiner produces no table filter for `f(col) < c`.
  3. `pushdown_expression` (`MSSQLPushdownExpression`, `table_scan.cpp:1158-1172`) refuses it too.
  4. Even if accepted, `array_distance` is `SetFallible()` (`duckdb/extension/core_functions/scalar/array/array_functions.cpp:240, 300`).
     The combiner pushes a throwing expression only when it is the scan's **only** filter
     (`pushdown_get.cpp:190-193`).

### What would need to change to push the distance as an ORDER BY key with TOP k

- **Catalog-scan path.** Extend `ResolveOrderExpression` with a two-argument arm:
  `array_cosine_distance` / `array_distance` / `array_negative_inner_product`, with `children[0]` a column of
  SQL type `vector` and `children[1]` a `BoundConstant` FLOAT[n] of the same n.
  - It would render `VECTOR_DISTANCE('cosine' | 'euclidean' | 'dot', [v], CAST(@q AS vector(n)))`. The SQL
    Server side was not checked here: that is the server note's job.
  - This bypasses `comparison_differs` and `OrdersLikeDuckDB` for that one shape.
  - Removing the TopN means the server's order and float arithmetic replace DuckDB's.
  - **Open, owner's call.** Either remove the TopN, or keep it and push `TOP k` / `TOP k+margin` plus `ORDER BY`,
    so DuckDB re-sorts the k rows. Near-ties at the k-th place may still pick a different set.
  - It stays behind the opt-in `mssql_order_pushdown`, unless the spec changes that.
- **079 path.** A vocabulary entry for the distance functions, typing the constant as `vector(n)`, would let a
  catalog-only kNN statement go whole as `SELECT TOP k … ORDER BY VECTOR_DISTANCE(…)`. `computed.orderable` and
  `approximate` must be decided for a float distance; today a floating-point aggregate is vetoed in ORDER BY (:731-733).
- **The filter `array_distance(v, c) < r`.** Teach `FilterEncoder::EncodeFunctionExpression` the distance
  functions over a vector column and a constant, and let the `comparison_differs` gate pass this shape.
  - Without that, the filter runs in DuckDB over every row streamed.
  - A radius filter on the server is exact only up to float differences. A row near the boundary may be dropped
    where DuckDB would keep it. Open, owner's call (D4 does not cover it).

---

## Q5. Spatial predicates with a constant

### Answer

**On a DuckDB table:**
- `WHERE g && ST_GeomFromWKB(…)` and `WHERE ST_Intersects_Extent(g, '…'::GEOMETRY)` reach the Seq Scan as an
  **`ExpressionFilter`**. The constant is folded:
  `Filters: g && 'POLYGON ((0 0, 5 0, 5 5, 0 5, 0 0))'::GEOMETRY`. The scan prunes row groups with
  `IntersectsExtentFilterPrune` (`q45.sql`).
- With a CRS column (`GEOMETRY('OGC:CRS84')`) the column side carries a CRS-only cast, even when the constant has
  the same CRS: `Filters: CAST(g AS GEOMETRY) && '…'::GEOMETRY` (`q5b.sql`).

**On the MSSQL catalog scan, today:**
- The predicate is first offered to `ComplexFilterPushdown` (`X/table_scan/table_scan.cpp:1334-1390`), where
  `FilterEncoder::EncodeSearchCondition` refuses it:
  - `st_intersects_extent` has no `FunctionFor` mapping (`filter_encoder.cpp:1051-1060`);
  - the geometry column is `comparison_differs` (not on the `ComparisonDiffersFor` allow-list since spec 093 T5,
    `X/catalog/mssql_column_info.cpp:130-146`), so `EncodeColumnRef` refuses it (`filter_encoder.cpp:1501-1508`).
- `pushdown_expression` refuses it for the same reason (`table_scan.cpp:1158-1172`). It therefore **stays a
  `LogicalFilter` above the scan**: DuckDB evaluates `&&` on every row streamed.
- An `IS NULL` test on a geometry column is still pushed (`Encode`, `filter_encoder.cpp:672-685`; `null_test_operand`).
- If a geometry filter ever arrived as a TableFilter (from a join, Q1), the per-column gate in `Encode`
  (:678-685) sends it to the client net.

### What it implies for the spec

- `ComplexFilterPushdown` erases only what it encodes. A **superset** predicate can be pushed while the exact
  `&&` stays in DuckDB: render the SQL and leave the expression in `filters`. That suits spatial filters, where
  the server's predicate differs.
- The encoder needs to see through the CRS-only cast. `ExpressionVocabulary::IsTransparentCast` handles
  string-to-string only today (`filter_encoder.cpp:982-986`).
- Server semantics, not checked here (server note):
  - `g.STIntersects(@bbox)` is a **subset** of `g && c`: an L-shaped line whose box meets the constant's box may
    not meet the box itself. Wrong as a pushed filter.
  - `g.STEnvelope().STIntersects(@bbox)` is the exact `&&` for planar geometry, but likely not index-sargable.
  - Whether `g.Filter(@bbox)` (spatial index) is a guaranteed superset of `&&` is **open**.
  - Geography has no planar envelope (see Q3).
- Choosing which server predicate to push is an owner decision for the spec.

---

## Open items

1. A suspected live correctness issue (Q1): an unwrapped min == max dynamic filter of a range join on a string
   key under a non-binary collation can drop matches. A test against a server is needed.
2. Option A's post-optimize variant (re-running `JoinFilterPushdownOptimizer::VisitOperator`) is untested. The
   pre-optimize variant (adding virtual columns to a bound Get) was not built here; only the plan shape was
   checked, with real columns.
3. Option C's parallel finalisation scheme.
4. kNN: whether the server's float order and the TopN removal are acceptable, or whether TopN is kept with `TOP k(+m)`.
5. Spatial server predicates: whether `Filter()` is a superset; geography.

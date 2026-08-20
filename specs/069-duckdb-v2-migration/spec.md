# Spec 069 — DuckDB 2.0 migration: main goes 2.0-only

**Status**: DRAFT
**Branch**: `spec/069-duckdb-v2-migration`
**Context**: DuckDB main after v1.5.5 (heading to 2.0) breaks the extension API
at a scale the `mssql_compat.hpp` shim mechanism (spec 051) cannot bridge: a
new `Identifier` value type threads through every catalog signature, vector
data access split into const/mutable pairs, the table-filter class hierarchy
was renamed, and bound-expression members went private. A clean build against
duckdb `d7a4366603` produced **~222 unique errors across 35 files** (444 in the
log — every TU compiles twice, static + loadable).

## Strategy decision

**Main targets DuckDB main/2.0 only.** The 1.5.5 world lives on branch
`duckdb-v1.5.5` (cut from main at `e2b8c01`, pushed 2026-08-20) for 0.2.x
maintenance releases and backports. `mssql_compat.hpp` retires on main — its
reason to exist was compiling one tree against two API shapes, and the 2.0
drift (type changes in signatures, member privatization, renamed class
hierarchies) is not shimmable at reasonable cost.

Rejected alternative: extending `mssql_compat.hpp` to cover 2.0. A shim can
alias a moved header or wrap a renamed method; it cannot paper over `const
Identifier &` replacing `const string &` in `Catalog::GetCatalog`, or
`FlatVector::GetData<T>` returning `const T *`, without wrapping essentially
every call site — at which point the wrapper *is* the migration, but paid
twice.

## Baseline

| Submodule | Pin |
|---|---|
| `duckdb` | `d7a4366603` ("Remove default database", #24837) |
| `extension-ci-tools` | `35759fd` |

Pins are fixed SHAs, bumped deliberately — never floating. A bump that breaks
the build is a normal event on this branch until 2.0 tags exist.

## Error inventory (clean build 2026-08-20, `ninja -k 0`)

### Cluster A — `Identifier` threads through the catalog API (~60 errors)

`duckdb/common/identifier.hpp` introduces a first-class identifier type;
`Catalog::GetCatalog(ctx, const Identifier &)` (34 call-site errors — the
single biggest item), and the Create/Drop info structs renamed/retyped their
name fields: `CreateSchemaInfo::schema`, `DropInfo::name`,
`CreateTableInfo::table` are gone. `no viable conversion from 'Identifier' to
'const string'` (22×) marks every place we pass our `string` names into the
new API or read theirs into ours.

Files: `mssql_schema_entry.cpp` (36), `mssql_catalog.cpp` (24),
`mssql_functions.cpp` (14), `mssql_table_entry.cpp` (14),
`mssql_table_set.cpp`, `mssql_ddl_translator.cpp`, `mssql_preload_catalog.cpp`,
`mssql_refresh_function.cpp`, `mssql_storage.cpp`, `azure_secret_reader.cpp`.

### Cluster B — vector const-correctness (~40 errors)

- `FlatVector::GetData<T>(Vector &)` now returns `const T *`; writes go
  through `FlatVector::GetDataMutable<T>` (44 call sites in 12 files: all 9
  codec families, `row_stager`, `type_converter`, `mssql_result_stream`,
  `mssql_refresh_function`).
- Read sites binding the result to a non-const local (`timestamp_t *`,
  `hugeint_t *`, `string_t *`, `int64_t *` initialization errors) need `const`
  locals.
- `ValidityMask::SetAllValid` on a `const ValidityMask` — masks obtained via
  const accessors need the mutable accessor.
- `Vector::ToUnifiedFormat(count, fmt)` deprecated → `ToUnifiedFormat(fmt)`
  (3 sites, warning today, will break later).

**Hot-path note**: the new accessors run `VerifyVectorType<T>` — a REAL check
(`throw InternalException`) in default release builds, compiled out only under
`DUCKDB_DEBUG_NO_SAFETY`. Our materialization takes one `GetData` per column
per chunk (per [[feedback-no-per-value-path]]), so the cost is per-column, not
per-value — hoisting the accessor out of row loops is now mandatory, not
stylistic. `GetDataUnsafe` / `GetDataMutableUnsafe` skip the check; reach for
them only where a measurement shows the checked form on a per-value path.

### Cluster C — table-filter hierarchy renamed (~60 errors)

`ConstantFilter`, `ConjunctionAndFilter`, `ConjunctionOrFilter`, `InFilter`
no longer exist under those names — the compiler suggests
`LegacyConjunctionOrFilter`, i.e. duckdb renamed the old hierarchy `Legacy*`
and introduced a new filter representation. `TableFilterSet` is also an
incomplete type at our include points (header moved) and its `filters` member
went private.

Files: `filter_encoder.hpp` (36) + `filter_encoder.cpp` (22),
`table_scan.cpp`, `mssql_optimizer.cpp`.

Phase 1 (this spec): mechanical — follow the renames (`Legacy*`), fix
includes, use accessors. Phase 2 (follow-up spec): adopt the NEW filter
representation, which is where any new pushdown capability will surface;
tracked separately because it changes what we can push, not just how we
compile.

### Cluster D — bound-expression member privatization (~20 errors)

`BoundColumnRefExpression::binding`, `BoundReferenceExpression::index`,
`BoundFunctionExpression::bind_info`, `ConstantExpression::value` are private;
accessors required. Some expression classes also moved headers
(`BoundCaseExpression`, `BoundBetweenExpression`, `BoundOperatorExpression`
unknown at current include points). Files: `mssql_optimizer.cpp`,
`filter_encoder.*`, `copy_function.cpp`.

### Cluster E — scatter (~40 errors)

- `StringVector` relocated (7× undeclared; `StructVector` suggestion nearby;
  `StringVector::GetEntries` moved).
- `dtime_t.micros` removed (datetime codec).
- `TableIndex` strong type replaces raw `idx_t` (no implicit conversion to
  `unsigned long long`).
- `LogicalType::SetAlias` → `WithAlias`.
- `TableFunction` constructor signature changed.
- Header moves: `ExpressionExecutor`, `TableFilterSet` incomplete at use.
- A `time_point` vs `TimePoint` mismatch and `ElapsedMs` overload (profiler
  surface — identify at fix time).
- `CreateSchemaInfo`/`DropInfo`/`CreateTableInfo` field renames (overlaps A).

### Cluster F — runtime contracts invisible to the compiler

Found by the test suite, not the error log — the reason step 7 exists:

- **`ScalarFunction::SetFallible()`**: a scalar function that throws at
  execution time without declaring `FunctionErrors::CAN_THROW_RUNTIME_ERROR`
  has its error converted to `INTERNAL Error: ... the function is not marked
  as fallible` (caught by `tds_connection/ping.test`, the one red test in the
  first green build). Every mssql scalar function that reaches a server or
  validates a handle throws by design; all 16 registrations now pair
  `SetFallible()` with the existing `SetVolatile()`.
- **Expression restructuring is semantic, not just naming**: comparisons,
  casts and BETWEEN are `BoundFunctionExpression`s in 2.0 (their old classes
  survive only as static helper structs — `BoundComparisonExpression::
  IsComparison/Left/Right`, `BoundCastExpression::IsCast/Child/TargetType`,
  `BoundBetweenExpression::Input/LowerBound/UpperBound`). The filter
  encoder's dispatch switched from ExpressionClass cases to type checks
  inside the `BOUND_FUNCTION` arm; pushdown equivalence is exactly what the
  filter-pushdown test files must confirm at step 7.

### Cluster G — found by the live integration suite

- **Vectors carry their own size** (`Vector::size()` from the buffer), and
  every consumer keyed on it — `VectorOperations::IsNull`, filter execution,
  the hash join — sees an empty vector unless the producer sets it. The
  deprecated `SetCardinality` sets only the chunk's logical count. All 15
  producer sites moved to `SetChildCardinality`, plus explicit
  `FlatVector::SetSize` on the rowid-STRUCT target vectors (they are not in
  `chunk.data`). One root, five distinct symptoms across 9 red tests.
- **The filter combiner pushes predicates as `ExpressionFilter`s with
  `BoundReference(0)`** (the column travels in the filter's slot, not in the
  expression). Encoder support added (the `filter_column` context). And
  because 2.0 does NOT re-apply TableFilters behind a `filter_pushdown`
  scan, a refused filter now silently returns wrong rows — closed by the
  client-filter net: refused filters run client-side per chunk via their
  `ToExpression` + `ExpressionExecutor::SelectExpression`, with a fetch loop
  so a filtered-to-empty chunk does not end the scan.
- **`LogicalInsert::column_index_map` is no longer populated**: the binder
  expands the insert child to every physical column, moving an unnamed
  column's `bound_defaults` slot into the projection (leaving it null) and
  keeping a named column's slot alive. PlanInsert now reads that tell —
  unnamed columns stay out of the generated column list, so identity and
  server-side defaults keep working; the batch serializer addresses the
  (always full-width) chunk by table ordinal.
- **Writer-scheduling dilution, not a code bug**: 2.0 parallelizes small
  scans across sink threads, so a 110k-row COPY split over 8 writers puts
  every writer below the 102400 columnstore threshold and nothing
  compresses. The threshold test pins one writer. Follow-up worth a spec:
  lazy writer ramp-up (spawn writer N+1 only once writer N has buffered a
  full flush batch) would keep compression for small loads without giving
  up parallelism on big ones.

## Out of scope

- Adopting the new (non-Legacy) filter representation — phase 2, own spec.
- Any behavior or performance change beyond what the API forces. Bench gate
  below exists to prove the "no change" claim.
- The `duckdb-v1.5.5` branch: untouched by construction.

## Verification

1. `GEN=ninja make` + `make test` green against the pinned duckdb SHA.
2. Integration suite (`make integration-test`) against the docker server.
3. Read/write bench sanity vs the 1.5.5 numbers (interleaved same-session A/B
   per [[bench-harness-and-protocol]]) — guards against `VerifyVectorType` or
   the new accessors sneaking into a per-value path.
4. CI on this branch builds the 2.0 submodule automatically (pin travels with
   the branch).
5. `CLAUDE.md` + `DATAMODEL.md`: compat-shim description replaced by the
   2.0-only statement and the `duckdb-v1.5.5` branch pointer.

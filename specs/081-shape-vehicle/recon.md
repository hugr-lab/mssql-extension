# Spec 081 recon: a vehicle that trusts a given shape; the DuckLake attach cost

Local branch `spec/081-shape-vehicle` (from `spec/079-e2`), not pushed until
#406 merges. Owner's plan (2026-10-05): its own PR after #406, before spec 080.

## Part 1 — the DuckLake attach regression (#406 bench)

Measured by the mssql-ducklake session (duckdb `4af9740da4` + ducklake PR
duckdb/ducklake#1519 + #406 @ `e0f4380`, 1000 tables x 40 columns, catalog
format 1.1-dev1): reattach 4.45 / 4.78 s with pushdown on vs 2.77 / 2.56 s off
(1.6-1.8x on every reattach phase; everything else within +-3%, totals 1.01x).
Attach-only probe: 5.65 s vs 3.14 s, `acquire_count` 4 -> 5 in both arms, so
no extra round trip and no describe: client CPU.

Found (2026-10-05), with the attach's statement list from mssql-ducklake (27
statements per attach, none per table): statement 17 is a `UNION ALL` of one
`(SELECT … FROM CAT.ducklake_inlined_data_<t>_<v> LIMIT 0)` per inlined-data
table, ~1000 branches on the bench catalog. A top-level set operation goes as
its parts, and `PushesMoreThanScan` counted any LIMIT as a gain, so each
branch became its own vehicle, `SELECT TOP (0) …`: a describe and an execution
each, on the attach transaction's pinned connection (which `acquire_count`
does not see -- hence "identical acquires"). DuckDB plans an `Empty Result`
for those branches and asks the server nothing. Measured on 200 branches in a
transaction: 370-550 ms pushed vs 160-180 ms off before; 180 ms vs 154-212 ms
after. Fix: a constant `LIMIT 0` is no gain (the first commit of this PR).

The rewriter's bottom-up walk (`SupportsPushdown(QueryNode)` per node whose
references resolve to one remote catalog; a refused node is not offered
again) was not the cost: the catalog-load statements with `LIST({…})` are
refused early, and an imitation of them plans within a few ms of pushdown off.

## Part 2 — the shape-trusting vehicle

Today (`src/mssql_functions.cpp`): `MSSQLScanBind` / `MSSQLScanParamsBind` ->
`BindDescribedScan` (describe cache, else `sp_describe_first_result_set`, else
run at bind) -> `ApplyColumnTypes` (`column_types := [...]`, each checked
against the describe with `ColumnTypeFits`). `MSSQLScanInitGlobal` runs the
batch and checks the stream against the described shape
(`StreamMatchesBoundShape`, `StreamMatchesDatetime2`).

Design sketch:

- `mssql_scan_unsafe(context, query, columns := {'name': 'TYPE', …})` and
  `mssql_scan_params_unsafe(context, statement, params [, declarations],
  columns := {…})`. Bind returns `columns` as the result (names in STRUCT
  order, types parsed as `column_types` are), touching no server: no
  connection, no describe. `prepared := true` refused (its handle is made at
  bind).
- Init: run as today; then the stream must have as many columns, and each
  must fit its declared type by the same rule `column_types` uses today,
  checked against the STREAM instead of the describe:
  `ColumnTypeFits(stream type, declared type, stream column is datetime2)`.
  Otherwise a loud error naming both shapes. Lossless widenings are a later
  question.
- The writer: `WrittenQuery::column_types` already holds the type of every
  column whose shape it knows (table columns through the catalog, COUNT /
  SUM / MIN / MAX, ranking windows); a server-typed computed column leaves
  `''`. When none is `''`, `VehicleFor` calls the `_unsafe` function with
  `column_names` / `column_types`; otherwise the describing vehicle (with the
  E2 cache) as now. To measure: the share of pushed statements in the suite
  and in DuckLake's reads whose shape is fully known.
- Users: their own T-SQL whose shape they know (mssql-ducklake's statements
  pay a describe today, inside transactions too).

Measured (2026-10-05): a temporary log line in `VehicleFor` over the
`test/sql/pushdown` suite -- 297 vehicles:

| what the writer knows | vehicles |
|---|---|
| every column's type (`column_types` has no `''`) | 229 (77%) |
| every column's WIRE type, counting the cast-back columns (an integer `sum`: decimal(38,0) on the wire, HUGEINT after the projection) | 33 (11%) |
| a server-typed computed column (arithmetic, CASE, `lag` over an expression) | 35 (12%) |

So about 88% of pushed statements can skip the describe entirely, once the
writer also hands over the wire type of a cast-back column (`WrittenQuery`
keeps `column_types` = the read-as type or INVALID for a cast-back column,
`cast_types` = the type after the projection; a third list, the type the
stream decodes into, is what the vehicle needs). The rest keep the E2 cache.

The argument form has a precedent: `read_csv(…, columns := {'name': 'TYPE',
…})` -- a keyword-only parameter of type ANY that must be a STRUCT, field
names in order, values parsed with `TransformStringToLogicalType` (which
already parses our `MSSQL_VARCHAR(n, 'collation')` labels for
`column_types`). The init-time check needs no new rule: today
`ApplyColumnTypes` checks each `column_types` entry against the DESCRIBE with
`ColumnTypeFits(described, wanted, datetime2)`; the trusting vehicle checks
the same predicate against the STREAM (its type, and whether its TDS type is
datetime2), plus the column count. A string into any string label fits (the
values are strings either way); an INTEGER declared where the stream says
BIGINT does not, and fails loudly.

Plan (commits after 1/n):

- 2/n: `mssql_scan_unsafe(context, query, columns := {…})` and
  `mssql_scan_params_unsafe(context, statement, params [, declarations],
  columns := {…})`: bind returns `columns` and touches no server (no
  connection, no describe; `prepared := true` refused); init runs the batch as
  today and checks the stream against `columns` (count, `ColumnTypeFits` per
  column, datetime2 from the stream's metadata). Tests: a right shape, a wrong
  type, a wrong count, inside a transaction with two of them in one statement
  (the case that fails when a scan runs at bind), pool stats showing one
  acquire per execution and none at bind.
- 3/n: the writer hands over the wire type of every column it knows, and
  `VehicleFor` calls the `_unsafe` function when it knows all of them; a
  server-typed column keeps the describing vehicle and the E2 cache. Tests:
  EXPLAIN names the function; the first execution of a new shape costs one
  acquire (was two).
- 4/n: docs (functions page, CLAUDE.md, DATAMODEL, CHANGELOG, a short spec).

Settled in 2/n: the check is strict (lossless widenings later, if asked for);
a declared type no SQL Server column is read as (TINYINT, a LIST, a STRUCT) is
refused at bind, before anything is sent; EXPLAIN says `Shape: given
(columns :=)`; a GEOMETRY column must arrive as WKB (`STAsBinary()`), as for
`column_types`. The optimizer counts the two new functions as raw scans: a
catalog scan beside one in a transaction materialises (uncounted, a UNION ALL
of the two found the pinned connection Executing).

3/n as built: `VehicleFor` takes the `_unsafe` form only when every
`column_types` entry is a type (the 77% row). A cast-back column keeps the
describing vehicle: the writer knows its wire type exactly only for an integer
`sum` (decimal(38,0)); a decimal sum or product over cast-back operands is
widened by the server in ways the writer does not model, and the strict check
would turn a statement that works today into an error. A typed statement over
a table changed behind the catalog now fails at execution rather than at bind
(the same failure, later), naming the statement and `mssql_invalidate_cache()`,
and clears the describe cache as the describing path does.

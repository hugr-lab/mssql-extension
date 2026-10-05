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

Open questions: the STRUCT form vs two lists for `columns`; whether the
`_unsafe` name or a `columns :=` keyword on the existing functions (the
owner named the functions `_unsafe`); how EXPLAIN shows it.

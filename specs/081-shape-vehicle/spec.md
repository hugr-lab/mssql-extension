# Spec 081: a vehicle that trusts the shape it is given

Status: implemented on `spec/081-shape-vehicle` (1/n..4/n). Research and
measurements: [recon.md](recon.md). Origin: spec 079's after-0.3.0 list,
moved before spec 080 by the owner (E2).

## Problem

Since spec 075 `mssql_scan` / `mssql_scan_params` describe at bind
(`sp_describe_first_result_set`) and run at init. Every pushed statement
(spec 079) and every caller's own T-SQL pays that round trip, plus a
connection, at bind, even when the caller knows the shape. Running at bind
instead is not the way out: DuckLake runs its reads in transactions, and
several scans bound there contend for the one pinned connection.

Separately, DuckLake's attach probes every inlined-data table with a UNION ALL
of `LIMIT 0` branches; each was pushed and paid a describe and an execution
(1.6-1.8x slower attaches on 1000 tables).

## Decisions

- **D1 -- `LIMIT 0` is no gain** (1/n). `PushesMoreThanScan` answers no for a
  constant zero row count: DuckDB plans an empty result and asks nothing.
- **D2 -- the unsafe functions** (2/n).
  `mssql_scan_unsafe(context, query, columns := {...})` and
  `mssql_scan_params_unsafe(context, statement, params [, declarations],
  columns := {...})`. `columns` is a STRUCT of type names (read_csv's form).
  Bind returns it and touches no connection. Init runs the batch and checks
  the stream: count, and per column `ColumnTypeFits(stream, declared,
  datetime2)` -- the rule `column_types` follows: strict on kind, no conversion
  (a string's length and collation, and which TIMESTAMP variant a datetime2
  is read into, are not checked). A
  mismatch is a loud error naming the statement, never a conversion. A
  declared type no SQL Server column is read as is refused at bind.
  `MSSQLOptimizer` counts both as raw scans.
- **D3 -- the writer's shape** (3/n). `VehicleFor` uses the `_unsafe` form when
  every `WrittenQuery::column_types` entry is a type (77% of the pushdown
  suite). A cast-back or server-typed column keeps the describing vehicle and
  the E2 cache. A typed statement over a table changed behind the catalog
  fails at init instead of bind -- the same failure -- naming the statement and
  `mssql_invalidate_cache()`, and clears the describe cache.

## Not done

- Cast-back columns (11%): the wire type is exact only for an integer `sum`.
- Lossless widenings at the check (int declared, smallint sent).
- The rewriter dry run's per-node CPU on plans with many remote scans
  (0.63 s vs 0.016 s on 1000 `LIMIT 0` branches): profiled separately.

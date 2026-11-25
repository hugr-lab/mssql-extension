# Ephyra property tests for mssql-extension

Driver: each case is one script fed to the built `build/release/duckdb -unsigned`
(a fresh process: a crash or hang kills only it), against the docker SQL Server
(`make docker-up`, port from `.env`). Database `EPH_DB` (default `ephyra0`).

## Properties and where "correct" comes from

1. **qdiff** — a SELECT over catalog tables returns the same rows and column types
   whether it is pushed whole (`remote_pushdown` on, catalog `a`), read through the
   catalog scan with filter pushdown (`remote_pushdown false`, catalog `b`), or run
   by DuckDB on a local copy (`l`). Source: CLAUDE.md, `mssql_remote_pushdown`:
   "a result column has DuckDB's type or is not pushed", every divergence is "a veto";
   filter pushdown: a filter the server would answer differently "is never pushed".
   Documented exception (not generated or masked): D4 for sets — GROUP BY / DISTINCT /
   join keys on strings follow the column collation with pushdown on.
2. **crash** — no INTERNAL / FATAL error, no signal, no hang (> 60 s) on valid input.
3. **roundtrip** — values written through DuckDB (INSERT statements, INSERT BULK,
   CTAS, COPY) read back equal. Source: spec 045 "round-trip losslessly", the type map.
4. **dmldiff** — a sequence of UPDATE / DELETE / INSERT / MERGE applied to a catalog
   table and to a local DuckDB table leaves the same rows, on every path
   (`mssql_dml_pushdown` on/off, `mssql_dml_stage_threshold` 0). Source: CLAUDE.md DML
   section (vetoes "keep the shipped path"; nothing may change the answer).

## Triage log

- `rows:a` `c3 <> 'a '` (seed 16233672520444968484:11): **intended**. README "A note on string
  comparisons" / #272: a pushed predicate follows the column's collation and padding. Excluded by
  generating binary collations only and no trailing-space values.
- `roundtrip:*:TIMESTAMP_NS` `…07.123456789`: **test bug** — datetime2(7) holds 100 ns; value removed.
- `roundtrip:*:DOUBLE` `5e-324`, `1e-310` -> 0, and `-0.0` -> `0.0` (FLOAT and DOUBLE), on the bulk and the
  statement path alike: **environment** — SQL Server's `float` flushes subnormals and negative zero
  (stored bytes are zero; `real` subnormals survive). Not refused by name the way NaN / inf are. Values removed.
- CTAS over the bulk path with a NaN: the error is the server's ("OLE DB provider 'STREAM' ... returned invalid
  data for column '[!BulkInsert].c0'") where the statement path names it ("DOUBLE NaN values cannot be
  rendered"). Error wording only; minor.
- **BUG 1** `dml:merge:*:server_error` (first seen seed of run 2, shrunk): `MERGE … WHEN MATCHED THEN UPDATE SET
  bitcol = <boolean expression>` (a comparison, AND / OR, IS NULL over a target column) fails with
  `SQL Server error 102: Incorrect syntax near '>'` — the predicate is written where T-SQL needs a value.
  Pushed (source in the catalog) and staged (`mssql_dml_pushdown = false`, local source) alike; the same
  value in an UPDATE works. Nothing is written. Masked in the generator (MERGE bit assignments: column or
  constant only) so the search goes on.
- `dml:*:missing_error:division_by_zero`: **test** — DuckDB evaluates `x % 0` for rows a false condition
  excludes, the server does not. Masked.
- `dml:*:txn:*` (first run): **test bug** — the stderr marker is an error and aborted the transaction.
- `dml:insert_select:*:violation_of_primary`: **test bug** — key offsets collided.
- `dml:merge:v0:rows` `日本` -> `??`: **environment** — a code-page varchar column cannot hold it.
- **BUG 2** `dml:delete:v3:server_error:…collation` (also a plain SELECT, default settings): a WHERE comparing
  two string columns of different collations is pushed by the catalog scan's filter and fails with error 468
  (457 through a CASE). Repro: `repro/filter_two_collations.test`. Still generated: qdiff / dmldiff will find it again.
- **BUG 3** `err:o:only:…column_has` : `order_pushdown true` + `ORDER BY k, k` -> error 169.
  Repro: `repro/order_pushdown_duplicate_key.test`.
- BUG 1 repro: `repro/merge_set_bit_boolean_expression.test`.
- `dml:merge:*:missing_error:overflow`: **test** — eager DuckDB evaluation of a branch no row takes. Masked.

## Runs (4128e1ac, DuckDB v2.0.0-dev89484, SQL Server 2022 container)

| property | cases | result |
|---|---|---|
| qdiff | 600 + 400 + 3x150 | clean until the pushable mode / order variant; then BUG 3 |
| roundtrip | 300 + 3000 | clean after the three exclusions above |
| dmldiff | 200 + 300 + 400 + 2500 | BUG 1, BUG 2 |

Push rate of qdiff's queries: 9 of ~525 pushed whole before the pushable mode, 36 after (still low: next step).

## How to run

    make docker-up                       # repo root
    cd test/ephyra
    EPH_DB=ephyra0 cargo test --test qdiff -- --test-threads=1
    EPH_DB=ephyra1 cargo test --test roundtrip -- --test-threads=1
    EPH_DB=ephyra2 cargo test --test dmldiff -- --test-threads=1
    # EPHYRA_TESTS / EPHYRA_SEED / EPHYRA_REPLAY as in ephyra's README; details of each
    # failure land in .ephyra/findings/<signature>.txt, refused writes in .ephyra/refusals/

# Property-based testing of mssql-extension with Ephyra: report

Tested: `main` at 4128e1ac (release build of 2026-11-20, DuckDB `v2.0.0-dev89484`,
extension 0.3.0) against the repo's docker SQL Server 2022 (`make docker-up`).
Not re-run on later commits.

## What this is

[Ephyra](https://github.com/datadog-labs/ephyra) is a property-based testing
library: a generator builds random but valid inputs, a property says what must
hold for all of them, and a failure is shrunk to a small input that replays from
a seed. This directory is a small Rust crate that applies it to the extension.
It is not part of the build or of CI, and nothing in the extension depends on it.

Each generated case becomes one SQL script fed to `build/release/duckdb` in a
fresh process (so a crash or a hang ends only that case), and the outputs of the
variants are compared.

| Property | File | What must hold |
|---|---|---|
| `qdiff` | `tests/qdiff.rs` | A SELECT over catalog tables returns the same rows and column types with remote pushdown on, with it off (filter pushdown only), with ORDER BY pushdown on, and when DuckDB runs it on a local copy of the tables. |
| `roundtrip` | `tests/roundtrip.rs` | Values written through DuckDB (CTAS and INSERT, bulk and statement paths) read back equal, or the write is refused by name. |
| `dmldiff` | `tests/dmldiff.rs` | A sequence of UPDATE / DELETE / INSERT / MERGE leaves a catalog table and a local DuckDB table with the same rows, under four setting variants and inside a transaction. |
| crash | all three | No INTERNAL / FATAL error, no signal, no hang. |

`NOTES.md` has the source of each expectation, the full triage log and the run commands.

## Bugs found

Each has a sqllogictest in `repro/` that states the expected behaviour and fails
on 4128e1ac. They are outside `test/sql` on purpose, so the suite stays green;
they move there with the fixes.

1. **A WHERE comparing two string columns of different collations fails with
   error 468, on default settings** (`repro/filter_two_collations.test`).
   `SELECT k FROM db.dbo.t WHERE v < u` gives `Cannot resolve the collation
   conflict …`; the same WHERE in a DELETE fails too. The catalog scan pushes the
   filter. The remote-pushdown writer already vetoes this case.
2. **`MERGE … WHEN MATCHED THEN UPDATE SET bitcol = <boolean expression>` fails
   with `SQL Server error 102: Incorrect syntax near '>'`**
   (`repro/merge_set_bit_boolean_expression.test`). A comparison, AND / OR, or
   IS NULL (error 156); pushed and staged MERGE alike. The same value in an
   UPDATE works. Nothing is written.
3. **With `order_pushdown true`, `ORDER BY k, k` fails with error 169**
   (`repro/order_pushdown_duplicate_key.test`). Opt-in setting only.

No existing issue was found for any of them. None was filed.

## Looked like bugs, triaged as not

- Pushed string comparisons follow the column's collation and pad trailing
  spaces: documented (README, #272). The generator uses binary collations and no
  trailing spaces.
- Subnormal doubles and `-0.0` are stored as `0`, and a code-page `varchar` turns
  `日本` into `??`: SQL Server's own behaviour. Unlike NaN and infinity, neither
  is refused by name.
- A CTAS over the bulk path with a NaN reports the server's raw `OLE DB provider
  'STREAM' …` message where the statement path names the value: wording only.

## Runs

| Property | Cases | Result |
|---|---|---|
| `qdiff` | about 1,600 | bug 3 |
| `roundtrip` | 3,300 | clean after the exclusions above |
| `dmldiff` | 3,400 | bugs 1 and 2 |

## Limits

- `qdiff` is weaker than its case count suggests: in the last measured run about
  36 of 525 generated queries were pushed whole to the server; the rest were
  vetoed and ran through the catalog scan. Raising that share is the next step.
- Bug 1 is still generated, so `qdiff` and `dmldiff` find it again until it is
  fixed or masked. Bug 2 is masked in the MERGE generator.
- Not covered yet: COPY, larger row counts, window functions and set operations
  under pushdown, the backup reader.
- Ephyra is a git dependency with no pinned revision beyond `Cargo.lock`, and
  its API is not frozen.

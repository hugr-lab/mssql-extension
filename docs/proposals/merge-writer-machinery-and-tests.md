# Merging the parallel-writer machinery, and consolidating the write tests

Follow-up to spec 057, scoped as its own PR. **Do this BEFORE
[closing the columnar gaps](columnar-write-close-the-gaps.md)** — that work
touches the kernels, this touches the two sinks, and landing this first means the
cursor PR modifies one implementation instead of keeping two in step.

## Part 1 — one writer implementation, not two

COPY and CTAS grew parallel bulk-load writers in the same week and in parallel,
and the two copies have already drifted. Duplicated today:

| piece | COPY | CTAS |
|---|---|---|
| `TryStartLocalWriter` | `copy_function.cpp:609` | `mssql_physical_ctas.cpp` (47 lines) |
| `FinishLocalWriter` | same file | same file (17 lines) |
| writer-limit resolution (setting, else `NumberOfThreads` capped at 8) | `copy_function.cpp:551-570` | `mssql_physical_ctas.cpp` `GetGlobalSinkState` |
| local sink state: connection, writer, pool_handle, rows_in_batch, rows_written, rows_confirmed, init_attempted, destructor | `copy_function.hpp` | `mssql_physical_ctas.hpp` |
| the parallel branch in `Sink` — write, flush at the threshold, re-execute `INSERT BULK`, re-send COLMETADATA | both | both |
| folding per-thread counts in `Combine` | both | both |

Known drift already: CTAS never sends `ROWS_PER_BATCH` in its `INSERT BULK`;
the `INSERT BULK` text is built in two places; the debug logging differs.

**This is not a size argument.** Measured: removing one copy saves roughly 150
lines out of the branch's 9915, about 1.5%. It is a drift argument — six ways
apart after one week is the rate.

### The design constraint that makes it more than an extraction

The two operators **deliberately differ**, and the shared abstraction has to
express that rather than erase it:

- **COPY** keeps the transaction's pinned connection and exactly one writer
  inside an explicit transaction. It may be loading into a table that already
  existed, whose rows it cannot undo, so the transaction has to own the load.
- **CTAS** never pins and keeps its N writers, in or out of a transaction,
  because the table is its own and the undo is dropping it.

So the seam is roughly "who supplies the first connection, and may there be more
than one" — not "here is a writer pool". Getting that boundary wrong reintroduces
the bug spec 057 spent a week removing. That is also why this deserves its own
review: in a PR that also changes behaviour, a reviewer cannot separate what
moved from what changed, and these two files carry the riskiest code on the
branch.

## Part 2 — consolidating the write tests

`test/sql/copy/` is 29 files / 4266 lines and `test/sql/ctas/` is 12 / 1771.
Several clusters cover one subject across several files, each paying its own
`ATTACH` (a full TCP connect + LOGIN7) and its own table setup:

| cluster | files | lines |
|---|---|---|
| TABLOCK | `copy_auto_tablock`, `tablock_by_target_shape`, `ctas_auto_tablock` | 342 |
| string length / collation | `copy_varchar_length`, `copy_nvarchar_length_validation`, `string_bound_truncation`, `copy_varchar_collation`, `ctas_varchar_collation` | 736 |
| temp tables | `copy_temp`, `copy_existing_temp`, `copy_empty_schema` | 452 |
| type coverage | `copy_types`, `columnar_encode_all_families`, `ctas_types`, `compatible_type_conversion` | 850 |
| parallel writers | `parallel_writers`, `ctas_parallel_writers` | 329 |
| both-paths byte equality | `time_rounding_both_paths`, `smalldatetime_write`, plus the row-path section of `string_bound_truncation` | ~300 |

Two of these are worth merging for a reason beyond tidiness:

- **parallel writers** — the two files are near-identical and will be testing
  *one* implementation after part 1. Keeping two invites them to drift the way
  the code did.
- **both-paths byte equality** — all of these are the same experiment: load the
  same values twice, once so every column resolves to a kernel and once with a
  column that drops the chunk to row-major, then require the two to agree. One
  file with a section per family makes the pattern explicit and makes "add the
  new family here" the obvious move. Today a new family gets tested by whoever
  remembers the pattern exists.

### What not to merge, and the cost of merging

- **A merged file stops at its first failure**, so later sections do not run. A
  cluster that fails often is worse as one file, not better. TABLOCK and type
  coverage are the safe ones; anything touching connection counts is not.
- **`copy_connection_leak` must stay alone.** It counts pool connections, so it
  needs a clean pool and a pinned writer count. Merging it into anything makes
  its numbers depend on what ran before.
- **Files whose value is isolation** — the parallel tests need their own
  `SET threads`, and a merged file's setting would leak into unrelated sections.
- Do not merge across `copy/` and `ctas/` directories purely by subject. The
  directory split is how the suite is filtered when only one operator changed.

### Worth fixing at the same time

- Four COPY files gate on `MSSQL_TEST_SERVER` and had never run until this
  branch exported it. **Audit every `require-env` in the suite** for others that
  nothing sets — the same class as issue #192, and a green suite reporting
  "skipped" is indistinguishable from a green suite that ran.
- The suite creates and drops its tables by hand in every file. A shared setup
  fragment would cut round trips, but only where it does not make a file's
  preconditions invisible.

## Sequencing

1. this PR — merge the writer machinery, consolidate the safe test clusters;
2. then [columnar-write-close-the-gaps](columnar-write-close-the-gaps.md) — the
   kernel work, landing on one sink implementation and one both-paths test file.

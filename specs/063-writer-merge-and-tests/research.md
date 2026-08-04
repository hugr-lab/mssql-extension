# Spec 063 — reconnaissance

Research for the PR that merges the parallel-writer machinery and consolidates
the write tests. Branched from `spec/057-write-recon`, because the machinery
being merged only exists there.

Three parts: the writer duplication (A), the test suite (B), and the open
contributor PR #232 (C).

What to **build** from this is in [`spec.md`](spec.md); this file is the
evidence it rests on.

---

## A. The writer machinery

COPY and CTAS grew parallel bulk-load writers in the same week, in parallel. The
copies have drifted, and the drift is not cosmetic.

### A.0 One piece is already shared — and it is the model

`ReleaseBcpConnectionOnError` (`mssql_connection_provider.cpp:247`) is the
mid-bulk-load release protocol: close the session so the INSERT BULK transaction
rolls back and the target's locks drop, then either leave the pin alone or return
the connection with `SetNeedsReset`. Both operators' local **and** global state
destructors call it, and it takes no `ClientContext`, so it is safe on a worker
thread (issue #178).

One function, the contract written down once, the operator supplying the one
parameter that genuinely differs (`transaction_pinned`). That is the shape the
rest of this should reach.

### A.1 Inventory

Line references verified 2026-08-04.

| piece | COPY | CTAS |
|---|---|---|
| `TryStartLocalWriter` | `copy_function.cpp:609-659` | `mssql_physical_ctas.cpp:157-203` |
| `FinishLocalWriter` | `copy_function.cpp:663-686` | `mssql_physical_ctas.cpp:207-223` |
| writer-limit resolution | `copy_function.cpp:546-570` | `mssql_physical_ctas.cpp:101-128` |
| local state + destructor | `copy_function.hpp` `MSSQLCopyLocalState` | `mssql_physical_ctas.hpp` `MSSQLCTASLocalSinkState` |
| parallel branch in the sink | `copy_function.cpp:723-767` | `mssql_physical_ctas.cpp:266-316` |
| `INSERT BULK` text | built inline, `copy_function.cpp:~490-528` | `CTASExecutionState::BuildInsertBulkSql` |

### A.2 Where they differ — classified

**Dangerous, and currently masked:**

1. **COPY acquires through `ConnectionProvider::GetConnection`, CTAS through
   `pool.Acquire()`.** Inside a transaction the provider returns the **pinned**
   connection. COPY only survives this because `parallel_writer_limit` is forced
   to 1 when pinned, so `TryStartLocalWriter` returns before it ever asks. Change
   that limit rule and N threads receive the same pinned connection. The merged
   version must take the pool explicitly and never route a *parallel* writer
   through the provider.
2. **COPY's error path releases with `GetConnectionPool().Release(conn)` a
   connection it acquired from the provider.** Correct only because the
   connection is never the pinned one — see above. Acquire and release must pair.

**Behavioural drift:**

3. **CTAS never sends `ROWS_PER_BATCH`.** COPY appends it whenever
   `flush_rows > 0` (`copy_function.cpp:509-516`); `BuildInsertBulkSql` emits
   only `WITH (TABLOCK)`. The hint tells the server the batch size up front.
4. **No interrupt check anywhere in CTAS.** Not just the parallel branch:
   `IsInterrupted` appears **0** times in `mssql_physical_ctas.cpp` against
   **5** in `copy_function.cpp` (counted 2026-08-04), where it is tested at sink
   entry, after encoding and after the flush. Ctrl+C on a CTAS is noticed late
   whether it went parallel or not.
5. **Error propagation is a different mechanism each side.** COPY: `has_error` +
   `error_mutex` + `error_message`, checked at every sink entry. CTAS:
   `load_failed` + `phase` under `mutex`, and the entry check only fires when
   `mssql_ctas_drop_on_failure` is set (deliberate — see spec 057 — but the two
   need one vocabulary).
6. **Counters exist only on the COPY side**: `counter_sink_ns`,
   `counter_encode_ns`, `counter_flush_ns`, `counter_sink_calls` — **20**
   occurrences in `copy_function.cpp`, **0** in either CTAS file. A CTAS reports
   nothing, so `MSSQL_COUNTERS=1` measures one of the two write paths.
7. **Flush threshold**: COPY asks `config.ShouldFlushToServer(rows_in_batch)`;
   CTAS inlines `bcp_flush_rows > 0 && rows_in_batch >= bcp_flush_rows`.
8. **Accounting**: COPY uses atomics on the global state
   (`rows_sent`, `rows_confirmed`, `batches_flushed`); CTAS accumulates per
   thread and folds under the mutex in `Combine`. CTAS has no
   `batches_flushed` at all.
9. **Debug logging**: COPY logs writer start and fallback with the slot number;
   CTAS is silent, so "did it go parallel?" is unanswerable without timing it.

**Deliberate, and the abstraction must PRESERVE it:**

10. **Inside an explicit transaction COPY pins and uses one writer; CTAS does
    neither.** Not drift. COPY may be loading into a table that already existed,
    whose rows it cannot undo, so the transaction has to own the load. CTAS
    creates its table, so the undo is dropping it. See spec 057 and
    `ctas_transaction.test`.

### A.3 The seam

The difference in (10) is the whole design question. The abstraction is **not**
"a writer pool". It is closer to:

- *who supplies the first connection* — the provider (COPY, possibly pinned) or
  the pool (CTAS, never pinned);
- *may there be more than one* — a policy the operator answers, not the writer;
- everything after that — claim a slot, open `INSERT BULK`, write, flush at the
  threshold, re-open, DONE, release — is identical and belongs in one place.

Getting that boundary wrong reintroduces exactly what spec 057 spent a week
removing. That is also why this wants its own review: in a PR that also changes
behaviour, a reviewer cannot separate what moved from what changed, and these are
the two files carrying the riskiest code on the branch.

**Size is not the argument**: removing one copy saves ~150 lines of 9915 (1.5%).
Six divergences in one week is the argument.

---

## B. The test suite

`test/sql/copy/` is 31 files / 4294 lines; `test/sql/ctas/` is 12 / 1771
(recounted 2026-08-04, after spec 057's last two test files landed).

### B.1 The dormant-test audit — a second instance of the same bug

Spec 057 found four COPY files gated on `MSSQL_TEST_SERVER`, which nothing ever
set. Auditing **every** `require-env` in the suite finds the same thing again:

| variable | files | Makefile | CI | verdict |
|---|---|---|---|---|
| `MSSQL_TEST_CONNECTION_STRING` | **4** | yes (spec 057, `edd90f5`) | via Makefile | fixed |
| `MSSQL_TEST_SERVER` | 4 | yes (spec 057) | via Makefile | fixed |
| `MSSQL_COUNTERS` | 1 | `make counters-test` | no | intentional opt-in |
| `MSSQL_KERBEROS_TEST`, `MSSQL_WINSSPI_TEST`, `MSSQL_NAMED_INSTANCE_HOST` | 1 each | no | no | intentional, environment-specific |
| `AZURE_*` | 1–3 each | no | some via secrets | intentional |

The four files gated on `MSSQL_TEST_CONNECTION_STRING`:

- `test/sql/copy/copy_auto_tablock.test`
- `test/sql/ctas/ctas_auto_tablock.test`
- `test/sql/ctas/ctas_if_not_exists.test`
- `test/sql/catalog/ddl_if_not_exists.test`

**All four pass when the variable is set** (verified 2026-08-04). The first two
are the auto-TABLOCK tests — the behaviour spec 057 *rewrote* — so the tests for
that change had never executed. The C++ unit test caught the policy change; these
did not, because they were not running.

**Fixed on spec 057** (`edd90f5`): the Makefile now sets and exports
`MSSQL_TEST_CONNECTION_STRING` alongside `MSSQL_TEST_SERVER`. The same commit
fixed the widest instance of the class, which @oluies found — `make
integration-test` filtered on `[integration]`/`[sql]`, matching 8 of 172 files,
while the `"[mssql]"` the other 164 carry matches **nothing and exits 0**. The
suite went from 8 files / 304 assertions to 155 cases / 4988.

So what remains for 063 is not the fix but the **gate**: a check that every
`require-env` variable is set by the Makefile or a workflow, or is on an explicit
opt-in list. Three instances of one class in one spec is a rate, and the fourth
will look exactly like the first three. A suite reporting "skipped" — or a filter
matching nothing — is indistinguishable from one reporting "passed" unless
someone reads the count.

### B.2 Clusters worth consolidating

| cluster | files | lines |
|---|---|---|
| TABLOCK | `copy_auto_tablock`, `tablock_by_target_shape`, `ctas_auto_tablock` | 342 |
| string length / collation | `copy_varchar_length`, `copy_nvarchar_length_validation`, `string_bound_truncation`, `copy_varchar_collation`, `ctas_varchar_collation` | 736 |
| temp tables | `copy_temp`, `copy_existing_temp`, `copy_empty_schema` | 452 |
| type coverage | `copy_types`, `columnar_encode_all_families`, `ctas_types`, `compatible_type_conversion` | 850 |
| parallel writers | `parallel_writers`, `ctas_parallel_writers` | 329 |
| both-paths byte equality | `time_rounding_both_paths`, `smalldatetime_write`, the row-path section of `string_bound_truncation` | ~300 |

Two of these merge for a reason beyond tidiness:

- **parallel writers** — after part A they test *one* implementation. Two files
  invite the same drift the code had.
- **both-paths byte equality** — all of them are one experiment: load the same
  values twice, once so every column resolves to a kernel and once with a column
  that drops the chunk to row-major, then require agreement. One file with a
  section per family makes "add the new family here" the obvious move instead of
  something you have to remember the pattern for. This matters immediately: the
  next PR adds cursor variants for three families.

  And the reason is stronger than tidiness, because the failure already happened.
  Four of these tests used a `decimal(38,0)` column fed `1::HUGEINT` believing it
  had no scatter arm. Against an **existing** table it has one — the catalog
  reports DECIMAL, `INT128` is in the accepted set, the width already matches — so
  the lever resolved COLUMNAR and the files compared the columnar path with
  itself, passing for the wrong reason. Fixed in `b6c21d3` by switching to a
  signed `TINYINT` into SQL Server's unsigned `tinyint`, verified by instrumenting
  the path choice.

  One file means **one lever to re-verify** when the resolver changes — and the
  columnar-gaps PR changes it. If that PR makes signed `TINYINT → tinyint`
  columnar, every one of these tests goes vacuous again and keeps passing.

### B.3 What must NOT be merged

- **`copy_connection_leak`** counts pool connections; it needs a clean pool and a
  pinned writer count. Anything merged into it makes its numbers depend on what
  ran before.
- **The parallel tests** each need their own `SET threads`; a merged file's
  setting leaks into unrelated sections.
- **Across `copy/` and `ctas/` purely by subject** — the directory split is how
  the suite is filtered when only one operator changed.
- Remember the cost: **a merged file stops at its first failure**, so later
  sections do not run. Merge the stable clusters (TABLOCK, type coverage), not
  the ones that fail while being developed.

---

## C. PR #232 (contributor `oluies`) — version × charset matrix

`test/compat/mssql_version_matrix.sh` (+531) and `docs/TESTING.md` (+79/-1). No
production code, not wired into CI.

### C.1 The hypothesis, tested

The suspicion was that spec 057 has already covered this. **It has not, and the
premise of #232 is still exactly true today.** Verified 2026-08-04:

- every `docker-compose` in the repo (`docker/docker-compose.yml`,
  `docker/docker-compose.linux-ci.yml`) pins `mssql/server:2022-latest`;
- every workflow that stands a server up (`ci.yml:565`,
  `concurrency-tests.yml:45`, `release.yml:644`) pins the same image.

So "CI runs exactly one SQL Server version, and everything version-dependent is
verified there and nowhere else" holds. Spec 057 rewrote the **write** path;
#232 tests **login, read decode and charset** across **versions**. They do not
overlap.

### C.2 What HAS been covered since #232 opened, and by what

| #232 concern | now covered by | still not covered |
|---|---|---|
| UTF8SUPPORT changes the wire form but never the value | `test/sql/query/utf8_support.test` (issue #225) | only on 2022, where the feature EXISTS |
| a server that LACKS UTF8SUPPORT still logs in when we advertise it | — | **nothing**: unreachable with a single-version container. This is #232's most valuable row (2017) |
| DBCS collations (CP932/936/949/950) on read | `utf8_support.test:117-140` | not across versions |
| CP1252 / single-byte read decode | `collation_filter.test` (filters, not decode) | the decode itself — and **issue #224 is OPEN** for exactly this: `mssql_scan()` returns invalid UTF-8 for non-UTF8 collation char/varchar |
| UTF-8 collation on write | `copy_varchar_collation.test`, `ctas_varchar_collation.test` (spec 060 / #225) | 2017, which has no UTF-8 collations at all |
| per-row wire cost | `MSSQL_COUNTERS` (spec 057 step 0b) | the script's own per-row normalisation is still the only cross-version comparison |

### C.3 Recommendation

**Do not close #232 as superseded — it is not.** What can be said honestly:

1. Its charset round-trip on **2022** now overlaps `utf8_support.test`, so that
   part is redundant with the suite.
2. Its **version axis** is not covered by anything and is the reason it exists.
3. Its 2017 result — advertising UTF8SUPPORT to a server that lacks it is
   harmless — is a real finding recorded nowhere else in the repo.

Options, in the order they should be considered:

- **Take it as-is** into 063, as a local dev tool under `test/compat/`. It costs
  nothing to carry (no production code, not in CI), and it is the only way to
  reach the version axis. Ask the contributor to rebase.
- **Trim the 2022-only charset round-trip** to avoid duplicating
  `utf8_support.test`, keeping the version probe and the login check.
- **Record its 2017 finding** in `docs/TESTING.md` regardless of what happens to
  the script, so the one durable result survives.

What #232 does **not** address and 063 should not pretend it does: **issue #224**
(invalid UTF-8 from a non-UTF8 collation in raw `mssql_scan`) is open, and the
script's `roundtrip` compares columns against `v_nvarchar` — it would *detect*
#224 on any version, but fixing it is read-path work, not this PR.

---

## Proposed scope for 063

1. Merge the writer machinery, preserving the pinned/unpinned difference as an
   explicit policy rather than an accident (A.3), and close drifts 3–9.
2. A lint gate for `require-env` variables nothing sets (B.1). The export itself
   already landed on spec 057.
3. Consolidate the parallel-writer and both-paths test clusters; leave the rest
   (B.2, B.3).
4. Decide #232 with the contributor on the evidence in C, rather than closing it
   as superseded.

Sequenced **before**
[`docs/proposals/columnar-write-close-the-gaps.md`](../../docs/proposals/columnar-write-close-the-gaps.md),
so that work lands on one writer implementation and one both-paths test file.

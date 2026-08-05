# Spec 063 — One bulk-load writer, and a write suite that tests it once

Follow-up to spec 057 (PR #234, `7804248`). Reconnaissance is in
[`research.md`](research.md); this is what to build.

Companion proposals:
[`merge-writer-machinery-and-tests.md`](../../docs/proposals/merge-writer-machinery-and-tests.md)
(this spec) and
[`columnar-write-close-the-gaps.md`](../../docs/proposals/columnar-write-close-the-gaps.md)
(the next one, which this goes before).

## 1. Goal

COPY and CTAS each carry their own copy of the parallel bulk-load machinery.
Collapse them into one — and **design the policy for the four consumers it will
have, not the two it has**.

The four:

| consumer | status | what it needs from the writer |
|---|---|---|
| COPY | shipped | permanent target, or a **temp table** — which CTAS cannot create |
| CTAS | shipped | its own new table, never pinned |
| INSERT via BCP | spec 062 | inside a transaction it must be **in** the transaction |
| UPDATE / DELETE staging | planned | a `#temp` staging table filled by BCP, then joined |

Extracting the common shape of the first two and discovering the policy again
for the last two is how the current duplication happened in the first place.

Three parts, in order of risk:

- **A — the writers.** One implementation of "claim a slot, open `INSERT BULK`,
  write, flush at the threshold, re-open, DONE, release", plus one function that
  decides *how many* and *on whose connection* — and, depending on it, an opt-out
  from the pool's connection reset so a user can own their temp objects (§4.2).
- **B — the tests.** Merge the clusters where a second file buys nothing; leave
  the ones whose value is isolation.
- **C — PR #232.** Merge the contributor's version × charset matrix, which is not
  superseded by anything (`research.md` §C).

**Issues this closes: #189** (the reset setting, §4.2). The three others
originally lined up for it — #233, #235, #236 — turned out to be already fixed by
spec 057 and were closed against PR #234 rather than carried here (#199 went with
them, as a duplicate of #235).

## 2. Why this goes before both follow-ups

Before **columnar-write-close-the-gaps**: that work changes the kernels and the
resolver. It touches the sink where the path is chosen — with two sinks, every
such change is made twice or made once and forgotten. And it changes **which
path a column takes**, which is the lever every "both paths agree" test depends
on. Spec 057 found four of those comparing the columnar path with itself
(`b6c21d3`) because the lever had silently stopped working. One file first means
**one** lever to re-verify.

Before **spec 062**: INSERT via BCP is a third consumer. It should arrive at a
finished seam rather than adding a third copy to it.

## 3. The seam — one predicate, not two axes

Reading all four consumers together collapses the design question to a single
predicate about the **target**:

> **Can a session other than the one driving the statement see this target, and
> may its rows land outside the current transaction?**

| case | answer | writers | connection |
|---|---|---|---|
| `#local` temp target | no — the table lives in its creating session | **1** | the session that created it |
| any target inside an explicit transaction whose rows must roll back | no | **1** | the pinned connection |
| CTAS's own new table | yes — the undo is dropping it | **N** | pool |
| permanent target outside a transaction | yes | **N** | pool |
| `##global` temp target | yes — visible across sessions | **N** | pool (see §4.2) |

This is worth stating as one predicate rather than as COPY's rule plus CTAS's
rule because **UPDATE/DELETE staging lands in the first two rows at once** and
needs no new rule: a `#temp` table inside a transaction is session-scoped *and*
transactional, and both readings give one writer on the pinned connection.

**The distinction that must survive.** COPY inside a transaction pins and uses
one writer; CTAS never pins and keeps its N. That is not drift — spec 057 spent a
week on it. COPY may be loading into a table that already existed, whose rows it
cannot undo, so the transaction has to own the load; CTAS creates its table, so
the undo is dropping it. An abstraction that erases the difference reintroduces
the bug.

**The latent trap the merge must make structural.** COPY's `TryStartLocalWriter`
acquires through `ConnectionProvider::GetConnection` (`copy_function.cpp:626`),
which inside a transaction returns the **pinned** connection. It is safe today
only because the limit is forced to 1 when pinned (`:551`), so the function
returns at its `<= 1` guard before ever asking. Relax that rule and N threads
receive the same pinned connection. **A parallel writer must take the pool
explicitly and must never route through the provider** — then the safety is
structural instead of a consequence of a limit set 75 lines away.

## 4. The temp target, and what an opt-out from the pool reset does to it

### 4.1 Where it stands today — narrower than it looks

The writer-limit resolver (`copy_function.cpp:546-570`) decides only on
`transaction_pinned`; **`IsTempTable()` does not appear in it.** But that gap is
currently covered by accident rather than being live, and the reason is worth
stating precisely before building on it.

A `#local` temp table is only usable at all **inside a transaction**, because
only then does the connection stay pinned and the session survive between
statements. COPY says so itself, in autocommit (`copy_function.cpp:~1180`):

> `WARNING: COPY to temp table '%s' in auto-commit mode. Temp table will be
> dropped when connection is released. Use BEGIN TRANSACTION to keep the temp
> table accessible.`

and `copy_existing_temp.test` wraps every case in `BEGIN` / `ROLLBACK`
accordingly. So today: **temp target ⇒ transaction ⇒ pinned ⇒ the limit is
already 1.** The parallel path is reached only in the autocommit case, where the
table is about to vanish anyway — N connection acquisitions and N failed
`INSERT BULK [#tmp]` round trips spent on a statement that was already pointless.

What makes even that *correct* is not the writer. It is the pool's reset policy:
a connection released outside a transaction is flagged `SetNeedsReset(true)`
(`mssql_connection_provider.cpp:229`), so `RESET_CONNECTION` on its next batch
drops any `#tmp` an earlier statement left behind. Without it, a parallel writer
could find a **stale same-named** temp table on a pooled connection and land its
rows there, silently.

### 4.2 `mssql_reset_connection` — issue #189, and why it makes §3's predicate load-bearing

This does not come from the writer story. It comes from **issue #189, "Allow
sharing temp tables across connections"**, and the setting was already proposed
there by name and accepted by the reporter.

The chain in that thread is worth keeping, because it rules out the two answers
that look better:

1. **`##global` cannot be kept alive by being cleverer about the reset.** Verified
   in #189: `@@SPID` is *identical* across two statements (65 — the same physical
   connection) and `##g` is still gone, because the reset ends the session that
   owns it. There is no selective form — it is a single bit in the TDS header with
   exactly two variants, and building with `RESET_CONNECTION_SKIP_TRAN` (0x10)
   instead drops `##g` and a local `#loc` identically; `SKIP_TRAN` differs only in
   how it treats transaction state.
2. **The database-component target does not solve it either.** Addressing
   `mssql://<alias>/tempdb/dbo/<name>` — the fix direction proposed in #189 and
   still worth having — makes the staging table an ordinary table with a normal
   lifetime. But an ordinary table in `tempdb` needs `CREATE TABLE` permission
   there, and the reporter works under read-only permissions in a regulated
   industry: `SQL Server error 262: CREATE TABLE permission denied in database
   'tempdb'`. Creating a `##` temp table needs no such grant. So for this user the
   target-database proposal is blocked by exactly the permission that temp tables
   exist to avoid needing.

That is what leaves the setting as the answer: default `true` (today's
behaviour), `false` meaning the user manages session objects themselves. It
unlocks the workflow #189 asks for — create `##global` once, COPY into it,
legitimately with N writers since a global temp table *is* visible across
sessions, read it, drop it when done.

**But it converts the paragraph above from an efficiency note into a correctness
precondition.** With the reset off:

- a `#temp` table survives release, so COPY into one **outside** a transaction
  becomes meaningful — which is the point of the setting;
- therefore `transaction_pinned` is false, so the limit derives from the thread
  count, so N parallel writers start;
- each executes `INSERT BULK [#tmp]` on a **different** pooled session, and with
  the reset off some of those sessions may hold a stale `#tmp` from an earlier
  statement. Those succeed, against the wrong table, with no error on either
  side.

So the setting and §3's target predicate **must ship together**: the predicate is
what stops the setting from being a data-loss switch. Ordering matters — D1
before D-reset, or both in one commit, never the reverse.

### 4.3 What the setting takes on besides temp tables

`RESET_CONNECTION` does not clear only temp objects. It resets session state
wholesale — `SET` options, session variables, `CONTEXT_INFO`, cursors — and it
rolls back an open transaction. Turning it off hands **all** of that to the user,
including the case where a statement leaves a transaction open on a pooled
connection and its locks stay held.

So the setting is not "keep my temp tables"; it is "I own this session's state".
It must be documented that way, and named so it does not read as temp-specific.

### 4.4 Precision required

The predicate is `is_temp_table`, **not** `IsTempTable()`. The latter
deliberately merges `#` and `##` (`target_resolver.hpp:70`), and a `##global`
table *is* visible to other sessions, so it may keep N writers — which is exactly
the workflow §4.2 unlocks. Both flags already exist separately (`:41`, `:44`).
Using the merged accessor would needlessly serialise the global case; using
neither is where we are now.

## 5. The reference implementation — `duckdb-postgres`

Read 2026-08-04, because it is the closest analogue and it makes the opposite
choice.

- `PostgresInsert::ParallelSink() { return false; }` — **explicit**, not the
  default. There is no parallel sink at all.
- Everything runs on the transaction's connection:
  `PostgresTransaction::Get(...).GetConnection()` — INSERT, CTAS, UPDATE alike.
- CTAS is the same `PostgresInsert` operator, so table creation and load share
  one connection and are **atomic**.
- UPDATE is precisely the planned staging shape:
  `CREATE LOCAL TEMPORARY TABLE ... ON COMMIT DROP`, filled by COPY on the
  transaction's connection, then `UPDATE ... FROM temp WHERE ctid = ...`.

Two consequences, pointing opposite ways.

**Our model diverges deliberately.** Theirs is fully transactional; ours loads
CTAS outside the transaction to keep N writers. That was a measured choice —
10.55 s at one writer vs 3.24 s at four on 44 columns × 1M rows — and it should
be recorded as a **decision against the reference implementation**, not left as
an implementation detail in a comment.

**And they solve a limitation we merely documented.** `PlanCreateTableAs` calls
`MaterializePostgresScans(inner_plan)` and forces `max_threads = 1`, so a scan of
their own catalog is materialised before the sink starts. That is a real answer
to "reading and writing the same catalog in one transaction", which our README
describes as a limitation. **Its own issue, not this PR** — it is planner work,
and this PR is already the riskiest refactor on the write path.

## 6. Inventory, verified 2026-08-04

### 6.1 Already shared — and it is the model

`ReleaseBcpConnectionOnError` (`mssql_connection_provider.cpp:247`) is the
mid-bulk-load release protocol: close the session so the `INSERT BULK`
transaction rolls back and the target's locks drop, then either leave the pin
alone or return the connection with `SetNeedsReset`. Both operators' local **and**
global state destructors call it, and it takes no `ClientContext`, so it is safe
on a worker thread (issue #178).

One function, the contract written once, the operator supplying the one
parameter that genuinely differs. That is the shape the rest should reach.

### 6.2 Duplicated

| piece | COPY | CTAS |
|---|---|---|
| `TryStartLocalWriter` | `copy_function.cpp:609-659` | `mssql_physical_ctas.cpp:157-203` |
| `FinishLocalWriter` | `copy_function.cpp:663-686` | `mssql_physical_ctas.cpp:207-223` |
| writer-limit resolution | `copy_function.cpp:546-570` | `mssql_physical_ctas.cpp:101-128` |
| local state + destructor | `copy_function.hpp` `MSSQLCopyLocalState` | `mssql_physical_ctas.hpp` `MSSQLCTASLocalSinkState` |
| the parallel branch in the sink | `copy_function.cpp:723-767` | `mssql_physical_ctas.cpp:266-316` |
| `INSERT BULK` text | inline, `copy_function.cpp:~490-528` | `CTASExecutionState::BuildInsertBulkSql` |

Roughly 150 lines of ~9900. **Size is not the argument** — 1.5% does not justify
a risky refactor. The argument is that these were written in the same week and
have already diverged nine ways, and that two more consumers are coming.

### 6.3 Drift — each a defect on the CTAS side

| # | drift | evidence |
|---|---|---|
| 1 | CTAS never sends `ROWS_PER_BATCH` | `BuildInsertBulkSql` emits only `WITH (TABLOCK)`; COPY appends it whenever `flush_rows > 0` |
| 2 | no interrupt check **anywhere** in CTAS | `IsInterrupted`: **0** occurrences in `mssql_physical_ctas.cpp`, **5** in `copy_function.cpp` |
| 3 | no counters in CTAS | `counter_*` / `CountersEnabled`: **0** vs **20**. `MSSQL_COUNTERS=1` measures one of the two write paths |
| 4 | flush threshold expressed twice | COPY asks `config.ShouldFlushToServer(...)`; CTAS inlines the comparison |
| 5 | accounting differs | COPY: atomics on the global state, and it has `batches_flushed`. CTAS: per-thread, folded in `Combine`, no `batches_flushed` |
| 6 | error vocabulary differs | COPY: `has_error` + `error_mutex` + `error_message`. CTAS: `load_failed` + `phase` under `mutex` |
| 7 | debug logging | COPY logs writer start and fallback with the slot number; CTAS is silent |

Drifts 5 and 6 are where the merge must **choose** rather than unify: the
per-thread fold (CTAS) allocates no atomics on the hot path, and CTAS's entry
check is deliberately conditional on `drop_on_failure` (spec 057 — if the table
is not being dropped, a thread with rows still to send is landing more of what
the user asked to keep). Keep that conditional; give it one name.

## 7. Deliverables

### D1 — `ResolveLoadPolicy`, one function, four callers

```cpp
LoadPolicy ResolveLoadPolicy(const BCPCopyTarget &target, bool in_transaction, ...);
// -> { ConnectionSource source; idx_t max_writers; }
```

Implements §3's predicate, including the `is_temp_table` case of §4 and the
`##global` exception. COPY and CTAS call it now; 062 and the DML staging path
call it later without changing it.

Its answer is derived from the target and the transaction state — never from
which operator is asking. If an operator needs to differ, that difference is an
input to this function and is written down here.

### D1a — `mssql_reset_connection` (closes #189), and it lands no earlier than D1

A boolean setting, default `true` — today's behaviour — controlling whether a
connection returned to the pool outside a transaction is flagged for
`RESET_CONNECTION` (`mssql_connection_provider.cpp:229`). `false` hands session
state to the user, which is what makes a `##global` temp table usable as a COPY
target across statements.

**The name is `mssql_reset_connection`**, not a variation on it: it was proposed
under that name in #189 and the reporter agreed to it there. Renaming it now
would mean the answer in the thread does not match the shipped setting.

Three things this deliverable owes, all from §4:

1. **It must not land before D1.** With the reset off and no target predicate, a
   COPY into a `#temp` target outside a transaction starts N parallel writers
   against N pooled sessions, some of which may hold a stale same-named temp
   table. Those writes succeed against the wrong table silently. One commit, or
   D1 first — never the reverse.
2. **Its name and its documentation must not say "temp".** The reset clears
   `SET` options, session variables, `CONTEXT_INFO` and cursors, and rolls back
   an open transaction. `false` means "I own this session's state", and the
   README entry has to say so, including that a statement leaving a transaction
   open on a pooled connection will hold its locks.
3. **A test that the setting actually changes the flag**, not merely that it
   parses — the class of defect in `mssql_copy_tablock`'s original "new tables"
   rule, which tested a flag that was always set (spec 057).

Open, and cheap to change later: a session `SET` (chosen here, next to
`mssql_connection_cache`) versus an ATTACH option. The pool is per-catalog since
spec 047, so an ATTACH option would scope it more precisely; the setting is read
at release time, where the context is available either way.

### D2 — one bulk-load session type

Carried the last unhonoured reset site with it, as its own commit.
`ReleaseBcpConnectionOnError` was the one place that flagged a reset
unconditionally — no `ClientContext` by design (issue #178: it runs from
destructors on worker threads), and honouring the setting meant threading the
answer through four BCP state structs. With one struct there are three carriers
instead: the session, COPY's global state, CTAS's execution state.

Its `reset_on_release` parameter has **no default**, which is the point rather
than a detail: a defaulted one is exactly how the setting came to be
half-honoured in the first place, and the compiler now makes every caller state
an answer.

Mostly it decides nothing — the function Closes the connection unless it was
already Idle, and closing ends the session regardless of the flag — so the value
is the contract holding everywhere, not an observable behaviour change.


Owns the connection, the `BCPWriter`, the pool handle, `rows_in_batch`,
`rows_written`, `rows_confirmed`, `init_attempted`, and the destructor contract
already implemented by `ReleaseBcpConnectionOnError`. Operations: `TryStart`,
`Write(chunk)` (including flush-and-reopen at the threshold), `Finish`.

Not a "writer pool" and not an operator base class — a **session**, with the
policy passed in.

### D3 — the connection source is stated, not inherited

`TryStart` takes the pool, never a `ClientContext` from which a provider might
hand back the pinned connection. The pinned first writer stays where it belongs:
in the operator, before any parallel session exists.

### D4 — one `INSERT BULK` builder

Target, columns, TABLOCK, `ROWS_PER_BATCH`, and the temp-table naming rule.
Closes drift 1 by construction. The `ORDER` hint is **not** in scope — it needs a
guarantee that the rows really are sorted, which is its own spec.

### D5 — close drifts 2, 3, 4, 7

Interrupt checks, counters, the shared flush predicate, the slot logging. CTAS
gains what COPY has because the code becomes the same code.

### D6 — one both-paths byte-equality file

`time_rounding_both_paths`, `smalldatetime_write`, and the row-path section of
`string_bound_truncation` are one experiment: load the same values twice — once
so every column resolves to a kernel, once with a column that drops the chunk to
row-major — and require agreement.

One file, one section per family, **one lever**. The lever is a signed `TINYINT`
into SQL Server's unsigned `tinyint`; the file must **assert** it still forces
the row path rather than assume it, and say in one place what would silently
break it.

### D7 — a gate for `require-env` variables nothing sets

Spec 057 hit this class three times: `MSSQL_TEST_SERVER` (4 files dormant),
`MSSQL_TEST_CONNECTION_STRING` (4 more, both auto-TABLOCK tests among them), and
the widest — `make integration-test` filtering on `[integration]`/`[sql]`, which
match 8 of 172 files, while the `"[mssql]"` the rest carry matches **nothing and
exits 0**. All three are fixed; what is missing is what stops the fourth.

A check that every `require-env` variable in `test/sql/` is set by the Makefile
or a workflow, or is on an explicit opt-in list (`MSSQL_COUNTERS`, `AZURE_*`,
`MSSQL_KERBEROS_TEST`, `MSSQL_WINSSPI_TEST`, `MSSQL_NAMED_INSTANCE_HOST`).

**A filter that matches nothing is indistinguishable from a suite that passed.**
Assert the case count, not the exit code.

**A fourth instance turned up while building D4, and its root cause is
different — which changes what this deliverable can achieve.** `make test-cpp`
builds 13 standalone C++ tests, and three of them had been failing on `main`:
`test_ddl_translator`, `test_ctas_type_mapping` and `test_integer_codec` all
still expected signed `TINYINT` to map to `tinyint`, which spec 057 deliberately
widened to `smallint` (SQL Server's `tinyint` is unsigned 0..255; the narrow
mapping turned -1 into 255 on the wire). Fixed here.

They went stale inside ONE spec, and the Makefile comment directly above them
says why it keeps happening — those files "existed in test/cpp/ for a long time
WITHOUT being built by anything... A test nobody runs is documentation that looks
like a guarantee." Spec 057 wired them into the Makefile. It did not wire them
into CI.

And it could not: **no CI job builds the extension on a pull request at all**
(`ci.yml:161` — the build job is `workflow_dispatch`/`schedule` only), which is
issue #212. `make test-cpp` links against the built archive, so a gate for it is
blocked behind that. So D7 splits:

- the `require-env` gate for `test/sql/` — doable here, catches instances of the
  first three's shape;
- a gate for the standalone C++ tests — needs #212 first. Record the dependency
  rather than pretending the deliverable covers it.

### D8 — the parallel-writer tests: decided NOT to merge

The decision this deliverable deferred to "once the code is one" is: **leave them
as two files.** Reading them after D2:

- what is duplicated is the PROSE — the paragraph about the lost 205376 rows, the
  thread-pinning note — not the assertions;
- what differs is real. CTAS **creates** its target, so each writer opens an
  `INSERT BULK` against a table that did not exist a moment ago, and the ordering
  (DDL in `GetGlobalSinkState`, writers on first chunk) is asserted by those loads
  simply succeeding. COPY loads into a table that already existed;
- and §B.3 of `research.md` already says not to merge across `copy/` and `ctas/`
  by subject, because the directory split is how the suite is filtered when only
  one operator changed. That rule was written before this deliverable and holds.

**What they were missing is the D6 problem, not duplication.** Both said, in a
comment, that the writer count is "a ceiling, not a floor" — and asserted
nothing. So a one-core runner, or any change that stopped a slot being claimed,
would have both files testing the serial path twice and reporting green.

Half of that is now asserted: after the serial section,
`mssql_pool_stats(...).connections_created` must be exactly **1**, which proves
`mssql_copy_parallel_writers = 1` really disables the feature. Verified to bite —
changing the setting to 4 fails the assertion.

The other half **could not be asserted, and the file says so instead of
pretending.** The obvious check — `connections_created > 1` after the parallel
section — is useless: the count is 2 even on a single thread, because the
operator opens the global writer's connection in its global state and the one
sink thread then claims a session of its own on top of it. Measured at
threads=1 → 2 and threads=4 → 3, so the only thing separating them is a
scheduling-dependent number that would flake. Making it verifiable needs a signal
the extension does not expose to SQL: the counters know (`writers=N/M`, D5) but
they go to stderr.

Still open, and not covered by anything: a COPY into a `#temp` target with
`threads > 1` opening **exactly one** bulk-load session (§4). It needs the same
missing signal.

### D9 — merge PR #232, with two corrections it has now outlived

**Decision: take it.** `research.md` §C establishes the premise still holds —
every compose file and workflow pins `mssql/server:2022-latest`, so the version
axis is covered by nothing, and its 2017 result (advertising `UTF8SUPPORT` to a
server that lacks it is harmless) is recorded nowhere else.

**Safety, read line by line 2026-08-04, not inferred from it being a test:**

| axis | finding |
|---|---|
| container name | `mssql-matrix-$$` — PID-suffixed, so it can never touch `mssql-dev` |
| port | 14333, and it exits if the port is already listening |
| cleanup | `trap cleanup EXIT INT TERM`, with `--keep` as the explicit opt-out |
| preconditions | refuses without a docker daemon and without `build/release/duckdb` |
| writes outside the repo | only the optional `--json <path>` the caller names |
| network | `docker pull` from `mcr.microsoft.com` only |
| shell hazards | no `eval`, no `curl \| sh`, no `sudo`, no `rm -rf`, no unquoted expansion into a destructive command |
| credentials | a hard-coded SA password for a throwaway container it creates itself; TruffleHog passes |
| CI | not wired into any workflow or Makefile target — verified by grep, so it cannot run automatically |
| conflicts | merges onto current `main` cleanly; nothing has touched `docs/TESTING.md` since it branched |

**Two things in it are now wrong, both because of what #233 turned out to be:**

1. `wire_bytes_per_row` collects counters with **`MSSQL_DEBUG=2`**
   (`mssql_version_matrix.sh:359`). Since spec 057 the flag for anything to be
   quoted is `MSSQL_COUNTERS=1`, because `MSSQL_DEBUG=2` logs from inside the
   phases it times. It is byte counts here rather than timings, so the figures
   are not wrong — but `MSSQL_DEBUG>=2` is also the level that used to **crash**
   on a CONSTANT-published chunk, which is very likely how the author met #233 in
   the first place. Switch the flag.
2. Its comment at `:348` — "the D4 stream counters are known to report a single
   chunk rather than the whole scan" — describes #233 as a sampling bug. It was a
   crash, fixed in #234 (`d2cd664`), and the counters report complete totals.
   Per-row normalisation is still the right cross-version comparison; the stated
   reason for it is not.

Merge the contributor's work as authored, then land both corrections as a
follow-up commit rather than rewriting their branch.

Optional, and only if it does not cost the contributor another round: trim the
2022-only charset round-trip, which now duplicates `test/sql/query/utf8_support.test`.

## 8. Acceptance criteria

1. `TryStartLocalWriter` / `FinishLocalWriter` exist **once**.
2. A parallel writer cannot reach `ConnectionProvider::GetConnection` — the pool
   is the only source, by inspection **and** by the transaction tests.
3. `ctas_transaction.test` unchanged and green: ROLLBACK still leaves the CTAS
   rows, and the same-catalog case still behaves as spec 057 documented.
4. `copy_connection_leak.test` unchanged and green, still running alone.
5. A COPY into a `#temp` target with `threads > 1` opens exactly one bulk-load
   session — asserted, not inferred from it being correct. Asserted **with
   `mssql_connection_reset = false`**, which is the configuration in which the
   alternative is silent corruption rather than wasted round trips.
6. `mssql_connection_reset = false` keeps a `##global` temp table alive across
   statements, and a COPY into it still uses N writers.
7. CTAS's `INSERT BULK` carries `ROWS_PER_BATCH` when `flush_rows > 0`.
8. `MSSQL_COUNTERS=1` reports non-zero sink/encode/flush for a parallel CTAS.
9. Ctrl+C during a parallel CTAS raises `InterruptException` from the sink.
10. The both-paths file proves its lever takes the row path, visibly in the file.
11. `make test` and `make integration-test` green, with the **case count**
    reported and compared against the pre-change count.
12. No throughput regression: 44 columns × 1M rows within 5% of the spec 057
    numbers, measured interleaved in one session.

## 9. Risks

- **The abstraction erases the pinned/unpinned difference.** The one that
  matters. Mitigated by D3 making the pool the only source a parallel writer can
  reach, and by criterion 3.
- **`mssql_connection_reset = false` is a foot-gun by design.** It hands session
  state to the user, and §4.2 shows it turning a wasteful case into a silently
  wrong one if D1 is not already in. Mitigated by the ordering constraint in
  D1a and by criterion 5 testing the dangerous configuration rather than the
  safe one. Not mitigated at all for the state the setting is *meant* to expose —
  that is what the user is opting into, and the documentation has to be blunt.
- **`ResolveLoadPolicy` is designed for consumers that do not exist yet**, so its
  fit for 062 and DML staging is reasoned, not tested. Mitigated by keeping it a
  pure function of (target, transaction state) — a wrong answer for a future
  caller is a changed table row, not a changed structure.
- **A refactor PR that also changes behaviour is unreviewable.** D5 changes CTAS
  behaviour deliberately. Keep those commits separate from the move commits so a
  reviewer can read "this moved" and "this changed" apart.
- **A merged test file stops at its first failure**, so later sections do not
  run. Hence only D6 and D8 merge; the rest of `research.md` §B.2 is left alone.
- **The counters are the instrument.** Adding them to CTAS changes what CTAS
  timings mean; any before/after comparison must have them in the same state on
  both sides (spec 057: `MSSQL_DEBUG` logging inside the timed phase inverted the
  read numbers).

## 10. Out of scope

- **Spec 062** (INSERT via BCP) and the UPDATE/DELETE staging path. They are why
  D1 exists; their code is not in this PR.
- The kernel work — cursor-path decimal/uuid/datetime, NULLs in fixed-width
  columns, HUGEINT/UBIGINT. That is
  [`columnar-write-close-the-gaps.md`](../../docs/proposals/columnar-write-close-the-gaps.md)
  and issue #238, landing **after** this.
- **Materialising a scan of our own catalog** before the sink (§5) — filed as
  **issue #239**. Planner work, and it removes a documented README limitation
  rather than merging machinery.
- The remaining test clusters (TABLOCK, string length/collation, temp tables,
  type coverage) — `research.md` §B.2, deliberately not merged here.
- `ORDER` in the `INSERT BULK` hint.
- Issue #224 (invalid UTF-8 from a non-UTF8 collation in raw `mssql_scan`) —
  read-path work that #232 would *detect* but this spec does not fix.

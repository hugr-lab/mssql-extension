# Analysis 078 — who touches the pinned connection, and when (issue #356)

**Status:** analysis only, on `recon/356-pinned-connection-race` from `main`
`b84e259`. No code changes. Written after four wrong fixes, each measured and
withdrawn; the point of this document is to stop the guessing and map the
actual flow first.

## 0. What is actually observed

`test/sql/transaction/sink_reads_own_catalog.test:130` — a COPY reading from
the catalog it writes to, inside an explicit transaction — fails
intermittently:

```
IO Error: Failed to execute SQL batch: Cannot execute: connection not in Idle state (current: Executing)
```

60 runs of that file per build, same machine, same server:

| build | failures |
|---|---|
| **main** | **2 / 60** |
| a branch rewriting the metadata queries | 3 / 60 |
| that branch plus a guard around the send | 2 / 60 |

All the same ~4%. **The defect is on main**, nothing in flight makes it worse,
and the guard did nothing. A second symptom shows up in the same runs and is
probably the same defect from the other end: `Connection closed while waiting
for COLMETADATA` on the statement after `COMMIT`, preceded by
`[MSSQL POOL] Closing connection in non-Idle state`.

**A sample of 30 proves nothing at this rate.** A clean run of 30 happens about
a third of the time with the bug present. Two of the four wrong conclusions in
this investigation came from exactly that, so every claim below says its sample
size.

## 1. Where the error is thrown

`MSSQLResultStream::Initialize`, `src/query/mssql_result_stream.cpp:145`. It is
a **scan** sending its SELECT and finding the connection already `Executing` —
not a sink sending `INSERT BULK`. Any fix aimed at the send is aimed at the
wrong side of the collision.

## 2. Threads: they are DuckDB's, not ours

`TableScanGlobalState::MaxThreads()` already returns 1, so the scan is
single-threaded by declaration. The concurrency is in **pipeline
initialisation**: DuckDB initialises the source's and the sink's global state
as separate tasks. Measured, thread ids logged at both entry points in one
failing statement:

```
TableScanInitGlobal:   START     tid=15699432364946540659
BCPCopyInitGlobal:     starting  tid=1910083427918873908
```

For every other statement in the same run both ran on the client thread. So
`MaxThreads` is not the lever, and neither is anything else we declare about
scan parallelism.

## 3. The audit: who can hold the pinned connection, and under what lock

Every caller of `ConnectionProvider::GetConnection` — the accessor that returns
the **pinned** connection inside a transaction — against whether its file knows
about `MaterializeMutex` at all:

| Call site | Takes MaterializeMutex? |
|---|---|
| `table_scan.cpp` (via `mssql_query_executor.cpp:95`) | **yes**, around the execute-and-drain |
| `mssql_functions.cpp:394, 958` (`mssql_scan`) | **yes** |
| `mssql_physical_insert.cpp:172` (INSERT sink) | **yes**, in InitGlobal |
| `copy_function.cpp:310` (COPY sink) | **yes**, in InitGlobal |
| `mssql_catalog.cpp:435` (`LookupSchema`) | **no — the file never mentions it** |
| `mssql_catalog.cpp:509` (`ScanSchemas`) | **no** |
| `mssql_statement_connection.cpp:17` (statement DML) | **no** |

The four operators that were designed around the mutex all take it. **Three
call sites can put metadata traffic on the same pinned connection with no lock
at all.** That is the shape of the hole: the mutex is not a property of the
connection, it is a convention four call sites follow and three do not.

`LookupSchema` and `ScanSchemas` are normally bind-time, and bind is
single-threaded — which is why this is rare rather than constant. What has NOT
been established is whether either can fire during **execution**, e.g. a lazy
schema or table load triggered from an operator's InitGlobal. That is the first
thing to prove or rule out, and it is provable by instrumenting those two
functions with a thread id and a phase marker rather than by reasoning.

## 4. What is already ruled out, with evidence

Do not re-try these. Five hypotheses were measured and rejected; the fifth was
rejected **wrongly**, and turned out to be the answer — see § 4.1.

- **"The scan is not marked for materialisation."** Instrumenting the bind-data
  pointer on both sides shows the optimizer marking `bd=0x…` for the scan and
  `TableScanInitGlobal` reading `mat=1` on that same pointer in a failing run.
- **"`Copy()` drops `requires_materialization`."** It does not; the field is on
  `MSSQLCatalogScanBindData` and its `Copy()` copies it.
- **"The COPY sink never takes the mutex."** It does, before any wire traffic
  in its init.
- **"Guarding `BulkLoadSession::OpenStream` fixes it."** 3/60 → 2/60, i.e.
  nothing.
- **"The metadata-query rewrite is the trigger."** main fails at the same rate
  without it.
- **"The publish-before-BEGIN window is the race."** `ConnectionProvider::GetConnection`
  does `SetPinnedConnection(conn)` at line 163 and only then sends
  `BEGIN TRANSACTION` at line 168, so another thread calling `GetConnection`
  in between gets the connection while BEGIN is in flight. Structurally it is a
  real window and it fits every symptom. It is **not** the cause: widening it
  with a 50 ms sleep gave **0 failures in 10**, where the window being the race
  would have made it near-certain. Worth tidying on its own merits; not this
  bug.

## 4.1 ROOT CAUSE, proven by the connection naming its own holder

The instrument this document asked for was built: `MSSQL_CONN_STATE=1` logs
every connection state transition with a thread tag, every path that takes a
connection to `Executing` records **what** it was, and the "not in Idle state"
error now names that holder. One reproduction was enough:

```
Cannot execute: connection not in Idle state (current: Executing);
it was taken to Executing by 'BEGIN TRANSACTION' on thread 4421914,
this is thread 3397863
```

**`ConnectionProvider::GetConnection` publishes the pinned connection before the
transaction has begun on it.**

```
161:  txn->SetPinnedConnection(conn);              // visible to every thread from here
165:  conn->ExecuteBatch("BEGIN TRANSACTION");     // connection -> Executing
      … receive the response, parse the ENVCHANGE descriptor …
210:  conn->TransitionState(Executing -> Idle);    // busy for this whole span
213:  txn->SetSqlServerTransactionActive(true);
```

Another operator's InitGlobal — DuckDB runs the source's and the sink's on
different threads, § 2 — calls `GetConnection` inside that window, takes the
early return at line ~136 because a pinned connection *is* set, and executes on
a connection that is mid-BEGIN. Whichever loses the CAS throws.

It explains every observation: the rate (the window is one round trip), that it
strikes the **first** statement after `BEGIN` (that is when the lazy pin
happens), that both sides take `MaterializeMutex` and still collide (the BEGIN
is outside it), and the second symptom of a connection going back to the pool
non-Idle.

**Why this was missed twice.** It was hypothesis five, and it was *rejected on a
bad experiment*: a 50 ms sleep was inserted **before** the BEGIN, which delays
the publishing thread and lets the other one finish first — masking the race
rather than widening it. Widening it correctly would have meant slowing the
BEGIN itself. A probe that can only make a race rarer is not a test of it.

## 4.2 The fix, and the hazard in the obvious version

Publishing after the BEGIN completes is necessary but **not sufficient on its
own**: two threads would then both miss the pinned connection, both acquire one
from the pool and both send BEGIN, leaving one connection pinned and the other
leaked with an open transaction.

The whole "is there a pinned connection; if not, acquire one, BEGIN on it, and
publish it" sequence has to be one critical section per transaction. Then a
second thread waits, and when it proceeds it finds a connection that is pinned,
begun and Idle. `MSSQLTransaction` already owns a `connection_mutex_`, but it is
taken *inside* the accessors, so this needs a coarser hold rather than a new
lock.

## 5. Why a mutex may be the wrong tool entirely

The sink holds the pinned connection in `Executing` for the **whole load**, not
for the send. A scan that starts executing after the stream opens finds it busy
whatever either side locks. Holding the mutex across the stream is not
available either: the stream is fed by the very scan that would be blocked on
it, so it deadlocks by construction. A plain `std::mutex` around the send
already deadlocked once for a related reason — one thread taking it twice, init
and deferred send on the same stack, with a stack sample showing a single
pipeline thread parked in `OpenStream` and no other holder.

So the question is not "who locks what". It is **which code puts traffic on the
pinned connection at a moment when spec 075 W3 assumes everything has drained**.

## 6. Two candidate designs, neither implemented

**A. Make the connection enforce it, not the callers.** The state lives on
`TdsConnection`; a pinned connection could refuse or wait on its own, so a call
site cannot forget. Cost: the wait has to be expressible without deadlocking
against the sink that holds it for a whole load, so this probably means "wait
while a scan is draining", not "wait while anyone is using it" — i.e. the
connection needs to distinguish the two, which it currently does not.

**B. Materialise where there is only one thread.** Bind is single-threaded, as
noted in the discussion that prompted this document. If the rows must be
materialised anyway, doing it at bind removes the whole class of races. The
obstacle is ordering: the decision to materialise is made by the **optimizer**,
after binding, so at bind time nobody knows the plan will sink into this
catalog. Making it unconditional inside a transaction would undo spec 075's
main win, which was that `DESCRIBE`/`EXPLAIN` stop executing the query.

## 7. The instrument this needs

A sqllogictest cannot test a 4% race; it can only flake. What can:

- a **C++ test** that drives the two InitGlobals on chosen threads in a chosen
  order against a live server, and asserts the connection state at each step;
- **connection-level logging of every state transition** — `Idle → Executing`
  and back — carrying the thread id and the SQL that caused it. That turns
  "whose stream was left open" from the guess it has been five times into a
  line in a log. This is the one thing that should be built next, before any
  further theory.

Both are cheap next to another round of hypothesis-and-revert.

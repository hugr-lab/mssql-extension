# Static analysis of the parsing layer with Joern

[Joern](https://joern.io) builds a **Code Property Graph** (CPG) — AST, control
flow, call graph and data flow in one queryable graph — and lets you ask
questions about the code in a small Scala DSL. Its C/C++ frontend (`c2cpg`, built
on Eclipse CDT) **compiles nothing**: no build, no vcpkg tree, no
`compile_commands.json`, and a header it cannot find degrades the graph locally
instead of failing the run.

It is the structural complement to [`fuzz/`](../fuzz/README.md). The fuzzers
prove that a specific input crashes a specific parser; Joern answers "which
other places look like the one that crashed". Same target surface: this
extension parses **untrusted, attacker-controllable bytes** — TDS token streams
from a malicious or MITM'd server, SQL Browser UDP datagrams, BCP payloads —
behind hand-written bounds checks.

## Install

```bash
brew install joern      # macOS
```

The bottle is ~1.6 GB and pulls `openjdk@25`, `astgen` and `php` (the formula
ships every language frontend; this project uses only the C/C++ one). Joern
brings its own JDK, so whatever `java` is on `PATH` is irrelevant. Other
platforms: <https://docs.joern.io/installation>.

## Use

```bash
joern/build.sh          # CPG of the untrusted-input surface  (~15 s)
joern/scan.sh           # run the query suite -> joern/out/report-tds.md
joern/scan.sh tds q3    # one query

joern/build.sh all      # CPG of all of src/                  (~36 s)
joern/scan.sh all q6    # the credential query, which needs the whole extension

joern/shell.sh          # REPL with the CPG loaded and helpers defined
joern/graph.sh ParseError   # render one method's CFG as an SVG (needs graphviz)
```

Or `make joern-cpg`, `make joern-scan` (`JOERN_QUERY=q3`), `make joern-shell`,
all taking `JOERN_SCOPE=tds|all`.

Everything under `joern/out/` is gitignored — the CPG, the report and Joern's
own `workspace/` working copies. The scripts `cd` into that directory before
launching Joern, because Joern writes `workspace/` into the current directory
and has no flag to move it.

## Scopes

| Scope | Input | Why |
|---|---|---|
| `tds` (default) | `src/tds/**`, `src/connection/instance_resolver.cpp`, `src/codec/staging/{row_stager,column_ops}.cpp` | The code that reads the wire. DuckDB-free — the same isolation finding the fuzz harnesses rely on — so it parses in seconds and the graph carries no DuckDB template noise. 183 files, 2.2k methods, 20k calls. |
| `all` | `src/**` | Whole-extension questions (q6 in particular: credentials live in `src/azure/`, `src/connection/`, `mssql_storage.cpp`). 285 files, 3k methods, 65k calls. CDT resolves fewer callees inside DuckDB-templated code, so call-graph answers are less trustworthy here. |

The `tds` scope is staged as a tree of symlinks under `joern/out/stage-tds/`,
because the frontend takes a single input path.

## The query suite

`joern/scan.sc`, run by `joern/scan.sh`. Each query prints `file:line` and the
offending expression. Counts below are from the tree as of this writing — they
are the **baseline**, not a target.

| Id | Query | What it is for | tds | all |
|---|---|---|---|---|
| `q1` | Wire-parsing surface inventory | Every method taking a `uint8_t*` buffer or touching the `TokenParser` receive buffer. The set that must never trust a declared length. | 170 | 182 |
| `q2` | Byte-buffer method that indexes without comparing anything | The convention here is that the parsing method checks `Available()` / its own length parameter itself (`TokenParser::ParseError` is the model). A hit is an exception to that convention — mostly the write path, where the caller sized the buffer. | 15 | 17 |
| `q3` | Length arithmetic that can lose information | `[wrap]` 32-bit length arithmetic — the **issue #185** class, where `5 + token_length` wrapped and the wrapped sum passed a bounds check it should have failed (PoC `EE FB FF FF FF`). `[trunc]` a length narrowed by a cast to `uint8_t`/`uint16_t`/`uint32_t` — the **issue #181** class. | 29 | 47 |
| `q4` | Allocation or append sized by a declared length | A server-declared 4 GB length reaching `resize`/`reserve`/`assign` is a remote OOM even when nothing is written out of bounds. | 40 | 58 |
| `q5` | *(dataflow)* fuzz entry-point parameter → copy size or index | Taint from the harness entry points to a `memcpy` size or an array index. Candidate fuzz seeds, not bugs. | 14 | 16 |
| `q6` | Credential inside a log, exception or format expression | Spec 047 T028a ("the ATTACH error path must never echo a password") and the `mssql_pool_stats` redaction gate, as a query. Needs the `all` scope. | 0 | 2 |
| `q7` | Socket/TLS call with a discarded return value | `recv`/`poll`/`SSL_*` return a count or an error that has to be read. | 12 | 12 |
| `q8` | Type-punning cast to a multi-byte pointer | Reading wire bytes through a `uint32_t*`/`char16_t*` is unaligned-access UB that UBSan only reports if an input reaches the site. | 8 | 14 |
| `q9` | Parsing method not reachable from any fuzz entry point | The call-graph answer to "what does `fuzz/` not cover". | 152 | 164 |

A full run of either scope takes about six seconds once the CPG exists.

### Reading the baseline

Three queries have a known, benign baseline. The signal is a **new** entry, not
the entries themselves:

- **q6 (`all` scope) = 2**, both `secret->GetType()` in a "Secret '%s' is not of
  type …" message (`mssql_storage.cpp:188`, `azure_secret_reader.cpp:44`). They
  match because the receiver is named `secret`; it is the secret's *type* that
  is logged, not its value. Identifiers ending in `_name`/`_type`/`_id` are
  already filtered out for the same reason — a credential's name is not the
  credential, and logging it is what those messages are for.
- **q8 = 8 (tds) / 14 (all)**, all of them deliberate and all on the write or
  decode side: `utf16.cpp` proves 2-byte alignment at the site before casting
  (`AlignedUtf16View`, which copies into a scratch buffer when the test fails),
  while `bcp_row_encoder.cpp`, `codec/datetime_codec.cpp` and
  `codec/string_codec.cpp` read `fmt.data` / a staging buffer — DuckDB vector
  memory, which the allocator aligns. An entry beyond these, especially one in a
  parser reading `data[...]`, is the find.
- **q7 = 12**, mostly `close()` / `SSL_shutdown()` on teardown paths that are
  `noexcept` by spec 047 and best-effort by design. The discarded
  `poll(&pfd, 1, remaining_ms)` at `tls_impl.cpp:592` and the two `setsockopt`
  calls are the ones worth an argument.
- **q9 = 152** is mostly honest: the BCP encoders and the row stager's append
  side never see wire bytes. What to look for is a *read*-path method in that
  list.

## What this can and cannot see

**It cannot decide that a bounds check is correct.** The guards here are
hand-written (`if (Available() < 3 + token_length) return false;`) and the
relation between the guard and the later indexing is arithmetic, not structural.
No query in this suite proves a read is in bounds; q2 and q3 only find places
where the guard is absent or where the arithmetic feeding it can lose
information.

**Expression types are mostly `ANY`.** c2cpg gives `3 + token_length` no type,
so every type test in the suite keys on the *operands* — an identifier's
`typeFullName` is accurate (`token_length:uint16_t`, `data:uint8_t*`,
`offset:size_t`). Likewise a cast's `typeFullName` never carries a `*`, so q8
matches the cast's source text. Both were verified on this codebase's CPG; a
query written against expression types silently finds nothing.

**q3 encodes two C++ rules, and has to.** Integer *promotion* widens a
`uint8_t`/`uint16_t` operand to `int` before the arithmetic, so `1 + data_length`
on a `uint8_t` cannot wrap; the usual arithmetic *conversions* make any
expression with a `size_t` operand 64-bit, so `header + data_length` with
`header:size_t` cannot wrap either. An earlier cut of the query ignored both and
reported 126 hits, including `static_cast<size_t>(token_length) + 5` — which is
how #185 was **fixed**. With the rules applied it reports 29, all genuinely
32-bit.

**Dataflow over-approximates.** Joern's OSS engine taints a call's result with
its arguments when it cannot see inside the callee, which makes some taint
questions useless here: a dataflow version of q6 reported 28 flows from
`password` into debug `fprintf`s in `DoLogin7` that log a response size, a hex
dump of the *response* and a SPID — every one false, because the taint travelled
through an unmodelled call. That is why q6 is structural. In the other
direction, CDT leaves some callees unresolved and an unresolved callee silently
**truncates** a flow, so q5 finding nothing would not be evidence of absence.
Building the extension first (so `build/release/vcpkg_installed/` exists for
`build.sh` to pass as include paths) makes the graph better.

**The report is a baseline, not a gate.** The intended workflow is to diff a
report against one taken from a tree you trust — a new line in q2, q3, q7, q8 or
q9 is a reviewable change in the parsing surface:

```bash
make joern-cpg && make joern-scan           # on the known-good tree
cp joern/out/report-tds.md /tmp/joern-baseline.md
# ... write code ...
make joern-cpg && make joern-scan
diff /tmp/joern-baseline.md joern/out/report-tds.md
```

The report itself is not committed (nothing under `joern/out/` is) because it
goes stale the moment the tree moves; the per-query totals in the table above
are the committed form of the baseline. A run of a single query writes
`report-<scope>-<query>.md` instead, so it cannot clobber the full report. There is
deliberately **no CI job** — a 1.6 GB tool producing hints does not belong in a
per-PR gate, where [ClusterFuzzLite](../.clusterfuzzlite) already runs and fails
on actual crashes.

## Ad-hoc queries

`joern/shell.sh` imports the CPG and defines `wireEntry`, `parsers`, `copies`,
`allocs`, `indexes`, `casts` and `where(node)`. Useful shapes:

```scala
// what a harness entry point can reach, two calls deep
cpg.method.name("Feed").callee.callee.name.dedup.l

// loops whose bound is a wire-declared length
cpg.controlStructure.controlStructureType("WHILE|FOR")
   .filter(_.condition.code.matches(".*(?i)(len|length|size|count).*")).code.l

// every early return in a parser -- the guard pattern itself
cpg.method.name("Parse.*").ast.isReturn.code.l

// which methods index `data` without an enclosing if
cpg.call.nameExact("<operator>.indirectIndexAccess")
   .filter(_.argument(1).code == "data")
   .filterNot(_.inAst.isControlStructure.nonEmpty).code.l
```

`joern-scan`, Joern's own default query bundle, also runs on these CPGs
(`joern-scan joern/out/tds.cpg.bin`) and is a reasonable second opinion — its
C/C++ queries are generic, so expect more noise than from this suite.

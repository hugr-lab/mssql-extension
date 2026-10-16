# Running Joern on this project

How to run the CPG query lane. What each query means, the known-benign baseline
and the frontend's limits are in [`joern/README.md`](joern/README.md).

## Once

```bash
brew install joern      # macOS; ~1.6 GB bottle, brings its own JDK
```

It also pulls `openjdk@25`, `astgen` and `php` — the formula ships every
language frontend; this project uses only the C/C++ one. Other platforms:
<https://docs.joern.io/installation>. Nothing else is needed: Joern parses with
Eclipse CDT and **compiles nothing**, so there is no build, no vcpkg tree and no
`compile_commands.json` to prepare.

## Every day

```bash
make joern-cpg          # build the graph   (~15 s)
make joern-scan         # run the suite     (~6 s)  -> joern/out/report-tds.md
```

`joern-scan` reuses the graph, so rebuild with `joern-cpg` after changing any
parsing code. Then read the report:

```bash
open joern/out/report-tds.md        # or: less joern/out/report-tds.md
```

## Narrower, wider

```bash
JOERN_QUERY=q3 make joern-scan                   # one query -> report-tds-q3.md
JOERN_SCOPE=all make joern-cpg joern-scan        # all of src/ (~36 s)
JOERN_SCOPE=all JOERN_QUERY=q6 make joern-scan   # the credential query
```

`JOERN_SCOPE=tds` (the default) is the code that reads the wire: `src/tds/**`,
the SQL Browser datagram parser and the row stager. `JOERN_SCOPE=all` is every
file under `src/`, and is what `q6` (credentials in log and exception messages)
needs — credentials live outside `src/tds/`.

The scripts take the same thing as positional arguments if you prefer:

```bash
joern/build.sh all
joern/scan.sh tds q3,q5
```

Heap and output location are overridable: `JOERN_HEAP=12g`, `JOERN_OUT=/tmp/cpg`.

## Interactively

```bash
make joern-shell        # REPL with the CPG open
```

```scala
cheatsheet()                               // the helpers, and queries to start from
parsers.name.l                             // methods taking a uint8_t* buffer
copies.map(c => (where(c), c.code)).l      // every memcpy with file:line
cpg.method.name("ParseRow").ast.isControlStructure.code.l
```

`:exit` or Ctrl-D leaves. Pre-defined: `wireEntry`, `parsers`, `copies`,
`allocs`, `indexes`, `casts`, `where(node)`, `cheatsheet()`.

## Is there a UI?

Not in the open-source distribution — the REPL *is* the interface, and the
graphical product associated with Joern (Qwiet AI, formerly ShiftLeft) is
commercial and separate. What the OSS install gives you instead:

**A rendered graph per method** — the one that actually helps when reading a
parser, because it draws the guard structure:

```bash
joern/graph.sh ParseError            # control flow -> joern/out/ParseError-cfg.svg
joern/graph.sh ParseError ast        # syntax tree
joern/graph.sh ReadPLPType pdg       # data + control dependence
joern/graph.sh ReadValue cfg all     # from the all-scope CPG
```

Needs `brew install graphviz`; the SVG opens automatically on macOS. Expect a
tall image — `TokenParser::ParseError` alone is ~240 nodes.

**The whole graph in a graph browser**, if you want to click around:

```bash
joern-export joern/out/tds.cpg.bin --repr cpg --format graphml -o joern/out/gml
joern-export joern/out/tds.cpg.bin --repr cpg --format neo4jcsv -o joern/out/neo4j
```

GraphML opens in [Gephi](https://gephi.org) or yEd; the `neo4jcsv` output is for
`neo4j-admin database import`, after which Neo4j Browser gives you a real query
UI over the CPG. Both are heavy for a 20k-call graph and neither knows the CPGQL
steps, so they are for exploring shape, not for asking the questions in
`joern/scan.sc`.

**An HTTP query server**, for an editor or notebook front-end:

```bash
joern --server --server-host 127.0.0.1 --server-port 8080 \
      --server-auth-username joern --server-auth-password <pick-one>
```

That exposes CPGQL over HTTP — an API, not a GUI, and it binds a port that
executes arbitrary Scala, so keep it on loopback.

## The queries

| Id | What it looks for | tds | all |
|---|---|---|---|
| `q1` | Wire-parsing surface inventory | 170 | 182 |
| `q2` | Byte-buffer method that indexes without comparing anything | 15 | 17 |
| `q3` | Length arithmetic that can wrap (#185) or truncate (#181) | 29 | 47 |
| `q4` | Allocation or append sized by a declared length | 40 | 58 |
| `q5` | *(dataflow)* fuzz entry-point parameter → copy size or index | 14 | 16 |
| `q6` | Credential inside a log, exception or format expression | 0 | 2 |
| `q7` | Socket/TLS call whose return value is discarded | 16 | 17 |
| `q8` | Type-punning cast to a multi-byte pointer | 8 | 14 |
| `q9` | Parsing method not reachable from any fuzz entry point | 152 | 164 |

Those counts are the current tree's, and they are the **baseline**: q6, q7, q8
and q9 have benign entries that are expected to be there (`joern/README.md` says
which and why). The signal is a *new* line, so diff against a tree you trust:

```bash
make joern-cpg && make joern-scan
cp joern/out/report-tds.md /tmp/joern-baseline.md
# ... write code ...
make joern-cpg && make joern-scan
diff /tmp/joern-baseline.md joern/out/report-tds.md
```

## Notes

- Nothing under `joern/out/` is committed — the CPGs, the reports, and Joern's
  own `workspace/` working copies. The scripts `cd` there before launching
  Joern, which otherwise writes `workspace/` into the repo root.
- A single-query run writes `report-<scope>-<query>.md`, so it never clobbers
  the full report.
- This lane is **not** in CI. ClusterFuzzLite ([`fuzz/`](fuzz/README.md)) is the
  per-PR gate and fails on actual crashes; these queries produce hints that need
  a human.

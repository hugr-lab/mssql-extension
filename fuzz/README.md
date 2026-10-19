# Fuzzing the mssql-extension TDS wire layer

This extension speaks the Microsoft **TDS wire protocol natively**, in both
directions, and both directions are fuzzed here.

**Inbound** it parses **untrusted, attacker-controllable bytes**: a malicious or
MITM'd SQL Server feeds the client binary token streams, result-set tokens and
type/length fields; a rogue **SQL Browser** (UDP/1434) feeds datagrams. That
parsing surface is the classic bug target, and ASan is the whole oracle for it —
feed it garbage, see whether it reads out of bounds.

**Outbound** it *frames* values for the server, and that half needs a different
oracle. The inputs there are our own values, so ASan catches only the subset of
bugs that overrun a buffer. A declared length that disagrees with the bytes
written is memory-safe and silent: the client exits cleanly and the **server**
desyncs, or stores something truncated. Every framing bug this codebase has had
was of that shape — #489, the review of #490, #181, #91 — and all four were
found by reading code, because nothing automated could see them. So
`fuzz_bcp_encode` carries three explicit oracles of its own (below).

These harnesses drive the codecs directly (in-process, **no network, no real SQL
Server**) under **ASan + UBSan**.

Three modes:

- **Discovery (local, deep):** AFL++ with CmpLog/RedQueen + dictionary + parallel
  cores, in Docker on Linux. Finds new bugs.
- **Regression (CI, short):** ClusterFuzzLite per-PR — fuzz the changed code,
  replay the corpus, **fail on any new crash**.
- **Replay (anywhere, seconds):** `REPLAY=1` links `replay_main.cc` instead of a
  fuzzing engine, so a harness becomes an ordinary executable that replays a
  corpus and fails on the first crash. This is how the two libduckdb-linked
  harnesses run at all — ClusterFuzzLite cannot afford a sanitizer build of
  DuckDB and the AFL++ container has no libduckdb — and it is the only way to
  build any harness with **Apple's toolchain, which ships no libFuzzer runtime**.

## Targets

| Harness | Entry point | Source | DuckDB-free? |
|---|---|---|---|
| `fuzz_browser_response` | `mssql::ParseBrowserResponse(const uint8_t*, size_t)` | `src/connection/instance_resolver.cpp` | ✅ standalone (PoC) |
| `fuzz_tds_tokens` | `tds::TokenParser::Feed` + `TryParseNext()` loop | `src/tds/tds_token_parser.cpp` (+ row reader, column metadata, types, utf16) | ✅ |
| `fuzz_utf16` | `tds::encoding::Utf16LEDecode` / `LegacyUtf16LEDecode` | `src/tds/encoding/utf16.cpp` | ✅ (uses simdutf) |
| `fuzz_envchange_txn` | `tds::FindBeginTxnDescriptor` | `src/tds/tds_token_parser.cpp` | ✅ |
| `fuzz_login_response` | `tds::TdsProtocol::ParseLoginResponse` | `src/tds/tds_protocol.cpp` (+ packet, types, utf16) | ✅ |
| `fuzz_row_stager` | `codec::staging::RowStager::StageRow` / `StageNBCRow` | `src/codec/staging/row_stager.cpp` | ❌ links libduckdb |
| `fuzz_bcp_encode` | `codec::string::PlanColumn` + `ScatterVarBlock` vs `EncodeToBcp` | `src/codec/string_codec.cpp` | ❌ links libduckdb |

The last two are **not in the default `TARGETS`** and do not run in the per-PR
ClusterFuzzLite job: they read and write `duckdb::Vector`, and a sanitizer build
of DuckDB is far too heavy for a time-boxed lane. Build them against a libduckdb
that already exists, and run them in replay mode:

```bash
REPLAY=1 MSSQL_DUCKDB_INC=duckdb/src/include MSSQL_DUCKDB_LIB=build/release/src \
  TARGETS=bcp_encode fuzz/build.sh
DYLD_LIBRARY_PATH=build/release/src fuzz/out/fuzz_bcp_encode fuzz/corpus/bcp_encode
# Linux: LD_LIBRARY_PATH instead of DYLD_LIBRARY_PATH
```

`make fuzz-encode-replay` wraps exactly that.

### `fuzz_bcp_encode`'s three oracles

ASan alone is not an oracle on the write path, so this harness checks the wire
format itself. Each oracle has been shown to fire by planting a canary in the
production code (and each canary reverted):

| Oracle | What it requires | The bug class it catches |
|---|---|---|
| **A. Framing** | the frame accounts for itself: a PLP value is an 8-byte UNKNOWN-length header, a 4-byte chunk length, that many payload bytes, a 4-byte terminator; the declared length equals `plan.lengths[r]` and the frame consumes exactly `plan.wire[r]` | **#489** — a declared length that disagrees with the bytes written. Needs no 2 GiB value to express |
| **B. Differential** | the row path and the columnar path produce **byte-identical** frames for every non-NULL row — which `string_codec.cpp` states as an invariant: *"the columnar path clamps during planning; this keeps the two producing the same bytes"* | **review of #490** — guards added to one path and not the other. The oracle does not care which path is wrong, which is the point |
| **C. Round trip** | the payload decodes back to a **prefix** of the source (a prefix, not equal: clamping to a bounded column is documented truncation, spec 057 §3g) | silent truncation where declaration and payload agree with each other and both are wrong; `Utf8PrefixForUtf16Limit` at a bound that falls inside a surrogate pair (**#91**) |

B and C are complements, not duplicates: a bug in a function **both** paths call
produces the same wrong bytes twice, so B passes and only C sees it. Verified —
the canary for C had to be placed in `Utf16LEEncodeValidDirect` and run against a
*non-ASCII* value, because for ASCII the columnar path takes `WidenAscii` instead
and B catches it first.

**What it found on its first campaign** (issue #496): a PLP value whose UTF-16
encode produced no payload was framed in **16 bytes** where the value ends at
**12**, leaving four stray bytes for the server to read as the next column or
the next token. Reduced from 6000 structured-random inputs to a **single `0xFF`
byte** into `nvarchar(max)`:

```
empty value     input=0 ->  12 bytes:  FE FF FF FF FF FF FF FF | 00 00 00 00
valid 'ab'      input=2 ->  20 bytes:  FE FF FF FF FF FF FF FF | 04 00 00 00 | 61 00 62 00 00 00 00 00
single 0xFF     input=1 ->  16 bytes:  FE FF FF FF FF FF FF FF | 00 00 00 00 | 00 00 00 00   <-- before
single 0xFF     input=1 ->  12 bytes:  FE FF FF FF FF FF FF FF | 00 00 00 00                 <-- after
```

`AppendNVarcharPlp`'s zero-guard tested the *input* length; only the encode
knows the *payload* length, and the legacy fallback that path exists for emits
nothing at all for an invalid or truncated lead byte. Oracle A caught it because
a zero chunk length **is** the PLP terminator, so a 16-byte frame cannot account
for itself. Pinned by `TestPlpEmptyFrameIsTwelveBytes` in
`test/cpp/codec/test_string_codec.cpp` and by three seeds named
`nvarchar_plp_encodes_to_zero_*`.

**What it cannot do, stated plainly:** it cannot build the >2 GiB value #489 and
#490 needed, so it will not rediscover those two instances — it fuzzes their
class at reachable sizes. The 2 GiB arithmetic itself stays covered by the unit
tests on `PlanOneLength` / `PlannedNVarcharBytes` / `RequirePlpWireBytes`, which
drive it with plain numbers precisely because no test can allocate the value.

**Isolation finding:** the whole `src/tds/` parsing layer and
`instance_resolver.cpp` have **zero DuckDB includes** (`grep -L duckdb/`), so the
harnesses link a *minimal* set of `.cpp` files — they do **not** build or link
DuckDB. The only external dependency is **simdutf** (used by `Utf16LEDecode`),
which `build.sh` fetches as a pinned single-header amalgamation and compiles with
the same sanitizer flags. The existing extension build is untouched.

Each harness wraps the parser in `try/catch (const std::exception&)` — malformed
input is *expected* to throw and that is **benign**. Only an ASan/UBSan report or
a signal is a real bug; the harnesses never `catch(...)`, so a sanitizer abort
propagates and crashes the process.

## Priority audit targets

1. **TDS token-stream decoder** (`fuzz_tds_tokens`) — COLMETADATA → ROW
   type+length decoding. The main attack surface.
2. **`ParseBrowserResponse`** (`fuzz_browser_response`) — clean `(data,len)`
   seam, the pipeline PoC.
3. **ENVCHANGE transaction-descriptor scan** (`fuzz_envchange_txn`) — the
   manual `offset`/`token_len` loop (incl. `offset += token_len - 1`, a
   `uint16_t` underflow shape) was extracted from
   `mssql_connection_provider.cpp` into the pure, hardened
   `tds::FindBeginTxnDescriptor(data, len, out[8])` so it is fuzzable. The new
   version keeps `offset` monotonic and bounds every read by `len`; fuzzing
   confirms no server-advertised length causes an OOB or a hang.
4. **`Utf16LEEncodeDirect` / `LegacyUtf16LEEncodeDirect`** — `fuzz_utf16`.

## Build & run locally (Docker, Linux)

> Fuzz on Linux, not the macOS host. The image is multi-arch (runs natively on
> Apple Silicon).

```bash
# Build the discovery image (AFL++ + clang/LLVM)
docker build -t mssql-fuzz -f fuzz/Dockerfile .

# Run a campaign (CmpLog + dict + all cores). Findings persist on the host.
docker run --rm -it -v "$PWD/fuzz/findings:/repo/fuzz/findings" mssql-fuzz \
    fuzz/run.sh tds_tokens          # or: browser_response | utf16
#   fuzz/run.sh tds_tokens 600 4    # 600s/core, 4 cores
```

`fuzz/run.sh <target> [seconds] [cores]`:
1. builds the harness with `afl-clang-fast` (ASan+UBSan) **and** a `AFL_LLVM_CMPLOG=1`
   build (RedQueen — auto-solves magic-byte / length guards),
2. launches `cores` `afl-fuzz` instances (`-M`/`-S`) with `-c <cmplog>` and
   `-x fuzz/tds.dict`,
3. persists `fuzz/findings/<target>/fuzzer*/queue` (corpus) and `.../crashes`.

### Plain libFuzzer build (no AFL++)

`fuzz/build.sh` also produces libFuzzer binaries directly:

```bash
docker run --rm -it -v "$PWD:/repo" -w /repo aflplusplus/aflplusplus \
    bash -c 'CC=clang CXX=clang++ fuzz/build.sh && \
             fuzz/out/fuzz_browser_response -runs=1000000 fuzz/corpus/browser_response'
```

## CI regression (ClusterFuzzLite)

`.github/workflows/cflite-pr.yml` runs on PRs that touch the parsing surface or
`fuzz/`. For each sanitizer (`address`, `undefined`) it builds via
`.clusterfuzzlite/{Dockerfile,build.sh}` (which calls the same `fuzz/build.sh`),
fuzzes the **changed** code for 300s (`mode: code-change`), replays the seed
corpus, dedups, and **fails the PR on a new crash**.

## Triage

When a campaign produces a crash:

```bash
# Reproduce
fuzz/out/fuzz_tds_tokens <crash-file>

# Minimize (libFuzzer)
fuzz/out/fuzz_tds_tokens -minimize_crash=1 -runs=100000 <crash-file>
# …or AFL++:
afl-tmin -i <crash-file> -o min.bin -- fuzz/out/fuzz_tds_tokens

# Hexdump for the report
xxd min.bin
```

Report a real crash with: the crashing input (hex), the minimized repro, the
sanitizer stack trace, the `file:line` source location, and a one-line root cause.

## Layout

```
fuzz/
  fuzz_browser_response.cc   fuzz_tds_tokens.cc   fuzz_utf16.cc
  build.sh        # single shared builder (libFuzzer / AFL++ / ClusterFuzzLite)
  run.sh          # local AFL++ CmpLog campaign
  Dockerfile      # local discovery image (AFL++)
  tds.dict        # token/type/length dictionary
  corpus/<target> # seed inputs
.clusterfuzzlite/ # CI build image + build.sh (delegates to fuzz/build.sh)
.github/workflows/cflite-pr.yml
```

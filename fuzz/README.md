# Fuzzing the mssql-extension TDS parsers

This extension speaks the Microsoft **TDS wire protocol natively** and parses
**untrusted, attacker-controllable bytes**: a malicious or MITM'd SQL Server
feeds the client binary token streams, result-set tokens and type/length fields;
a rogue **SQL Browser** (UDP/1434) feeds datagrams; BCP carries data. That
parsing surface is the bug target. These harnesses drive those decoders directly
(in-process, **no network, no real SQL Server**) under **ASan + UBSan**.

Two modes:

- **Discovery (local, deep):** AFL++ with CmpLog/RedQueen + dictionary + parallel
  cores, in Docker on Linux. Finds new bugs.
- **Regression (CI, short):** ClusterFuzzLite per-PR — fuzz the changed code,
  replay the corpus, **fail on any new crash**.

## Targets

| Harness | Entry point | Source | DuckDB-free? |
|---|---|---|---|
| `fuzz_browser_response` | `mssql::ParseBrowserResponse(const uint8_t*, size_t)` | `src/connection/instance_resolver.cpp` | ✅ standalone (PoC) |
| `fuzz_tds_tokens` | `tds::TokenParser::Feed` + `TryParseNext()` loop | `src/tds/tds_token_parser.cpp` (+ row reader, column metadata, types, utf16) | ✅ |
| `fuzz_utf16` | `tds::encoding::Utf16LEDecode` / `LegacyUtf16LEDecode` | `src/tds/encoding/utf16.cpp` | ✅ (uses simdutf) |
| `fuzz_envchange_txn` | `tds::FindBeginTxnDescriptor` | `src/tds/tds_token_parser.cpp` | ✅ |
| `fuzz_login_response` | `tds::TdsProtocol::ParseLoginResponse` | `src/tds/tds_protocol.cpp` (+ packet, types, utf16) | ✅ |

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

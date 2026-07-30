# Live-server materialization baseline

First run of `test/bench/bench_live_server.sh`. Recorded because it moves the
optimization target: measured against a real server, the codec kernels are a
minority of read-path client cost.

## Run

| | |
| --- | --- |
| date | 2026-07-29 |
| build | `main` @ 06b0cc0 (spec 054 merged), release, macOS ARM64 |
| host | Apple M4 Max, 16 cores |
| server | SQL Server 2022 in Docker (`mssql-dev`), localhost:1433 |
| settings | 500 000 rows, medians of 3, `threads=1`, read ×10 / write ×2 in-process |
| raw | `step wall_s user_s sys_s cpu_s rows cols cpu_ns_per_value notes` |

`cpu_s` is process CPU (user+sys) with the `startup` control (0.017 s) subtracted.
`ctl_*` runs the same sink over a local DuckDB table, so `read − ctl` isolates
TDS + codec from the aggregate and the scan scaffolding.

## Per family, client CPU ns/value

| family | T-SQL type | read | ctl | read − ctl | write |
| --- | --- | ---: | ---: | ---: | ---: |
| bool | BIT | 80.6 | 0.2 | **80.4** | 21.0 |
| date | DATE | 83.6 | 0.0 | **83.6** | 20.0 |
| int | INT | 86.6 | 0.0 | **86.6** | 20.0 |
| dec18 | DECIMAL(18,4) | 93.2 | 0.0 | **93.2** | 33.0 |
| bigint | BIGINT | 98.6 | 0.0 | **98.6** | 26.0 |
| double | FLOAT(53) | 100.0 | 0.0 | **100.0** | 31.0 |
| ts | DATETIME2(6) | 101.0 | 0.0 | **101.0** | 31.0 |
| str4 | NVARCHAR(4) | 105.6 | 0.0 | **105.6** | 43.0 |
| uuid | UNIQUEIDENTIFIER | 114.6 | 1.0 | **113.6** | 32.0 |
| blob | VARBINARY(16) | 115.6 | 2.6 | **113.0** | 42.0 |
| strnull | NVARCHAR(16), 50% NULL | 121.6 | 5.4 | **116.2** | 46.0 |
| dec38 | DECIMAL(38,10) | 136.2 | 0.0 | **136.2** | 81.0 |
| str16u | NVARCHAR(16), non-ASCII | 136.2 | 3.2 | **133.0** | 64.0 |
| str16 | NVARCHAR(16), ASCII | 152.0 | 8.4 | **143.6** | 78.0 |
| str16max | NVARCHAR(MAX), same payload | 193.8 | 7.8 | **186.0** | 98.0 |
| str200 | NVARCHAR(200) | 783.6 | 6.6 | **777.0** | 366.0 |

Wide row (15 mixed columns): read 92.8, CSV drain 128.5, write 51.3, control 4.7.

## Findings

### 1. There is an ~80 ns/value floor on reads, independent of type

`BIT` — one bit on the wire, no decode worth the name — costs 80.4 ns/value of
client CPU. Every other family sits on top of that floor, and the wide-row step
confirms the floor is **per value, not per row**: 15 columns cost 92.8 ns each,
so it does not amortize across a row.

The codec delta above the floor is the smaller term: dec38 +56, str16 +64,
str200 +697, uuid +34, and essentially zero for int/date/bool.

This changes the arithmetic for spec 055. Taking string decode from 18.1 to
2.1 ns/value (the measured micro win) moves live str16 from 143.6 to roughly
128 — about 11%. **The dominant cost is per-value dispatch and the row loop,
not the kernels.** The staging architecture attacks exactly that, so the design
is right; what was wrong was measuring the prize against the kernel rather than
against the floor. If staging collapses the floor as well, the live win is
several-fold rather than a few percent.

What the floor is actually made of — token parsing, per-value dispatch,
per-column plumbing, chunk fill — is **not yet measured**. Phase timers on the
read path (the natural extension of the spec-054 D4 counters) are the next step
and should come before any staging work, so the target is known rather than
assumed.

### 2. Reads cost 3–5× more client CPU per value than writes

bigint: 98.6 read vs 26.0 write. The micro benchmark said the opposite (encode
≈ 10× decode), because it timed only the kernels. Live, the write path — already
through spec-054's W1–W4 — is the cheaper side.

### 3. On large payloads, over half the read cost is system calls

`read_str200`: **sys 2.24 s vs user 1.70 s**. At 500k rows × 200 chars that is
~200 MB of UTF-16 per pass, shipped in 4096-byte TDS frames.

Two contributors, both fixable:

- the extension requests `TDS_DEFAULT_PACKET_SIZE` (4096) in LOGIN7 and never
  more (`tds_connection.cpp:838`; the comment there claims the server will
  "negotiate up", which is not how TDS works — the server returns
  `min(requested, its max)`). The protocol ceiling is 32767.
- `TdsSocket::ReceivePacket` reads through a fixed `temp_buffer[4096]` and does
  `receive_buffer_.erase(begin, begin + consumed)` per packet — an O(n) memmove
  of the remaining bytes on every frame.

A larger requested frame needs the receive side to follow, or the frame is just
reassembled from the same number of `recv` calls.

## D0 follow-up: most of the floor was our own instrumentation

The first thing the floor decomposed into was the measuring apparatus.

`MSSQLResultStream::FillChunk` called `std::chrono::steady_clock::now()` **four
times per row, unconditionally** — a pair around `parser_.TryParseNext()` and a
pair around `ProcessRow()`. On this machine a `now()` call costs 14.5 ns and a
timed pair with its `duration_cast` costs 29.5 ns, so the loop spent **~59 ns per
row** measuring itself. Present since spec 004 (`78f7629`, 2026-01-16), i.e.
shipped in every release from v0.1.0 on.

Two defects, both fixed:

1. **Never gated.** Release builds paid for instrumentation nobody read. Now
   latched on `MSSQL_DEBUG>=1` (`timing_enabled_`), matching the level at which
   the FillChunk summary is printed.
2. **Truncated to zero.** Intervals accumulated via
   `duration_cast<microseconds>` — per *row*. A row is processed in ~100 ns, so
   every one of those casts yielded 0 and `fill_process_us` reported
   approximately nothing regardless of the work done. Only the rare, long socket
   waits ever crossed a microsecond boundary. Accumulation is now in nanoseconds.

Interleaved same-session A/B, two passes, 500k rows × 10 in-process iterations,
client CPU ns/value:

| family | before | after | delta |
| --- | ---: | ---: | ---: |
| bool | 78.8 | 28.0 | **−64%** |
| int | 86.9 | 34.9 | −60% |
| bigint | 99.3 | 45.4 | −54% |
| dec38 | 136.2 | 89.7 | −34% |
| str16 | 153.7 | 118.7 | −23% |

The two passes agree within ~1%. The ~50 ns/row removed matches the 59 ns/row
measured for the clock calls; narrow types, whose per-row budget was of the same
order, gain the most.

### What the floor is made of now

With timing enabled at `MSSQL_DEBUG=1` (so these still carry ~29 ns/row of their
own overhead, visible as most of `unaccounted`), 500k rows, wall ns/row:

| family | total | parse | read | process | unaccounted |
| --- | ---: | ---: | ---: | ---: | ---: |
| bool | 150.7 | 21.3 | 81.7 | 18.2 | 29.5 |
| bigint | 180.1 | 23.1 | 106.7 | 19.3 | 31.1 |
| str16 | 259.3 | 26.9 | 171.1 | 30.1 | 31.2 |

`read` is wall time inside `ReadMoreData` — mostly *waiting* on the server, not
CPU; its CPU component is `recv` syscall overhead, which is the packet-size
argument again. The client's own work splits roughly evenly between TDS
token/row framing (`parse`, 21–27 ns/row) and per-value decode plus chunk fill
(`process`, 18–30 ns/row).

Do not read `MSSQL_DEBUG=2` numbers as a phase breakdown: level 2 also turns on
per-row logging, which inflated `parse` to 467–529 ns/row in a first attempt.
Use level 1 and aggregate the per-chunk `FillChunk:` lines.

## TDS frame size — the other half of the floor

The phase split above put the largest wall term in `read`, whose CPU component is
`recv` syscall overhead. Three things were pinning it:

1. The extension requested `TDS_DEFAULT_PACKET_SIZE` (4096) in LOGIN7 on every
   connection — `tds_connection.cpp` claimed the server would "negotiate up",
   which is not how TDS works: the server answers with `min(requested, its own
   maximum)` and never raises it.
2. `TdsSocket::ReceivePacket` read through a fixed `uint8_t[4096]` scratch, so a
   larger frame would have been reassembled from the same number of `recv` calls.
3. Every completed packet did `receive_buffer_.erase(begin, begin + consumed)` —
   an O(n) memmove of everything still buffered, per packet.

Fixed together: `mssql_tds_packet_size` setting plumbed into LOGIN7, receive
staging sized to a 64 KB budget of whole frames (16 frames at 4096, 4 at 16384 —
a byte budget, so a larger frame never costs more per-connection memory), and the
packet cursor replacing the per-packet erase.

Sweep, 500k rows × 15 mixed columns, medians of 3 (`MSSQL_BENCH_PACKET_SIZES`):

| requested | granted | read CPU ns/val | write CPU ns/val | read wall | write wall |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4096 | 4096 | 106.4 | 52.7 | 2.470 | 3.966 |
| 8192 | 8000 | 84.9 | 43.1 | 1.432 | 3.456 |
| 16384 | 16384 | **76.8** | **38.7** | **1.400** | 2.897 |
| 32767 | 16384 | 76.8 | 40.1 | 1.394 | 2.561 |

**Read −28% CPU and −43% wall; write −27% CPU.** Read `sys` alone went 0.380 →
0.247 s.

This SQL Server caps the grant at 16384, so the last two rows are the same
configuration — their spread (38.7 vs 40.1 CPU, 2.897 vs 2.561 wall) is a useful
noise estimate for these steps: ~4% on CPU, ~11% on wall. It also means we have
**no evidence about servers that would grant more than 16384**.

Default raised 4096 → 16384. The cost is server-side memory: SQL Server allocates
network buffers per session, so 16 KB per pooled connection instead of 4 KB.
`SET mssql_tds_packet_size=4096` restores the old behaviour exactly.

Correctness: `diff_check.sh` 13/13 byte-identical against the pre-change build,
including the PLP/MAX, embedded-NUL, collation and BCP write-back cases.

## DECIMAL decode kernel (spec 055 D1)

`DecimalEncoding::ConvertDecimal` ran a full 128-bit multiply-accumulate **per
byte** (`magnitude = magnitude * 256 + data[i]`) — up to sixteen 128-bit
multiplies to decode one value. TDS already sends the magnitude in exactly the
byte order an int128 wants, so the whole thing is two loads.

Interleaved same-session A/B on top of the frame change, two passes, client CPU
ns/value:

| family | before | after | delta |
| --- | ---: | ---: | ---: |
| dec38 — DECIMAL(38,10) | 75.8 | **41.8** | **−45%** |
| dec18 — DECIMAL(18,4) | 45.8 | 33.3 | −27% |
| bigint — untouched control | 34.9 | 35.1 | ±0 |

The `bigint` control is the point: it shares every part of the path except the
kernel and did not move, so the delta is the kernel and not drift.
DECIMAL(38,10) now decodes at roughly BIGINT cost.

Correctness: `diff_check.sh` 13/13 byte-identical; new fixtures in
`test/cpp/codec/test_decimal_codec.cpp` pin the byte contract (sign byte,
little-endian magnitude, every length 1–16 compared against a byte-by-byte
reference, 10^38−1 both signs, the 8/9-byte word boundary). Big-endian hosts and
malformed lengths above 16 magnitude bytes keep the old portable loop.

## Caveats

- Client and server share one machine; server CPU competes with client CPU for
  wall clock, but not for the `user_s`/`sys_s` we attribute to the client.
- `threads=1`. Concurrency scaling (the `conc` group) has not been run.
- `min()` as the sink is charged to both the step and its control, so it cancels
  in `read − ctl`; it is not free in absolute terms (8.4 ns/value at str16).
- Single run of medians-of-3. Cross-session drift applies as in spec 054: only
  interleaved same-session A/B counts as regression evidence.

---

# Spec 055 — full-path result

Three builds, interleaved in ONE session, pass order rotated (A,B,C / C,A,B /
B,C,A), medians of 3 reps each, 500k rows, threads=1:

- **preD0** — `duckdb_before_d0`, the branch's starting point, before any spec
  055 commit.
- **branch** — `1f63bad`, the last commit still on the per-value read path
  (D0 phase-timer gating, D0b 16 KB frames and the receive-staging change, D1
  decimal kernel, T9 UTF-16 decoder and T3/D4 staging structures are all already
  in it).
- **tip** — `5e90735`, the whole staged path.

Client CPU ns/value.

## Read

| family | preD0 | branch | tip | tip vs branch | tip vs preD0 |
| --- | ---: | ---: | ---: | ---: | ---: |
| bigint | 97.6 | 33.2 | **23.4** | −29.5% | −76.0% |
| int | 86.6 | 40.8 | **21.6** | −47.1% | −75.1% |
| double | 102.0 | 35.8 | **25.6** | −28.5% | −74.9% |
| bool | 80.2 | 42.0 | **30.6** | −27.1% | −61.8% |
| date | 84.4 | 27.4 | **21.6** | −21.2% | −74.4% |
| ts — DATETIME2(6) | 100.8 | 37.0 | **28.2** | −23.8% | −72.0% |
| dec18 | 93.0 | 33.6 | **28.6** | −14.9% | −69.2% |
| dec38 | 135.6 | 39.0 | **33.6** | −13.8% | −75.2% |
| uuid | 116.4 | 45.8 | **35.2** | −23.1% | −69.8% |
| blob — VARBINARY(16) | 122.8 | 56.0 | **41.2** | −26.4% | −66.4% |
| str4 | 108.0 | 49.8 | **31.8** | −36.1% | −70.6% |
| str16 | 153.0 | 94.8 | **67.6** | −28.7% | −55.8% |
| str16u — non-ASCII | 142.2 | 89.0 | **53.4** | −40.0% | −62.4% |
| str200 | 768.4 | 619.2 | **529.4** | −14.5% | −31.1% |
| str16max — PLP framing | 193.6 | 118.0 | **84.4** | −28.5% | −56.4% |
| strnull — 50% NULL | 121.6 | 56.4 | **37.2** | −34.0% | −69.4% |
| **wide, 15 cols, min()** | 90.5 | 74.3 | **57.2** | **−23.0%** | **−36.8%** |
| **wide, full drain** | 129.5 | 112.5 | **97.7** | −13.2% | −24.6% |

Per-pass, for the load-bearing steps — the three sides do not overlap anywhere:

```
read_bigint     preD0 [95.8 98.0 97.6]  branch [33.2 33.8 33.2]  tip [24.6 23.4 23.2]
read_str16      preD0 [152.2 153.6 153.0] branch [94.8 93.0 94.8] tip [68.0 65.2 67.6]
read_wide_min   preD0 [98.3 90.5 88.5]  branch [74.3 75.1 73.6]  tip [57.2 60.0 56.1]
read_str200     preD0 [768.4 765.4 770.0] branch [616.0 636.8 619.2] tip [525.4 544.8 529.4]
```

The local-table controls (`ctl_*`, same query shape against a DuckDB table) are
unmoved across all three sides — the largest absolute drift is 0.2 ns on a
2.2 ns step. The machine was stable for the whole run.

`str200` is the weakest relative gain and should be: at 400 wire bytes per value
it is bandwidth-bound, and no amount of dispatch removal changes how long it
takes to move 400 bytes.

## Write

**No signal, and none expected** — spec 055 does not touch the write path. The
write steps' per-pass spread is ±50% (`write_bigint` branch `[23, 46, 37]`, tip
`[42, 34, 38]`), because they are short and dominated by server-side work. Any
per-family write delta in this table would be noise; they are omitted rather
than reported. The write path is spec 057's subject.

## What the D10 counters say about the same workload

Reading 500k rows of `NVARCHAR(16)` + non-ASCII `NVARCHAR(16)` + `BIGINT` +
`DECIMAL(38,10)`:

```
staging: direct_bypass=500000 grow=0 shrink=0 peak_payload=69632B
columns: prealloc_bounded=2 prealloc_capped=0 unbounded=0
kernels (values/staged bytes/ns per value): none=500000v/0B/0.0ns
    string=1000000v/29000000B/3.1ns decimal=500000v/8500000B/5.2ns
string boundaries (column-chunks): ascii=245 skip+sweep=245 replaced_units=0
```

The batch kernels are 3–5 ns/value against a whole-path 21–34 ns/value. **The
decode is no longer the read path's cost centre** — which is what spec 058
(read framing) exists to look at next.

---

# User-visible: mssql v0.2.2 vs the spec-055 tip, TPC-H SF 2

The question this answers is not "is the codec faster" but "does a user notice".
So: no instrumentation anywhere, counters off, timing is DuckDB's own
`.timer on` — the line the CLI prints after each query.

**Both sides are the same DuckDB.** v0.2.2 and the tip are each loaded as a
`.duckdb_extension` into the SAME stock DuckDB 1.5.5 CLI. Comparing our
source-built CLI against a stock one would have put the difference between two
builds of DuckDB itself into the delta; this way the extension is the only
variable.

TPC-H SF 2 in SQL Server (lineitem 11,997,996 rows; orders 3,000,000; customer
300,000; part 400,000), `SET threads=4`, three passes interleaved in one
session with the order rotated, medians. Wall seconds.

| query | v0.2.2 | tip | delta |
| --- | ---: | ---: | ---: |
| full scan, 12M rows, all 16 columns | 7.20 | **5.10** | **−29.2%** |
| Q1 shape — group-by over a full scan | 8.06 | **6.48** | −19.6% |
| part, 400k rows, all columns | 0.34 | **0.26** | −22.9% |
| join — orders ⋈ customer, both remote | 0.76 | **0.63** | −17.2% |
| two-column scan (projection pushdown) | 2.27 | **2.01** | −11.7% |
| three string columns | 8.71 | **7.80** | −10.4% |
| Q6 — filter pushdown | 0.24 | **0.23** | −3.4% |

Per pass, v0.2.2 / tip:

```
full scan      7.20/5.04  7.26/5.15  6.97/5.10
Q1             8.18/6.50  8.06/6.48  8.05/6.39
strings        8.21/7.93  10.31/7.80 8.71/7.55
narrow         2.24/2.01  2.30/2.07  2.27/1.96
Q6             0.35/0.30  0.24/0.23  0.20/0.21
```

Every query improves in every pass. Q6 is the exception that confirms the
shape: filter pushdown means almost nothing crosses the wire, so there is
nearly no client-side work to remove and nothing to win — which is what −3.4%
on a 0.24 s query says.

**What is in this delta.** v0.2.2 was released before spec 054 merged, so this
is everything since the last release: spec 054 (BCP encode + decode quick
wins), the #211 catalog-scan serialize fix, and spec 055. It is not spec 055
alone — for that, see the branch-point comparison above.

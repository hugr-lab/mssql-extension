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

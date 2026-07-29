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

## Caveats

- Client and server share one machine; server CPU competes with client CPU for
  wall clock, but not for the `user_s`/`sys_s` we attribute to the client.
- `threads=1`. Concurrency scaling (the `conc` group) has not been run.
- `min()` as the sink is charged to both the step and its control, so it cancels
  in `read − ctl`; it is not free in absolute terms (8.4 ns/value at str16).
- Single run of medians-of-3. Cross-session drift applies as in spec 054: only
  interleaved same-session A/B counts as regression evidence.

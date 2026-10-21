# TPC-H, local run (2026-10-20)

The first measurement with `tpch_bench.py`, taken to build and debug the harness.

**Setup.** One Apple-silicon Mac runs both the clients and SQL Server 2022 in docker, the server under x86 emulation. The numbers therefore compare clients with one another; they are not absolutes, and column C here is not what native SQL Server does on real hardware.

| client | what runs the query |
|---|---|
| **A** | DuckDB 1.5.6 + mssql 0.2.5 (community) |
| **B** | DuckDB 2.0 (`v2.0-cyanoptera` `2ec7e8401c4`) + mssql 0.3.0 (`bfd347f`) |
| **Bscan** | B with `remote_pushdown false` |
| **C** | SQL Server running the queries itself |

All tables are clustered columnstore and loaded with `tpch_bench.py load`, with TLS on.

## SF10, interleaved timed runs

Two repetitions per client, each after a warmup pass; the client order rotates between repetitions. Medians in seconds.

| | A | B | Bscan | C |
|---|---|---|---|---|
| sum of 22 queries | 249.5 | **182.3 (0.73×)** | 221.9 (0.89×) | 7.0 |
| q18 (B pushes it whole) | 24.9 | **0.84** | 14.1 | 0.79 |
| q13 (B pushes it partly) | 11.3 | **1.98** | 14.5 | 1.02 |
| q10 | 12.5 | 6.05 | 6.13 | 0.46 |
| q16 | 2.75 | 1.31 | 1.57 | 0.31 |
| q09 | 35.9 | 24.6 | 29.3 | 0.51 |
| q12 | 0.36* | 3.31 | 3.93 | 0.22 |
| q11 | 2.90 | 3.33 | 3.93 | 0.15 |

The table uses TPC-H's own string types: `CHAR(n)` / `VARCHAR(n)`, in the server's default collation `SQL_Latin1_General_CP1_CI_AS`.

\* **A gives wrong answers for q07, q08, q12 and q19 on this schema.** 0.2.5 hands a `CHAR(n)` value over padded (#405, fixed in 0.3.0), so a comparison that DuckDB evaluates itself never matches. In q12 that made the join's build side empty, and A skipped reading `orders` altogether, hence its 0.36 s. B, Bscan and C match the DuckDB oracle on all 22 queries. Without the four wrong ones, B against A is still 0.72×.

**q11.** DuckDB 2.0 reads `partsupp` twice, once for the query and once for its `HAVING` subquery, where 1.5.6 reads it once. This is a planner difference, not the extension.

## What goes to the server (SF10 traces)

`tpch_bench.py trace` reads `sys.dm_exec_query_stats` after each query: the statements a client sent, the rows they returned and the server's time. Single runs with a cold plan cache.

- **On the scan path the server's CPU is the bottleneck.**
  - A moves 910 M rows into DuckDB over the 22 queries, B 758 M.
  - The server spends about 0.15–0.45 µs of CPU on every row it hands over, and a query's wall time is close to that CPU time.
  - Less data on the wire is the only lever that moves it: the more of a query runs on the server, the faster.
- **What B sends whole depends on the string types** (next table). The other queries stop on the writer's vetoes, recorded in `specs/079-select-pushdown/after-0.3.0.md` ("Measured on TPC-H"):
  - decimal arithmetic over different types: about half of the queries;
  - `ORDER BY` a string without a LIMIT;
  - `YEAR()`;
  - DOUBLE arithmetic;
  - `substring`.

## The string column types (SF10 traces)

The same data in four schemas, set with `--strings` / `--collation`:

| strings | B wall | B server CPU | A wall | B/A | B sends whole |
|---|---|---|---|---|---|
| `CHAR`/`VARCHAR`, code page, CI | 195.5 s | 232 s | 228.9 s* | 0.85 | q18 |
| `NVARCHAR(n)` | 172.3 s | 240 s | 214.4 s | 0.80 | q18 |
| `VARCHAR(n)`, `Latin1_General_100_CI_AS_SC_UTF8` | 153.2 s | 230 s | 212.1 s | 0.72 | q02, q18, q21 |
| `VARCHAR(n)`, `Latin1_General_100_BIN2_UTF8` | **150.4 s** | **188 s** | 208.0 s | **0.72** | q02, q18, q21 |

\* wrong answers, as above. A is correct on the other three schemas.

- **A code-page `char` / `varchar` costs the server a CAST on every row.** The read sends it as `NCHAR` / `NVARCHAR`. NVARCHAR or UTF-8 columns avoid that, cutting 12–23 % from the scan path; per row, the server's CPU drops by 25–50 % on the string-heavy scans (q01, q07, q08, q10, q12).
- **UTF-8 `varchar` is ordered by its bytes under a LIMIT**, which is how DuckDB orders strings. So q02 and q21 (`ORDER BY … LIMIT 100`) go to the server whole: q21 20.7 s → 0.47 s.
- **BIN2 makes string comparison cheap on the server.** q13's `NOT LIKE '%special%requests%'` costs 20 s of server CPU under BIN2 (and the code page) against 56–63 s under NVARCHAR or the CI UTF-8 collation. BIN2 is also the collation whose comparisons and order are DuckDB's own; `mssql_utf8_collation` defaults to it for CTAS.

## The raw output

[`measured/2026-10-20-local/`](measured/2026-10-20-local/) holds the files these tables come from, as `tpch_bench.py` wrote them under `results/`:

| directory | what is in it |
|---|---|
| `sf1/`, `sf10/` | the timed runs: `report.md` (medians and ratios) and `timings.csv` (every rep); `sf10/` is the SF10 table above. SF1 was the harness's first pass |
| `*/trace/` | `trace-<client>.md`: per query, the statements the client sent, rows, server elapsed / CPU / reads. `sf10/trace` is the code-page `CHAR`/`VARCHAR` schema; `sf10-nvarchar`, `sf10-utf8-ci` and `sf10-utf8-bin2` are the other three rows of the string table |
| `sf10-utf8-bin2/explain-B.txt` | `EXPLAIN` of all 22 queries under B on the BIN2 UTF-8 schema: the vehicles and the DuckDB operators left above them |

Left out: the `verify` CSVs (each client's result per query against the DuckDB oracle, 6 MB), which `tpch_bench.py verify` writes again, and the `.log` files the repository ignores, which hold the same numbers as `timings.csv` and `trace-*.md`.

## Not yet measured

- The rowstore layout.
- SF ≥ 100.
- A remote client (network between DuckDB and the server).
- Native SQL Server on real hardware.

The README's Hetzner section describes that run.

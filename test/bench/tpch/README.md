# TPC-H in SQL Server, queried from DuckDB

`tpch_bench.py` runs the 22 TPC-H queries against data that lives in SQL Server. It compares, query by query:

| client | what runs the query |
|---|---|
| **A** | DuckDB 1.5.6 + mssql 0.2.5: the stock CLI, with the extension installed from community |
| **B** | DuckDB 2.0 + mssql 0.3.0, remote pushdown on (the default) |
| **Bscan** | B with `remote_pushdown false` on the ATTACH: every query takes the scan path |
| **C** | SQL Server itself: the same queries in T-SQL (`queries/tsql/`), sent through `mssql_scan` |

Other commands:
- `trace`: what each client sends to the server, read from `sys.dm_exec_query_stats`;
- `drop`: remove the test databases;
- `--strings spec|nvarchar|utf8` and `--collation`: the string schema.

`queries/duckdb/` holds DuckDB's own TPC-H texts (`tpch_queries()`). `queries/tsql/` holds the same queries in T-SQL:
- `substring(x, a, b)`, `year(x)` and `TOP n` replace DuckDB's spellings;
- `CAST('…' AS date)` replaces the `date '…'` literal;
- q15 groups by `l_suppkey` (T-SQL cannot group by a select-list alias).

## Running it

The server credentials come from the environment and are never printed: `MSSQL_BENCH_HOST`, `_PORT`, `_USER`, `_PASSWORD`, falling back to the test suite's `MSSQL_TEST_*`.

```sh
B=test/bench/tpch/tpch_bench.py
python3 $B selftest                           # check the harness's own verdicts; no server, no database, no CLI
python3 $B --sf 1 gen                         # parquet into test/bench/tpch/data/sf1 (needs DuckDB with tpch: the A CLI)
python3 $B --sf 1 --layout columnstore load   # CREATE DATABASE tpch_sf1_columnstore, DDL, INSERT via BCP, stats
python3 $B --sf 1 verify                      # every client once, compared with DuckDB over the same parquet
python3 $B --sf 1 explain                     # which queries client B sends to the server whole
python3 $B --sf 1 --reps 3 --run-id sf1 run   # interleaved timed runs -> results/sf1/timings.csv
python3 $B --timings test/bench/tpch/results/sf1/timings.csv --report report.md report
```

**Clients.**
- A is `$DUCKDB_A`, by default `~/.duckdb/cli/latest/duckdb`: a stock 1.5.6 where `INSTALL mssql FROM community; INSTALL tpch` has been run.
- B, Bscan and C use `$DUCKDB_B`, by default `build/release/duckdb` of this repo. Once DuckDB 2.0 is released, point it at a stock 2.0 CLI with `mssql` installed from community.

**Layouts.**
- `--layout columnstore` makes every table a clustered columnstore index: the analytic layout, and what favours C most.
- `--layout rowstore` makes every table a clustered primary key.
- Strings use the TPC-H specification types (`CHAR(n)` / `VARCHAR(n)`). Numbers use dbgen's (`BIGINT` keys, `DECIMAL(15,2)`), so a join compares values of one type.

**Protocol.**
- Each repetition runs every client in a fresh session: ATTACH, then `--warmup` untimed passes, then one timed pass.
- The client order rotates with each repetition, so no client is always first.
- Times are the CLI's `.timer` wall times. `report` gives the median per query, the ratio against A, and the sum.
- Each session's full output is kept beside `timings.csv`.

**Correctness first.** `verify` compares each client with DuckDB over the very parquet the server was loaded from. A wrong answer makes that query's timing meaningless, and the report should say so.

How a value is compared depends on **what the oracle says the column is**, not on how the value happens to print. `verify` writes the column types beside each answer (`DESCRIBE` each query, one extra statement per query in the same session) and compares per column:

| the oracle's column type | rule |
|---|---|
| `DECIMAL(p,s)`, the integer family, `VARCHAR`, `DATE`, `BOOLEAN`, `TIMESTAMP` | **exact** — compared by VALUE, so a client padding `1.5` to `1.500000` matches while a cent of difference does not |
| `DOUBLE`, `FLOAT`, `REAL` | 1e-6 absolute or 1e-13 relative, whichever is larger |
| anything unrecognised | **exact** — a new type fails loudly rather than quietly taking a tolerance |

For q01, the query at the centre of every defect this comparison has had, `DESCRIBE` gives seven exact columns and three tolerance ones:

```
sum_qty = HUGEINT            avg_qty   = DOUBLE
sum_base_price = DECIMAL(38,2)   avg_price = DOUBLE
sum_disc_price = DECIMAL(38,4)   avg_disc  = DOUBLE
sum_charge = DECIMAL(38,6)
count_order = BIGINT
l_returnflag, l_linestatus = VARCHAR
```

The tolerance exists only because DuckDB returns `avg(decimal)` and `decimal / decimal` as `DOUBLE` (q01's three averages, q08's `mkt_share`), and the extension's pushed `CAST(SUM(x) AS float) / COUNT_BIG(x)` differs from DuckDB's `avg` in its last bits by design. 1e-13 leaves about 724 ulps of margin at an SF1000 magnitude.

If the types are missing the run reports `NO TYPES` rather than comparing: a comparison that cannot tell an exact column from a double is what this replaced.

Nothing is stripped, so `CHAR(n)` trailing-blank padding is a mismatch and gets its own `PADDING` verdict — that is the #405 class, and an exact comparison is the only thing that can see it. Rows must be in order, or match as a multiset, which is reported as `ok (same rows, different order)`.

**The harness checks its own judgements.** `selftest` runs every rule above against the cases that were once decided wrongly — q08's two `mkt_share` values 0.00705 apart passing inside a 0.011 absolute floor, a count off by one row passing inside 1e-6 relative, an aggregate 120 units out at SF1000 magnitude passing inside 1e-9 relative, a `Catalog Error` published as a 4 ms timing, a session timeout crashing the whole run — plus the verdicts, the timeout path and the report's sum row. It needs no server, no database and no DuckDB binary, so it runs anywhere and in CI. A benchmark harness that cannot be wrong is worth more than one that is fast.

## Results

See `RESULTS-local.md` for the first local run: SF1 and SF10, four string schemas, and per-query server traces. In short:
- 0.3.0 beats 0.2.5 by 20–28 %, and by orders of magnitude on the queries it sends to the server whole.
- On TPC-H's own `CHAR(n)` schema, 0.2.5 fails 13 of the 22 queries: **4 wrong answers** (#405 — q07 and q12 return no rows at all, q08 and q19 differ) and **9 more whose values differ only in `CHAR(n)` padding**, which the old comparison stripped away before looking.
- On the scan path the server's CPU per row handed over is the bottleneck.
- Which queries go whole depends on the string types: UTF-8 `varchar` lets `ORDER BY … LIMIT` go, and BIN2 is the cheapest for the server.

Remove the databases afterwards with `python3 tpch_bench.py --all drop`, which drops every `tpch_sf*` database on the server.

## On a rented machine (Hetzner)

The local server is the docker image under Rosetta on an ARM Mac. Column C is meaningless there, and A against B is only a relative comparison. For published numbers:
- **Machine.** A dedicated x86 box: for SF100, 16+ cores, 128 GB RAM and 1 TB NVMe. SF300 / SF1000 need proportionally more disk: raw data is about 1 GB per SF, plus parquet and SQL Server files.
- **Server.** SQL Server 2022 for Linux, natively (docker `mcr.microsoft.com/mssql/server:2022-latest` is fine on x86), with `max server memory` set and the data files on the NVMe.
- **Clients** on the same box: DuckDB 1.5.6 and 2.0 CLIs with `mssql` from community. A client on another box would add the network to every scan, which is the point of B's pushdown but not of A against B.
- **Generation.** At SF ≥ 100, `gen` holds the data in one DuckDB file: give it the disk and `--data-dir`.
- **Server settings.** Pin and record the version, the `max server memory`, the MAXDOP and the database collation in the report; a CI collation versus a UTF-8 one changes the read path.

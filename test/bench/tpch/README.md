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

The server credentials come from the environment and are never printed: `MSSQL_BENCH_HOST`, `_PORT`, `_USER`, `_PASSWORD`, falling back to the test suite's `MSSQL_TEST_*`. The DSN goes to the CLI on **stdin**, never in `argv` or a file, so it reaches neither `ps` nor shell history.

The masking works on the output's side, not the secret's: it redacts the **value** of `Password` wherever the text carries a connection string. So every form the password travels in is covered by construction — brace-quoted, `esc()`'d into a SQL literal, or both — and nothing outside a `key=value` is rewritten, which is what keeps a short password from rewriting the timings beside it (`real 11.500` once came back `real 11.***00`). The password, the user name and the database are brace-quoted in the DSN, so a `;` in any of them cannot add a key; the host and port cannot be brace-quoted (the value of `Server` is `host,port`), so they are checked instead, and a host carrying a `;` or a brace is refused naming the variable it came from (issue #509, review of #546).

`python3` must be 3.8 or newer, which the harness checks and says. (It used `argparse.BooleanOptionalAction`, which is 3.9, so on the Hetzner target below — Ubuntu 20.04, Python 3.8.10 — it died at `--help`: issue #507.)

`--db` and `--collation` must be bare names (letters, digits and `_`), checked once for **every** command before it runs: both reach the server inside `ALTER` / `DROP` / `CREATE DATABASE`, where the name travels through `QUOTENAME`, and `trace` also formats `--db` into T-SQL (`DB_ID(N'…')`), where escaping for the DuckDB literal around it is not the same escape. Anything but a bare name is refused by name rather than quoted.

```sh
B=test/bench/tpch/tpch_bench.py
python3 $B selftest                           # check the harness's own verdicts; no server, no database, no CLI
python3 $B --sf 1 gen                         # parquet into test/bench/tpch/data/sf1 (needs DuckDB with tpch: the A CLI)
python3 $B --sf 1 --layout columnstore load   # CREATE DATABASE tpch_sf1_columnstore, DDL, INSERT via BCP, keys, stats
python3 $B --sf 1 verify                      # every client once, compared with DuckDB over the same parquet
python3 $B --sf 1 --clients B explain         # which queries client B sends to the server whole
python3 $B --sf 1 --reps 3 --run-id sf1 run   # interleaved timed runs -> results/sf1/timings.csv
python3 $B --timings test/bench/tpch/results/sf1/timings.csv --report report.md report
```

**Clients.**
- A is `$DUCKDB_A`, by default `~/.duckdb/cli/latest/duckdb`: a stock 1.5.6 where `INSTALL mssql FROM community; INSTALL tpch` has been run.
- B, Bscan and C use `$DUCKDB_B`, by default `build/release/duckdb` of this repo. Once DuckDB 2.0 is released, point it at a stock 2.0 CLI with `mssql` installed from community.

**The database name carries the whole schema**, so two schemas never share one: `--sf`, `--layout`, `--strings` and `--collation` each contribute a component. The four rows of the string table are

```sh
python3 $B --sf 10 load                                                     # tpch_sf10_columnstore
python3 $B --sf 10 --strings nvarchar load                                  # tpch_sf10_columnstore_nvarchar
python3 $B --sf 10 --strings utf8 load                                      # tpch_sf10_columnstore_utf8
python3 $B --sf 10 --strings utf8 \
         --collation Latin1_General_100_BIN2_UTF8 load                      # tpch_sf10_columnstore_utf8_latin1general100bin2utf8
```

and every later command takes the same flags to address the same database. Until issue #507 the name left `--collation` out, so the last two were one name and loading either dropped the other — which is why the published BIN2 row needed a `--db` that no documented command produced.

**Layouts.**
- `--layout columnstore` makes every table a clustered columnstore index: the analytic layout, and what favours C most.
- `--layout rowstore` makes every table a clustered primary key.
- **Either way the tables carry TPC-H's primary keys** (issue #507). The rowstore layout declares its key with the table; the columnstore layout adds it `NONCLUSTERED` *after* the load, because a clustered columnstore that already carries a nonclustered index refuses the parallel bulk writers the load depends on. The key is not cosmetic: without a usable unique key a table has no rowid key, and the remote-pushdown join-gain check loses its unique-key rung and falls back on `mssql_pushdown_join_rows_threshold` — so the layout was changing pushdown decisions, not only storage.
- Strings use the TPC-H specification types (`CHAR(n)` / `VARCHAR(n)`). Numbers use dbgen's (`BIGINT` keys, `DECIMAL(15,2)`), so a join compares values of one type.

**Protocol.**
- Each repetition runs every client in a fresh session: ATTACH, then, per query, `--warmup` untimed runs of that query followed by one timed run. (Warming a whole pass at a time, as it did before issue #507, left q01 the coldest query in the timed pass and q22 the hottest — a bias along the query order, inside the warmup that exists to remove one.)
- The client order rotates with each repetition, so no client is always first.
- Times are the CLI's `.timer` wall times, with `.mode csv` pinned: the renderer runs inside the timer, and duckbox measures every column over every row and is not the same code in 1.5.6 and 2.0.
- `report` gives the median per query, the ratio against A, and the sum. Without `--timings` it takes `--out/<run-id>/timings.csv`.
- Each session's full output is kept beside `timings.csv`. **Re-running a subset of the clients keeps the others' rows** in `timings.csv` and clears only the re-measured clients' session logs, so a four-client run is no longer destroyed by a one-client repeat (issue #507).

**Correctness first.** `verify` compares each client with DuckDB over the very parquet the server was loaded from. A wrong answer makes that query's timing meaningless, and the report should say so.

**The oracle belongs to no client.** It is computed on *both* engines — the 1.5.6 CLI and the 2.0 one — over that parquet, and they must agree before a client's answer is taken as right or wrong. Where they disagree the query is reported as `ORACLE DISAGREEMENT` and counted as a failure of its own, once, however many clients read it; each client is then still graded against the oracle of the engine **it** runs on, and the verdict says `[split oracle: A]` or `[split oracle: B]`. That keeps a permanent 1.5.6-vs-2.0 difference from quietly removing a query from the comparison for good, while the weaker guarantee — client A checked against its own engine, which is the thing this fixed — stays visible in the line (review of #548). It used to run on client A's binary alone, so A was graded by its own engine while B, Bscan and C were graded by a different one: a wrong answer shared by DuckDB 1.5.6 and mssql 0.2.5 was invisible by construction (the #405 padding answers had to be found by hand), and a 1.5.6-versus-2.0 difference showed up as a diff for B and Bscan only, reading like an extension bug (issue #507).

How a value is compared depends on **what the oracle says the column is**, not on how the value happens to print. `verify` writes the column types beside each answer (`DESCRIBE` each query, one extra statement per query in the same session) and compares per column:

| the oracle's column type | rule |
|---|---|
| `DECIMAL(p,s)` and the integer family | **exact, by value** — a client printing the same number at another scale matches (`1.5` = `1.500000`), a cent of difference does not |
| `VARCHAR`, `DATE`, `BOOLEAN`, `TIMESTAMP`, `BLOB`, `INTERVAL` | **exact, by identity** — so `CHAR(n)` padding is a mismatch, and so is `07` against `7` |
| `DOUBLE`, `FLOAT`, `REAL` | 1e-6 absolute or 1e-13 relative, whichever is larger |
| anything unrecognised | **exact, by identity** — the strictest kind, so a new column fails loudly rather than quietly taking a tolerance |

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

Remove the databases afterwards with `python3 tpch_bench.py --all drop`, which drops every `tpch_sf*` database on the server. Each is dropped in its own `TRY`/`CATCH`: a `DROP` that fails — 5030 / 3702, any session connecting in the window — puts *that* database back to `MULTI_USER`, the rest are still dropped, and the batch ends by **throwing** what it could not drop and why. It throws rather than `PRINT`s because `mssql_exec` hands a server's `PRINT` to `duckdb_logs` at INFO, not to the CLI, so printed reasons would never reach you (review of #548). The restore has a `TRY` of its own, because the window this guards is a session taking the database's only connection — and that same session makes `SET MULTI_USER` fail with 5064, which, raised inside the `CATCH`, used to abandon every later database. `WITH ROLLBACK IMMEDIATE` does not rescue it (measured: in single-user mode the `ALTER` cannot get in at all), so when the restore fails the message says the database is **left in `SINGLE_USER`** and names the statement that puts it back. Before issue #507 one failure left its database in `SINGLE_USER`, unusable until someone issued `SET MULTI_USER` by hand, and abandoned the rest while exiting 0.

`test/bench/bench_tpch_e2e.sh` is the older, smaller TPC-H-against-SQL-Server harness in this repository — same environment defaults, a masked banner and a timing loop. Worth reading before this one grows further.

## On a rented machine (Hetzner)

The local server is the docker image under Rosetta on an ARM Mac. Column C is meaningless there, and A against B is only a relative comparison. For published numbers:
- **Machine.** A dedicated x86 box: for SF100, 16+ cores, 128 GB RAM and 1 TB NVMe. SF300 / SF1000 need proportionally more disk: raw data is about 1 GB per SF, plus parquet and SQL Server files.
- **Server.** SQL Server 2022 for Linux, natively (docker `mcr.microsoft.com/mssql/server:2022-latest` is fine on x86), with `max server memory` set and the data files on the NVMe.
- **Clients** on the same box: DuckDB 1.5.6 and 2.0 CLIs with `mssql` from community. A client on another box would add the network to every scan, which is the point of B's pushdown but not of A against B.
- **Generation.** At SF ≥ 100, `gen` holds the data in one DuckDB file: give it the disk and `--data-dir`.
- **Server settings.** Pin and record the version, the `max server memory`, the MAXDOP and the database collation in the report; a CI collation versus a UTF-8 one changes the read path.

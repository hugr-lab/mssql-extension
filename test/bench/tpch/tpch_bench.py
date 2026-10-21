#!/usr/bin/env python3
"""TPC-H with the data in SQL Server, queried from DuckDB.

Compares, query by query:

  A      DuckDB 1.5.6 + mssql 0.2.5 (stock CLI, extension from community)
  B      DuckDB 2.0   + mssql 0.3.0 (remote pushdown on, the default)
  Bscan  DuckDB 2.0   + mssql 0.3.0 with `remote_pushdown false` (the scan path)
  C      SQL Server itself: the same queries in T-SQL, sent through mssql_scan

Subcommands, in the order a fresh machine runs them:

  gen      generate TPC-H data at --sf as parquet (needs a DuckDB with tpch)
  load     create the database on SQL Server and load the parquet into it
  verify   run every query once per client and compare with a DuckDB oracle
  explain  record whether client B pushes each query to the server
  run      the timed, interleaved runs; writes timings.csv
  report   medians per query and client, as markdown

The server credentials come from the environment and are never printed:
MSSQL_BENCH_HOST / _PORT / _USER / _PASSWORD, falling back to the test
suite's MSSQL_TEST_HOST / _PORT / _USER / _PASS. See README.md.
"""
import argparse
import csv
import datetime
import os
import pathlib
import re
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
TABLES = ["region", "nation", "part", "supplier", "partsupp", "customer", "orders", "lineitem"]
QUERIES = [f"q{n:02d}" for n in range(1, 23)]

# TPC-H column types. Numbers follow DuckDB's dbgen (BIGINT keys, DECIMAL(15,2)),
# so a join compares values of one type on both sides; strings follow the
# TPC-H specification (CHAR / VARCHAR with its lengths), as a real schema would.
COLUMNS = {
    "region": "r_regionkey INT NOT NULL, r_name CHAR(25) NOT NULL, r_comment VARCHAR(152) NOT NULL",
    "nation": "n_nationkey INT NOT NULL, n_name CHAR(25) NOT NULL, n_regionkey INT NOT NULL, n_comment VARCHAR(152) NOT NULL",
    "part": "p_partkey BIGINT NOT NULL, p_name VARCHAR(55) NOT NULL, p_mfgr CHAR(25) NOT NULL, p_brand CHAR(10) NOT NULL, "
    "p_type VARCHAR(25) NOT NULL, p_size INT NOT NULL, p_container CHAR(10) NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL, "
    "p_comment VARCHAR(23) NOT NULL",
    "supplier": "s_suppkey BIGINT NOT NULL, s_name CHAR(25) NOT NULL, s_address VARCHAR(40) NOT NULL, s_nationkey INT NOT NULL, "
    "s_phone CHAR(15) NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR(101) NOT NULL",
    "partsupp": "ps_partkey BIGINT NOT NULL, ps_suppkey BIGINT NOT NULL, ps_availqty BIGINT NOT NULL, "
    "ps_supplycost DECIMAL(15,2) NOT NULL, ps_comment VARCHAR(199) NOT NULL",
    "customer": "c_custkey BIGINT NOT NULL, c_name VARCHAR(25) NOT NULL, c_address VARCHAR(40) NOT NULL, c_nationkey INT NOT NULL, "
    "c_phone CHAR(15) NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, c_mktsegment CHAR(10) NOT NULL, c_comment VARCHAR(117) NOT NULL",
    "orders": "o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus CHAR(1) NOT NULL, o_totalprice DECIMAL(15,2) NOT NULL, "
    "o_orderdate DATE NOT NULL, o_orderpriority CHAR(15) NOT NULL, o_clerk CHAR(15) NOT NULL, o_shippriority INT NOT NULL, "
    "o_comment VARCHAR(79) NOT NULL",
    "lineitem": "l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, l_linenumber BIGINT NOT NULL, "
    "l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, "
    "l_tax DECIMAL(15,2) NOT NULL, l_returnflag CHAR(1) NOT NULL, l_linestatus CHAR(1) NOT NULL, l_shipdate DATE NOT NULL, "
    "l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, l_shipinstruct CHAR(25) NOT NULL, l_shipmode CHAR(10) NOT NULL, "
    "l_comment VARCHAR(44) NOT NULL",
}
UTF8_COLLATION = "Latin1_General_100_CI_AS_SC_UTF8"


def columns(table, strings):
    """The table's column list for a string flavour.

    spec      CHAR(n) / VARCHAR(n) as TPC-H specifies, in the database's collation
    nvarchar  every string column NVARCHAR(n), the same lengths: a Unicode schema
    utf8      CHAR(n) becomes VARCHAR(n), in a UTF-8 collation (see UTF8_COLLATION)
    """
    cols = COLUMNS[table]
    if strings == "nvarchar":
        return re.sub(r"(?<![A-Z])(?:VAR)?CHAR\((\d+)\)", r"NVARCHAR(\1)", cols)
    if strings == "utf8":
        return re.sub(r"(?<![A-Z])CHAR\((\d+)\)", r"VARCHAR(\1)", cols)
    return cols


PRIMARY_KEYS = {
    "region": "r_regionkey",
    "nation": "n_nationkey",
    "part": "p_partkey",
    "supplier": "s_suppkey",
    "partsupp": "ps_partkey, ps_suppkey",
    "customer": "c_custkey",
    "orders": "o_orderkey",
    "lineitem": "l_orderkey, l_linenumber",
}

CLIENTS = {
    "A": {"label": "DuckDB 1.5.6 + mssql 0.2.5", "bin": "A", "queries": "duckdb", "attach": ""},
    "B": {"label": "DuckDB 2.0 + mssql 0.3.0 (pushdown)", "bin": "B", "queries": "duckdb", "attach": ""},
    "Bscan": {"label": "DuckDB 2.0 + mssql 0.3.0 (scan path)", "bin": "B", "queries": "duckdb", "attach": ", remote_pushdown false"},
    "C": {"label": "SQL Server (native T-SQL)", "bin": "B", "queries": "tsql", "attach": ""},
}


# --------------------------------------------------------------------------- server

def server():
    env = os.environ
    def pick(*names, default=None):
        for n in names:
            if env.get(n):
                return env[n]
        return default
    cfg = {
        "host": pick("MSSQL_BENCH_HOST", "MSSQL_TEST_HOST", default="localhost"),
        "port": pick("MSSQL_BENCH_PORT", "MSSQL_TEST_PORT", default="1433"),
        "user": pick("MSSQL_BENCH_USER", "MSSQL_TEST_USER", default="sa"),
        "password": pick("MSSQL_BENCH_PASSWORD", "MSSQL_TEST_PASS"),
    }
    if not cfg["password"]:
        sys.exit("error: set MSSQL_BENCH_PASSWORD (or MSSQL_TEST_PASS)")
    return cfg


def dsn(cfg, database, encrypt):
    # Encrypt and TrustServerCertificate agree, so 0.2.5 (where the latter is an
    # alias of the former) and 0.3.0 read the same thing: TLS on or off for both.
    flag = "yes" if encrypt else "no"
    return (f"Server={cfg['host']},{cfg['port']};Database={database};User Id={cfg['user']};"
            f"Password={cfg['password']};Encrypt={flag};TrustServerCertificate={flag}")


def mask(text, cfg):
    return text.replace(cfg["password"], "***") if cfg.get("password") else text


def binary(which):
    if which == "A":
        return os.environ.get("DUCKDB_A", str(pathlib.Path.home() / ".duckdb/cli/latest/duckdb"))
    return os.environ.get("DUCKDB_B", str(REPO / "build/release/duckdb"))


def run_cli(bin_path, script, cfg, database=":memory:", timeout=None):
    """Run a DuckDB CLI with the script on stdin (so the DSN never sits in argv or a file)."""
    # The 2.0 CLI switches to an "agent mode" (other table and error formats) when it
    # sees an AI agent's variables; both clients must print the same way.
    env = {k: v for k, v in os.environ.items()
           if k not in ("AI_AGENT", "CLAUDECODE") and not k.startswith("CLAUDE_CODE")}
    # stderr joins stdout so an error lands next to the marker of its query.
    proc = subprocess.run([bin_path, database], input=script, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, env=env, timeout=timeout)
    return proc.returncode, mask(proc.stdout, cfg)


def esc(s):
    return s.replace("'", "''")


# --------------------------------------------------------------------------- gen

def cmd_gen(args):
    out = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    out.mkdir(parents=True, exist_ok=True)
    dbfile = out / "tpch.duckdb"
    script = ["INSTALL tpch;", "LOAD tpch;", f"CALL dbgen(sf={args.sf});"]
    for t in TABLES:
        script.append(f"COPY {t} TO '{out / (t + '.parquet')}' (FORMAT parquet);")
    rc, text = run_cli(binary("A"), "\n".join(script) + "\n", {"password": ""}, database=str(dbfile))
    print(text)
    if rc != 0:
        sys.exit(rc)
    dbfile.unlink(missing_ok=True)
    print(f"generated SF{args.sf:g} into {out}")


# --------------------------------------------------------------------------- load

def cmd_load(args):
    cfg = server()
    data = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    missing = [t for t in TABLES if not (data / f"{t}.parquet").exists()]
    if missing:
        sys.exit(f"error: no parquet for {missing} under {data} -- run `gen` first")
    db = args.db
    if args.strings == "utf8" and not args.collation:
        args.collation = UTF8_COLLATION
    # No statement timeout: REORGANIZE and FULLSCAN statistics on a large lineitem
    # run for minutes.
    s = ["LOAD mssql;", "SET mssql_query_timeout = 0;", f"ATTACH '{esc(dsn(cfg, 'master', args.encrypt))}' AS m (TYPE mssql);"]
    s.append(
        "SELECT mssql_exec('m', '" + esc(
            f"IF DB_ID(N'{db}') IS NOT NULL BEGIN ALTER DATABASE [{db}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE; "
            f"DROP DATABASE [{db}]; END; CREATE DATABASE [{db}]"
            + (f" COLLATE {args.collation}" if args.collation else "")
            + f"; ALTER DATABASE [{db}] SET RECOVERY SIMPLE; ALTER DATABASE [{db}] SET QUERY_STORE = OFF;") + "');")
    s.append("DETACH m;")
    s.append(f"ATTACH '{esc(dsn(cfg, db, args.encrypt))}' AS t (TYPE mssql, remote_pushdown false);")
    for tb in TABLES:
        ddl = f"CREATE TABLE dbo.{tb} ({columns(tb, args.strings)}"
        if args.layout == "rowstore":
            ddl += f", CONSTRAINT pk_{tb} PRIMARY KEY CLUSTERED ({PRIMARY_KEYS[tb]})"
        ddl += ")"
        if args.layout == "columnstore":
            ddl += f"; CREATE CLUSTERED COLUMNSTORE INDEX cci_{tb} ON dbo.{tb}"
        s.append(f"SELECT mssql_exec('t', '{esc(ddl)}');")
    s.append("SELECT mssql_invalidate_cache('t');")
    s.append(".timer on")
    for tb in TABLES:
        s.append(f".print @@LOAD {tb}@@")
        s.append(f"INSERT INTO t.dbo.{tb} SELECT * FROM read_parquet('{data / (tb + '.parquet')}');")
    s.append(".timer off")
    for tb in TABLES:
        post = f"UPDATE STATISTICS dbo.{tb} WITH FULLSCAN"
        if args.layout == "columnstore":
            post = f"ALTER INDEX ALL ON dbo.{tb} REORGANIZE WITH (COMPRESS_ALL_ROW_GROUPS = ON); " + post
        s.append(f"SELECT mssql_exec('t', '{esc(post)}');")
    for tb in TABLES:
        s.append(f"SELECT '{tb}' AS t, count(*) AS n FROM t.dbo.{tb};")
    rc, text = run_cli(binary("B"), "\n".join(s) + "\n", cfg)
    print(text)
    if rc != 0:
        sys.exit(rc)


# --------------------------------------------------------------------------- sessions

#: q11's FRACTION is the one TPC-H constant that scales with the scale factor:
#: 0.0001/SF. Both query texts carry the SF1 value, inherited from DuckDB
#: upstream, whose shipped answers stop at SF1.
#:
#: Left unscaled, the threshold stays put while the per-part values grow, so
#: from about SF2 upward the HAVING passes NOTHING and q11 measures an empty
#: result set. It was still published as a four-client comparison with a
#: conclusion drawn from it, and `compare()` called it `ok` because 0 == 0
#: (issue #505): at SF10 client C returned 0 rows while A streamed 8,004,050
#: and B 16,008,100 rows to DuckDB to compute nothing.
#:
#: Every other TPC-H constant (dates, quantities, brands) is fixed by the spec
#: and does not scale, so this is the only substitution needed.
Q11_SF1_FRACTION = "0.0001000000"


def read_query(kind, q, sf):
    """The query text from `queries/<kind>/<q>.sql`, scale-factor substituted.

    Fails loudly when a substitution it expects to make has nothing to match:
    a silently-skipped scaling is what put an empty result set into the
    published numbers, and a check that quietly stops checking is worse than
    no check.
    """
    sql = (HERE / "queries" / kind / f"{q}.sql").read_text().strip().rstrip(";").strip()
    if q == "q11":
        if Q11_SF1_FRACTION not in sql:
            sys.exit(f"{kind}/{q}.sql no longer contains the SF1 FRACTION {Q11_SF1_FRACTION}, so the "
                     f"0.0001/SF scaling cannot be applied. Fix the substitution rather than running "
                     f"q11 unscaled -- unscaled it returns no rows above about SF2 (issue #505).")
        # 12 decimals covers the README's planned range: SF1 -> 0.000100000000,
        # SF1000 -> 0.000000100000.
        sql = sql.replace(Q11_SF1_FRACTION, f"{0.0001 / sf:.12f}")
    return sql


def query_text(client, q, sf):
    kind = CLIENTS[client]["queries"]
    sql = read_query(kind, q, sf)
    if kind == "tsql":
        return f"SELECT * FROM mssql_scan('tpch', '{esc(sql)}');"
    return sql + ";"


def session_head(client, cfg, args):
    c = CLIENTS[client]
    # No statement timeout: at a large scale factor a query the server runs whole
    # may think for longer than the 30 s default before its first row.
    return ["LOAD mssql;", "SET mssql_query_timeout = 0;",
            f"ATTACH '{esc(dsn(cfg, args.db, args.encrypt))}' AS tpch (TYPE mssql{c['attach']});",
            "USE tpch.dbo;"]


RUN_TIME = re.compile(r"Run Time \(s\): real ([0-9.]+)")


def parse_marked(text, tag):
    """Map each `@@<tag> <q>@@` marker to (seconds, error) from the lines after it."""
    out = {}
    current, seconds, err = None, None, []
    def close():
        if current is not None:
            # No `Run Time` line means the statement failed: the CLI times only what ran.
            out[current] = (seconds, (" ".join(err)[:300] or "failed (see the session log)") if seconds is None else None)
    for line in text.splitlines():
        m = re.match(r"@@(\S+) (\S+)@@", line.strip())
        if m:
            close()
            current, seconds, err = (m.group(2), None, []) if m.group(1) == tag else (None, None, [])
            continue
        if current is None:
            continue
        r = RUN_TIME.search(line)
        if r and seconds is None:
            seconds = float(r.group(1))
        elif line.strip() and seconds is None:
            err.append(line.strip())
    close()
    return out


def timed_session(client, cfg, args, queries):
    s = session_head(client, cfg, args)
    s.append(".timer on")
    for tag in (["W"] * args.warmup) + ["T"]:
        for q in queries:
            s += [".output", f".print @@{tag} {q}@@", ".output /dev/null", query_text(client, q, args.sf)]
    s += [".output", ".print @@END end@@"]
    rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg, timeout=args.timeout)
    return parse_marked(text, "T"), text


# --------------------------------------------------------------------------- run / report

def selected(args):
    clients = [c.strip() for c in args.clients.split(",") if c.strip()]
    queries = [q.strip() for q in args.queries.split(",")] if args.queries else QUERIES
    return clients, queries


def cmd_run(args):
    cfg = server()
    clients, queries = selected(args)
    run_id = args.run_id or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = pathlib.Path(args.out) / run_id
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for rep in range(args.reps):
        order = clients[rep % len(clients):] + clients[:rep % len(clients)]  # rotate: no client always first
        for client in order:
            res, text = timed_session(client, cfg, args, queries)
            (out / f"session-{rep + 1}-{client}.log").write_text(text)
            for q in queries:
                sec, err = res.get(q, (None, "no marker in output"))
                rows.append({"run": run_id, "db": args.db, "client": client, "rep": rep + 1, "query": q,
                             "seconds": "" if sec is None else f"{sec:.4f}", "error": err or ""})
                print(f"rep {rep + 1} {client:6s} {q} {'ERROR ' + err if err else f'{sec:9.3f} s'}", flush=True)
    with open(out / "timings.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run", "db", "client", "rep", "query", "seconds", "error"])
        w.writeheader()
        w.writerows(rows)
    print(f"timings: {out / 'timings.csv'}")


def cmd_report(args):
    path = pathlib.Path(args.timings)
    rows = list(csv.DictReader(open(path)))
    clients = [c for c in CLIENTS if any(r["client"] == c for r in rows)]
    queries = sorted({r["query"] for r in rows})
    med = {}
    for c in clients:
        for q in queries:
            vals = [float(r["seconds"]) for r in rows if r["client"] == c and r["query"] == q and r["seconds"]]
            errs = [r["error"] for r in rows if r["client"] == c and r["query"] == q and r["error"]]
            med[c, q] = (statistics.median(vals) if vals else None, len(vals), errs[0] if errs else "")
    base = "A" if "A" in clients else clients[0]
    lines = [f"Median wall time per query, seconds (n = timed reps). Ratios are against {base}: below 1 is faster.", ""]
    head = "| query | " + " | ".join(f"{c} ({CLIENTS[c]['label']})" for c in clients) + " | " + \
        " | ".join(f"{c}/{base}" for c in clients if c != base) + " |"
    lines += [head, "|" + "---|" * (1 + 2 * len(clients) - 1)]
    totals = {c: 0.0 for c in clients}
    for q in queries:
        cells, ratios = [], []
        for c in clients:
            v, n, e = med[c, q]
            cells.append(f"{v:.3f}" if v is not None else f"error: {e[:40]}")
            if v is not None:
                totals[c] += v
        b = med[base, q][0]
        for c in clients:
            if c == base:
                continue
            v = med[c, q][0]
            ratios.append(f"{v / b:.2f}" if v is not None and b else "")
        lines.append(f"| {q} | " + " | ".join(cells) + " | " + " | ".join(ratios) + " |")
    tb = totals[base]
    lines.append("| **sum** | " + " | ".join(f"**{totals[c]:.2f}**" for c in clients) + " | " +
                 " | ".join(f"**{totals[c] / tb:.2f}**" if tb else "" for c in clients if c != base) + " |")
    text = "\n".join(lines) + "\n"
    print(text)
    if args.report:
        pathlib.Path(args.report).write_text(text)


# --------------------------------------------------------------------------- verify

def cmd_verify(args):
    cfg = server()
    clients, queries = selected(args)
    data = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    out = pathlib.Path(args.out) / "verify"
    out.mkdir(parents=True, exist_ok=True)
    # The oracle: DuckDB 1.5.6 over the same parquet the server was loaded from.
    s = [f"CREATE VIEW {t} AS SELECT * FROM read_parquet('{data / (t + '.parquet')}');" for t in TABLES]
    for q in queries:
        sql = read_query("duckdb", q, args.sf)
        s.append(f"COPY ({sql}) TO '{out / ('oracle-' + q + '.csv')}' (HEADER false);")
    rc, text = run_cli(binary("A"), "\n".join(s) + "\n", cfg)
    if rc != 0:
        sys.exit(text)
    for client in clients:
        s = session_head(client, cfg, args)
        for q in queries:
            body = query_text(client, q, args.sf).rstrip(";")
            s.append(f"COPY ({body}) TO '{out / (client + '-' + q + '.csv')}' (HEADER false);")
        rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg, timeout=args.timeout)
        if text.strip():
            (out / f"{client}.log").write_text(text)
    bad = 0
    for client in clients:
        for q in queries:
            verdict = compare(out / f"oracle-{q}.csv", out / f"{client}-{q}.csv")
            if verdict != "ok":
                bad += 1
            print(f"{client:6s} {q} {verdict}")
    print(f"{bad} mismatch(es)")
    sys.exit(1 if bad else 0)


def compare(oracle, got):
    if not got.exists():
        return "MISSING (the query failed; see the client log)"
    a = list(csv.reader(open(oracle)))
    b = list(csv.reader(open(got)))
    if len(a) != len(b):
        return f"ROWS {len(b)} != {len(a)}"
    def same(x, y):
        if x.strip() == y.strip():
            return True
        try:
            fx, fy = float(x), float(y)
        except ValueError:
            return False
        return abs(fx - fy) <= max(1e-6 * max(abs(fx), abs(fy)), 0.011)
    def rows_equal(ra, rb):
        return len(ra) == len(rb) and all(same(x, y) for x, y in zip(ra, rb))
    if all(rows_equal(ra, rb) for ra, rb in zip(a, b)):
        return "ok"
    if all(rows_equal(ra, rb) for ra, rb in zip(sorted(a), sorted(b))):
        return "ok (row order of ties differs)"
    for i, (ra, rb) in enumerate(zip(a, b)):
        if not rows_equal(ra, rb):
            return f"DIFF at row {i + 1}: {rb[:6]} vs {ra[:6]}"
    return "DIFF"


# --------------------------------------------------------------------------- explain

def cmd_explain(args):
    cfg = server()
    _, queries = selected(args)
    s = session_head("B", cfg, args) + ["PRAGMA explain_output = 'physical_only';"]
    for q in queries:
        s += [f".print @@X {q}@@", "EXPLAIN " + query_text("B", q, args.sf)]
    rc, text = run_cli(binary("B"), "\n".join(s) + "\n", cfg)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "explain-B.txt").write_text(text)
    parts = re.split(r"@@X (q\d\d)@@", text)
    plans = dict(zip(parts[1::2], parts[2::2]))
    print("query  pushed  (vehicles / catalog scans)  DuckDB operators left above them")
    for q in queries:
        p = plans.get(q, "")
        vehicles = len(re.findall(r"(?i)mssql[_ ]scan[_ ]params|mssql[_ ]scan[_ ]unsafe|MSSQL_SCAN_PARAMS", p))
        scans = len(re.findall(r"(?i)mssql[_ ]catalog[_ ]scan", p))
        # What DuckDB still computes above the vehicles: anything but projections.
        work = sorted(set(re.findall(r"(Hash Group By|Perfect Hash Group By|Ungrouped Aggregate|Hash Join|"
                                     r"Nested Loop Join|Piecewise Merge Join|Cross Product|Order By|Top N|"
                                     r"Filter|Window|Streaming Window|Union|Delim Join|Left Delim Join|"
                                     r"Right Delim Join|Mark Join|Semi Join|Anti Join)", p)))
        if vehicles and not scans and not work:
            verdict = "whole"
        elif vehicles:
            verdict = "partly"
        else:
            verdict = "no"
        print(f"{q}    {verdict:6s}  ({vehicles} / {scans})  {', '.join(work)}")


# --------------------------------------------------------------------------- trace

DMV = """SELECT CAST(qs.execution_count AS bigint) AS execs,
       CAST(qs.total_rows AS bigint) AS rows_out,
       CAST(qs.total_elapsed_time / 1000.0 AS decimal(18,1)) AS elapsed_ms,
       CAST(qs.total_worker_time / 1000.0 AS decimal(18,1)) AS cpu_ms,
       CAST(qs.total_logical_reads AS bigint) AS reads,
       LEFT(REPLACE(REPLACE(SUBSTRING(st.text, qs.statement_start_offset / 2 + 1,
            (CASE qs.statement_end_offset WHEN -1 THEN DATALENGTH(st.text) ELSE qs.statement_end_offset END
             - qs.statement_start_offset) / 2 + 1), CHAR(13), ' '), CHAR(10), ' '), 4000) AS stmt
FROM sys.dm_exec_query_stats qs
CROSS APPLY sys.dm_exec_sql_text(qs.sql_handle) st
CROSS APPLY sys.dm_exec_plan_attributes(qs.plan_handle) pa
WHERE pa.attribute = 'dbid' AND CAST(pa.value AS int) = DB_ID(N'{db}')
  AND st.text NOT LIKE '%plan_persist%' AND st.text NOT LIKE '%@plansToCheck%'
ORDER BY qs.total_elapsed_time DESC"""


def cmd_trace(args):
    """What each client sends to SQL Server for each query: statements, rows returned, server time.

    Per query: run it once (metadata warm), DBCC FREEPROCCACHE, run it again
    timed, then read sys.dm_exec_query_stats for the statements that ran in the
    TPC-H database. elapsed_ms includes the time the server waited for the
    client to read (ASYNC_NETWORK_IO); cpu_ms is the server's own work.
    """
    cfg = server()
    clients, queries = selected(args)
    out = pathlib.Path(args.out) / "trace"
    out.mkdir(parents=True, exist_ok=True)
    dmv = esc(DMV.format(db=args.db))
    summary = []
    for client in clients:
        opts = "" if CLIENTS[client]["bin"] == "A" else ", remote_pushdown false"
        s = session_head(client, cfg, args)
        s.append(f"ATTACH '{esc(dsn(cfg, 'master', args.encrypt))}' AS adm (TYPE mssql{opts});")
        s.append(".mode csv")
        for q in queries:
            qt = query_text(client, q, args.sf)
            s += [".output /dev/null", qt,
                  "SELECT mssql_exec('adm', 'DBCC FREEPROCCACHE WITH NO_INFOMSGS');",
                  ".output", f".print @@Q {q}@@", ".output /dev/null", ".timer on", qt, ".timer off", ".output",
                  f"SELECT * FROM mssql_scan('adm', '{dmv}');"]
        s += [".print @@Q end@@"]
        rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg, timeout=args.timeout)
        (out / f"trace-{client}.log").write_text(text)
        parts = re.split(r"@@Q (\S+)@@", text)
        md = [f"# {client}: {CLIENTS[client]['label']} -- {args.db}", ""]
        for q, body in zip(parts[1::2], parts[2::2]):
            if q == "end":
                continue
            r = RUN_TIME.search(body)
            wall = float(r.group(1)) if r else None
            rows = [row for row in csv.reader(body.splitlines()) if len(row) == 6 and row[0].isdigit()]
            n_rows = sum(int(x[1]) for x in rows)
            el = sum(float(x[2]) for x in rows)
            cpu = sum(float(x[3]) for x in rows)
            summary.append((client, q, wall, len(rows), n_rows, el, cpu))
            md.append(f"## {q}: client {wall if wall is not None else 'ERROR'} s, {len(rows)} statement(s), "
                      f"{n_rows} rows to the client, server {el:.0f} ms elapsed / {cpu:.0f} ms CPU")
            md.append("")
            for x in rows:
                md.append(f"- {x[0]} exec, {x[1]} rows, {x[2]} ms elapsed, {x[3]} ms CPU, {x[4]} reads: `{x[5][:600]}`")
            md.append("")
        (out / f"trace-{client}.md").write_text("\n".join(md) + "\n")
    print("client query  wall_s  stmts  rows_to_client  server_elapsed_ms  server_cpu_ms")
    for c, q, wall, n, rows, el, cpu in summary:
        print(f"{c:6s} {q}  {wall if wall is not None else 'ERR':>7}  {n:5d}  {rows:14d}  {el:17.0f}  {cpu:13.0f}")
    print(f"details: {out}/trace-<client>.md")


# --------------------------------------------------------------------------- drop

def cmd_drop(args):
    """Drop the benchmark database: --db, or with --all every tpch_sf* database on the server."""
    cfg = server()
    s = ["LOAD mssql;", "SET mssql_query_timeout = 0;",
         f"ATTACH '{esc(dsn(cfg, 'master', args.encrypt))}' AS m (TYPE mssql);"]
    if args.all:
        target = "SELECT name FROM sys.databases WHERE name LIKE N'tpch[_]sf%'"
    else:
        target = f"SELECT N'{args.db}' AS name WHERE DB_ID(N'{args.db}') IS NOT NULL"
    batch = ("DECLARE @n sysname, @sql nvarchar(max); "
             f"DECLARE c CURSOR LOCAL FAST_FORWARD FOR {target}; OPEN c; FETCH NEXT FROM c INTO @n; "
             "WHILE @@FETCH_STATUS = 0 BEGIN "
             "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET SINGLE_USER WITH ROLLBACK IMMEDIATE; "
             "DROP DATABASE ' + QUOTENAME(@n); EXEC (@sql); FETCH NEXT FROM c INTO @n; END; CLOSE c; DEALLOCATE c;")
    left = "SELECT name FROM sys.databases WHERE name LIKE N'tpch[_]sf%'"
    s.append(f"SELECT mssql_exec('m', '{esc(batch)}');")
    s.append(f"SELECT count(*) AS tpch_databases_left FROM mssql_scan('m', '{esc(left)}');")
    rc, text = run_cli(binary("B"), "\n".join(s) + "\n", cfg)
    print(text)
    sys.exit(rc)


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--data-dir", default=str(HERE / "data"))
    ap.add_argument("--db", default=None, help="SQL Server database (default tpch_sf<sf>_<layout>)")
    ap.add_argument("--layout", choices=["columnstore", "rowstore"], default="columnstore")
    ap.add_argument("--strings", choices=["spec", "nvarchar", "utf8"], default="spec",
                    help="string column types: TPC-H's CHAR/VARCHAR, NVARCHAR(n), or VARCHAR(n) in a UTF-8 collation")
    ap.add_argument("--encrypt", action=argparse.BooleanOptionalAction, default=True)
    ap.add_argument("--collation", default=None, help="CREATE DATABASE ... COLLATE (default: the server's)")
    ap.add_argument("--clients", default="A,B,Bscan,C")
    ap.add_argument("--queries", default=None, help="comma list, e.g. q01,q06 (default: all 22)")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--warmup", type=int, default=1, help="untimed passes per session before the timed one")
    ap.add_argument("--timeout", type=int, default=7200, help="per session, seconds")
    ap.add_argument("--out", default=str(HERE / "results"))
    ap.add_argument("--run-id", default=None)
    ap.add_argument("--timings", default=None, help="report: the timings.csv to summarise")
    ap.add_argument("--report", default=None, help="report: also write the markdown here")
    ap.add_argument("--all", action="store_true", help="drop: every tpch_sf* database, not only --db")
    ap.add_argument("command", choices=["gen", "load", "verify", "explain", "trace", "run", "report", "drop"])
    args = ap.parse_args()
    if args.db is None:
        args.db = (f"tpch_sf{args.sf:g}_{args.layout}" + ("" if args.strings == "spec" else f"_{args.strings}")).replace(".", "_")
    {"gen": cmd_gen, "load": cmd_load, "verify": cmd_verify, "explain": cmd_explain, "trace": cmd_trace,
     "run": cmd_run, "report": cmd_report, "drop": cmd_drop}[args.command](args)


if __name__ == "__main__":
    main()

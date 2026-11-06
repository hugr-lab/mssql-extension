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
import decimal
import os
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile

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
    env = dict(os.environ)
    # Agent mode (issue #505 item 4). The 2.0 CLI switches to other table and
    # error formats, and does an extra ExtractPlan before every SELECT, when it
    # sees any of eleven markers -- AI_AGENT, AGENT, CLAUDECODE, CODEX_*,
    # CURSOR_AGENT, GEMINI_CLI, COPILOT_*. That extra bind+optimize pass lands
    # INSIDE the CLI's own BEGIN_TIMER/END_TIMER region, and for this extension
    # it is a second remote-pushdown dry run, so it inflates client B only --
    # 1.5.6 has no agent mode at all. One-sided inflation on exactly the
    # comparison this harness exists to make.
    #
    # The first version stripped two of those eleven and `CLAUDE_CODE*`, which
    # matches none of them, and missed DUCKDB_AGENT_MODE=1, which forces the
    # mode on regardless. Setting the variable to 0 is complete and
    # version-portable instead of chasing the list: measured, it overrides even
    # an unstripped marker (CURSOR_AGENT), and 1.5.6 ignores a variable it does
    # not know.
    env["DUCKDB_AGENT_MODE"] = "0"
    # The extension's own debug logging, dropped from the CHILD environment
    # only (review of #518 item 5). CLAUDE.md tells an operator to export
    # MSSQL_COUNTERS=1 whenever a number is going to be quoted, and these two
    # often travel with it -- but their logging runs INSIDE the phases being
    # timed (level 2 took parse from 22 to 1133 ns/row and inverted the
    # numbers), so a session carrying them measures something else. They also
    # write to stderr, which joins stdout here, so under parse_marked's
    # structural rule every query in the session would come back an error
    # instead of a timing -- and only after the whole session had run.
    # MSSQL_COUNTERS itself is deliberately LEFT ALONE: it is the one that does
    # not distort, so the operator keeps it, and parse_marked ignores its lines.
    for var in ("MSSQL_DEBUG", "MSSQL_DML_DEBUG"):
        env.pop(var, None)
    # -no-init (issue #505 item 3): without it the operator's ~/.duckdbrc runs
    # inside every measured session. A `SET mssql_remote_pushdown=false` there
    # would make client B measure as Bscan while the report still labels it
    # "(pushdown)", and session_head never re-asserts it; threads, memory_limit
    # and .mode likewise, none of them recorded. Measured: threads came out 3
    # with an rc file present and 18 with the flag. The flag exists in 1.5.6
    # (shell_command_line_option.cpp: {"no-init", 0, "", SkipInit, ...}), so it
    # is safe for client A.
    argv = [bin_path, "-no-init", database]
    # stderr joins stdout so an error lands next to the marker of its query.
    proc = subprocess.run(argv, input=script, text=True, stdout=subprocess.PIPE,
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
MARKER = re.compile(r"@@(\S+) (\S+)@@")
#: Named, because cmd_run has to recognise this verdict to replace it on the
#: one query a session timeout interrupted.
NO_RUN_TIME = "no `Run Time` line (see the session log)"
#: MSSQL_COUNTERS=1 is the documented way to get numbers out of this extension
#: without the logging that distorts them, so an operator may well have it set.
#: Its lines are expected output, not a failed statement.
COUNTERS = "[MSSQL COUNTERS]"


def parse_marked(text, tag):
    """Map each `@@<tag> <q>@@` marker to (seconds, error) from the lines after it.

    A query's result rows go to /dev/null (timed_session redirects before every
    statement), so between one marker and the next the ONLY expected line is the
    CLI's `Run Time`. Anything else means the statement did not run cleanly.

    "No `Run Time` means it failed" is NOT true, which is what the first version
    keyed on (issue #505 item 2). The CLI prints a failure's text and THEN a
    `Run Time` line for it, because END_TIMER fires before the error check.
    Measured:

        @@T q01@@
        Run Time (s): real 0.001 user 0.000230 sys 0.001059
        @@T q02@@
        Catalog Error: Table with name no_such_table does not exist!
        Did you mean "pg_tables"?

        LINE 1: SELECT * FROM no_such_table;
                              ^^^^^^^^^^^^^
        Run Time (s): real 0.004 user 0.003579 sys 0.000725

    So the old code collected those error lines and then threw them away because
    `seconds` had been set -- publishing a Catalog Error as a 4 ms timing, and
    making the whole error branch dead code. A session whose LOAD or ATTACH
    failed produced 22 sub-millisecond "timings".

    Judging on "was there unexpected output" rather than on a list of error
    prefixes is deliberate. A prefix list cannot be complete, and the first
    thing it misses is published as a fast success. This way, output that is
    unexpected but harmless is reported as an error instead -- the direction a
    measurement tool should fail in.
    """
    out = {}
    current, seconds, extra = None, None, []

    def close():
        if current is None:
            return
        if extra:
            out[current] = (None, " ".join(extra)[:300])
        elif seconds is None:
            out[current] = (None, NO_RUN_TIME)
        else:
            out[current] = (seconds, None)

    for line in text.splitlines():
        m = MARKER.match(line.strip())
        if m:
            close()
            current, seconds, extra = (m.group(2), None, []) if m.group(1) == tag else (None, None, [])
            continue
        if current is None:
            continue
        if line.lstrip().startswith(COUNTERS):
            continue
        if RUN_TIME.search(line):
            if seconds is None:
                seconds = float(RUN_TIME.search(line).group(1))
            continue
        if line.strip():
            extra.append(line.strip())
    close()
    return out


def decode_output(raw):
    """`subprocess.TimeoutExpired.output` as text.

    It is BYTES even when the call passed text=True -- documented, and the
    self-test proves it against a real timeout rather than against a value we
    constructed. Calling mask() straight on it raised TypeError and aborted the
    whole run (review of #518 item 1).
    """
    if raw is None:
        return ""
    return raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw


def name_hung_query(res, note):
    """Attribute a session timeout to the query it interrupted. -> that query.

    The last marker with no `Run Time` line is the one still running when the
    timeout fired. `setdefault` cannot reach it: parse_marked has already filed
    it as NO_RUN_TIME, which is true and tells the operator nothing.
    """
    hung = next((q for q in reversed(list(res)) if res[q] == (None, NO_RUN_TIME)), None)
    if hung is not None:
        res[hung] = (None, f"{note} (this query was still running)")
    return hung


def session_preamble(text):
    """The output BEFORE the first marker: the session's own failure.

    Review of #518 item 4. With a database that does not exist the session log
    opens with the cause --

        Invalid Input Error: ... error 4063 ... Cannot open database "tpch_no_such_db"

    -- and every query after it reports only the consequence, `Catalog Error:
    Table with name lineitem does not exist!`, 22 times. The cause lives in
    lines that no marker owns, so parse_marked drops them by construction.
    """
    lines = []
    for line in text.splitlines():
        if MARKER.match(line.strip()):
            break
        if line.strip() and not line.lstrip().startswith(COUNTERS):
            lines.append(line.strip())
    return " ".join(lines)[:300]


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
    # Written and flushed per SESSION rather than once after both loops (issue
    # #505 item 7). At SF10 a rep is ~23 minutes, so --reps 3 across four
    # clients is ~70 minutes and 264 rows; a stall in the last session used to
    # mean a two-hour wait and then NO file at all, because the rows lived in a
    # local list and subprocess.TimeoutExpired was uncaught -- the only `except`
    # in this file was a ValueError in same(). The harness also sets
    # mssql_query_timeout = 0, so --timeout per session is the only ceiling.
    path = out / "timings.csv"
    errored = 0
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run", "db", "client", "rep", "query", "seconds", "error"])
        w.writeheader()
        f.flush()
        for rep in range(args.reps):
            order = clients[rep % len(clients):] + clients[:rep % len(clients)]  # rotate: no client always first
            for client in order:
                try:
                    res, text = timed_session(client, cfg, args, queries)
                except subprocess.TimeoutExpired as exc:
                    # The exception still carries the stdout of every query that
                    # DID finish, Run Time lines included, so parse it rather
                    # than discarding the session. mask() it: this path never
                    # went through run_cli's masking, and an ATTACH failure can
                    # quote the connection string back.
                    #
                    # `exc.output` is BYTES even though run_cli passes
                    # text=True -- documented, and it reproduces: `bytes
                    # b'partial\n'`. Calling mask() on it raised TypeError
                    # inside str.replace, so the FIRST hung session aborted the
                    # whole run and none of the per-session flushing below
                    # happened (review of #518 item 1). Decode defensively
                    # rather than asserting the type: a future Python that
                    # honours text=True here would hand back str.
                    text = mask(decode_output(exc.output), cfg)
                    res = parse_marked(text, "T")
                    note = f"session timed out after {args.timeout}s"
                    hung = name_hung_query(res, note)
                    for q in queries:
                        res.setdefault(q, (None, note))
                    print(f"rep {rep + 1} {client:6s} -- SESSION TIMEOUT after {args.timeout}s"
                          + (f", {hung} was still running" if hung else "")
                          + f", keeping {sum(1 for v in res.values() if v[0] is not None)} completed query(s)",
                          flush=True)
                (out / f"session-{rep + 1}-{client}.log").write_text(text)
                cause = session_preamble(text)
                if cause:
                    print(f"rep {rep + 1} {client:6s} -- SESSION ERROR (before the first query): {cause}",
                          flush=True)
                for q in queries:
                    sec, err = res.get(q, (None, "no marker in output"))
                    w.writerow({"run": run_id, "db": args.db, "client": client, "rep": rep + 1, "query": q,
                                "seconds": "" if sec is None else f"{sec:.4f}", "error": err or ""})
                    errored += 1 if err else 0
                    print(f"rep {rep + 1} {client:6s} {q} {'ERROR ' + err if err else f'{sec:9.3f} s'}", flush=True)
                f.flush()
    print(f"timings: {path}")
    # A run in which every query errored used to exit 0, so a scripted
    # `run && report` published a table of errors as a result (review of #518
    # item 4). The CSV is still written and still worth reading -- the exit
    # status is the only thing that changes.
    if errored:
        print(f"FAIL: {errored} query run(s) errored; the timings for them are absent, not fast.")
        return 1
    return 0


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
    # The sum must cover the SAME work for every client (issue #505 item 5).
    # Accumulating whatever each client happened to complete published a client
    # that errored on an expensive query as several times faster, with nothing
    # in the bolded row marking it partial -- and that row is the one
    # RESULTS-local.md quotes as its headline.
    common = [q for q in queries if all(med[c, q][0] is not None for c in clients)]
    missing = [q for q in queries if q not in common]
    totals = {c: sum(med[c, q][0] for q in common) for c in clients}
    # The rep count was stored in every cell and never read, though the header
    # promised "n = timed reps" -- so a median over one surviving rep looked
    # identical to one over three. Annotate any cell that is short.
    reps_max = max((n for _, n, _ in med.values()), default=0)

    lines = [f"Median wall time per query, seconds. Ratios are against {base}: below 1 is faster."]
    if reps_max > 1:
        lines.append(f"A cell shows `(n=N)` when fewer than {reps_max} timed reps survived for it.")
    lines.append("")
    ratio_cols = [c for c in clients if c != base]
    cols = ["query"] + [f"{c} ({CLIENTS[c]['label']})" for c in clients] + [f"{c}/{base}" for c in ratio_cols]
    # Built from one list so the header and the separator cannot disagree: with
    # a single --clients the old form emitted three header cells against two
    # separators and rendered as a broken table.
    lines += ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for q in queries:
        cells = []
        for c in clients:
            v, n, e = med[c, q]
            if v is None:
                cells.append(f"error: {e[:40]}")
            else:
                cells.append(f"{v:.3f}" + (f" (n={n})" if n < reps_max else ""))
        b = med[base, q][0]
        for c in ratio_cols:
            v = med[c, q][0]
            cells.append(f"{v / b:.2f}" if v is not None and b else "")
        lines.append(f"| {q} | " + " | ".join(cells) + " |")
    tb = totals[base]
    # With no query completed in every client the totals are all 0, and a bold
    # **0.00** in the row RESULTS-local.md quotes reads like a measured result
    # (review of #518 item 7). An em dash cannot be mistaken for one.
    sum_cells = ([f"**{totals[c]:.2f}**" for c in clients] if common else ["--" for _ in clients]) + \
                [f"**{totals[c] / tb:.2f}**" if common and tb else "--" for c in ratio_cols]
    lines.append(f"| **sum** ({len(common)}/{len(queries)} queries) | " + " | ".join(sum_cells) + " |")
    if missing:
        noun = "query" if len(common) == 1 else "queries"
        lines += ["", f"The sum covers only the {len(common)} {noun} every client completed, so the totals and "
                      f"ratios compare like with like. Excluded because at least one client has no median: "
                      f"{', '.join(missing)}."]
    text = "\n".join(lines) + "\n"
    print(text)
    if args.report:
        pathlib.Path(args.report).write_text(text)


# --------------------------------------------------------------------------- verify

def cmd_verify(args):
    cfg = server()
    clients, queries = selected(args)
    data = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    # Per DATABASE, not one shared directory (issue #506 item 3). The old path
    # had no --sf, --db or --strings component and nothing ever removed a CSV
    # from it, so a four-schema sweep compared against whatever the previous
    # schema left behind.
    out = pathlib.Path(args.out) / "verify" / args.db
    out.mkdir(parents=True, exist_ok=True)
    # And clear this run's files anyway, so a query that fails at BIND -- where
    # the COPY never opens its output file at all -- cannot be compared against
    # the previous run's result and reported `ok`. Bind-time failure is client
    # C's own failure mode, since its mssql_scan bind calls
    # sp_describe_first_result_set.
    for q in queries:
        (out / f"oracle-{q}.csv").unlink(missing_ok=True)
        for client in clients:
            (out / f"{client}-{q}.csv").unlink(missing_ok=True)
    # A non-zero session exit, reported ONCE for the client (review of #518
    # item 3). It used to replace all 22 of that client's verdicts: both CLIs
    # exit 1 if any statement failed, so one bind failure hid the other 21
    # comparisons, never named the query that failed, and still printed a count
    # of 22. The per-query comparisons run regardless -- the file clearing above
    # turns the query that actually failed into MISSING, which names it.
    sessions = {}
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
        # The client session's return code was bound and never read, while the
        # oracle's was checked (issue #506 item 3). The CLI does not bail on
        # error for piped stdin, so this is the only signal that the session as
        # a whole went wrong, and dropping it is how a failure became an `ok`.
        if rc != 0:
            sessions[client] = f"client session exited {rc} (see {out / (client + '.log')})"
    bad = 0
    for client in clients:
        if client in sessions:
            print(f"{client:6s} --- {sessions[client]}")
        for q in queries:
            # `verdict != "ok"` rejected compare()'s OWN accepting verdict for
            # a reordered result, so a tie emitted in the other order failed the
            # run while every per-query line still read `ok` -- teaching the
            # operator to disregard the exit code, which is the channel a real
            # wrong answer travels (issue #506 item 4).
            verdict = compare(out / f"oracle-{q}.csv", out / f"{client}-{q}.csv")
            if not verdict.startswith("ok"):
                bad += 1
            print(f"{client:6s} {q} {verdict}")
    print(f"{bad} mismatch(es)" + (f", {len(sessions)} failed session(s)" if sessions else ""))
    sys.exit(1 if bad or sessions else 0)


#: A plain decimal number as both engines print one: optional sign, digits,
#: optionally a fractional part. Deliberately NOT int()/float(): `int()` accepts
#: underscores and surrounding blanks, so `as_int` made `" 13"` and `"1_000"`
#: equal to `"13"` and `"1000"` (review of #518 nit 7); float() would also take
#: `1e9`, `inf` and `nan`, which no exact column prints.
DECIMAL = re.compile(r"-?\d+(?:\.\d+)?")
#: The same discipline for the float fallback, which exists for the forms a
#: DOUBLE can print that an exact decimal cannot (`1e3`). Without it the
#: fallback quietly re-admitted everything DECIMAL had just rejected: float()
#: strips blanks and accepts digit separators, so `" 13"` and `"1_000"` were
#: still equal to `"13"` and `"1000"` after the regex went in. The self-test
#: caught it.
FLOATING = re.compile(r"-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?")


def as_float(s):
    """The value as a float when it is written as a number, else None."""
    return float(s) if FLOATING.fullmatch(s) else None


#: The widest decimal scale this harness sees: SQL Server returns TPC-H's
#: aggregates as decimal(38,6).
MAX_DECIMAL_SCALE = 6
#: A DOUBLE's shortest round-trip form needs up to 17 significant digits. An
#: exact decimal of these magnitudes needs far fewer, so a value carrying this
#: many digits is a double however few decimal places it happens to print --
#: which is what rules out the large-magnitude case (a double near 1e12 prints
#: only four decimals).
MAX_EXACT_DIGITS = 15


def exact_pair(dx, dy):
    """Are these two both printed as an EXACT decimal of ONE scale?

    An equal scale ALONE is not evidence of that, which is the defect roborev
    found in the first cut of this rule. A DOUBLE in shortest round-trip form
    parses as a decimal too, and DuckDB returns `avg(decimal)` and
    `decimal / decimal` as DOUBLE -- q01's avg_qty, avg_price and avg_disc and
    q08's mkt_share. Meanwhile CLAUDE.md records that the extension's pushed
    `CAST(SUM(x) AS float) / COUNT_BIG(x)` differs from DuckDB's avg in its
    last bits BY DESIGN. So two doubles one ulp apart whose shortest forms
    happen to print at the same scale were being required to be EQUAL.

    Measured over 4000 ulp-apart pairs per magnitude, before this guard:

        q01 avg_qty ~25        1148 of 4000 print at the same scale -- all 1148 rejected
        q01 avg_price ~38000   1446 -- all rejected
        q01 avg_disc ~0.05     1233 -- all rejected
        q08 mkt_share ~0.034   1266 -- all rejected
        q06 revenue ~1.2e8     2820 -- all rejected

    Every one of those would have been published as a wrong answer for client
    B on exactly the queries this harness exists to judge.

    Hence two further conditions: the scale must be one a decimal column could
    actually have, and neither value may carry a double's full precision.
    """
    exponent = dx.as_tuple().exponent
    if exponent != dy.as_tuple().exponent:
        return False
    if -exponent > MAX_DECIMAL_SCALE:
        return False
    return max(len(dx.as_tuple().digits), len(dy.as_tuple().digits)) <= MAX_EXACT_DIGITS


def as_decimal(s):
    """The value as a Decimal when it is written as a plain decimal, else None.

    The SCALE is the point: `decimal.Decimal` keeps the digits as printed, so
    `"37734107.00"` and `"37734107.0"` are distinguishable, which is how same()
    tells an exact-vs-exact comparison from an exact-vs-DOUBLE one.
    """
    return decimal.Decimal(s) if DECIMAL.fullmatch(s) else None


def same(x, y):
    """Is the client's value `y` the oracle's `x`?

    Issue #506 items 1 and 2. The first version was
    `x.strip() == y.strip()` then
    `abs(fx-fy) <= max(1e-6 * max(|fx|,|fy|), 0.011)`, which failed twice over:

    * The 0.011 ABSOLUTE floor was wider than whole answers. q08's entire
      result is two rows, mkt_share 0.03443589 and 0.04148552 -- 0.00705
      apart, inside the floor -- so swapping the two years printed `ok`.
      q01's avg_disc groups are ~1e-4 apart and so were not checked at all.
    * The 1e-6 RELATIVE term tolerated off-by-N on counts: q01's count_order
      is 1,478,493 at SF1, so +-1.48 rows, +-14.8 at SF10, +-148 at SF100 --
      a row-dropping pushdown bug on exactly the aggregates TPC-H is made of
      passed the gate.
    * `.strip()` made CHAR(n) trailing-blank padding invisible, which is the
      very defect (#405) this harness's headline correctness result is about.
      It was caught then only because the padding also broke comparisons and
      changed row counts; a regression that padded only the OUTPUT -- q16
      groups by p_brand CHAR(10), q12 by l_shipmode CHAR(10) -- would have
      compared equal on every row.

    So: integers must match EXACTLY, strings must match EXACTLY, and only a
    real-valued pair gets a tolerance, sized to what the two engines can
    legitimately disagree by rather than to a round number. The server returns
    these as decimal(38,6) against the oracle's DOUBLE, a half-digit of 5e-7,
    and a large sum carries double's own relative error; 1e-6 absolute or 1e-9
    relative covers both and still leaves q08's 0.00705 spread four orders of
    magnitude outside it.

    Narrowing "integer" to "exact decimal" (review of #518 item 2): keying the
    exact branch on int() let a value with a fractional part fall through to
    the 1e-9 RELATIVE term even when both engines had printed the same exact
    decimal. `"37734107.00"` is not an int, and at SF100-1000 magnitudes 1e-9
    relative is tens to hundreds of units -- `123456789012.34` compared `ok`
    against `123456789132.34`, which is item 1 of #506 (a dropped row in an
    aggregate passes) returning at exactly the scale the fix was for.

    What the pair LOOKS like decides which branch applies, because it is the
    only evidence of what the columns are -- see `exact_pair`, which wants an
    equal scale, a scale a decimal column could have, and digit counts below a
    double's full precision. Anything else gets the tolerance, including two
    doubles whose shortest forms happen to print alike.
    """
    if x == y:
        return True
    dx, dy = as_decimal(x), as_decimal(y)
    if dx is not None and dy is not None:
        if exact_pair(dx, dy):
            return dx == dy
        bound = max(decimal.Decimal("1e-6"), decimal.Decimal("1e-9") * max(abs(dx), abs(dy)))
        return abs(dx - dy) <= bound
    fx, fy = as_float(x), as_float(y)
    if fx is None or fy is None:
        return False
    return abs(fx - fy) <= max(1e-6, 1e-9 * max(abs(fx), abs(fy)))


def rows_equal(ra, rb):
    return len(ra) == len(rb) and all(same(x, y) for x, y in zip(ra, rb))


def compare(oracle, got):
    if not got.exists():
        return "MISSING (the query failed; see the client log)"
    a = list(csv.reader(open(oracle)))
    b = list(csv.reader(open(got)))
    if len(a) != len(b):
        return f"ROWS {len(b)} != {len(a)}"
    if all(rows_equal(ra, rb) for ra, rb in zip(a, b)):
        return "ok"
    # Same rows, different order. The old wording was "row order of ties
    # differs", which it has no evidence for: an inverted ORDER BY with no ties
    # at all reaches here too. Say what is actually established.
    if all(rows_equal(ra, rb) for ra, rb in zip(sorted(a), sorted(b))):
        return "ok (same rows, different order)"
    # Name trailing-blank padding specifically, since it is the #405 class and
    # an exact comparison is the only thing that can see it.
    if all(len(ra) == len(rb) and all(x.strip() == y.strip() for x, y in zip(ra, rb))
           for ra, rb in zip(a, b)):
        return "PADDING (values differ only in surrounding blanks -- the #405 class)"
    for i, (ra, rb) in enumerate(zip(a, b)):
        if not rows_equal(ra, rb):
            return f"DIFF at row {i + 1}: {rb[:6]} vs {ra[:6]}"
    return "DIFF"


# --------------------------------------------------------------------------- selftest

# Every fixture below is a case this harness got WRONG, each named with the
# review item that found it. Nothing here needs a server, a database or a
# DuckDB binary, so it runs anywhere and belongs in CI: the defects these guard
# were all of one kind -- the harness reporting a verdict it had not earned --
# and that kind is invisible until something checks the judge.

#: (label, oracle value, client value, must same() accept it)
SAME_TESTS = [
    # Review of #518 item 2: the exact branch used to key on int(), so a value
    # with a fractional part fell through to 1e-9 RELATIVE. VGSML's own case.
    ("two exact decimals of one scale, 120 apart", "123456789012.34", "123456789132.34", False),
    ("the same exact decimal", "37734107.00", "37734107.00", True),
    ("an exact decimal one unit out at SF1000 magnitude", "123456789012.34", "123456789013.34", False),
    ("an exact decimal one hundredth out", "37734107.00", "37734107.01", False),
    # Different scales ARE the DOUBLE-vs-decimal(38,6) case the tolerance exists
    # for, and must still pass.
    ("DOUBLE beside decimal(38,6), same value", "0.05", "0.050000", True),
    ("DOUBLE beside decimal(38,6), within a half digit", "1.0000001", "1.000000", True),
    ("DOUBLE beside decimal(38,6), plainly different", "0.05", "0.060000", False),
    # Issue #506 item 2: 1e-6 relative tolerated off-by-N on counts.
    ("a count one row short", "1478493", "1478492", False),
    ("the same count", "1478493", "1478493", True),
    # Issue #506 item 1: the 0.011 ABSOLUTE floor was wider than q08's answer.
    ("q08's two mkt_share values", "0.03443589", "0.04148552", False),
    # Issue #405: .strip() hid CHAR(n) padding, the defect this harness is about.
    ("a CHAR(10) value padded", "MACHINERY", "MACHINERY ", False),
    ("a CHAR(10) value padded on the left", "MACHINERY", " MACHINERY", False),
    ("the same string", "MACHINERY", "MACHINERY", True),
    # Review of #518 nit 7: int() accepts both of these.
    ("a count with a leading blank", "13", " 13", False),
    ("a count written with an underscore", "1000", "1_000", False),
    # DOUBLE against DOUBLE. The gap that let the first cut of the exact rule
    # through: these parse as decimals and print at the SAME scale, so nothing
    # in the fixture set distinguished them from two decimal(38,6) values, and
    # every ulp-apart pair became a reported wrong answer. DuckDB returns
    # avg(decimal) and decimal/decimal as DOUBLE, so these ARE q01's and q08's
    # columns -- the queries the harness exists to judge.
    ("two DOUBLEs one ulp apart, same printed scale (q01 avg_qty)",
     "25.522005853257337", "25.522005853257333", True),
    ("two DOUBLEs one ulp apart (q08 mkt_share)",
     "0.034435890263343130", "0.034435890263343136", True),
    ("two DOUBLEs one ulp apart at a large magnitude (q06 revenue)",
     "123141078.2283", "123141078.22830002", True),
    # ... and the guard must not swallow a real difference at double precision.
    ("two DOUBLEs a whole percent apart", "25.522005853257337", "25.777005853257337", False),
    # Seven decimals is past decimal(38,6), so the pair cannot be two exact
    # decimals of one declared scale -- tolerance, not equality.
    ("a scale wider than decimal(38,6) takes the tolerance",
     "1.00000005", "1.00000004", True),
    # Not exact decimals: these fall to the float path rather than crashing.
    ("scientific notation, equal", "1e3", "1000.0", True),
    ("a NULL against an empty string", "", "", True),
    ("text against a number", "MACHINERY", "17", False),
]

#: (label, oracle rows, client rows, expected verdict prefix)
COMPARE_TESTS = [
    ("identical", [["a", "1"]], [["a", "1"]], "ok"),
    ("reordered rows", [["a", "1"], ["b", "2"]], [["b", "2"], ["a", "1"]], "ok (same rows"),
    ("a dropped row", [["a", "1"], ["b", "2"]], [["a", "1"]], "ROWS"),
    ("trailing-blank padding", [["MACHINERY", "1"]], [["MACHINERY ", "1"]], "PADDING"),
    ("a wrong value", [["a", "1"]], [["a", "2"]], "DIFF"),
]

#: (label, session text, expected per-query result)
PARSE_TESTS = [
    # Issue #505 item 2: the CLI prints the error AND a Run Time for it, because
    # END_TIMER fires before the error check. The old code kept the seconds.
    ("an error followed by its own Run Time is an error, not a 4 ms timing",
     "@@T q01@@\nCatalog Error: Table with name no_such_table does not exist!\n"
     "Run Time (s): real 0.004 user 0.003 sys 0.000\n",
     {"q01": "error"}),
    ("a clean query is its seconds",
     "@@T q01@@\nRun Time (s): real 1.250 user 0.1 sys 0.1\n", {"q01": 1.25}),
    # Review of #518 item 5: MSSQL_COUNTERS=1 is the documented way to get
    # numbers out of this extension, so its lines are expected output.
    ("a [MSSQL COUNTERS] line does not make a clean query an error",
     "@@T q01@@\n[MSSQL COUNTERS] remote_pushdown: statements=3\n"
     "Run Time (s): real 1.250 user 0.1 sys 0.1\n", {"q01": 1.25}),
    ("an interrupted query has no Run Time",
     "@@T q01@@\nRun Time (s): real 0.5 user 0 sys 0\n@@T q02@@\n",
     {"q01": 0.5, "q02": "error"}),
]


def _check(label, ok, detail, width=62):
    print(f"  [{'ok' if ok else 'FAIL'}] {label:{width}s} -> {detail}")
    return 0 if ok else 1


def _check_same():
    print("same() -- what counts as the oracle's answer:")
    failures = 0
    for label, x, y, want in SAME_TESTS:
        got = same(x, y)
        failures += _check(label, got == want, "accepted" if got else "rejected")
    return failures


def _check_compare(tmp):
    print("\ncompare() -- the verdict for a whole result:")
    failures = 0
    for i, (label, a, b, want) in enumerate(COMPARE_TESTS):
        oracle, got_path = tmp / f"o{i}.csv", tmp / f"g{i}.csv"
        for path, rows in ((oracle, a), (got_path, b)):
            with open(path, "w", newline="") as f:
                csv.writer(f).writerows(rows)
        verdict = compare(oracle, got_path)
        failures += _check(label, verdict.startswith(want), verdict[:48])
    # A client whose query failed at BIND writes no file at all.
    failures += _check("a missing client file is MISSING, never ok",
                       compare(tmp / "o0.csv", tmp / "absent.csv").startswith("MISSING"), "MISSING")
    return failures


def _check_parse():
    print("\nparse_marked() -- a timing or an error, per query:")
    failures = 0
    for label, text, want in PARSE_TESTS:
        res = parse_marked(text, "T")
        ok = True
        for q, expect in want.items():
            sec, err = res.get(q, (None, "no marker"))
            ok = ok and ((err is not None and sec is None) if expect == "error" else sec == expect)
        failures += _check(label, ok, str({q: v for q, v in res.items()})[:48])
    return failures


def _check_timeout_path():
    """Item 1, proved against a REAL timeout rather than a constructed value."""
    print("\nthe session-timeout path (review of #518 item 1):")
    failures = 0
    cfg = {"password": "s3cret"}
    script = ("import sys, time\n"
              "sys.stdout.write('@@T q01@@\\nRun Time (s): real 0.5 user 0 sys 0\\n@@T q02@@\\n')\n"
              "sys.stdout.flush()\n"
              "time.sleep(30)\n")
    try:
        subprocess.run([sys.executable, "-c", script], text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=2)
        return _check("a 2 s timeout on a 30 s child raises TimeoutExpired", False, "it did NOT time out")
    except subprocess.TimeoutExpired as exc:
        # This is the assertion that matters: the type Python really hands back.
        failures += _check("exc.output is bytes even though the call passed text=True",
                           isinstance(exc.output, bytes), type(exc.output).__name__)
        try:
            text = mask(decode_output(exc.output), cfg)
            failures += _check("mask(decode_output(...)) does not raise", True, "returned text")
        except Exception as e:  # noqa: BLE001 -- the point is that nothing escapes
            return failures + _check("mask(decode_output(...)) does not raise", False,
                                     f"{type(e).__name__}: {e}")
        res = parse_marked(text, "T")
        failures += _check("the queries that DID finish keep their timings",
                           res.get("q01", (None,))[0] == 0.5, str(res.get("q01")))
        hung = name_hung_query(res, "session timed out after 2s")
        failures += _check("the interrupted query is named as the timeout, not as `no Run Time`",
                           hung == "q02" and "timed out" in (res["q02"][1] or ""),
                           f"{hung}: {res['q02'][1]}")
    # A session that produced nothing at all must still be survivable.
    failures += _check("a timeout with no output at all decodes to empty",
                       decode_output(None) == "" and decode_output(b"") == "", "''")
    return failures


def _check_preamble():
    print("\nsession_preamble() -- the session's own failure (item 4):")
    text = ('Invalid Input Error: Failed to attach: error 4063 Cannot open database "tpch_no_such_db"\n'
            "@@T q01@@\nCatalog Error: Table with name lineitem does not exist!\n"
            "Run Time (s): real 0.001 user 0 sys 0\n")
    cause = session_preamble(text)
    failures = _check("the cause before the first marker is reported",
                      "Cannot open database" in cause, cause[:48])
    failures += _check("a clean session has no preamble",
                       session_preamble("@@T q01@@\nRun Time (s): real 1.0 user 0 sys 0\n") == "", "''")
    failures += _check("a [MSSQL COUNTERS] line is not a cause",
                       session_preamble("[MSSQL COUNTERS] dml: pushed\n@@T q01@@\n") == "", "''")
    return failures


def _check_report(tmp):
    """Item 7: the sum row must not print a bold 0.00 when nothing completed."""
    print("\ncmd_report() -- the row RESULTS-local.md quotes (item 7):")
    path = tmp / "timings.csv"
    rows = [
        # B completes both; A errors on q06, so NO query is common to both.
        {"run": "r", "db": "d", "client": "A", "rep": 1, "query": "q01", "seconds": "", "error": "boom"},
        {"run": "r", "db": "d", "client": "A", "rep": 1, "query": "q06", "seconds": "1.0", "error": ""},
        {"run": "r", "db": "d", "client": "B", "rep": 1, "query": "q01", "seconds": "2.0", "error": ""},
        {"run": "r", "db": "d", "client": "B", "rep": 1, "query": "q06", "seconds": "", "error": "boom"},
    ]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    report = tmp / "report.md"
    cmd_report(argparse.Namespace(timings=str(path), report=str(report)))
    text = report.read_text()
    sum_row = next(l for l in text.splitlines() if l.startswith("| **sum**"))
    failures = _check("no query common to both clients prints no bold total",
                      "**0.00**" not in sum_row, sum_row[:52])
    failures += _check("the sum row says 0 of 2 queries", "(0/2 queries)" in sum_row, sum_row[:52])

    # The fixture above would also pass against a version that printed "--"
    # unconditionally, so pin the ordinary case too.
    rows = [{"run": "r", "db": "d", "client": c, "rep": 1, "query": q, "seconds": s, "error": ""}
            for c, q, s in (("A", "q01", "1.0"), ("A", "q06", "3.0"),
                            ("B", "q01", "0.5"), ("B", "q06", "1.0"))]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    cmd_report(argparse.Namespace(timings=str(path), report=str(report)))
    sum_row = next(l for l in report.read_text().splitlines() if l.startswith("| **sum**"))
    failures += _check("a complete run still prints the bold total and ratio",
                       "**4.00**" in sum_row and "**1.50**" in sum_row and "**0.38**" in sum_row,
                       sum_row[:62])
    return failures


def cmd_selftest(args):
    failures = _check_same()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        failures += _check_compare(tmp)
        failures += _check_parse()
        failures += _check_timeout_path()
        failures += _check_preamble()
        failures += _check_report(tmp)
    print(f"\n{'FAIL' if failures else 'PASS'}: self-test, {failures} failure(s)")
    return 1 if failures else 0



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
    ap.add_argument("command", choices=["gen", "load", "verify", "explain", "trace", "run", "report",
                                       "drop", "selftest"])
    args = ap.parse_args()
    if args.db is None:
        args.db = (f"tpch_sf{args.sf:g}_{args.layout}" + ("" if args.strings == "spec" else f"_{args.strings}")).replace(".", "_")
    # `or 0`: only run and verify have a status to report, and verify exits on
    # its own. Every other command returns None, which is success.
    return {"gen": cmd_gen, "load": cmd_load, "verify": cmd_verify, "explain": cmd_explain,
            "trace": cmd_trace, "run": cmd_run, "report": cmd_report,
            "drop": cmd_drop, "selftest": cmd_selftest}[args.command](args) or 0


if __name__ == "__main__":
    sys.exit(main())

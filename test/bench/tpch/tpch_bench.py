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

The server credentials come from the environment and are never printed --
masked in every form they can take in the output, the escaped one included
(issue #509):
MSSQL_BENCH_HOST / _PORT / _USER / _PASSWORD, falling back to the test
suite's MSSQL_TEST_HOST / _PORT / _USER / _PASS. See README.md.
"""
import argparse
import contextlib
import csv
import datetime
import decimal
import io
import os
import pathlib
import re
import statistics
import subprocess
import sys
import tempfile

#: Issue #507 item 6. `argparse.BooleanOptionalAction` (now gone, below) set
#: an UNDECLARED 3.9 floor, so on the README's own Hetzner target -- Ubuntu
#: 20.04, Python 3.8.10 -- the harness died at `--help` before it could say
#: why. The floor is 3.8 (`unlink(missing_ok=True)`), and it is declared here
#: rather than discovered there.
if sys.version_info < (3, 8):
    sys.exit("error: tpch_bench.py needs Python 3.8 or newer (found %d.%d)" % sys.version_info[:2])

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
#: The collation each string flavour takes when `--collation` is not given.
#: `utf8` needs one -- a VARCHAR column in a code-page collation is not UTF-8
#: at all -- and the others inherit the server's.
DEFAULT_COLLATIONS = {"utf8": UTF8_COLLATION}


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


def collation_tag(collation):
    """The database-name component for a collation: lowercase, no separators."""
    return re.sub(r"[^A-Za-z0-9]", "", collation).lower()


def derived_db_name(args):
    """The default `--db`, which must name every schema choice it was loaded with.

    Issue #507 item 1. The name carried --sf, --layout and --strings but NOT
    --collation, which only `cmd_load` ever read -- so `--strings utf8`
    resolved to `tpch_sf10_columnstore_utf8` WHICHEVER collation was passed.
    The two UTF-8 rows of RESULTS-local.md therefore collide on one name, and
    loading the second drops the first; the published BIN2 row came from a
    hand-passed `--db` that no documented command produces.

    The whole collation name goes into the component, not a family-less
    fragment of it: `bin2` alone would collide again across collation families.
    The component is appended only when the collation differs from the default
    for the string flavour, so the three schemas that use their own default
    keep the names they had.
    """
    name = f"tpch_sf{args.sf:g}_{args.layout}"
    if args.strings != "spec":
        name += f"_{args.strings}"
    if args.collation and args.collation != DEFAULT_COLLATIONS.get(args.strings):
        name += f"_{collation_tag(args.collation)}"
    return name.replace(".", "_")


def resolve_schema(args):
    """Fill in the collation and the database name the schema flags imply.

    Here rather than inside `cmd_load`, because every command needs the same
    answer: `run`, `verify` and `trace` address the database by name, and a
    name derived without the collation is the whole of item 1.
    """
    if args.strings == "utf8" and not args.collation:
        args.collation = DEFAULT_COLLATIONS["utf8"]
    if args.db is None:
        args.db = derived_db_name(args)
    return args


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


def quote_dsn_value(value):
    """A connection-string value in braces, as the extension's parser reads them.

    Issue #509 item 2. `Password={cfg['password']}` was unquoted, and the parser
    (`src/mssql_storage.cpp`) splits an unquoted value at `;` -- so
    `MSSQL_BENCH_PASSWORD='pw;Database=master'` built a SECOND `Database` key
    and silently retargeted the connection, after which `load` created its
    tables wherever that landed. Inside braces `}}` is a literal `}`, which is
    what that parser accepts.
    """
    return "{" + value.replace("}", "}}") + "}"


def dsn(cfg, database, encrypt):
    # Encrypt and TrustServerCertificate agree, so 0.2.5 (where the latter is an
    # alias of the former) and 0.3.0 read the same thing: TLS on or off for both.
    flag = "yes" if encrypt else "no"
    # The password and the user name are brace-quoted: both are operator-supplied
    # and a `;` in either would otherwise add a key (issue #509 item 2).
    return (f"Server={cfg['host']},{cfg['port']};Database={database};"
            f"User Id={quote_dsn_value(cfg['user'])};"
            f"Password={quote_dsn_value(cfg['password'])};"
            f"Encrypt={flag};TrustServerCertificate={flag}")


def mask(text, cfg):
    """Replace the password wherever it appears, in every form it takes.

    Issue #509 item 1. It used to search for the password AS GIVEN, but the DSN
    reaches almost all output embedded in an `ATTACH '...'` statement, where it
    has been through `esc()` -- so a password containing a single quote was
    carried as `pw''X` and the masking missed it entirely:

        password in cfg:    "pw'X"
        in the SQL text:    Password=pw''X;Enc
        after mask():       Password=pw''X;Enc     <- unmasked

    while this module's docstring and README promise it is masked in all output.
    Longest form first, so replacing one cannot leave a fragment of another.
    """
    password = cfg.get("password")
    if not password:
        return text
    for form in sorted({password, esc(password), quote_dsn_value(password)}, key=len, reverse=True):
        text = text.replace(form, "***")
    return text


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


#: A database or collation name the harness sends to the server. SQL Server
#: identifiers allow far more, but every name this harness GENERATES is of this
#: shape, and the alternative is interpolating an operator-supplied value into a
#: batch that runs ALTER / DROP / CREATE DATABASE as `sa` (issue #509 item 3):
#: `esc()` doubles single quotes and does not touch `]`, so
#: `--db "x] ; CREATE LOGIN hax WITH PASSWORD='p'; --"` closed the bracket at
#: `x]`, ran the injected statement and commented out the rest.
SAFE_NAME = re.compile(r"[A-Za-z0-9_]+")


def safe_name(value, what):
    """`value` if it is a bare name, else exit saying why."""
    if not SAFE_NAME.fullmatch(value or ""):
        sys.exit(f"error: {what} {value!r} is not a bare name (letters, digits and _). "
                 f"It is sent to the server inside ALTER / DROP / CREATE DATABASE.")
    return value


def esc(s):
    return s.replace("'", "''")


# --------------------------------------------------------------------------- gen

def cmd_gen(args):
    out = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    out.mkdir(parents=True, exist_ok=True)
    dbfile = out / "tpch.duckdb"
    script = ["INSTALL tpch;", "LOAD tpch;", f"CALL dbgen(sf={args.sf});"]
    for t in TABLES:
        script.append(f"COPY {t} TO '{esc(str(out / (t + '.parquet')))}' (FORMAT parquet);")
    rc, text = run_cli(binary("A"), "\n".join(script) + "\n", {"password": ""}, database=str(dbfile))
    print(text)
    # Removed whatever happened (issue #507 item 6). The exit used to come
    # first, so a failed `gen` left a half-built tpch.duckdb behind that already
    # held some of the tables -- and `CALL dbgen` into those fails, so every
    # retry failed too until someone deleted the file by hand.
    dbfile.unlink(missing_ok=True)
    if rc != 0:
        sys.exit(rc)
    print(f"generated SF{args.sf:g} into {out}")


# --------------------------------------------------------------------------- load

def recreate_database_statements(cfg, args):
    """Drop and recreate the benchmark database, on a `master` attachment.

    The name reaches the server through QUOTENAME over a `sysname` variable, as
    `cmd_drop` already did and as the extension itself does
    (`src/catalog/mssql_metadata_cache.cpp`) -- not hand-bracketed as `[{db}]`,
    which `esc()` does not protect because it doubles single quotes and leaves
    `]` alone (issue #509 item 3). `safe_name` has already refused anything but
    a bare name, so this is the second of two guards rather than the only one.
    """
    db = safe_name(args.db, "--db")
    collation = safe_name(args.collation, "--collation") if args.collation else None
    collate = f" + N' COLLATE {collation}'" if collation else ""
    # The SINGLE_USER switch and the DROP are two statements, and the DROP is
    # in a TRY (issue #507 item 5). They used to be one batch: a DROP that
    # failed -- 5030 / 3702, any session connecting in the window -- aborted
    # before CREATE, SET RECOVERY SIMPLE and SET QUERY_STORE = OFF, leaving the
    # old database in SINGLE_USER with no connection holding it. Unusable until
    # someone issued SET MULTI_USER by hand, and a re-run hit the same pair.
    # Now the CATCH puts it back and re-raises, so the failure is reported and
    # the database is left as it was found.
    batch = ("DECLARE @n sysname = N'" + db + "'; DECLARE @sql nvarchar(max); "
             "IF DB_ID(@n) IS NOT NULL BEGIN "
             "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET SINGLE_USER WITH ROLLBACK IMMEDIATE'; "
             "EXEC (@sql); "
             "BEGIN TRY SET @sql = N'DROP DATABASE ' + QUOTENAME(@n); EXEC (@sql); END TRY "
             "BEGIN CATCH "
             "IF DB_ID(@n) IS NOT NULL BEGIN "
             "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET MULTI_USER'; EXEC (@sql); END; "
             "THROW; END CATCH; END; "
             "SET @sql = N'CREATE DATABASE ' + QUOTENAME(@n)" + collate + "; EXEC (@sql); "
             "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET RECOVERY SIMPLE'; EXEC (@sql); "
             "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET QUERY_STORE = OFF'; EXEC (@sql);")
    return [
        f"ATTACH '{esc(dsn(cfg, 'master', args.encrypt))}' AS m (TYPE mssql);",
        f"SELECT mssql_exec('m', '{esc(batch)}');",
        "DETACH m;",
    ]


def create_table_statements(args):
    """The eight CREATE TABLEs, and the clustered index each layout wants.

    The columnstore layout's primary keys are NOT here: they are added after
    the load, see `primary_key_statements`.
    """
    out = []
    for tb in TABLES:
        ddl = f"CREATE TABLE dbo.{tb} ({columns(tb, args.strings)}"
        if args.layout == "rowstore":
            ddl += f", CONSTRAINT pk_{tb} PRIMARY KEY CLUSTERED ({PRIMARY_KEYS[tb]})"
        ddl += ")"
        if args.layout == "columnstore":
            ddl += f"; CREATE CLUSTERED COLUMNSTORE INDEX cci_{tb} ON dbo.{tb}"
        out.append(f"SELECT mssql_exec('t', '{esc(ddl)}');")
    return out


def load_statements(data):
    """One marked, timed INSERT per table."""
    out = [".timer on"]
    for tb in TABLES:
        out.append(f".print @@LOAD {tb}@@")
        # esc() on the path too (issue #507 item 6): a --data-dir holding a
        # quote -- `/Volumes/Team's data` -- was a parse error, with esc()
        # sitting right there.
        out.append(f"INSERT INTO t.dbo.{tb} SELECT * FROM read_parquet('{esc(str(data / (tb + '.parquet')))}');")
    out.append(".timer off")
    return out


def primary_key_statements(args):
    """TPC-H's primary keys on a columnstore layout, added AFTER the rows.

    Issue #507 item 3. The columnstore layout -- the default, and the layout
    behind every published number -- declared no key at all, so its tables had
    no rowid key: `KeyCovered` is false on an empty unique key
    (`src/pushdown/mssql_node_writer_from.cpp`), the remote-pushdown join-gain
    check loses its unique-key rung and falls back on
    `mssql_pushdown_join_rows_threshold` (1,000,000), and at SF10 `lineitem`,
    `orders`, `partsupp`, `part` and `customer` are all above it. So `--layout`
    silently changed pushdown DECISIONS, not just storage, while TPC-H's own
    required keys were absent from every measurement.

    NONCLUSTERED, because the clustered slot is the columnstore index. After
    the load, because a clustered columnstore CARRYING a nonclustered index
    refuses the parallel bulk writers the load depends on (CLAUDE.md, spec 062:
    the extra writer times out mid-stream and the whole INSERT rolls back).
    The rowstore layout keeps its clustered primary key from the CREATE, where
    it costs the load nothing.
    """
    if args.layout != "columnstore":
        return []
    out = []
    for tb in TABLES:
        ddl = f"ALTER TABLE dbo.{tb} ADD CONSTRAINT pk_{tb} PRIMARY KEY NONCLUSTERED ({PRIMARY_KEYS[tb]})"
        out.append(f"SELECT mssql_exec('t', '{esc(ddl)}');")
    return out


def row_count_statement():
    """The eight row counts in ONE statement the server answers.

    Issue #507 item 6. These were eight `count(*)` over the `remote_pushdown
    false` attachment, so each streamed a whole column into DuckDB to print one
    number: about 86.6M rows / 700 MB at SF10 to print eight numbers. COUNT_BIG
    on the server costs one round trip and no rows.
    """
    counts = " UNION ALL ".join(f"SELECT '{tb}' AS t, COUNT_BIG(*) AS n FROM dbo.{tb}" for tb in TABLES)
    return f"SELECT * FROM mssql_scan('t', '{esc(counts)}');"


def post_load_statements(args):
    """Statistics, and for a columnstore the rowgroup compression first."""
    out = []
    for tb in TABLES:
        post = f"UPDATE STATISTICS dbo.{tb} WITH FULLSCAN"
        if args.layout == "columnstore":
            post = f"ALTER INDEX ALL ON dbo.{tb} REORGANIZE WITH (COMPRESS_ALL_ROW_GROUPS = ON); " + post
        out.append(f"SELECT mssql_exec('t', '{esc(post)}');")
    return out


def cmd_load(args):
    cfg = server()
    data = pathlib.Path(args.data_dir) / f"sf{args.sf:g}"
    missing = [t for t in TABLES if not (data / f"{t}.parquet").exists()]
    if missing:
        sys.exit(f"error: no parquet for {missing} under {data} -- run `gen` first")
    # The collation and the database name were resolved by `resolve_schema`
    # before any command ran (issue #507 item 1), so `load`, `run`, `verify`
    # and `trace` all address the same database.
    # No statement timeout: REORGANIZE and FULLSCAN statistics on a large lineitem
    # run for minutes.
    s = ["LOAD mssql;", "SET mssql_query_timeout = 0;"]
    s += recreate_database_statements(cfg, args)
    s.append(f"ATTACH '{esc(dsn(cfg, args.db, args.encrypt))}' AS t (TYPE mssql, remote_pushdown false);")
    s += create_table_statements(args)
    s.append("SELECT mssql_invalidate_cache('t');")
    s += load_statements(data)
    s += primary_key_statements(args)
    s += post_load_statements(args)
    s.append(row_count_statement())
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


def _marker(line, tag):
    """(is this a marker, the query it opens).

    The query is None for a marker of ANOTHER tag -- a warmup pass's `@@W q01@@`
    while reading the timed `T` ones -- which closes the current section and
    starts no new one, so the warmup's output is attributed to nothing.
    """
    m = MARKER.match(line.strip())
    if not m:
        return False, None
    return True, m.group(2) if m.group(1) == tag else None


def _absorb(line, seconds, extra):
    """Fold one line of a query's section in. -> the section's seconds.

    Appends to `extra` -- the unexpected output that makes the query an error
    rather than a timing. A `[MSSQL COUNTERS]` line is neither: it is expected
    output from a variable CLAUDE.md tells the operator to set.
    """
    if line.lstrip().startswith(COUNTERS):
        return seconds
    timing = RUN_TIME.search(line)
    if timing:
        # A SECOND Run Time inside one section is consumed rather than counted
        # as unexpected output: the CLI prints one for a failed statement too,
        # after its error text.
        return float(timing.group(1)) if seconds is None else seconds
    if line.strip():
        extra.append(line.strip())
    return seconds


def _verdict(seconds, extra):
    """One query's (seconds, error), from what followed its marker.

    Unexpected output wins over a timing: the CLI prints a failure's text and
    THEN a `Run Time` for it, because END_TIMER fires before the error check
    (issue #505 item 2).
    """
    if extra:
        return None, " ".join(extra)[:300]
    if seconds is None:
        return None, NO_RUN_TIME
    return seconds, None


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

    for line in text.splitlines():
        is_marker, query = _marker(line, tag)
        if is_marker:
            if current is not None:
                out[current] = _verdict(seconds, extra)
            current, seconds, extra = query, None, []
        elif current is not None:
            seconds = _absorb(line, seconds, extra)
    if current is not None:
        out[current] = _verdict(seconds, extra)
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


def timed_script(client, cfg, args, queries):
    """The session a timed run sends: per query, `--warmup` untimed passes then one timed.

    Two things here are issue #507 item 6.

    `.mode csv` pins the renderer, which runs INSIDE the timer: the rows go to
    /dev/null, but the default duckbox formatter still measures every column's
    width over every row, and its implementation differs between 1.5.6 and 2.0
    -- a client-version difference inside the number being compared. csv is
    the same trivial writer in both.

    Each query is warmed immediately before it is timed, not once per pass.
    With a full warmup pass first, by the time the timed pass reached q01 its
    data had been untouched the longest while q22's was still hot: a systematic
    bias along the query order, inside the warmup that exists to remove one.
    """
    s = session_head(client, cfg, args)
    s.append(".mode csv")
    s.append(".timer on")
    for q in queries:
        text = query_text(client, q, args.sf)
        for _ in range(args.warmup):
            s += [".output", f".print @@W {q}@@", ".output /dev/null", text]
        s += [".output", f".print @@T {q}@@", ".output /dev/null", text]
    return s + [".output", ".print @@END end@@"]


def timed_session(client, cfg, args, queries):
    s = timed_script(client, cfg, args, queries)
    rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg, timeout=args.timeout)
    return parse_marked(text, "T"), text


# --------------------------------------------------------------------------- run / report

def selected(args):
    """The clients and queries to work on, every name checked at the flag.

    Issue #507 item 6: a stray comma used to reach the body of a command as an
    empty name -- `--queries q01,` opened `queries/duckdb/.sql` and died with a
    FileNotFoundError, `--clients ,` divided by a client count of zero -- and an
    unknown name failed the same way, deep inside a session, long after the
    flag that held it.
    """
    clients = [c.strip() for c in args.clients.split(",") if c.strip()]
    queries = [q.strip() for q in (args.queries or "").split(",") if q.strip()] or list(QUERIES)
    unknown = [c for c in clients if c not in CLIENTS]
    if unknown or not clients:
        sys.exit(f"error: --clients {args.clients!r} names "
                 + (f"no such client: {', '.join(unknown)}" if unknown else "no client")
                 + f". Pick from {', '.join(CLIENTS)}.")
    unknown = [q for q in queries if q not in QUERIES]
    if unknown:
        sys.exit(f"error: --queries {args.queries!r} names no such query: {', '.join(unknown)}. "
                 f"Pick from {QUERIES[0]}..{QUERIES[-1]}.")
    return clients, queries


def session_results(client, cfg, args, queries, rep):
    """One client's timed session. -> (results, session text).

    A timeout is not a lost session: the exception still carries the stdout of
    every query that DID finish, Run Time lines included, so it is parsed
    rather than discarded.
    """
    try:
        return timed_session(client, cfg, args, queries)
    except subprocess.TimeoutExpired as exc:
        # mask() it: this path never went through run_cli's masking, and an
        # ATTACH failure can quote the connection string back.
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
        return res, text


def write_results(writer, args, run_id, rep, client, queries, res):
    """One session's rows into timings.csv. -> how many errored."""
    errored = 0
    for q in queries:
        sec, err = res.get(q, (None, "no marker in output"))
        writer.writerow({"run": run_id, "db": args.db, "client": client, "rep": rep + 1, "query": q,
                         "seconds": "" if sec is None else f"{sec:.4f}", "error": err or ""})
        errored += 1 if err else 0
        print(f"rep {rep + 1} {client:6s} {q} {'ERROR ' + err if err else f'{sec:9.3f} s'}", flush=True)
    return errored


def rotated(clients, rep):
    """The client order for this rep: rotated, so no client is always first."""
    k = rep % len(clients)
    return clients[k:] + clients[:k]


def preserved_rows(path, clients):
    """The rows of an existing timings.csv this invocation must not lose.

    Issue #507 item 4. `run` opened timings.csv in "w" and wrote only the
    current invocation's rows, while leaving the previous invocation's session
    logs in place and `README.md` fixing the run id -- so re-running
    `--run-id sf10 --clients B run` replaced a published 4-client, 176-row file
    with a 22-row B-only one, beside `session-1-{A,Bscan,C}.log` that survived
    and still claimed to document the medians now unrecoverable from it.

    The rows of the clients this invocation measures ARE replaced; every other
    client's are kept.
    """
    if not path.exists():
        return []
    with open(path, newline="") as f:
        return [r for r in csv.DictReader(f) if r.get("client") not in clients]


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
    kept = preserved_rows(path, clients)
    errored = 0
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run", "db", "client", "rep", "query", "seconds", "error"])
        w.writeheader()
        for row in kept:
            w.writerow({k: row.get(k, "") for k in w.fieldnames})
        f.flush()
        if kept:
            print(f"keeping {len(kept)} row(s) of {', '.join(sorted({r['client'] for r in kept}))} "
                  f"already in {path}", flush=True)
        # The session logs of the clients being measured again, so no stale log
        # sits beside rows from another invocation (the other half of item 4).
        for client in clients:
            for stale in sorted(out.glob(f"session-*-{client}.log")):
                stale.unlink()
        for rep in range(args.reps):
            for client in rotated(clients, rep):
                res, text = session_results(client, cfg, args, queries, rep)
                (out / f"session-{rep + 1}-{client}.log").write_text(text)
                cause = session_preamble(text)
                if cause:
                    print(f"rep {rep + 1} {client:6s} -- SESSION ERROR (before the first query): {cause}",
                          flush=True)
                errored += write_results(w, args, run_id, rep, client, queries, res)
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


def samples(rows, client, query):
    """(the timings that survived, the first error) for one client and query."""
    vals, errs = [], []
    for r in rows:
        if r["client"] != client or r["query"] != query:
            continue
        if r["seconds"]:
            vals.append(float(r["seconds"]))
        if r["error"]:
            errs.append(r["error"])
    return vals, errs[0] if errs else ""


def medians(rows, clients, queries):
    """{(client, query): (median seconds or None, surviving reps, first error)}."""
    med = {}
    for c in clients:
        for q in queries:
            vals, err = samples(rows, c, q)
            med[c, q] = (statistics.median(vals) if vals else None, len(vals), err)
    return med


def comparable(med, clients, queries):
    """(queries every client completed, the rest, per-client totals over the first).

    The sum must cover the SAME work for every client (issue #505 item 5).
    Accumulating whatever each client happened to complete published a client
    that errored on an expensive query as several times faster, with nothing
    in the bolded row marking it partial -- and that row is the one
    RESULTS-local.md quotes as its headline.
    """
    common = [q for q in queries if all(med[c, q][0] is not None for c in clients)]
    missing = [q for q in queries if q not in common]
    totals = {c: sum(med[c, q][0] for q in common) for c in clients}
    return common, missing, totals


def cell(value, reps, error, reps_max):
    """One client's cell for one query."""
    if value is None:
        return f"error: {error[:40]}"
    # The rep count was stored in every cell and never read, though the header
    # promised "n = timed reps" -- so a median over one surviving rep looked
    # identical to one over three. Annotate any cell that is short.
    return f"{value:.3f}" + (f" (n={reps})" if reps < reps_max else "")


def query_row(q, clients, ratio_cols, med, base, reps_max):
    cells = [cell(*med[c, q], reps_max=reps_max) for c in clients]
    b = med[base, q][0]
    for c in ratio_cols:
        v = med[c, q][0]
        cells.append(f"{v / b:.2f}" if v is not None and b else "")
    return f"| {q} | " + " | ".join(cells) + " |"


def sum_row(clients, ratio_cols, totals, base, common, queries):
    tb = totals[base]
    # With no query completed in every client the totals are all 0, and a bold
    # **0.00** in the row RESULTS-local.md quotes reads like a measured result
    # (review of #518 item 7). An em dash cannot be mistaken for one.
    cells = ([f"**{totals[c]:.2f}**" for c in clients] if common else ["--" for _ in clients]) + \
            [f"**{totals[c] / tb:.2f}**" if common and tb else "--" for c in ratio_cols]
    return f"| **sum** ({len(common)}/{len(queries)} queries) | " + " | ".join(cells) + " |"


def exclusion_note(common, missing):
    if not missing:
        return []
    noun = "query" if len(common) == 1 else "queries"
    return ["", f"The sum covers only the {len(common)} {noun} every client completed, so the totals and "
                f"ratios compare like with like. Excluded because at least one client has no median: "
                f"{', '.join(missing)}."]


def report_lines(clients, queries, med, base, reps_max):
    common, missing, totals = comparable(med, clients, queries)
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
    lines += [query_row(q, clients, ratio_cols, med, base, reps_max) for q in queries]
    lines.append(sum_row(clients, ratio_cols, totals, base, common, queries))
    lines += exclusion_note(common, missing)
    return lines


def timings_path(args):
    """The timings.csv `report` summarises.

    Issue #507 item 6: without `--timings` this was `pathlib.Path(None)`, a
    bare TypeError from inside the command. `--run-id` names the run's own
    file, which is what a scripted `run && report` has at hand.
    """
    if args.timings:
        return pathlib.Path(args.timings)
    if getattr(args, "run_id", None):
        return pathlib.Path(args.out) / args.run_id / "timings.csv"
    sys.exit("error: report needs --timings <path to timings.csv>, "
             "or --run-id (then --out/<run-id>/timings.csv)")


def cmd_report(args):
    path = timings_path(args)
    if not path.exists():
        sys.exit(f"error: no timings at {path}")
    rows = list(csv.DictReader(open(path)))
    clients = [c for c in CLIENTS if any(r["client"] == c for r in rows)]
    if not clients:
        sys.exit(f"error: {path} holds no rows of a known client "
                 f"({', '.join(CLIENTS)}); nothing to report")
    queries = sorted({r["query"] for r in rows})
    med = medians(rows, clients, queries)
    base = "A" if "A" in clients else clients[0]
    reps_max = max((n for _, n, _ in med.values()), default=0)

    text = "\n".join(report_lines(clients, queries, med, base, reps_max)) + "\n"
    print(text)
    if args.report:
        pathlib.Path(args.report).write_text(text)


# --------------------------------------------------------------------------- verify

#: The engines the oracle is computed on, and they must agree.
#:
#: Issue #507 item 2. `cmd_verify` ran the oracle on `binary("A")` alone, so
#: client A was graded against its OWN engine (and, through the parquet, the
#: nearest thing to its own version of DuckDB) while B, Bscan and C were graded
#: against a different one. Any wrong answer shared by DuckDB 1.5.6 and mssql
#: 0.2.5 was invisible by construction -- which is exactly the configuration
#: whose #405 CHAR-padding wrong answers RESULTS-local.md had to find and
#: footnote by hand instead of reading off `verify`. In the other direction, a
#: 1.5.6-versus-2.0 aggregation or typing difference surfaced as a diff for B
#: and Bscan only, and read like an extension bug: q01's
#: `sum(l_extendedprice*(1-l_discount))`, q17's `0.2*avg(...)`.
#:
#: Both engines now compute it over the same parquet and must produce the same
#: answer; where they do not, the query is NOT graded and the disagreement is
#: reported as itself. That is the one verdict neither client can be blamed
#: for, and it belongs to the engines, not to the extension.
ORACLES = ("A", "B")


def clear_outputs(out, clients, queries):
    """Remove this run's CSVs before anything writes them.

    Issue #506 item 3. A query that fails at BIND never opens its output file
    at all, so a stale file from the previous run would be compared and
    reported `ok`. Bind-time failure is client C's own failure mode, since its
    mssql_scan bind calls sp_describe_first_result_set.
    """
    for q in queries:
        for which in ORACLES:
            (out / f"oracle-{which}-{q}.csv").unlink(missing_ok=True)
            (out / f"types-{which}-{q}.csv").unlink(missing_ok=True)
        for client in clients:
            (out / f"{client}-{q}.csv").unlink(missing_ok=True)


def write_oracle(cfg, args, data, out, queries, which):
    """One engine, over the same parquet the server was loaded from.

    `which` is an entry of ORACLES, not a client: the oracle reads parquet, so
    the extension is out of its path either way and only the ENGINE differs.
    The paths go through esc() (issue #507 item 6): a --data-dir or --out
    holding a quote was a parse error.
    """
    s = [f"CREATE VIEW {t} AS SELECT * FROM read_parquet('{esc(str(data / (t + '.parquet')))}');"
         for t in TABLES]
    for q in queries:
        sql = read_query("duckdb", q, args.sf)
        s.append(f"COPY ({sql}) TO '{esc(str(out / ('oracle-' + which + '-' + q + '.csv')))}' (HEADER false);")
        # The column TYPES, in order, beside the answers. This is what makes
        # the comparison type-driven instead of guessing from how a value
        # happens to print: `DESCRIBE` works as a subquery source, so one extra
        # statement per query gets them with no second session.
        s.append(f"COPY (SELECT column_type FROM (DESCRIBE {sql})) "
                 f"TO '{esc(str(out / ('types-' + which + '-' + q + '.csv')))}' (HEADER false);")
    rc, text = run_cli(binary(which), "\n".join(s) + "\n", cfg)
    if rc != 0:
        sys.exit(f"oracle {which}: {text}")


def oracle_agreement(out, queries):
    """What the oracle engines say, per query.

    -> ({query: (the answer both produced, its column types)},
        {query: why they disagree})

    The types are compared first and as strictly as the values: two engines
    that call a column DOUBLE and DECIMAL(38,6) do not agree about it, and the
    type is what decides how every client's value is then judged.
    """
    agreed, disputed = {}, {}
    for q in queries:
        answers = [(w, out / f"oracle-{w}-{q}.csv", read_types(out / f"types-{w}-{q}.csv"))
                   for w in ORACLES]
        missing = [w for w, path, _ in answers if not path.exists()]
        if missing:
            disputed[q] = f"oracle {', '.join(missing)} produced no answer (see the oracle log)"
            continue
        first, first_path, first_types = answers[0]
        for which, path, types in answers[1:]:
            if types != first_types:
                disputed[q] = (f"{first} says the columns are {first_types} "
                               f"and {which} says {types}")
                break
            verdict = compare(first_path, path, first_types)
            if not verdict.startswith("ok"):
                disputed[q] = f"{first} and {which} do not agree: {verdict}"
                break
        else:
            agreed[q] = (first_path, first_types)
    return agreed, disputed


def write_client(cfg, args, out, client, queries):
    """One client's answers. -> a session-level error line, or None.

    The client session's return code was bound and never read, while the
    oracle's was checked (issue #506 item 3). The CLI does not bail on error
    for piped stdin, so this is the only signal that the session as a whole
    went wrong, and dropping it is how a failure became an `ok`. It is reported
    ONCE rather than replacing all 22 of that client's verdicts (review of #518
    item 3): the clearing above turns the query that actually failed into
    MISSING, which names it.
    """
    s = session_head(client, cfg, args)
    for q in queries:
        body = query_text(client, q, args.sf).rstrip(";")
        s.append(f"COPY ({body}) TO '{out / (client + '-' + q + '.csv')}' (HEADER false);")
    rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg, timeout=args.timeout)
    if text.strip():
        (out / f"{client}.log").write_text(text)
    if rc != 0:
        return f"client session exited {rc} (see {out / (client + '.log')})"
    return None


def print_verdicts(out, clients, queries, sessions, agreed, disputed):
    """-> how many verdicts were not an `ok`.

    A query the oracle engines disagree about is counted ONCE, as its own
    failure, and no client is graded on it: grading it would publish an engine
    difference as a wrong answer by whichever client sat on the other side
    (issue #507 item 2).
    """
    bad = len(disputed)
    for q in queries:
        if q in disputed:
            print(f"oracle {q} ORACLE DISAGREEMENT -- {disputed[q]}")
    for client in clients:
        if client in sessions:
            print(f"{client:6s} --- {sessions[client]}")
        for q in queries:
            if q in disputed:
                print(f"{client:6s} {q} not graded (the oracle engines disagree)")
                continue
            oracle_path, types = agreed[q]
            # `verdict != "ok"` rejected compare()'s OWN accepting verdict for
            # a reordered result, so a tie emitted in the other order failed the
            # run while every per-query line still read `ok` -- teaching the
            # operator to disregard the exit code, which is the channel a real
            # wrong answer travels (issue #506 item 4).
            verdict = compare(oracle_path, out / f"{client}-{q}.csv", types)
            if not verdict.startswith("ok"):
                bad += 1
            print(f"{client:6s} {q} {verdict}")
    return bad


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
    clear_outputs(out, clients, queries)

    for which in ORACLES:
        write_oracle(cfg, args, data, out, queries, which)
    agreed, disputed = oracle_agreement(out, queries)
    sessions = {}
    for client in clients:
        failure = write_client(cfg, args, out, client, queries)
        if failure:
            sessions[client] = failure

    bad = print_verdicts(out, clients, queries, sessions, agreed, disputed)
    print(f"{bad} mismatch(es)"
          + (f", {len(disputed)} of them a disagreement between the oracle engines" if disputed else "")
          + (f", {len(sessions)} failed session(s)" if sessions else ""))
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


#: DuckDB type names whose values can legitimately differ between the two
#: engines. Everything else -- DECIMAL, the integer family, VARCHAR, DATE,
#: BOOLEAN -- is exact on both sides and must match.
#:
#: An inclusion list, so an unrecognised type defaults to EXACT. A mistake here
#: therefore makes the gate stricter and loud, not looser and silent, which is
#: the direction every defect in this comparison has gone the other way.
INEXACT_TYPES = ("DOUBLE", "FLOAT", "REAL")
#: Exact columns whose values are NUMBERS, so a client printing the same number
#: at another scale is not a difference. Everything not here and not inexact is
#: compared as TEXT.
NUMERIC_TYPES = ("DECIMAL", "NUMERIC", "TINYINT", "SMALLINT", "INTEGER", "INT",
                 "BIGINT", "HUGEINT", "UTINYINT", "USMALLINT", "UINTEGER",
                 "UBIGINT", "UHUGEINT")

#: How a column must be compared.
TOLERANCE, NUMERIC, TEXT = "tolerance", "numeric", "text"


def column_kind(type_name):
    """How must this column be compared: TOLERANCE, NUMERIC or TEXT?

    The ORACLE's type decides, because it is the only evidence of what the
    values are. Four successive attempts to infer it from how a value PRINTS
    each had a hole, and the last could not be fixed at all: at an SF1000
    magnitude one ulp of a double is 0.0078, so a cent of rounding error in an
    exact decimal is 1.3 ulps -- no relative bound separates them.

    Three kinds rather than two (roborev on 6ea78d9). Comparing every exact
    column by `Decimal` value loosened the gate for TEXT: `"07"` equalled
    `"7"`, `"13"` equalled `"13.0"` and `"-0"` equalled `"0"`, so a client that
    dropped a leading zero from a string key would have read `ok`. Only a
    NUMERIC column wants value comparison -- that is what lets the server's
    padded decimal(38,6) match the oracle's decimal(38,2).

    An unknown type is TEXT, the strictest kind: a new column fails loudly
    rather than quietly taking a tolerance or a looser equality.
    """
    head = type_name.split("(")[0].strip().upper()
    if head in INEXACT_TYPES:
        return TOLERANCE
    return NUMERIC if head in NUMERIC_TYPES else TEXT


def read_types(path):
    """The oracle's column types, in order, or None when they are absent.

    None is a REFUSAL, not a default: a comparison that cannot tell an exact
    column from a double is the thing this replaced, so it reports rather than
    guesses.
    """
    if not path.exists():
        return None
    with open(path) as fh:
        types = [row[0] for row in csv.reader(fh) if row]
    return types or None


#: The relative term, for DOUBLE columns only now, sized to what the two
#: engines' doubles can actually disagree by.
#:
#: Every DOUBLE TPC-H produces on either side is an exact decimal sum or count
#: converted and divided ONCE -- `avg(decimal)`, `decimal / decimal`, or the
#: extension's pushed `CAST(SUM(x) AS float) / COUNT_BIG(x)` -- which puts the
#: two sides within a few ulps, about 1e-15 relative. 1e-13 leaves ~724 ulps of
#: margin at an SF1000 magnitude.
#:
#: It is no longer a backstop for pairs a heuristic could not classify, because
#: there is no heuristic left: an exact column never reaches it.
REL_TOLERANCE = decimal.Decimal("1e-13")
#: The absolute floor: a half digit of the server's decimal(38,6) is 5e-7.
ABS_TOLERANCE = decimal.Decimal("1e-6")


def tolerance(magnitude):
    """How far apart two DOUBLE answers may legitimately be."""
    return max(ABS_TOLERANCE, REL_TOLERANCE * magnitude)


def as_decimal(s):
    """The value as a Decimal when it is written as a plain decimal, else None."""
    return decimal.Decimal(s) if DECIMAL.fullmatch(s) else None


def within_tolerance(x, y, dx, dy):
    """Two DOUBLE values, as close as the two engines may legitimately be.

    By `Decimal` where both sides are plain decimals, and by float otherwise --
    the forms a DOUBLE can print that an exact decimal cannot (`1e-05`). Both
    paths are deliberately strict about shape: `FLOATING` rejects a leading
    blank and a digit separator that `float()` would accept, so a formatting
    difference is still a difference.
    """
    if dx is not None and dy is not None:
        return abs(dx - dy) <= tolerance(max(abs(dx), abs(dy)))
    fx, fy = as_float(x), as_float(y)
    if fx is None or fy is None:
        return False
    return abs(fx - fy) <= float(tolerance(decimal.Decimal(max(abs(fx), abs(fy)))))


def same(x, y, kind):
    """Is the client's value `y` the oracle's `x`, for a column of this kind?

    Issue #506 items 1 and 2, and three reviews of #518. The history is worth
    keeping because every version failed in the same direction -- accepting a
    wrong answer -- and each fix was narrower than it looked:

    * `x.strip() == y.strip()` then a 0.011 ABSOLUTE floor: wider than whole
      answers. q08's entire result is two rows 0.00705 apart, so swapping the
      two years printed `ok`. `.strip()` also hid CHAR(n) padding, which is the
      #405 class this harness exists to catch.
    * a 1e-6 RELATIVE term: tolerated off-by-N on counts. q01's count_order is
      1,478,493 at SF1, so +-1.48 rows and +-148 at SF100 -- a row-dropping
      pushdown bug on exactly the aggregates TPC-H is made of passed.
    * exact when the two PRINTED SCALES agree: a DOUBLE in shortest
      round-trip form parses as a decimal too, and DuckDB returns
      `avg(decimal)` and `decimal / decimal` as DOUBLE, so ulp-apart pairs of
      q01's avg_qty and q08's mkt_share became wrong answers -- 1148 of 4000
      at one magnitude.
    * ... and exact only below 15 significant digits: q01's own
      `decimal(38,6)` sum_charge prints 18 at SF1, so an EXACT sum took the
      tolerance branch where a dropped row passed from SF10 up.
    * ... and exact unless both sides round-trip through `repr()`: a scale-2 or
      scale-4 decimal of 16 or fewer digits usually does, so a one-cent
      difference in q01's sum_base_price passed at SF10.

    So the form of a value is not evidence of its type, and the last of those
    cannot be fixed by tightening: at 5.7e13 one ulp is 0.0078, making a cent
    1.3 ulps. The oracle's column TYPE is asked instead, and `kind` is its
    answer -- no inference, no magnitudes, no digit counts.

    A NUMERIC column is compared by VALUE, so a padded `decimal(38,6)` equals
    the oracle's `decimal(38,2)` (`Decimal("1.50") == Decimal("1.5")`) while a
    cent of difference does not. A TEXT column gets only the identity check
    above, so CHAR(n) padding -- and a dropped leading zero -- is a mismatch.
    """
    if x == y:
        return True
    if kind == TEXT:
        return False
    dx, dy = as_decimal(x), as_decimal(y)
    if kind == NUMERIC:
        return dx is not None and dy is not None and dx == dy
    return within_tolerance(x, y, dx, dy)


def rows_equal(ra, rb, kinds):
    """`kinds` is the per-column answer from the oracle's types."""
    return len(ra) == len(rb) and all(same(x, y, k) for x, y, k in zip(ra, rb, kinds))


def difference_verdict(a, b, kinds):
    """What to call two results that did not compare equal."""
    # Same rows, different order. The old wording was "row order of ties
    # differs", which it has no evidence for: an inverted ORDER BY with no ties
    # at all reaches here too. Say what is actually established.
    if all(rows_equal(ra, rb, kinds) for ra, rb in zip(sorted(a), sorted(b))):
        return "ok (same rows, different order)"
    # Name trailing-blank padding specifically, since it is the #405 class and
    # an exact comparison is the only thing that can see it.
    if all(len(ra) == len(rb) and all(x.strip() == y.strip() for x, y in zip(ra, rb))
           for ra, rb in zip(a, b)):
        return "PADDING (values differ only in surrounding blanks -- the #405 class)"
    for i, (ra, rb) in enumerate(zip(a, b)):
        if not rows_equal(ra, rb, kinds):
            return f"DIFF at row {i + 1}: {rb[:6]} vs {ra[:6]}"
    return "DIFF"


def types_refusal(types, rows):
    """Why these types cannot be used, or "" when they can.

    A refusal rather than a default: a comparison that cannot tell an exact
    column from a double is what this replaced.

    The width is only checked when the rows HAVE a width. `csv.reader` reads
    the blank line COPY writes for a single-column NULL as `[]`, so a result
    like q17's NULL `avg_yearly` at a small scale factor measured 0 columns and
    was refused as "the oracle's types do not fit its rows" -- blaming the
    oracle for a correct answer, and masking a client that returned a number
    where the oracle had NULL behind the same misleading verdict (review of
    #537 by VGSML, reproduced at SF 0.01).
    """
    if types is None:
        return "NO TYPES (the oracle did not emit them; cannot compare)"
    width = max((len(r) for r in rows), default=0)
    if width and len(types) != width:
        return f"TYPES {len(types)} for {width} column(s) (the oracle's types do not fit its rows)"
    return ""


def compare(oracle, got, types=None):
    """The verdict for one query. `types` is the oracle's column types.

    Without them nothing is compared: a comparison that cannot tell an exact
    column from a double is what this replaced, so a missing types file is
    reported rather than guessed around.
    """
    if not got.exists():
        return "MISSING (the query failed; see the client log)"
    a = list(csv.reader(open(oracle)))
    b = list(csv.reader(open(got)))
    if len(a) != len(b):
        return f"ROWS {len(b)} != {len(a)}"
    refusal = types_refusal(types, a)
    if refusal:
        return refusal
    kinds = [column_kind(t) for t in types]
    if all(rows_equal(ra, rb, kinds) for ra, rb in zip(a, b)):
        return "ok"
    return difference_verdict(a, b, kinds)


# --------------------------------------------------------------------------- selftest

# Every fixture below is a case this harness got WRONG, each named with the
# review item that found it. Nothing here needs a server, a database or a
# DuckDB binary, so it runs anywhere and belongs in CI: the defects these guard
# were all of one kind -- the harness reporting a verdict it had not earned --
# and that kind is invisible until something checks the judge.

#: (label, the oracle's column type, oracle value, client value, accepted?)
#
# Every fixture is a case this harness decided WRONGLY, and the TYPE column is
# the point: the same two strings must be judged differently depending on what
# the oracle says the column is. That is the whole change -- `234603658237.28`
# against `...29` is a wrong answer in a DECIMAL column and noise in a DOUBLE.
SAME_TESTS = [
    # ---- DECIMAL and integer columns: exact, at every magnitude -------------
    # #506 item 1 and the review of #518. A one-cent difference in an exact sum
    # is a wrong answer whether the value is 11 digits or 18, and whether or
    # not it happens to round-trip through repr() -- the two properties three
    # earlier versions of this rule keyed on.
    ("a decimal sum one cent apart", "DECIMAL(38,2)",
     "566012345678.34", "566012345678.35", False),
    ("an 18-digit decimal(38,6) sum one cent apart", "DECIMAL(38,6)",
     "234603658237.282580", "234603658237.292580", False),
    ("an SF1000-sized decimal sum one cent apart", "DECIMAL(38,2)",
     "56586554400730.17", "56586554400730.18", False),
    ("an SF1000-sized decimal sum 10 000 apart", "DECIMAL(38,2)",
     "56586554400730.17", "56586554410730.17", False),
    ("a decimal sum a thousandth apart", "DECIMAL(38,4)",
     "540012345678.1234", "540012345678.1235", False),
    ("the same decimal", "DECIMAL(38,2)", "37734107.00", "37734107.00", True),
    # Compared BY VALUE, so the client padding to a wider scale is not a
    # difference. This is what VGSML pointed out makes a scale fallback
    # unnecessary: Decimal("1.50") == Decimal("1.5").
    ("a decimal the client padded to a wider scale", "DECIMAL(38,2)",
     "234603658237.28", "234603658237.280000", True),
    ("a decimal the client printed with fewer decimals", "DECIMAL(38,6)",
     "1.500000", "1.5", True),
    # #506 item 2: a 1e-6 relative term tolerated off-by-N on counts.
    ("a count one row short", "BIGINT", "1478493", "1478492", False),
    ("the same count", "BIGINT", "1478493", "1478493", True),
    ("a count the client padded", "BIGINT", "1478493", "1478493.0", True),
    # ---- DOUBLE columns: the tolerance, and only here -----------------------
    # DuckDB returns avg(decimal) and decimal/decimal as DOUBLE -- q01's
    # avg_qty / avg_price / avg_disc and q08's mkt_share -- and CLAUDE.md
    # records that the extension's pushed CAST(SUM(x) AS float) / COUNT_BIG(x)
    # differs in its last bits BY DESIGN.
    ("two DOUBLEs one ulp apart (q01 avg_qty)", "DOUBLE",
     "25.522005853257337", "25.522005853257333", True),
    ("two DOUBLEs one ulp apart (q08 mkt_share)", "DOUBLE",
     "0.034435890263343130", "0.034435890263343136", True),
    ("adjacent DOUBLEs at an SF1000 magnitude", "DOUBLE",
     "56586554400730.17", "56586554400730.18", True),
    ("a DOUBLE beside the server's decimal(38,6)", "DOUBLE", "0.05", "0.050000", True),
    ("a DOUBLE within a half digit", "DOUBLE", "1.0000001", "1.000000", True),
    # ... but the tolerance is not a licence: q08's whole result is two rows
    # 0.00705 apart, which the old 0.011 ABSOLUTE floor swallowed.
    ("q08's two mkt_share values", "DOUBLE", "0.03443589", "0.04148552", False),
    ("two DOUBLEs a whole percent apart", "DOUBLE",
     "25.522005853257337", "25.777005853257337", False),
    ("a DOUBLE sum 10 000 apart at an SF1000 magnitude", "DOUBLE",
     "56586554400730.17", "56586554410730.17", False),
    # ---- strings and the rest: exact, so #405 padding is a mismatch ---------
    ("a CHAR(10) value padded on the right", "VARCHAR",
     "MACHINERY", "MACHINERY ", False),
    ("a CHAR(10) value padded on the left", "VARCHAR",
     "MACHINERY", " MACHINERY", False),
    ("the same string", "VARCHAR", "MACHINERY", "MACHINERY", True),
    ("a date one day out", "DATE", "1995-03-15", "1995-03-16", False),
    ("the same date", "DATE", "1995-03-15", "1995-03-15", True),
    ("an empty value against an empty value", "VARCHAR", "", "", True),
    ("text against a number", "VARCHAR", "MACHINERY", "17", False),
    # A TEXT column is identity only. Comparing every exact column by Decimal
    # value made all four of these equal, so a client that dropped a leading
    # zero from a string key read `ok` (roborev on 6ea78d9).
    ("a VARCHAR with a leading zero dropped", "VARCHAR", "07", "7", False),
    ("a VARCHAR printed as a number", "VARCHAR", "13", "13.0", False),
    ("a VARCHAR signed zero", "VARCHAR", "-0", "0", False),
    ("a VARCHAR trailing zero", "VARCHAR", "1.50", "1.5", False),
    # ... while the same four in a NUMERIC column ARE the same number.
    ("the same number written with a leading zero", "BIGINT", "07", "7", True),
    ("the same number at another scale", "DECIMAL(38,2)", "1.50", "1.5", True),
    # Restored: the type rewrite dropped these three and took the coverage of
    # DECIMAL's strictness, FLOATING's shape and the as_float path with them.
    # VGSML mutated the code three ways and the self-test stayed green --
    # removing the as_float branch, loosening DECIMAL to accept blanks and
    # digit separators, and setting FLOATING to `.*` (review of #537).
    #
    # int() accepts both of these, which is why as_int was replaced by a
    # regex with no strip in the first place.
    ("a NUMERIC value with a leading blank", "BIGINT", "13", " 13", False),
    ("a NUMERIC value written with a digit separator", "BIGINT", "1000", "1_000", False),
    ("a NUMERIC value with a trailing blank", "DECIMAL(38,2)", "13.00", "13.00 ", False),
    # The as_float path: the forms a DOUBLE can print that an exact decimal
    # cannot. Only a DOUBLE column reaches it.
    ("a DOUBLE in scientific notation, equal", "DOUBLE", "1e3", "1000.0", True),
    ("a DOUBLE in scientific notation, small", "DOUBLE", "1e-05", "0.00001", True),
    ("a DOUBLE in scientific notation, different", "DOUBLE", "1e3", "2000.0", False),
    # FLOATING's own fixture: a leading blank is a formatting difference the
    # gate must see, even in a DOUBLE column. float() would strip it, so this
    # is what fails when FLOATING is loosened to `.*` -- the third mutation in
    # the review of #537, which nothing else caught.
    ("a DOUBLE value with a leading blank", "DOUBLE", "1000.0", " 1000.0", False),
    ("a DOUBLE with a digit separator", "DOUBLE", "1000.0", "1_000.0", False),
    # An unrecognised type defaults to EXACT, so a new column kind fails loudly
    # rather than quietly taking a tolerance.
    ("an unknown type is treated as exact", "SOME_NEW_TYPE", "1.00", "1.01", False),
]

#: (the type name, its kind)
TYPE_TESTS = [
    # Compared by VALUE, so a client printing the same number at another scale
    # matches.
    ("DECIMAL(38,6)", NUMERIC), ("DECIMAL(18,2)", NUMERIC), ("decimal(38,2)", NUMERIC),
    ("NUMERIC(10,2)", NUMERIC), ("BIGINT", NUMERIC), ("INTEGER", NUMERIC),
    ("HUGEINT", NUMERIC), ("UBIGINT", NUMERIC), ("TINYINT", NUMERIC),
    # Identity only, so a dropped leading zero or CHAR(n) padding is a
    # mismatch.
    ("VARCHAR", TEXT), ("DATE", TEXT), ("BOOLEAN", TEXT), ("TIMESTAMP", TEXT),
    ("INTERVAL", TEXT), ("BLOB", TEXT),
    # The only kind that may differ at all.
    ("DOUBLE", TOLERANCE), ("FLOAT", TOLERANCE), ("REAL", TOLERANCE),
    ("double", TOLERANCE),
    # An unknown type gets the STRICTEST kind, not a tolerance.
    ("SOME_NEW_TYPE", TEXT),
]

#: (label, column types, oracle rows, client rows, expected verdict prefix)
COMPARE_TESTS = [
    ("identical", ["VARCHAR", "BIGINT"], [["a", "1"]], [["a", "1"]], "ok"),
    ("reordered rows", ["VARCHAR", "BIGINT"],
     [["a", "1"], ["b", "2"]], [["b", "2"], ["a", "1"]], "ok (same rows"),
    ("a dropped row", ["VARCHAR", "BIGINT"],
     [["a", "1"], ["b", "2"]], [["a", "1"]], "ROWS"),
    ("trailing-blank padding", ["VARCHAR", "BIGINT"],
     [["MACHINERY", "1"]], [["MACHINERY ", "1"]], "PADDING"),
    ("a wrong value", ["VARCHAR", "BIGINT"], [["a", "1"]], [["a", "2"]], "DIFF"),
    # The types reach the whole-result level too: one cent in a DECIMAL column
    # is a DIFF, and the same cent in a DOUBLE column is ok.
    ("one cent in a DECIMAL column", ["DECIMAL(38,2)"],
     [["566012345678.34"]], [["566012345678.35"]], "DIFF"),
    ("the same cent in a DOUBLE column", ["DOUBLE"],
     [["566012345678.34"]], [["566012345678.35"]], "ok"),
    # And the refusals, which must never read as a pass.
    ("no types at all", None, [["a", "1"]], [["a", "1"]], "NO TYPES"),
    ("too few types for the rows", ["VARCHAR"], [["a", "1"]], [["a", "1"]], "TYPES 1 for 2"),
    # A single-column NULL: COPY writes a blank line and csv.reader reads it as
    # [], so the rows measure 0 columns. The width check must not fire, or a
    # correct answer is refused as the oracle's fault -- q17's avg_yearly at a
    # small scale factor (review of #537).
    ("a one-column NULL on both sides", ["DOUBLE"], [[]], [[]], "ok"),
    ("a one-column NULL the client filled in", ["DOUBLE"], [[]], [["5"]], "DIFF"),
    ("a NULL among several columns", ["VARCHAR", "DOUBLE"],
     [["a", ""]], [["a", ""]], "ok"),
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
    print("same() -- the oracle's TYPE decides, not the value's shape:")
    failures = 0
    for label, type_name, x, y, want in SAME_TESTS:
        got = same(x, y, column_kind(type_name))
        failures += _check("%s [%s]" % (label, type_name), got == want,
                           "accepted" if got else "rejected", width=72)
    return failures


def _check_types():
    print("\ncolumn_kind() -- how each column must be compared:")
    failures = 0
    for type_name, want in TYPE_TESTS:
        got = column_kind(type_name)
        failures += _check("%-18s -> %s" % (type_name, want), got == want, got)
    return failures


def _check_same_pair_both_ways():
    """The same two strings, judged differently by type. The whole point.

    If this ever passes with one verdict for both, the comparison has stopped
    being type-driven and is inferring from the values again.
    """
    print("\none pair, two verdicts, decided only by the column type:")
    failures = 0
    for x, y, label in (("56586554400730.17", "56586554400730.18", "an SF1000 sum one cent apart"),
                        ("234603658237.28", "234603658237.29", "a mid-size sum one cent apart")):
        as_dec = same(x, y, NUMERIC)
        as_dbl = same(x, y, TOLERANCE)
        failures += _check("%s: DECIMAL rejects, DOUBLE accepts" % label,
                           as_dec is False and as_dbl is True,
                           "DECIMAL=%s DOUBLE=%s" % (as_dec, as_dbl), width=72)
    return failures


def _check_compare(tmp):
    print("\ncompare() -- the verdict for a whole result:")
    failures = 0
    for i, (label, types, a, b, want) in enumerate(COMPARE_TESTS):
        oracle, got_path = tmp / f"o{i}.csv", tmp / f"g{i}.csv"
        for path, rows in ((oracle, a), (got_path, b)):
            with open(path, "w", newline="") as f:
                csv.writer(f).writerows(rows)
        verdict = compare(oracle, got_path, types)
        failures += _check(label, verdict.startswith(want), verdict[:48])
    # A client whose query failed at BIND writes no file at all.
    failures += _check("a missing client file is MISSING, never ok",
                       compare(tmp / "o0.csv", tmp / "absent.csv", ["VARCHAR", "BIGINT"])
                       .startswith("MISSING"), "MISSING")

    # read_types(): the file the oracle writes, and its absence.
    types_path = tmp / "types-q01.csv"
    with open(types_path, "w", newline="") as f:
        csv.writer(f).writerows([["DECIMAL(38,2)"], ["DOUBLE"], ["VARCHAR"]])
    failures += _check("read_types() reads the oracle's file in order",
                       read_types(types_path) == ["DECIMAL(38,2)", "DOUBLE", "VARCHAR"],
                       str(read_types(types_path)))
    failures += _check("a missing types file is None, so compare() refuses",
                       read_types(tmp / "types-absent.csv") is None, "None")
    with open(types_path, "w") as f:
        f.write("")
    failures += _check("an empty types file is None too",
                       read_types(types_path) is None, "None")
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


#: How long the fixture's child is given before the timeout fires. One
#: constant, because the first version of this fix changed the call and the
#: note but left the check's LABEL saying 2 s, so the self-test misreported
#: what it had run (roborev on c59b50b).
CHILD_TIMEOUT = 5


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
                       stderr=subprocess.STDOUT, timeout=CHILD_TIMEOUT)
        return _check(f"a {CHILD_TIMEOUT} s timeout on a 30 s child raises TimeoutExpired",
                      False, "it did NOT time out")
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
        # Guarded with .get, and the child is given CHILD_TIMEOUT rather than
        # the original 2 s. On a
        # loaded runner the child may not be scheduled and flushed inside the
        # window, leaving `exc.output` empty: parse_marked then returns {} and
        # a bare res["q02"] raised KeyError, so an intermittent CI failure
        # looked like a crash in the harness rather than a slow machine. Seen
        # once locally while a crapper run and a differential were competing
        # for the CPU.
        failures += _check("the queries that DID finish keep their timings",
                           res.get("q01", (None,))[0] == 0.5, str(res.get("q01")))
        hung = name_hung_query(res, f"session timed out after {CHILD_TIMEOUT}s")
        note = (res.get("q02") or (None, ""))[1] or ""
        failures += _check("the interrupted query is named as the timeout, not as `no Run Time`",
                           hung == "q02" and "timed out" in note,
                           f"{hung}: {note or 'NO OUTPUT CAPTURED (the child never ran)'}")
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


def _check_extracted():
    """The helpers the CC split made independently reachable.

    Each was buried inside a 15-30 branch command before, so none of these
    invariants had ever been checked on its own.
    """
    print("\nthe pieces the split exposed:")
    failures = 0

    # The rotation rule, documented since #505 and never tested: no client is
    # always first, and every rep is a permutation of the same clients.
    clients = ["A", "B", "Bscan", "C"]
    orders = [rotated(clients, rep) for rep in range(8)]
    failures += _check("every rep runs the same clients", all(sorted(o) == sorted(clients) for o in orders),
                       "4 clients x 8 reps")
    failures += _check("every client leads at least once in 4 reps",
                       {o[0] for o in orders[:4]} == set(clients),
                       ", ".join(o[0] for o in orders[:4]))
    failures += _check("the rotation repeats with the client count",
                       orders[0] == orders[4] and orders[1] == orders[5], "period 4")
    failures += _check("one client needs no rotation", rotated(["A"], 7) == ["A"], "['A']")

    # comparable(): the sum must cover identical work, which is #505 item 5.
    med = {("A", "q01"): (1.0, 1, ""), ("A", "q06"): (None, 0, "boom"),
           ("B", "q01"): (2.0, 1, ""), ("B", "q06"): (4.0, 1, "")}
    common, missing, totals = comparable(med, ["A", "B"], ["q01", "q06"])
    failures += _check("a query one client failed is excluded from the sum",
                       common == ["q01"] and missing == ["q06"], "common=%s" % common)
    failures += _check("the totals cover only the common queries",
                       totals == {"A": 1.0, "B": 2.0}, str(totals))

    # samples(): the first error is kept even when later reps succeeded, so a
    # flaky query is still visible in the report.
    rows = [{"client": "A", "query": "q01", "seconds": "", "error": "first"},
            {"client": "A", "query": "q01", "seconds": "2.0", "error": ""},
            {"client": "A", "query": "q01", "seconds": "", "error": "second"},
            {"client": "B", "query": "q01", "seconds": "9.0", "error": ""}]
    vals, err = samples(rows, "A", "q01")
    failures += _check("samples() takes only its own client and query",
                       vals == [2.0] and err == "first", "%s / %r" % (vals, err))

    # cell(): the (n=N) annotation, which was stored and never read until #518.
    failures += _check("a short cell is annotated with its rep count",
                       cell(1.5, 1, "", 3) == "1.500 (n=1)", cell(1.5, 1, "", 3))
    failures += _check("a full cell is not annotated",
                       cell(1.5, 3, "", 3) == "1.500", cell(1.5, 3, "", 3))
    failures += _check("a failed cell shows its error", cell(None, 0, "boom", 3) == "error: boom",
                       cell(None, 0, "boom", 3))
    return failures


#: (label, the password, must it be absent from the masked text)
#
# Issue #509 item 1. The DSN reaches almost all output embedded in an
# `ATTACH '...'` statement, so the form that matters is the one `esc()` wrote,
# not the one the operator set.
MASK_TESTS = [
    ("a plain password", "pw"),
    ("a password with a single quote", "pw'X"),
    ("a password with two", "a'b'c"),
    ("a password with a brace", "pw}X"),
    ("a password with a semicolon", "pw;Database=master"),
    ("a password that looks like SQL", "'); DROP DATABASE x; --"),
]

#: (label, the name, is it accepted)
#
# Issue #509 item 3: these reach the server inside ALTER / DROP / CREATE
# DATABASE as sa, and `esc()` does not touch `]`.
NAME_TESTS = [
    ("the generated default", "tpch_sf1_columnstore", True),
    ("with a string schema suffix", "tpch_sf10_columnstore_utf8", True),
    ("a bare collation", "Latin1_General_100_BIN2_UTF8", True),
    ("a closing bracket and a statement", "x] ; CREATE LOGIN hax WITH PASSWORD='p'; --", False),
    ("a collation with a trailing DROP", "Latin1_General_100_BIN2_UTF8; DROP DATABASE [master]", False),
    ("a quote", "x'y", False),
    ("a space", "my db", False),
    ("empty", "", False),
    ("a dot", "db.name", False),
]


def _parse_connection_string(cs):
    """Brace-aware split, as src/mssql_storage.cpp reads one.

    The point of the fixture is what the PARSER sees: a `;` inside the braces
    must not become another key, and counting `Database=` in the text cannot
    tell you that.
    """
    out, i, n = {}, 0, len(cs)
    while i < n:
        j = cs.index("=", i)
        key = cs[i:j].strip()
        i = j + 1
        if i < n and cs[i] == "{":
            i += 1
            value = []
            while i < n:
                if cs[i] == "}":
                    if i + 1 < n and cs[i + 1] == "}":
                        value.append("}")
                        i += 2
                        continue
                    i += 1
                    break
                value.append(cs[i])
                i += 1
            out[key] = "".join(value)
            while i < n and cs[i] != ";":
                i += 1
        else:
            j = cs.find(";", i)
            j = n if j < 0 else j
            out[key] = cs[i:j]
            i = j
        i += 1
    return out


def _check_credentials():
    """Issue #509: the password never reaches output, and no name injects."""
    print("\nthe password is masked in the form the text carries (#509.1):")
    failures = 0
    for label, password in MASK_TESTS:
        cfg = {"host": "h", "port": "1433", "user": "sa", "password": password}
        # Exactly how the harness emits it: the DSN inside an ATTACH literal.
        text = "ATTACH '" + esc(dsn(cfg, "db", True)) + "' AS t;"
        masked = mask(text, cfg)
        leaked = [form for form in (password, esc(password), quote_dsn_value(password))
                  if form and form in masked]
        failures += _check(label, not leaked, "masked" if not leaked else "LEAKED %r" % leaked[0])

    print("\nthe DSN keeps one Database key whatever the password (#509.2):")
    cfg = {"host": "h", "port": "1433", "user": "sa", "password": "pw;Database=master"}
    parsed = _parse_connection_string(dsn(cfg, "tpch_sf1", True))
    failures += _check("a `;` in the password adds no key",
                       parsed.get("Database") == "tpch_sf1", "Database=%r" % parsed.get("Database"))
    failures += _check("and the password arrives whole",
                       parsed.get("Password") == "pw;Database=master", "intact")
    braced = {"host": "h", "port": "1433", "user": "sa", "password": "pw}X"}
    parsed = _parse_connection_string(dsn(braced, "db", True))
    failures += _check("a `}` in the password is doubled and read back",
                       parsed.get("Password") == "pw}X", "%r" % parsed.get("Password"))

    print("\na database or collation name is a bare name or refused (#509.3):")
    for label, name, want in NAME_TESTS:
        try:
            safe_name(name, "--db")
            got = True
        except SystemExit:
            got = False
        failures += _check(label, got == want, "accepted" if got else "refused")
    return failures


#: (label, the schema flags, the database name they must derive)
#
# Issue #507 item 1. The last three lines are the decisive ones: before the
# fix the UTF-8 CI and UTF-8 BIN2 schemas both derived
# `tpch_sf10_columnstore_utf8`, so loading one DROPPED the other, and the
# published BIN2 row can only have come from a hand-passed `--db` that no
# documented command produces.
NAME_DERIVATION_TESTS = [
    ("the default", dict(sf=1, layout="columnstore", strings="spec", collation=None),
     "tpch_sf1_columnstore"),
    ("the rowstore layout", dict(sf=1, layout="rowstore", strings="spec", collation=None),
     "tpch_sf1_rowstore"),
    ("a fractional scale factor", dict(sf=0.1, layout="columnstore", strings="spec", collation=None),
     "tpch_sf0_1_columnstore"),
    ("the nvarchar schema", dict(sf=10, layout="columnstore", strings="nvarchar", collation=None),
     "tpch_sf10_columnstore_nvarchar"),
    ("utf8, whose collation is implied", dict(sf=10, layout="columnstore", strings="utf8", collation=None),
     "tpch_sf10_columnstore_utf8"),
    ("utf8 with that same collation spelled out",
     dict(sf=10, layout="columnstore", strings="utf8", collation=UTF8_COLLATION),
     "tpch_sf10_columnstore_utf8"),
    ("utf8 BIN2 -- the published row",
     dict(sf=10, layout="columnstore", strings="utf8", collation="Latin1_General_100_BIN2_UTF8"),
     "tpch_sf10_columnstore_utf8_latin1general100bin2utf8"),
    ("a collation on the spec schema",
     dict(sf=1, layout="columnstore", strings="spec", collation="Latin1_General_100_BIN2_UTF8"),
     "tpch_sf1_columnstore_latin1general100bin2utf8"),
]


def _check_names():
    print("\nthe database name carries every schema choice (#507.1):")
    failures, names = 0, []
    for label, flags, want in NAME_DERIVATION_TESTS:
        args = argparse.Namespace(db=None, **flags)
        resolve_schema(args)
        failures += _check(label, args.db == want, args.db, width=72)
        names.append(args.db)

    # The collision itself, as such: two schemas that differ only in their
    # collation must not share a database.
    ci = resolve_schema(argparse.Namespace(db=None, sf=10, layout="columnstore", strings="utf8",
                                           collation=None))
    bin2 = resolve_schema(argparse.Namespace(db=None, sf=10, layout="columnstore", strings="utf8",
                                             collation="Latin1_General_100_BIN2_UTF8"))
    failures += _check("the two UTF-8 schemas of RESULTS-local.md get two databases",
                       ci.db != bin2.db, f"{ci.db} / {bin2.db}", width=72)
    failures += _check("an explicit --db is still taken as given",
                       resolve_schema(argparse.Namespace(db="mine", sf=1, layout="columnstore",
                                                         strings="utf8", collation=None)).db == "mine",
                       "mine", width=72)
    failures += _check("every derived name is a bare name the server accepts",
                       all(SAFE_NAME.fullmatch(n) for n in names), f"{len(names)} names", width=72)
    return failures


def _check_schema_sql():
    """The statements the server is sent: items 3 and 5, and two of item 6."""
    print("\nthe statements the server is sent (#507.3, #507.5, #507.6):")
    failures = 0
    cs = argparse.Namespace(sf=1, layout="columnstore", strings="spec", collation=None,
                            db="tpch_sf1_columnstore", encrypt=True)
    rs = argparse.Namespace(sf=1, layout="rowstore", strings="spec", collation=None,
                            db="tpch_sf1_rowstore", encrypt=True)

    create_cs = " ".join(create_table_statements(cs))
    keys = primary_key_statements(cs)
    failures += _check("a columnstore table is created without its key",
                       "PRIMARY KEY" not in create_cs, "no PRIMARY KEY in the CREATE")
    failures += _check("every columnstore table gets TPC-H's key after the load",
                       len(keys) == len(TABLES)
                       and all(f"ALTER TABLE dbo.{tb} " in keys[i] for i, tb in enumerate(TABLES))
                       and all(f"({PRIMARY_KEYS[tb]})" in keys[i] for i, tb in enumerate(TABLES)),
                       f"{len(keys)} ALTER TABLE statement(s)")
    failures += _check("and it is NONCLUSTERED, beside the columnstore index",
                       all("PRIMARY KEY NONCLUSTERED" in s for s in keys), "NONCLUSTERED")
    create_rs = " ".join(create_table_statements(rs))
    failures += _check("a rowstore table keeps its clustered key in the CREATE",
                       create_rs.count("PRIMARY KEY CLUSTERED") == len(TABLES), "8 clustered keys")
    failures += _check("and takes no second key after the load", primary_key_statements(rs) == [], "none")

    cfg = {"host": "h", "port": "1433", "user": "sa", "password": "p"}
    recreate = " ".join(recreate_database_statements(cfg, cs))
    failures += _check("the DROP sits in a TRY whose CATCH restores MULTI_USER",
                       "BEGIN TRY" in recreate and "SET MULTI_USER" in recreate
                       and recreate.index("DROP DATABASE") < recreate.index("SET MULTI_USER"),
                       "TRY / CATCH")
    failures += _check("and re-raises, so a failed DROP is not swallowed",
                       "THROW" in recreate, "THROW")
    failures += _check("the name travels through QUOTENAME, never bracketed by hand",
                       "QUOTENAME(@n)" in recreate and "[tpch" not in recreate, "QUOTENAME(@n)")
    batch = drop_batch("SELECT name FROM sys.databases WHERE name LIKE N'tpch[_]sf%'")
    failures += _check("drop puts each database back and goes on to the next",
                       "BEGIN CATCH" in batch and "SET MULTI_USER" in batch
                       and batch.index("SET MULTI_USER") < batch.rindex("FETCH NEXT"),
                       "the loop continues")
    failures += _check("and counts what it could not drop", "@failed" in batch, "@failed")

    counts = row_count_statement()
    failures += _check("the eight row counts are one statement the server answers",
                       counts.count("COUNT_BIG") == len(TABLES)
                       and counts.startswith("SELECT * FROM mssql_scan"), "one mssql_scan")
    quoted = " ".join(load_statements(pathlib.Path("/Volumes/Team's data/sf1")))
    failures += _check("a data path holding a quote is escaped, not a parse error",
                       "Team''s data" in quoted, "Team''s data")
    return failures


def _write_rows(path, rows):
    with open(path, "w", newline="") as fh:
        csv.writer(fh).writerows(rows)


def _check_oracle(tmp):
    """Item 2: the oracle is two engines that must agree, not one client's engine."""
    print("\nthe oracle belongs to no client (#507.2):")
    out = tmp / "verify-oracle"
    out.mkdir(parents=True, exist_ok=True)
    failures = _check("it is computed on more than one engine", len(set(ORACLES)) > 1,
                      " and ".join(ORACLES))
    failures += _check("so client A is not graded by its own engine alone",
                       set(ORACLES) != {"A"}, ", ".join(ORACLES))

    def put(which, q, rows, types):
        _write_rows(out / f"oracle-{which}-{q}.csv", rows)
        _write_rows(out / f"types-{which}-{q}.csv", [[name] for name in types])

    # q01 the engines agree on. q02 they are a cent apart in a DECIMAL column,
    # which in that column is a wrong answer -- so neither client may be
    # graded on it. q03 they disagree about the TYPE, which is what decides
    # how every client's value is judged. q04 one engine answered not at all.
    put("A", "q01", [["a", "1.00"]], ["VARCHAR", "DECIMAL(38,2)"])
    put("B", "q01", [["a", "1.00"]], ["VARCHAR", "DECIMAL(38,2)"])
    put("A", "q02", [["566012345678.34"]], ["DECIMAL(38,2)"])
    put("B", "q02", [["566012345678.35"]], ["DECIMAL(38,2)"])
    put("A", "q03", [["1.00"]], ["DECIMAL(38,2)"])
    put("B", "q03", [["1.00"]], ["DOUBLE"])
    put("A", "q04", [["1.00"]], ["DECIMAL(38,2)"])

    queries = ["q01", "q02", "q03", "q04"]
    agreed, disputed = oracle_agreement(out, queries)
    failures += _check("a query both engines answer alike is the one that is graded",
                       list(agreed) == ["q01"], "agreed on " + (", ".join(agreed) or "nothing"))
    failures += _check("a cent between the engines is a disagreement, not a client's fault",
                       "q02" in disputed, disputed.get("q02", "ACCEPTED")[:44])
    failures += _check("so is disagreeing about the column type",
                       "q03" in disputed, disputed.get("q03", "ACCEPTED")[:44])
    failures += _check("so is an engine that produced no answer",
                       "q04" in disputed, disputed.get("q04", "ACCEPTED")[:44])

    # And the verdicts: a disputed query is counted once, whatever the client
    # answered, and the client is not graded on it.
    _write_rows(out / "B-q01.csv", [["a", "1.00"]])
    _write_rows(out / "B-q02.csv", [["566012345678.34"]])
    text = io.StringIO()
    with contextlib.redirect_stdout(text):
        bad = print_verdicts(out, ["B"], queries, {}, agreed, disputed)
    lines = text.getvalue().splitlines()
    failures += _check("each disagreement is counted once, not once per client",
                       bad == len(disputed), f"{bad} of {len(queries)} queries")
    failures += _check("and the client's own answer to it is not judged",
                       sum(1 for l in lines if "not graded" in l) == len(disputed),
                       "3 not graded")
    failures += _check("the query both engines agreed on is still judged",
                       any(l.startswith("B") and "q01" in l and l.endswith("ok") for l in lines),
                       "B q01 ok")
    return failures


def _check_run_files(tmp):
    """Item 4: re-running one client must not destroy the other clients' rows."""
    print("\nre-running one client keeps the others' rows (#507.4):")
    path = tmp / "timings-item4.csv"
    rows = [{"run": "sf10", "db": "d", "client": c, "rep": 1, "query": q, "seconds": "1.0", "error": ""}
            for c in ("A", "B", "Bscan", "C") for q in ("q01", "q06")]
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    kept = preserved_rows(path, ["B"])
    held = sorted({r["client"] for r in kept})
    failures = _check("the clients this run does not measure keep their rows",
                      held == ["A", "Bscan", "C"], f"{len(kept)} row(s) of {', '.join(held)}")
    failures += _check("Bscan is not mistaken for B",
                       sum(1 for r in kept if r["client"] == "Bscan") == 2, "2 Bscan rows")
    failures += _check("the re-measured client's rows are replaced, not doubled",
                       all(r["client"] != "B" for r in kept), "no B rows kept")
    failures += _check("a four-client re-run keeps nothing, having measured it all",
                       preserved_rows(path, ["A", "B", "Bscan", "C"]) == [], "[]")
    failures += _check("and a first run starts from nothing",
                       preserved_rows(tmp / "absent.csv", ["B"]) == [], "[]")
    return failures


def _check_flags(tmp):
    """Item 6: the flags that used to fail with a bare traceback."""
    print("\nevery flag answers at the flag (#507.6):")
    failures = 0

    def sel(clients, queries):
        try:
            return selected(argparse.Namespace(clients=clients, queries=queries))
        except SystemExit as exc:
            return exc

    got = sel("A,B", None)
    failures += _check("no --queries means all 22",
                       isinstance(got, tuple) and got[1] == list(QUERIES), f"{len(QUERIES)} queries")
    got = sel("A,B", "q01,")
    failures += _check("a trailing comma in --queries is not an empty query name",
                       isinstance(got, tuple) and got[1] == ["q01"], str(got if isinstance(got, SystemExit) else got[1]))
    failures += _check("--queries q01,qZZ names the one that does not exist",
                       isinstance(sel("A", "q01,qZZ"), SystemExit), "refused")
    failures += _check("--clients , is refused rather than dividing by zero",
                       isinstance(sel(",", None), SystemExit), "refused")
    failures += _check("--clients A,Z names the one that does not exist",
                       isinstance(sel("A,Z", None), SystemExit), "refused")

    try:
        timings_path(argparse.Namespace(timings=None, run_id=None, out=str(tmp)))
        why = "ACCEPTED"
    except SystemExit as exc:
        why = str(exc)
    except Exception as exc:
        # A bare `TypeError: argument should be a str...` from inside the
        # command WAS the defect, so it must read as a failure here rather
        # than take the whole self-test down with it.
        why = f"{type(exc).__name__}: {exc}"
    failures += _check("report without --timings says which flag it needs",
                       "--timings" in why, why[:44])
    failures += _check("report finds the run's own file from --run-id",
                       timings_path(argparse.Namespace(timings=None, run_id="sf10", out=str(tmp)))
                       == tmp / "sf10" / "timings.csv", "out/<run-id>/timings.csv")

    args = argparse.Namespace(sf=1, db="d", encrypt=True, warmup=2, timeout=10)
    script = timed_script("B", {"host": "h", "port": "1433", "user": "sa", "password": "p"},
                          args, ["q01", "q02"])
    markers = [f"{tag} {q}" for line in script for tag, q in MARKER.findall(line)]
    failures += _check("each query is warmed immediately before it is timed",
                       markers == ["W q01", "W q01", "T q01", "W q02", "W q02", "T q02", "END end"],
                       ", ".join(markers))
    failures += _check("the renderer is pinned, since it runs inside the timer",
                       ".mode csv" in script, ".mode csv")

    failures += _check("one client's plans cannot overwrite another's",
                       explain_path(tmp, "B", "tpch_sf10_columnstore")
                       != explain_path(tmp, "Bscan", "tpch_sf10_columnstore"), "one file per client")
    failures += _check("and one schema's cannot overwrite another's",
                       explain_path(tmp, "B", "tpch_sf10_columnstore")
                       != explain_path(tmp, "B", "tpch_sf10_columnstore_utf8"), "one file per database")
    return failures


def cmd_selftest(args):
    failures = _check_same()
    failures += _check_types()
    failures += _check_same_pair_both_ways()
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        failures += _check_compare(tmp)
        failures += _check_parse()
        failures += _check_timeout_path()
        failures += _check_preamble()
        failures += _check_report(tmp)
        failures += _check_oracle(tmp)
        failures += _check_run_files(tmp)
        failures += _check_flags(tmp)
    failures += _check_extracted()
    failures += _check_credentials()
    failures += _check_names()
    failures += _check_schema_sql()
    print(f"\n{'FAIL' if failures else 'PASS'}: self-test, {failures} failure(s)")
    return 1 if failures else 0



# --------------------------------------------------------------------------- explain

def explain_path(out, client, db):
    """Where one client's plans are kept.

    Issue #507 item 6: the file was `explain-B.txt`, with no scale-factor,
    layout or string-schema component -- so every sweep overwrote the last, and
    `measured/.../sf10-utf8-bin2/explain-B.txt` is only distinguishable because
    someone filed it by hand. `--db` names all four choices since item 1.
    """
    return out / f"explain-{client}-{db}.txt"


def explain_client(cfg, args, out, client, queries):
    """One client's plans: the file, and the per-query verdict on stdout."""
    s = session_head(client, cfg, args) + ["PRAGMA explain_output = 'physical_only';"]
    for q in queries:
        s += [f".print @@X {q}@@", "EXPLAIN " + query_text(client, q, args.sf)]
    rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(s) + "\n", cfg)
    explain_path(out, client, args.db).write_text(text)
    parts = re.split(r"@@X (q\d\d)@@", text)
    plans = dict(zip(parts[1::2], parts[2::2]))
    print(f"\n{client} ({CLIENTS[client]['label']}) -- {explain_path(out, client, args.db)}")
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
    return rc


def cmd_explain(args):
    """Which queries each client sends to the server, from its own plan.

    Issue #507 item 6: `--clients` was parsed and discarded here, and the
    command always explained B. A client whose plans nobody can see is a client
    whose pushdown nobody can check -- Bscan's `remote_pushdown false` is an
    ATTACH option, and whether it holds is exactly the kind of thing this
    command answers.
    """
    cfg = server()
    clients, queries = selected(args)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for client in clients:
        explain_client(cfg, args, out, client, queries)


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


def trace_script(client, cfg, args, queries, dmv):
    """Per query: run it once warm, FREEPROCCACHE, run it timed, read the DMV."""
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
    return s + [".print @@Q end@@"]


def trace_statements(body):
    """The DMV rows of one query's section: 6 CSV fields, the first a count."""
    return [row for row in csv.reader(body.splitlines()) if len(row) == 6 and row[0].isdigit()]


def trace_query_section(client, q, body):
    """-> (summary tuple, markdown lines) for one traced query."""
    timing = RUN_TIME.search(body)
    wall = float(timing.group(1)) if timing else None
    rows = trace_statements(body)
    n_rows = sum(int(x[1]) for x in rows)
    el = sum(float(x[2]) for x in rows)
    cpu = sum(float(x[3]) for x in rows)
    md = [f"## {q}: client {wall if wall is not None else 'ERROR'} s, {len(rows)} statement(s), "
          f"{n_rows} rows to the client, server {el:.0f} ms elapsed / {cpu:.0f} ms CPU", ""]
    md += [f"- {x[0]} exec, {x[1]} rows, {x[2]} ms elapsed, {x[3]} ms CPU, {x[4]} reads: `{x[5][:600]}`"
           for x in rows]
    md.append("")
    return (client, q, wall, len(rows), n_rows, el, cpu), md


def trace_report(client, args, text):
    """-> (summary rows, markdown lines) for one client's whole session."""
    parts = re.split(r"@@Q (\S+)@@", text)
    summary, md = [], [f"# {client}: {CLIENTS[client]['label']} -- {args.db}", ""]
    for q, body in zip(parts[1::2], parts[2::2]):
        if q == "end":
            continue
        row, lines = trace_query_section(client, q, body)
        summary.append(row)
        md += lines
    return summary, md


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
        script = trace_script(client, cfg, args, queries, dmv)
        rc, text = run_cli(binary(CLIENTS[client]["bin"]), "\n".join(script) + "\n", cfg,
                           timeout=args.timeout)
        (out / f"trace-{client}.log").write_text(text)
        rows, md = trace_report(client, args, text)
        summary += rows
        (out / f"trace-{client}.md").write_text("\n".join(md) + "\n")
    print("client query  wall_s  stmts  rows_to_client  server_elapsed_ms  server_cpu_ms")
    for c, q, wall, n, rows, el, cpu in summary:
        print(f"{c:6s} {q}  {wall if wall is not None else 'ERR':>7}  {n:5d}  {rows:14d}  {el:17.0f}  {cpu:13.0f}")
    print(f"details: {out}/trace-<client>.md")


# --------------------------------------------------------------------------- drop

#: `drop` reads the leftover count out of its own output: the CLI does not
#: bail on error for piped stdin, so a rendered cell is the only channel, and
#: a marker survives whatever the renderer does to the box around it.
LEFT_MARKER = re.compile(r"@@LEFT (\d+)@@")


def drop_batch(target):
    """Drop every database `target` names, each in its own TRY/CATCH.

    Issue #507 item 5. The ALTER ... SINGLE_USER and the DROP were one string
    in one EXEC in a cursor loop, so a DROP that failed -- 5030 / 3702, any
    session connecting in the window -- aborted the whole batch: that database
    was left in SINGLE_USER with no connection holding it (unusable until
    someone issued SET MULTI_USER by hand), AND every remaining tpch_sf*
    database was abandoned. Now a failure puts its own database back, says
    why, and the loop goes on to the next.
    """
    return ("DECLARE @n sysname, @sql nvarchar(max), @failed int = 0; "
            f"DECLARE c CURSOR LOCAL FAST_FORWARD FOR {target}; OPEN c; FETCH NEXT FROM c INTO @n; "
            "WHILE @@FETCH_STATUS = 0 BEGIN "
            "BEGIN TRY "
            "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET SINGLE_USER WITH ROLLBACK IMMEDIATE; "
            "DROP DATABASE ' + QUOTENAME(@n); EXEC (@sql); "
            "END TRY BEGIN CATCH "
            "SET @failed = @failed + 1; "
            "IF DB_ID(@n) IS NOT NULL BEGIN "
            "SET @sql = N'ALTER DATABASE ' + QUOTENAME(@n) + N' SET MULTI_USER'; EXEC (@sql); END; "
            "PRINT N'could not drop ' + QUOTENAME(@n) + N': ' + ERROR_MESSAGE(); "
            "END CATCH; "
            "FETCH NEXT FROM c INTO @n; END; CLOSE c; DEALLOCATE c; "
            "IF @failed > 0 PRINT N'left ' + CAST(@failed AS nvarchar(10)) + N' database(s) in place';")


def cmd_drop(args):
    """Drop the benchmark database: --db, or with --all every tpch_sf* database on the server."""
    cfg = server()
    s = ["LOAD mssql;", "SET mssql_query_timeout = 0;",
         f"ATTACH '{esc(dsn(cfg, 'master', args.encrypt))}' AS m (TYPE mssql);"]
    if args.all:
        target = "SELECT name FROM sys.databases WHERE name LIKE N'tpch[_]sf%'"
        left = "SELECT count(*) AS n FROM sys.databases WHERE name LIKE N'tpch[_]sf%'"
        what = "every tpch_sf* database"
    else:
        db = safe_name(args.db, "--db")
        target = f"SELECT N'{db}' AS name WHERE DB_ID(N'{db}') IS NOT NULL"
        left = f"SELECT count(*) AS n FROM sys.databases WHERE name = N'{db}'"
        what = db
    s.append(f"SELECT mssql_exec('m', '{esc(drop_batch(target))}');")
    # What is left of the TARGET, in a marker. It used to be `count(*)` over
    # every tpch_sf* database whatever --db said, printed and never read, so
    # `drop` exited 0 with the database still there (issue #507 item 5).
    s.append("SELECT '@@LEFT ' || CAST(n AS VARCHAR) || '@@' AS still_there "
             f"FROM mssql_scan('m', '{esc(left)}');")
    rc, text = run_cli(binary("B"), "\n".join(s) + "\n", cfg)
    print(text)
    if rc != 0:
        sys.exit(rc)
    found = LEFT_MARKER.search(text)
    if found is None:
        sys.exit("error: could not read how many databases are left (see the output above)")
    if int(found.group(1)):
        sys.exit(f"FAIL: {found.group(1)} database(s) not dropped; the PRINT lines above say why")
    print(f"dropped: {what}")
    sys.exit(0)


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf", type=float, default=1)
    ap.add_argument("--data-dir", default=str(HERE / "data"))
    ap.add_argument("--db", default=None, help="SQL Server database (default tpch_sf<sf>_<layout>)")
    ap.add_argument("--layout", choices=["columnstore", "rowstore"], default="columnstore")
    ap.add_argument("--strings", choices=["spec", "nvarchar", "utf8"], default="spec",
                    help="string column types: TPC-H's CHAR/VARCHAR, NVARCHAR(n), or VARCHAR(n) in a UTF-8 collation")
    # Two flags rather than argparse.BooleanOptionalAction, which is Python
    # 3.9 and killed `--help` itself on a 3.8 host (issue #507 item 6). The
    # command-line surface is the same: --encrypt / --no-encrypt.
    ap.add_argument("--encrypt", dest="encrypt", action="store_true", default=True,
                    help="TLS on the connection (the default)")
    ap.add_argument("--no-encrypt", dest="encrypt", action="store_false",
                    help="turn TLS off")
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
    resolve_schema(args)
    # `or 0`: only run and verify have a status to report, and verify exits on
    # its own. Every other command returns None, which is success.
    return {"gen": cmd_gen, "load": cmd_load, "verify": cmd_verify, "explain": cmd_explain,
            "trace": cmd_trace, "run": cmd_run, "report": cmd_report,
            "drop": cmd_drop, "selftest": cmd_selftest}[args.command](args) or 0


if __name__ == "__main__":
    sys.exit(main())

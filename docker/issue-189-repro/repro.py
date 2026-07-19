#!/usr/bin/env python3
"""Issue #189 reproduction — no connection lifetime a shared temp table can anchor to.

Populates Repro189.dbo.Source (1000 rows), then exercises four failure modes:

  R1  ## global temp table created via mssql_exec is dropped as soon as the
      creating pooled connection is reused (RESET_CONNECTION on release).
  R2  # session temp table is unusable across autocommit statements.
  R3  A pinned transaction keeps the session alive, but COMMIT resets and
      unpins the connection — the ## table does not survive it.
  R4  Creating a ## table inside an open transaction holds Sch-M locks:
      other pooled connections block until COMMIT/ROLLBACK.

Exit code 0 = all four failure modes reproduced (the limitation is present).
"""

import os
import sys
import threading
import time

import duckdb

HOST = os.environ.get("MSSQL_HOST", "sqlserver")
PORT = os.environ.get("MSSQL_PORT", "1433")
SA_PASSWORD = os.environ.get("MSSQL_SA_PASSWORD", "TestPassword1")
LOCAL_EXT = os.environ.get("MSSQL_EXTENSION_PATH", "")

DB = "Repro189"
ROWS = 1000

results = []


def record(rid, scenario, reproduced, detail):
    results.append((rid, scenario, reproduced, detail))
    status = "REPRODUCED" if reproduced else "NOT reproduced"
    print(f"[{rid}] {status}: {scenario}\n     {detail}", flush=True)


def q(tsql):
    """Escape a T-SQL string for embedding in a DuckDB single-quoted literal.

    mssql_exec/mssql_scan require constant arguments (no prepared parameters),
    so the T-SQL is inlined.
    """
    return tsql.replace("'", "''")


def dsn(database):
    return (
        f"Server={HOST},{PORT};Database={database};User Id=sa;"
        f"Password={SA_PASSWORD};TrustServerCertificate=true"
    )


def connect():
    cfg = {"allow_unsigned_extensions": "true"} if LOCAL_EXT else {}
    con = duckdb.connect(config=cfg)
    if LOCAL_EXT:
        con.execute(f"LOAD '{LOCAL_EXT}'")
    else:
        con.execute("INSTALL mssql FROM community")
        con.execute("LOAD mssql")
    return con


def mexec(cur, tsql):
    return cur.execute(f"SELECT mssql_exec('mss', '{q(tsql)}')").fetchall()[0][0]


def mscan1(cur, tsql):
    """Run a single-value query via mssql_scan and return the value."""
    return cur.execute(f"SELECT * FROM mssql_scan('mss', '{q(tsql)}')").fetchall()[0][0]


def poll_gone(cur, table_name, attempts=10):
    """Return the number of pool statements it took for the ## table to vanish
    from tempdb (each poll statement is itself a pooled statement and so churns
    the pool), or None if it survived all attempts."""
    for i in range(1, attempts + 1):
        c = mscan1(cur, f"SELECT COUNT(*) AS c FROM tempdb.sys.tables WHERE name = '{table_name}'")
        if c == 0:
            return i
        time.sleep(0.2)
    return None


def wait_and_init(con):
    # Spec 047 eager ATTACH validation makes ATTACH itself the readiness probe.
    last_err = None
    for _ in range(60):
        try:
            con.execute(f"ATTACH '{dsn('master')}' AS init (TYPE mssql)")
            break
        except Exception as e:  # noqa: BLE001 - retry any startup error
            last_err = e
            time.sleep(2)
    else:
        raise RuntimeError(f"SQL Server never became reachable: {last_err}")

    def iexec(tsql):
        return con.execute(f"SELECT mssql_exec('init', '{q(tsql)}')").fetchall()[0][0]

    iexec(f"IF DB_ID('{DB}') IS NOT NULL BEGIN ALTER DATABASE {DB} SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE {DB}; END")
    iexec(f"CREATE DATABASE {DB}")
    iexec(
        f"SELECT TOP ({ROWS}) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS id, "
        f"CONCAT('payload-', NEWID()) AS payload "
        f"INTO {DB}.dbo.Source FROM sys.all_objects a CROSS JOIN sys.all_objects b"
    )
    con.execute("DETACH init")


def main():
    con = connect()
    wait_and_init(con)
    con.execute(f"ATTACH '{dsn(DB)}' AS mss (TYPE mssql)")

    ext_version = con.execute("SELECT mssql_version()").fetchall()[0][0]
    server_version = mscan1(con, "SELECT CAST(SERVERPROPERTY('ProductVersion') AS NVARCHAR(64)) AS v")
    print(f"duckdb={duckdb.__version__} mssql-extension={ext_version} "
          f"sqlserver={server_version} source={'local build' if LOCAL_EXT else 'community'}\n", flush=True)

    # R0 sanity: catalog scan sees the source table.
    n = con.execute("SELECT COUNT(*) FROM mss.dbo.Source").fetchall()[0][0]
    assert n == ROWS, f"sanity: expected {ROWS} source rows, got {n}"

    # ---- R1: ## global temp table dropped by pooled-connection reset --------
    rows = mexec(con, "SELECT * INTO ##repro189_shared FROM dbo.Source")
    gone_after = poll_gone(con, "##repro189_shared")
    record(
        "R1", "## global temp table dropped once the creating pooled connection is reused",
        rows == ROWS and gone_after is not None,
        f"SELECT INTO reported {rows} rows inserted (creation succeeded), "
        f"but the table was gone from tempdb.sys.tables after {gone_after} subsequent pool statement(s)"
        if gone_after is not None else
        f"table still present after 10 statements (rows={rows})",
    )

    # ---- R2: # session temp table unusable across autocommit statements ----
    rows = mexec(con, "SELECT * INTO #repro189_local FROM dbo.Source")
    try:
        c = mscan1(con, "SELECT COUNT(*) AS c FROM #repro189_local")
        record("R2", "# session temp table unusable across autocommit statements",
               False, f"unexpectedly still visible, count={c}")
    except Exception as e:  # noqa: BLE001 - the error IS the expected result
        first_line = str(e).splitlines()[0]
        record("R2", "# session temp table unusable across autocommit statements",
               "Invalid object name" in str(e),
               f"created with {rows} rows, next statement fails: {first_line}")

    # ---- R3: pinned transaction cannot anchor the table beyond COMMIT ------
    con.execute("BEGIN")
    rows = mexec(con, "SELECT * INTO ##repro189_tx FROM dbo.Source")
    inside = mscan1(con, "SELECT COUNT(*) AS c FROM ##repro189_tx")  # same pinned connection
    con.execute("COMMIT")
    gone_after = poll_gone(con, "##repro189_tx")
    record(
        "R3", "pinned transaction: ## table usable inside, dropped after COMMIT (reset + unpin)",
        inside == ROWS and gone_after is not None,
        f"count inside transaction={inside}; after COMMIT gone within {gone_after} pool statement(s)"
        if gone_after is not None else
        f"count inside transaction={inside}; table still present after COMMIT",
    )

    # ---- R4: ## created in an open transaction blocks other connections ----
    # A timer thread COMMITs the creating transaction after 3s; the reader on a
    # second pooled connection must block on the Sch-M lock until exactly then.
    # (Don't try SET LOCK_TIMEOUT in the reader's batch: SQL Server compiles
    # the whole batch before executing statement 1, so the reader blocks on
    # schema stability during compilation — before the SET runs (session
    # default: infinite). It then hangs until the extension's own 30s
    # COLMETADATA timeout closes the connection.)
    cur1 = con.cursor()
    cur2 = con.cursor()
    cur1.execute("BEGIN")
    mexec(cur1, "SELECT * INTO ##repro189_locked FROM dbo.Source")  # txn stays open
    release = threading.Timer(3.0, lambda: cur1.execute("COMMIT"))
    release.start()
    start = time.monotonic()
    try:
        c = mscan1(cur2, "SELECT COUNT(*) AS c FROM ##repro189_locked")
        elapsed = time.monotonic() - start
        record("R4", "## created in open transaction holds Sch-M locks, blocking other connections",
               c == ROWS and elapsed >= 2.0,
               f"second pooled connection blocked {elapsed:.1f}s until the creating "
               f"transaction committed (timer fired at 3.0s), then read count={c}")
    except Exception as e:  # noqa: BLE001 - report unexpected failure shape
        elapsed = time.monotonic() - start
        first_line = str(e).splitlines()[0]
        record("R4", "## created in open transaction holds Sch-M locks, blocking other connections",
               False, f"after {elapsed:.1f}s: {first_line}")
    finally:
        release.join()

    print(flush=True)
    reproduced = sum(1 for r in results if r[2])
    print(f"{reproduced}/{len(results)} failure modes reproduced", flush=True)
    sys.exit(0 if reproduced == len(results) else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Spec 092 D10: the gate on transcoding single-byte code pages on the client.

Interleaved A/B in one DuckDB session (the house rule): two ATTACHes of one DSN, A with
client_transcode false (the server's CAST, UTF-16 on the wire) and B with it on (bytes on
the wire, the client's kernel). Queries alternate A, B, A, B; the median of the rounds.

    MSSQL_TEST_DSN='Server=...;Database=TestDB;...' python3 test/bench/bench_code_page_gate.py
    ROUNDS=5 ROWS=200000 python3 test/bench/bench_code_page_gate.py

The gate: every workload but W3 meets B <= A x 1.02 in wall time. Server CPU is reported beside
it, from sys.dm_exec_query_stats deltas, and does not gate. W3 is a double-byte raw scan: A is
mssql_scan of CAST(c AS nvarchar(64)), B of c; it is reported, not gated.

Client numbers come from MSSQL_COUNTERS=1, never MSSQL_DEBUG, whose logging runs inside the
phases it times. Wall time is the CLI's .timer. Prints a Markdown table to stdout.
"""
import os
import pathlib
import random
import re
import statistics
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
ROWS = int(os.environ.get("ROWS", "1000000"))
ROUNDS = int(os.environ.get("ROUNDS", "3"))
DISTINCT = 1000
PREFIX = "cpg_"

LATIN_ASCII = "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ"
LATIN_HIGH = "éèàüöñçßåøæ"
CYRILLIC = "абвгдежзийклмнопрстуфхцчшщъыьэюяАБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЭЮЯ "
TYPOGRAPHY = "—«»…“”"
JAPANESE = "あいうえおかきくけこさしすせそたちつてとカタカナ漢字仮名交換情報処理"


def latin(rng, n, high):
    return "".join(rng.choice(LATIN_HIGH) if rng.random() < high else rng.choice(LATIN_ASCII) for _ in range(n))


def cyrillic(rng, n, typography=False):
    text = [rng.choice(CYRILLIC) for _ in range(n)]
    if typography:
        for i in range(0, n, 40):
            text[min(n - 1, i + rng.randrange(40))] = rng.choice(TYPOGRAPHY)
    return "".join(text)


def japanese(rng, n):
    return "".join(rng.choice("0123456789") if rng.random() < 0.1 else rng.choice(JAPANESE) for _ in range(n))


# name, declared column, collation, value maker, rows, gated
WORKLOADS = [
    ("W1", "varchar(64)", "SQL_Latin1_General_CP1_CI_AS", lambda r: latin(r, 40, 0.05), 1.0, True),
    ("W1a", "varchar(64)", "SQL_Latin1_General_CP1_CI_AS", lambda r: latin(r, 40, 0.0), 1.0, True),
    ("W2", "varchar(64)", "Cyrillic_General_CI_AS", lambda r: cyrillic(r, 40), 1.0, True),
    ("W2a", "varchar(64)", "Cyrillic_General_CI_AS", lambda r: cyrillic(r, 40, True), 1.0, True),
    ("W3", "varchar(64)", "Japanese_CI_AS", lambda r: japanese(r, 20), 1.0, False),
    ("W4", "varchar(max)", "SQL_Latin1_General_CP1_CI_AS", lambda r: latin(r, 40, 0.05), 1.0, True),
    ("W4b", "varchar(max)", "SQL_Latin1_General_CP1_CI_AS", lambda r: latin(r, 4000, 0.05), 0.05, True),
    ("W5", "char(40)", "SQL_Latin1_General_CP1_CI_AS", lambda r: latin(r, 30, 0.05), 1.0, True),
]


def q(text):
    return text.replace("'", "''")


def duckdb(script, counters=True):
    env = {k: v for k, v in os.environ.items() if k not in ("AI_AGENT", "CLAUDECODE", "MSSQL_DEBUG")}
    if counters:
        env["MSSQL_COUNTERS"] = "1"
    binary = os.environ.get("DUCKDB", str(REPO / "build/release/duckdb"))
    # The script holds the DSN: stdin only, and the output is printed by the caller with
    # the secret removed.
    return subprocess.run([binary, "-batch", "-noheader", "-list"], input=script, text=True, capture_output=True,
                          env=env)


def attach(dsn):
    return (f"ATTACH '{q(dsn)}' AS a (TYPE mssql, client_transcode false, remote_pushdown false);\n"
            f"ATTACH '{q(dsn)}' AS b (TYPE mssql, remote_pushdown false);\n")


def exec_line(sql):
    return f"SELECT mssql_exec('a', '{q(sql)}');\n"


def prepare(dsn):
    rng = random.Random(92)
    script = attach(dsn)
    for name, declared, collation, make, share, _ in WORKLOADS:
        table = f"dbo.{PREFIX}{name}"
        rows = int(ROWS * share)
        script += exec_line(f"DROP TABLE IF EXISTS {table}; "
                            f"CREATE TABLE {table} (x {declared} COLLATE {collation} NOT NULL);")
        # The distinct values are stored once, then multiplied on the server. Sent as
        # nvarchar: the column's collation decides the bytes.
        values = [make(rng) for _ in range(DISTINCT)]
        for start in range(0, DISTINCT, 100):
            literals = ",".join(f"(N'{q(v)}')" for v in values[start:start + 100])
            script += exec_line(f"INSERT {table} VALUES {literals};")
        repeat = rows // DISTINCT
        script += exec_line(f"WITH n AS (SELECT TOP {max(repeat - 1, 0)} 1 AS i FROM sys.all_objects a CROSS JOIN "
                            f"sys.all_objects b) INSERT {table} SELECT t.x FROM (SELECT x FROM {table}) t CROSS JOIN n;")
        script += f"SELECT 'loaded', '{name}', count(*) FROM mssql_scan('a', 'SELECT 1 AS o FROM {table}');\n"
    return script


def statement(name, side):
    table = f"dbo.{PREFIX}{name}"
    # W3 is the raw scan: a double-byte column's catalog read keeps its cast on both sides.
    if name == "W3":
        inner = f"SELECT x FROM {table}" if side == "b" else f"SELECT CAST(x AS nvarchar(64)) AS x FROM {table}"
        return f"SELECT sum(length(x)) FROM mssql_scan('b', '{inner}');"
    return f"SELECT sum(length(x)) FROM {side}.{table};"


RUN_TIME = re.compile(r"Run Time \(s\): real ([0-9.]+)")
WIRE = re.compile(r"stream close: rows=(\d+) chunks=\d+ nulls=\d+ wire_in=(\d+)B")
STATS = ("SELECT SUBSTRING(t.text, 1, 300) AS text, CAST(s.total_worker_time AS bigint) AS cpu, "
         "CAST(s.execution_count AS bigint) AS n FROM sys.dm_exec_query_stats s "
         f"CROSS APPLY sys.dm_exec_sql_text(s.sql_handle) t WHERE t.text LIKE '%{PREFIX}%' "
         "AND t.text NOT LIKE '%dm_exec_query_stats%'")


def server_stats_line(tag):
    return f"SELECT '@@{tag}', text, cpu, n FROM mssql_scan('a', '{q(STATS)}');\n"


def parse_stats(text, tag):
    out = {}
    for line in text.splitlines():
        if line.startswith(f"@@{tag}|"):
            parts = line.rsplit("|", 2)
            key = parts[0].split("|", 1)[1]
            out[key] = (int(parts[1]), int(parts[2]))
    return out


def worker_time_ms(before, after, name, side):
    """The server's CPU per execution for this workload's own statement text."""
    table = f"{PREFIX}{name}".lower()
    total, runs = 0, 0
    for text, (cpu, n) in after.items():
        low = text.lower()
        if table + "]" not in low and table + " " not in low and not low.rstrip().endswith(table):
            continue
        # A's statement is the server's cast: NVARCHAR(n), or NCHAR(n) for a char(n).
        if ("nvarchar" in low or "nchar" in low) != (side == "a"):
            continue
        prior_cpu, prior_n = before.get(text, (0, 0))
        total += cpu - prior_cpu
        runs += n - prior_n
    return total / runs / 1000 if runs else float("nan")


def measure(dsn):
    script = attach(dsn) + ".timer on\n"
    order = []
    names = [w[0] for w in WORKLOADS]
    for name in names:
        script += ".timer off\n" + server_stats_line(f"{name}0") + ".timer on\n"
        # One warm-up each, not counted: the table's pages come into the buffer pool.
        script += statement(name, "a") + "\n" + statement(name, "b") + "\n"
        order += [(name, "warm"), (name, "warm")]
        for _ in range(ROUNDS):
            for side in ("a", "b"):
                script += statement(name, side) + "\n"
                order.append((name, side))
        script += ".timer off\n" + server_stats_line(f"{name}1") + ".timer on\n"
    run = duckdb(script)
    if run.returncode != 0:
        sys.exit("the measurement failed:\n" + run.stderr[-2000:].replace(dsn, "<dsn>"))
    times = [float(m) for m in RUN_TIME.findall(run.stdout + run.stderr)]
    # The statistics queries are streams too, of a few rows.
    wires = [(int(r), int(w)) for r, w in WIRE.findall(run.stderr) if int(r) >= DISTINCT]
    if len(times) != len(order):
        sys.exit(f"{len(times)} timings for {len(order)} statements:\n" + run.stderr[-2000:].replace(dsn, "<dsn>"))
    wall, wire = {}, {}
    streams = iter(wires)
    for (name, side), seconds in zip(order, times):
        rows_wire = next(streams, None)
        if side == "warm":
            continue
        wall.setdefault((name, side), []).append(seconds)
        if rows_wire:
            wire[(name, side)] = rows_wire[1]
    print(f"Rows: {ROWS}, rounds: {ROUNDS}, median wall time. B = client_transcode on, A = the server's cast.\n")
    print("| workload | A s | B s | B / A | wire A MB | wire B MB | server CPU A ms | server CPU B ms | gate |")
    print("|---|---|---|---|---|---|---|---|---|")
    failed = []
    for name, declared, collation, _, _, gated in WORKLOADS:
        a = statistics.median(wall[(name, "a")])
        b = statistics.median(wall[(name, "b")])
        before, after = parse_stats(run.stdout, f"{name}0"), parse_stats(run.stdout, f"{name}1")
        verdict = ("pass" if b <= a * 1.02 else "FAIL") if gated else "reported"
        if verdict == "FAIL":
            failed.append(name)
        print(f"| {name} {declared} {collation} | {a:.3f} | {b:.3f} | {b / a:.2f} | "
              f"{wire.get((name, 'a'), 0) / 1e6:.1f} | {wire.get((name, 'b'), 0) / 1e6:.1f} | "
              f"{worker_time_ms(before, after, name, 'a'):.0f} | {worker_time_ms(before, after, name, 'b'):.0f} | "
              f"{verdict} |")
    print(f"\nGate: {'FAILED on ' + ', '.join(failed) if failed else 'passed'}.")
    return 1 if failed else 0


def main():
    dsn = os.environ.get("MSSQL_TEST_DSN")
    if not dsn:
        sys.exit("set MSSQL_TEST_DSN")
    if os.environ.get("SKIP_LOAD") != "1":
        load = duckdb(prepare(dsn), counters=False)
        loaded = re.findall(r"loaded\|(\w+)\|(\d+)", load.stdout)
        if load.returncode != 0 or len(loaded) != len(WORKLOADS):
            sys.exit("loading failed:\n" + (load.stderr or load.stdout)[-2000:].replace(dsn, "<dsn>"))
        print("Loaded: " + ", ".join(f"{name} {rows}" for name, rows in loaded), file=sys.stderr)
    status = measure(dsn)
    if os.environ.get("KEEP_TABLES") != "1":
        duckdb(attach(dsn) + "".join(exec_line(f"DROP TABLE IF EXISTS dbo.{PREFIX}{w[0]};") for w in WORKLOADS),
               counters=False)
    sys.exit(status)


if __name__ == "__main__":
    main()

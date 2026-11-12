"""Spec 085 PR 2d: mssql_backup_scan against the restored database, read through TYPE mssql.

    MSSQL_ROWS_PORT=14330 MSSQL_ROWS_PASSWORD=... MSSQL_ROWS_DATABASE=cmp \\
        python3 -I test/backup/compare_rows.py --duckdb build/release/duckdb /tmp/bak/2022/catalog_full.bak

The backup (a file, or its stripes comma-separated) has been restored on a server of the version that
made it (test/backup/compare_rows.sh does that, one set at a time) as MSSQL_ROWS_DATABASE, reached on
localhost:MSSQL_ROWS_PORT as sa with MSSQL_ROWS_PASSWORD (the password goes to DuckDB on stdin, never
on a command line, and is masked in anything printed). For every table mssql_backup_tables says the
scan reads, every column it converts (not one read as its stored bytes, not one that is not readable)
is compared both ways with EXCEPT ALL against the restored table attached with native types off (D6:
the types TYPE mssql reads), and the row counts too. A column whose values are stored off row
(row-overflow, a LOB, a text pointer: PR 3) is left out, named, and the table compared without it.
Exits 1 on any difference or failure, or when fewer than --min-tables tables were compared.
"""
import argparse
import json
import os
import re
import subprocess
import sys


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


def ident(s):
    return '"' + s.replace('"', '""') + '"'


class Session:
    def __init__(self, binary, port, password, database):
        self.binary = binary
        self.password = password
        dsn = (f"Server=localhost,{port};Database={database};User Id=sa;Password={password};"
               "Encrypt=yes;TrustServerCertificate=yes")
        self.preamble = (".output /dev/null\n"
                         f"ATTACH {sql_string(dsn)} AS srv (TYPE mssql, native_types false);\n"
                         ".output\n")

    def mask(self, text):
        return text.replace(self.password, "***") if self.password else text

    def query(self, statement):
        """(True, rows) or (False, the error)."""
        script = self.preamble + ".mode json\n" + statement.rstrip().rstrip(";") + ";\n"
        run = subprocess.run([self.binary, "-no-agent", "-bail"], input=script, capture_output=True, text=True)
        if run.returncode != 0:
            return False, self.mask((run.stderr + run.stdout).strip())
        out = run.stdout.strip()
        return True, (json.loads(out) if out else [])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("backup", help="the backup: a file, or its stripes comma-separated")
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--min-tables", type=int, default=1)
    args = parser.parse_args()
    try:
        session = Session(args.duckdb, os.environ["MSSQL_ROWS_PORT"], os.environ["MSSQL_ROWS_PASSWORD"],
                          os.environ["MSSQL_ROWS_DATABASE"])
    except KeyError as e:
        sys.exit(f"compare_rows: set {e.args[0]}")
    files = [f for f in args.backup.split(",") if f]
    paths = "[" + ", ".join(sql_string(f) for f in files) + "]"

    ok, tables = session.query(
        f"SELECT schema_name, table_name, shape, decodable, reason, "
        f"to_json([{{'name': c.name, 'decodable': c.decodable, 'reason': c.reason}} FOR c IN columns]) AS columns "
        f"FROM mssql_backup_tables({paths}) ORDER BY schema_name, table_name")
    if not ok:
        sys.exit(f"compare_rows: mssql_backup_tables failed on {args.backup}:\n{tables}")

    compared, failures, rows_total = 0, [], 0
    for t in tables:
        schema, table = t["schema_name"], t["table_name"]
        where = f"{schema}.{table}"
        if t["decodable"] in (False, "false"):
            print(f"  skip {where}: {t['reason']}")
            continue
        columns = json.loads(t["columns"]) if isinstance(t["columns"], str) else t["columns"]
        cols = [c["name"] for c in columns if c["decodable"] and c["reason"] is None]
        # A column the scan cannot read is left out of SELECT * and refuses a query that names it, by
        # name (owner, review of #552): checked on the first such column of each table.
        unread = [c["name"] for c in columns if not c["decodable"]]
        if unread:
            scan_of = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)})"
            ok, result = session.query(f"SELECT {ident(unread[0])} FROM {scan_of} LIMIT 1")
            if ok or "is not read" not in str(result):
                failures.append(f"{where}: naming {unread[0]}, which is not read, was not refused by name")
        left_out = {c["name"]: c["reason"] for c in columns if not (c["decodable"] and c["reason"] is None)}
        while True:
            if not cols:
                failures.append(f"{where}: no column left to compare ({left_out})")
                break
            select = ", ".join(ident(c) for c in cols)
            scan = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)})"
            server = f"srv.{ident(schema)}.{ident(table)}"
            ok, result = session.query(
                f"SELECT (SELECT count(*) FROM (SELECT {select} FROM {scan} EXCEPT ALL SELECT {select} FROM {server})) "
                f"AS only_backup, (SELECT count(*) FROM (SELECT {select} FROM {server} EXCEPT ALL "
                f"SELECT {select} FROM {scan})) AS only_server, (SELECT count(*) FROM {scan}) AS backup_rows, "
                f"(SELECT count(*) FROM {server}) AS server_rows")
            if not ok:
                off_row = re.search(r"backup: [^ ]+\.([^ .]+) holds a value stored off row", result)
                if off_row and off_row.group(1) in cols:
                    cols.remove(off_row.group(1))
                    left_out[off_row.group(1)] = "values stored off row (PR 3)"
                    continue
                failures.append(f"{where}: {result.splitlines()[0] if result else 'failed'}")
                break
            r = result[0]
            only_backup, only_server = int(r["only_backup"]), int(r["only_server"])
            backup_rows, server_rows = int(r["backup_rows"]), int(r["server_rows"])
            status = "ok" if (only_backup, only_server) == (0, 0) and backup_rows == server_rows else "DIFF"
            if status != "ok":
                failures.append(f"{where}: {only_backup} rows only in the backup, {only_server} only on the server, "
                                f"{backup_rows} against {server_rows} rows")
            # _mssql_rid (D8): one per row; a heap row's is 0x00 ‖ its %%physloc%% (its original place:
            # the server answers a forwarded row's stub, E1§3).
            ok, rid = session.query(f"SELECT count(DISTINCT _mssql_rid) AS rids FROM {scan}")
            rid_note = ""
            if not ok or int(rid[0]["rids"]) != backup_rows:
                status = "DIFF"
                failures.append(f"{where}: _mssql_rid is not one per row ({rid if not ok else rid[0]['rids']})")
            elif t["shape"] == "heap":
                loc_sql = f"SELECT CAST(%%physloc%% AS varbinary(8)) AS loc FROM [{schema}].[{table}]"
                physloc = f"mssql_scan('srv', {sql_string(loc_sql)})"
                ok, rid = session.query(
                    f"SELECT (SELECT count(*) FROM (SELECT _mssql_rid FROM {scan} EXCEPT ALL "
                    f"SELECT '\\x00'::BLOB || loc FROM {physloc})) AS only_backup, "
                    f"(SELECT count(*) FROM (SELECT '\\x00'::BLOB || loc FROM {physloc} EXCEPT ALL "
                    f"SELECT _mssql_rid FROM {scan})) AS only_server")
                if not ok or (int(rid[0]["only_backup"]), int(rid[0]["only_server"])) != (0, 0):
                    status = "DIFF"
                    failures.append(f"{where}: _mssql_rid against %%physloc%%: {rid if not ok else rid[0]}")
                else:
                    rid_note = ", _mssql_rid = %%physloc%%"
            note = f"; left out: {', '.join(sorted(left_out))}" if left_out else ""
            print(f"  {status:4} {where}: {backup_rows} rows, {len(cols)} columns{rid_note}{note}")
            compared += 1
            rows_total += backup_rows
            break

    print(f"compare_rows: {args.backup}: {compared} tables, {rows_total} rows compared, {len(failures)} failures")
    for f in failures:
        print(f"  FAIL {f}")
    if failures or compared < args.min_tables:
        sys.exit(1)


if __name__ == "__main__":
    main()

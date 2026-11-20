"""Spec 085 PR 2d: mssql_backup_scan against the restored database, read through TYPE mssql.

    MSSQL_ROWS_PORT=14330 MSSQL_ROWS_PASSWORD=... MSSQL_ROWS_DATABASE=cmp \\
        python3 -I test/backup/compare_rows.py --duckdb build/release/duckdb /tmp/bak/2022/catalog_full.bak

The backup (a file, or its stripes comma-separated) has been restored on a server of the version that
made it (test/backup/compare_rows.sh does that, one set at a time) as MSSQL_ROWS_DATABASE, reached on
localhost:MSSQL_ROWS_PORT as sa with MSSQL_ROWS_PASSWORD (the password goes to DuckDB on stdin, never
on a command line, and is masked in anything printed). For every table mssql_backup_tables says the
scan reads, every column it converts (not one read as its stored bytes, not one that is not readable)
is compared both ways with EXCEPT ALL against the restored table attached with native types off (D6:
the types TYPE mssql reads), and the row counts too; values stored off row (row-overflow, LOB trees,
text pointers) are compared like any others since PR 3, and each table's _mssql_lob is built for every
row (its map of them, NULL where a row has none).
Exits 1 on any difference or failure, or when fewer than --min-tables tables were compared.

With --log FILES (spec 085 task 27) the backup's log is compared instead: mssql_backup_transaction_records
against fn_dump_dblog on the server over the same files (FILES: their paths on the server, comma-separated),
record by record on the LSN, the operation, the context, the transaction id, the page and the allocation
unit, and a BEGIN_XACT's transaction name; up to the backup's LastLSN (a data backup's tail ends there).

With --fuzzy (spec 085 D11, task 27: a backup taken under load, which RESTORE recovers and the reader
does not) a table may differ, and its fuzzy_status (mssql_backup_tables) is checked against it instead:
a table that differs from the restored one must not be 'consistent', and one that does not differ must be
(acceptance 6: every differing table flagged, no consistent table flagged). Only the rows are compared
there: a fuzzy backup may hold a row twice (a split torn by the backup, E3§3), so _mssql_rid and
_mssql_lob are not.
"""
import argparse
import json
import os
import subprocess
import sys


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


def ident(s):
    return '"' + s.replace('"', '""') + '"'


def tsql_ident(s):
    # A T-SQL bracketed name: ] doubled (names.sql's dbo.[q]]t'x]).
    return "[" + s.replace("]", "]]") + "]"


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
        try:
            return True, (json.loads(out) if out else [])
        except json.JSONDecodeError as e:
            # Named, not a traceback that ends every set's comparison (spec 086 PR 1).
            return False, self.mask(f"not JSON ({e}): {out[:300]!r}")


def lsn_text(value):
    """fn_dblog's spelling of an LSN given as HEADERONLY's number."""
    n = int(value)
    return f"{n // 10**15:08X}:{(n // 10**5) % 10**10:08X}:{n % 10**5:04X}"


def compare_log(session, args, paths):
    """mssql_backup_transaction_records against fn_dump_dblog: 0 when every record agrees."""
    server_files = [f for f in args.log.split(",") if f]
    devices = ", ".join("'" + f.replace("'", "''") + "'" for f in server_files) + ", DEFAULT" * (64 - len(server_files))
    dblog = ("SELECT [Current LSN] AS lsn, Operation AS operation, Context AS context, [Transaction ID] AS xid, "
             "[Page ID] AS page, AllocUnitId AS au, [Transaction Name] AS name "
             f"FROM fn_dump_dblog(NULL, NULL, 'DISK', 1, {devices})")
    ok, info = session.query(f"SELECT backup_type, last_lsn FROM mssql_backup_info({paths})")
    if not ok:
        print(f"compare_rows --log: mssql_backup_info failed on {args.backup}:\n{info}")
        return 1
    last = lsn_text(info[0]["last_lsn"])
    ok, server = session.query(f"SELECT * FROM mssql_scan('srv', {sql_string(dblog)})")
    if not ok:
        print(f"compare_rows --log: fn_dump_dblog failed on {args.backup}:\n{server}")
        return 1
    ok, ours = session.query(
        f"SELECT lsn::VARCHAR AS lsn, operation, context, transaction_id AS xid, page, allocation_unit AS au, "
        f"CASE WHEN operation = 'LOP_BEGIN_XACT' THEN contents.name::VARCHAR END AS name "
        f"FROM mssql_backup_transaction_records({paths})")
    if not ok:
        print(f"compare_rows --log: mssql_backup_transaction_records failed on {args.backup}:\n{ours}")
        return 1

    def server_page(p):
        if not p:
            return None
        f, n = p.split(":")
        return f"{int(f, 16)}:{int(n, 16)}"

    want = {}
    for r in server:
        # Up to 2019 the server spells an LSN's hex in lower case (00000028:000007d0:000a), from 2022 on in
        # upper case, as lsn_text does: compared, and ordered against LastLSN, in upper case. A transaction id
        # is compared in lower case, as the reader spells it.
        lsn = r["lsn"].upper()
        xid = r["xid"].lower() if r["xid"] else r["xid"]
        if lsn >= last:
            continue  # LastLSN is the next record after the backup's (HEADERONLY): not the backup's
        want[lsn] = (r["operation"], r["context"], xid if xid != "0000:00000000" else None,
                          server_page(r["page"]), int(r["au"]) if r["au"] not in (None, "") else None, r["name"])
    got = {}
    duplicates = []
    for r in ours:
        key = lsn_text(r["lsn"])
        if key in got:
            duplicates.append(key)  # the reader under test giving one record twice (review of #591)
        got[key] = (r["operation"], r["context"], r["xid"].lower() if r["xid"] else r["xid"], r["page"], int(r["au"]) if r["au"] is not None else None,
                    r["name"])
    failures = [f"{lsn}: given twice by the reader" for lsn in duplicates]
    for lsn in sorted(set(want) | set(got)):
        w, g = want.get(lsn), got.get(lsn)
        if w is None or g is None:
            failures.append(f"{lsn}: {'only ours' if w is None else 'only the server'}: {g or w}")
            continue
        fields = ["operation", "context", "transaction", "page", "allocation unit", "name"]
        for name, a, b in zip(fields, w, g):
            # The server prints no page or unit where the record names none (and some records name a unit
            # on page 0:0); only what it does print is compared.
            if name in ("page", "allocation unit") and a in (None, "0:0", 0):
                continue
            if name == "name" and not a:
                continue
            if a != b:
                failures.append(f"{lsn} {w[0]}: {name} {a!r} on the server, {b!r} here")
    print(f"compare_rows --log: {args.backup}: {len(want)} records of the server, {len(got)} here, "
          f"{len(failures)} differences")
    for f in failures[:40]:
        print(f"  FAIL {f}")
    return 1 if failures or not want else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("backup", help="the backup: a file, or its stripes comma-separated")
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--min-tables", type=int, default=1)
    parser.add_argument("--diff", default="", help="a differential to lay over the backup: its stripes comma-separated")
    parser.add_argument("--log", default="",
                        help="compare the backup's log with fn_dump_dblog over these server paths (comma-separated)")
    parser.add_argument("--fuzzy", action="store_true",
                        help="a backup taken under load: check each table's fuzzy_status against whether it differs")
    args = parser.parse_args()
    try:
        session = Session(args.duckdb, os.environ["MSSQL_ROWS_PORT"], os.environ["MSSQL_ROWS_PASSWORD"],
                          os.environ["MSSQL_ROWS_DATABASE"])
    except KeyError as e:
        sys.exit(f"compare_rows: set {e.args[0]}")
    files = [f for f in args.backup.split(",") if f]
    paths = "[" + ", ".join(sql_string(f) for f in files) + "]"
    diff = [f for f in args.diff.split(",") if f]
    # Spec 085 PR 4c: every call reads the differential over the full (a named parameter, last).
    opts = (", diff := [" + ", ".join(sql_string(f) for f in diff) + "]") if diff else ""
    # A table's status is checked from the listing; its scan reads it whatever the status (no WARNING in the output).
    scan_opts = opts + (", fuzzy := 'ignore'" if args.fuzzy else "")

    if args.log:
        sys.exit(compare_log(session, args, paths))

    ok, tables = session.query(
        f"SELECT schema_name, table_name, shape, decodable, reason, fuzzy_status, open_transactions, stale_transactions, "
        f"to_json([{{'name': c.name, 'decodable': c.decodable, 'reason': c.reason}} FOR c IN columns]) AS columns "
        f"FROM mssql_backup_tables({paths}{opts}) ORDER BY schema_name, table_name")
    if not ok:
        sys.exit(f"compare_rows: mssql_backup_tables failed on {args.backup}:\n{tables}")

    compared, failures, rows_total = 0, [], 0
    statuses = {}
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
            scan_of = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)}{scan_opts})"
            ok, result = session.query(f"SELECT {ident(unread[0])} FROM {scan_of} LIMIT 1")
            if ok or "is not read" not in str(result):
                failures.append(f"{where}: naming {unread[0]}, which is not read, was not refused by name")
        left_out = {c["name"]: c["reason"] for c in columns if not (c["decodable"] and c["reason"] is None)}
        while True:
            if not cols:
                failures.append(f"{where}: no column left to compare ({left_out})")
                break
            select = ", ".join(ident(c) for c in cols)
            scan = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)}{scan_opts})"
            server = f"srv.{ident(schema)}.{ident(table)}"
            ok, result = session.query(
                f"SELECT (SELECT count(*) FROM (SELECT {select} FROM {scan} EXCEPT ALL SELECT {select} FROM {server})) "
                f"AS only_backup, (SELECT count(*) FROM (SELECT {select} FROM {server} EXCEPT ALL "
                f"SELECT {select} FROM {scan})) AS only_server, (SELECT count(*) FROM {scan}) AS backup_rows, "
                f"(SELECT count(*) FROM {server}) AS server_rows")
            if not ok:
                failures.append(f"{where}: {result.splitlines()[0] if result else 'failed'}")
                break
            r = result[0]
            only_backup, only_server = int(r["only_backup"]), int(r["only_server"])
            backup_rows, server_rows = int(r["backup_rows"]), int(r["server_rows"])
            differs = (only_backup, only_server) != (0, 0) or backup_rows != server_rows
            if args.fuzzy:
                fuzzy = t["fuzzy_status"]
                statuses[fuzzy] = statuses.get(fuzzy, 0) + 1
                verdict = "ok"
                if differs and fuzzy == "consistent":
                    verdict = "MISSED"
                    failures.append(f"{where}: differs from the restored table ({only_backup} rows only in the backup, "
                                    f"{only_server} only on the server) but its fuzzy_status is 'consistent'")
                elif not differs and fuzzy != "consistent":
                    verdict = "FLAGGED"
                    failures.append(f"{where}: equal to the restored table but its fuzzy_status is '{fuzzy}' "
                                    f"({t['open_transactions']} open, {t['stale_transactions']} stale transactions)")
                print(f"  {verdict:7} {where}: {fuzzy} ({t['open_transactions']} open, {t['stale_transactions']} stale), "
                      f"{'differs' if differs else 'equal'}: {only_backup} rows only in the backup, {only_server} only "
                      f"on the server, {backup_rows} against {server_rows} rows")
                compared += 1
                rows_total += backup_rows
                break
            status = "DIFF" if differs else "ok"
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
                loc_sql = f"SELECT CAST(%%physloc%% AS varbinary(8)) AS loc FROM {tsql_ident(schema)}.{tsql_ident(table)}"
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
            # _mssql_lob (PR 3): built for every row -- every off-row value of it, projected or not -- and
            # present at least for each row the server holds a value of more than 8,000 bytes in (no such
            # value fits in a row).
            readable = [c["name"] for c in columns if c["decodable"]]
            long_sql = (f"SELECT count(*) AS n FROM {tsql_ident(schema)}.{tsql_ident(table)} WHERE " +
                        " OR ".join(f"DATALENGTH({tsql_ident(c)}) > 8000" for c in readable)) if readable else None
            ok, lob = session.query(f"SELECT count(*) AS n, count(_mssql_lob) AS maps FROM {scan}")
            ok_long, long_rows = (session.query(f"SELECT n FROM mssql_scan('srv', {sql_string(long_sql)})")
                                  if long_sql else (True, [{"n": 0}]))
            lob_note = ""
            if not ok or not ok_long:
                status = "DIFF"
                error = lob if not ok else long_rows
                failures.append(f"{where}: _mssql_lob: {error.splitlines()[0] if error else 'failed'}")
            elif int(lob[0]["n"]) != backup_rows or int(lob[0]["maps"]) < int(long_rows[0]["n"]):
                status = "DIFF"
                failures.append(f"{where}: _mssql_lob in {lob[0]['maps']} rows, where the server holds "
                                f"{long_rows[0]['n']} rows with a value past 8,000 bytes")
            elif int(lob[0]["maps"]) > 0:
                lob_note = f", {lob[0]['maps']} rows with values off row"
            note = f"; left out: {', '.join(sorted(left_out))}" if left_out else ""
            print(f"  {status:4} {where}: {backup_rows} rows, {len(cols)} columns{rid_note}{lob_note}{note}")
            compared += 1
            rows_total += backup_rows
            break

    # An indexed view's rows (task 16f): its unique clustered index, read as a clustered table's, against
    # the view through TYPE mssql (the same rows as WITH (NOEXPAND) in a backup at rest, typed as the
    # catalog types them); every column the scan returns, _mssql_rid one per row.
    ok, views = session.query(f"SELECT schema_name, view_name, decodable, scan_reason FROM mssql_backup_views({paths}{opts}) "
                              "WHERE is_indexed ORDER BY schema_name, view_name")
    if not ok:
        sys.exit(f"compare_rows: mssql_backup_views failed on {args.backup}:\n{views}")
    for v in views:
        schema, view = v["schema_name"], v["view_name"]
        where = f"{schema}.{view} (indexed view)"
        if v["decodable"] in (False, "false"):
            print(f"  skip {where}: {v['scan_reason']}")
            continue
        scan = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(view)}{scan_opts})"
        server = f"srv.{ident(schema)}.{ident(view)}"
        ok, described = session.query(f"DESCRIBE SELECT * FROM {scan}")
        ok_server, server_described = session.query(f"DESCRIBE SELECT * FROM {server}")
        if not ok or not ok_server:
            failures.append(f"{where}: {(described if not ok else server_described).splitlines()[0]}")
            continue
        if [c["column_name"] for c in described] != [c["column_name"] for c in server_described]:
            failures.append(f"{where}: the scan's columns {[c['column_name'] for c in described]} are not the "
                            f"server view's {[c['column_name'] for c in server_described]}")
            continue
        select = ", ".join(ident(c["column_name"]) for c in described)
        ok, result = session.query(
            f"SELECT (SELECT count(*) FROM (SELECT {select} FROM {scan} EXCEPT ALL SELECT {select} FROM {server})) "
            f"AS only_backup, (SELECT count(*) FROM (SELECT {select} FROM {server} EXCEPT ALL "
            f"SELECT {select} FROM {scan})) AS only_server, (SELECT count(*) FROM {scan}) AS backup_rows, "
            f"(SELECT count(*) FROM {server}) AS server_rows, (SELECT count(DISTINCT _mssql_rid) FROM {scan}) AS rids")
        if not ok:
            failures.append(f"{where}: {result.splitlines()[0] if result else 'failed'}")
            continue
        r = result[0]
        backup_rows = int(r["backup_rows"])
        status = "ok" if (int(r["only_backup"]), int(r["only_server"])) == (0, 0) and backup_rows == int(
            r["server_rows"]) and int(r["rids"]) == backup_rows else "DIFF"
        if status != "ok":
            failures.append(f"{where}: {r}")
        print(f"  {status:4} {where}: {backup_rows} rows, {len(described)} columns")
        compared += 1
        rows_total += backup_rows

    # Spec 086 acceptance 2: a compressed table <x>_row beside its uncompressed twin <x>_plain (the same rows,
    # cd.sql) reads alike, _mssql_rid included -- the key bytes are the uncompressed form's.
    # Not on a backup taken under load (--fuzzy): its tables may differ, which the status checks above judge.
    names = {} if args.fuzzy else {(t["schema_name"], t["table_name"]): t for t in tables}
    for (schema, table), t in sorted(names.items()):
        twin = names.get((schema, table[:-4] + "_plain")) if table.endswith("_row") else None
        if not twin or t["decodable"] in (False, "false") or twin["decodable"] in (False, "false"):
            continue
        where = f"{schema}.{table} against {twin['table_name']}"
        a = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)}{scan_opts})"
        b = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(twin['table_name'])}{scan_opts})"
        ok, result = session.query(
            f"SELECT (SELECT count(*) FROM (SELECT *, _mssql_rid FROM {a} EXCEPT ALL SELECT *, _mssql_rid FROM {b})) "
            f"AS only_row, (SELECT count(*) FROM (SELECT *, _mssql_rid FROM {b} EXCEPT ALL "
            f"SELECT *, _mssql_rid FROM {a})) AS only_plain")
        if not ok or (int(result[0]["only_row"]), int(result[0]["only_plain"])) != (0, 0):
            failures.append(f"{where}: {result if not ok else result[0]}")
            print(f"  DIFF {where}")
            continue
        # Review of #569: each column projected alone beside _mssql_rid, so the compressed scan converts
        # that column only (CdRow::Read's `needed`: the others are located, not read) -- and still reads
        # alike, the key columns converted for the rid though not projected.
        ok, described = session.query(f"DESCRIBE SELECT * FROM {a}")
        if not ok:
            failures.append(f"{where}: {described.splitlines()[0] if described else 'DESCRIBE failed'}")
            print(f"  DIFF {where}")
            continue
        differing = []
        for column in (c["column_name"] for c in described):
            sel = f"{ident(column)}, _mssql_rid"
            ok, result = session.query(
                f"SELECT (SELECT count(*) FROM (SELECT {sel} FROM {a} EXCEPT ALL SELECT {sel} FROM {b})) AS only_row, "
                f"(SELECT count(*) FROM (SELECT {sel} FROM {b} EXCEPT ALL SELECT {sel} FROM {a})) AS only_plain")
            if not ok or (int(result[0]["only_row"]), int(result[0]["only_plain"])) != (0, 0):
                differing.append(f"{column}: {(result.splitlines()[0] if result else 'failed') if not ok else result[0]}")
        if differing:
            failures.extend(f"{where}, projected {d}" for d in differing)
            print(f"  DIFF {where}: projected alone, {len(differing)} columns differ")
        else:
            print(f"  ok   {where}: equal, _mssql_rid included; each of {len(described)} columns alone too")

    if args.fuzzy:
        print(f"compare_rows: fuzzy_status of the tables compared: "
              f"{', '.join(f'{n} {k}' for k, n in sorted(statuses.items())) or 'none'}")
    print(f"compare_rows: {args.backup}: {compared} tables and indexed views, {rows_total} rows compared, "
          f"{len(failures)} failures")
    for f in failures:
        print(f"  FAIL {f}")
    if failures or compared < args.min_tables:
        sys.exit(1)


if __name__ == "__main__":
    main()

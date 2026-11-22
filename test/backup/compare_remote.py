#!/usr/bin/env python3
"""Spec 085 task 25: the backup functions read through object storage answer as they do over local files.

    python3 -I test/backup/compare_remote.py --duckdb cli/duckdb --local test/data/backup \\
        --remote s3://backups/samples --remote https://localhost:9000/backups/samples

LOCAL is a directory of the committed samples (test/data/backup: sets.txt, the backups, and the
directories of other databases beside them, each with a sets.txt of its own); each REMOTE holds the same
files under the same names (an s3:// prefix, or an http(s):// one the files are served under, read by
range requests). Every statement below is run with LOCAL's paths and with each REMOTE's, through one
DuckDB build (httpfs linked in: the oracle lane's), and the answers must be equal, a path in them read
as LOCAL's. The statements: mssql_backup_info of each set; the listings of each full, and of the full
with its differential; each table's rows (a count and a hash over every column and the virtual ones);
mssql_backup_table_changes and _schema_changes of the overlays; and over s3:// (a glob needs a listing:
http has none) a history attached from a glob, its snapshots and each version's rows.

Credentials come from the environment, reach DuckDB on its stdin and are never printed:
MSSQL_S3_KEY_ID, MSSQL_S3_SECRET, MSSQL_S3_ENDPOINT (host:port) for s3://; MSSQL_CA_CERT_FILE, when set,
is the certificate httpfs trusts (a test server's own, self-signed) and https is used for both.
Exits 1 on any difference -- a differential that lies over no full beside it, or a state with no table
read, counts as one: the comparisons must not shrink unseen -- or when fewer than --min-statements
comparisons were made.
"""
import argparse
import json
import os
import re
import subprocess
import sys


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


class DuckDB:
    def __init__(self, binary):
        self.binary = binary
        self.secrets = []
        lines = [".output /dev/null"]
        ca = os.environ.get("MSSQL_CA_CERT_FILE")
        if ca:
            lines.append(f"SET ca_cert_file = {sql_string(ca)};")
        key, secret, endpoint = (os.environ.get(n) for n in ("MSSQL_S3_KEY_ID", "MSSQL_S3_SECRET",
                                                               "MSSQL_S3_ENDPOINT"))
        if key and secret and endpoint:
            self.secrets = [v for s in (key, secret) if s for v in {s, s.replace("'", "''")}]
            # One option a line: a parser error echoes the line it stopped on, never the key's.
            lines += ["CREATE SECRET remote_backups (TYPE s3,", f"KEY_ID {sql_string(key)},",
                      f"SECRET {sql_string(secret)},", f"ENDPOINT {sql_string(endpoint)},",
                      f"USE_SSL {'true' if ca else 'false'},", "REGION 'us-east-1', URL_STYLE 'path');"]
        lines.append(".output")
        self.preamble = "\n".join(lines) + "\n"
        self.warnings = []

    def mask(self, text):
        for s in sorted(self.secrets, key=len, reverse=True):
            text = text.replace(s, "***")
        return text

    def query(self, statement, setup=""):
        """The rows of one statement (a list of dicts), or the error's first line; `setup` runs before it,
        its output dropped."""
        script = self.preamble + (f".output /dev/null\n{setup}\n.output\n" if setup else "") + ".mode json\n" + \
            statement.rstrip().rstrip(";") + ";\n"
        self.warnings = []
        run = subprocess.run([self.binary, "-no-agent", "-bail"], input=script, capture_output=True, text=True)
        if run.returncode != 0:
            first = next((line for line in run.stderr.splitlines() if line.strip()), "(no message)")
            return "ERROR: " + self.mask(first)
        # The CLI prints a WARNING (a fuzzy table's, spec 085 PR 5; a clamped max_threads) before the rows,
        # in grey: kept apart, in self.warnings, so that the rows are the JSON after them and the warnings
        # are compared too.
        text = re.sub(r"\x1b\[[0-9;]*m", "", run.stdout)
        lines = text.splitlines()
        start = next((i for i, line in enumerate(lines) if line.startswith("[")), len(lines))
        # Sorted: a scan's tasks may warn in either order.
        self.warnings = sorted(w for w in (line.strip() for line in lines[:start]) if w and w != "WARNING:")
        out = "\n".join(lines[start:]).strip()
        return json.loads(out) if out else []


def read_sets(path):
    """sets.txt's (name, [file, ...])."""
    sets = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                name, files = line.split("|", 1)
                sets.append((name, files.split(",")))
    return sets


def at(template, base):
    """`template` with {P} -- always inside a SQL string literal -- the directory `base`."""
    return template.replace("{P}", base.replace("'", "''"))


def relocated(value, remote, local):
    """`value` (an answer, decoded) with each string's `remote` prefix read as `local`."""
    if isinstance(value, str):
        return value.replace(remote, local)
    if isinstance(value, list):
        return [relocated(v, remote, local) for v in value]
    if isinstance(value, dict):
        return {k: relocated(v, remote, local) for k, v in value.items()}
    return value


def statements(local):
    """(label, template[, setup, s3 only]) for every comparison, {P} a directory of the files (LOCAL or a
    REMOTE); and what the local files did not give that a comparison needs (a differential with no full
    it lies over, a state with no table read): reported as differences, so coverage cannot drop unseen."""
    out, problems = [], []
    db = DuckDB(ARGS.duckdb)
    # Each database's directory: the samples, and every directory beside them with a sets.txt.
    dirs = [""] + sorted(d + "/" for d in os.listdir(local)
                         if os.path.isfile(os.path.join(local, d, "sets.txt")))
    for sub in dirs:
        sets = read_sets(os.path.join(local, sub, "sets.txt"))
        fulls, diffs = [], []
        for name, files in sets:
            listed = "[" + ", ".join(sql_string("{P}/" + sub + f) for f in files) + "]"
            out.append((f"info {sub}{name}", f"SELECT * FROM mssql_backup_info({listed})"))
            info = db.query(at(f"SELECT backup_type FROM mssql_backup_info({listed})", local))
            kind = info[0]["backup_type"] if isinstance(info, list) and info else None
            if kind == "full":
                fulls.append((name, listed))
            elif kind == "differential":
                diffs.append((name, listed))
            elif kind is None:
                problems.append(f"info {sub}{name}: local: {info}")
        # A differential over each full of its database it can lie over (its base: the other fulls of the
        # samples are copies, COPY_ONLY, which a differential's base is never).
        pairs = [(f, None) for f in fulls]
        for d in diffs:
            bases = 0
            for f in fulls:
                probe = db.query(at(f"SELECT count(*) AS n FROM mssql_backup_tables({f[1]}, diff := {d[1]})", local))
                if isinstance(probe, list):
                    pairs.append((f, d))
                    bases += 1
            if bases == 0:
                problems.append(f"{sub}{d[0]}: a differential over none of the fulls beside it")
        for (fname, flisted), d in pairs:
            over = f", diff := {d[1]}" if d else ""
            label = fname + (f"+{d[0]}" if d else "")
            for listing in ("tables", "views", "columns", "schemas"):
                out.append((f"{listing} {sub}{label}",
                            f"SELECT * FROM mssql_backup_{listing}({flisted}{over}) ORDER BY ALL"))
            tables = db.query(at(f"SELECT schema_name, table_name FROM mssql_backup_tables({flisted}{over}) "
                                 f"WHERE decodable ORDER BY ALL", local))
            if not isinstance(tables, list) or not tables:
                problems.append(f"tables {sub}{label}: no table read locally ({tables})")
            for t in tables if isinstance(tables, list) else []:
                schema, table = t["schema_name"], t["table_name"]
                args = f"{flisted}, {sql_string(schema)}, {sql_string(table)}{over}"
                out.append((f"scan {sub}{label} {schema}.{table}",
                            f"SELECT count(*) AS n, sum(hash(x)) AS h FROM (SELECT *, _mssql_rid, _mssql_page, "
                            f"_mssql_page_lsn, _mssql_lob FROM mssql_backup_scan({args})) x"))
                if d:
                    out.append((f"table_changes {sub}{label} {schema}.{table}",
                                f"SELECT change_type, count(*) AS n, sum(hash(x)) AS h FROM "
                                f"mssql_backup_table_changes({args}) x GROUP BY ALL ORDER BY ALL"))
            if d:
                out.append((f"schema_changes {sub}{label}",
                            f"SELECT * EXCLUDE (start_version, end_version) FROM "
                            f"mssql_backup_schema_changes({flisted}{over}) ORDER BY ALL"))
    # A history attached from a glob (s3 only: http has no listing): its versions, and each one's rows.
    attach = "ATTACH '{P}/*.bak' AS h (TYPE mssql_backup, history true);"
    out.append(("history snapshots", "SELECT * FROM h.snapshots() ORDER BY snapshot_id", attach, True))
    versions = db.query("SELECT max(snapshot_id) AS n FROM h.snapshots()", at(attach, local))
    count = (versions[0]["n"] or 0) if isinstance(versions, list) and versions else 0
    if count == 0:
        problems.append(f"history: no version locally ({versions})")
    for v in range(1, count + 1):
        out.append((f"history version {v}", f"SELECT count(*) AS n, sum(hash(x)) AS h FROM "
                    f"(SELECT *, _mssql_rid FROM h.dbo.items AT (VERSION => {v})) x", attach, True))
    return out, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--local", default="test/data/backup")
    parser.add_argument("--remote", action="append", required=True)
    parser.add_argument("--min-statements", type=int, default=100)
    global ARGS
    ARGS = parser.parse_args()
    local = os.path.abspath(ARGS.local)
    db = DuckDB(ARGS.duckdb)
    compared = 0
    kinds = {}
    entries, differences = statements(local)
    for entry in entries:
        kinds[entry[0].split()[0]] = kinds.get(entry[0].split()[0], 0) + 1
        label, template = entry[0], entry[1]
        setup = entry[2] if len(entry) > 2 else ""
        glob = len(entry) > 3 and entry[3]
        expected = db.query(at(template, local), at(setup, local))
        if not isinstance(expected, list):
            differences.append(f"{label}: local: {expected}")
            continue
        expected_text = json.dumps(expected, sort_keys=True)
        expected_warnings = db.warnings
        for remote in ARGS.remote:
            if glob and not remote.startswith("s3://"):
                continue
            remote = remote.rstrip("/")
            got = db.query(at(template, remote), at(setup, remote))
            compared += 1
            if not isinstance(got, list):
                differences.append(f"{label}: {remote}: {got}")
            elif json.dumps(relocated(got, remote, local), sort_keys=True) != expected_text:
                differences.append(f"{label}: {remote}: {json.dumps(got)[:300]} where local gives "
                                   f"{expected_text[:300]}")
            elif relocated(db.warnings, remote, local) != expected_warnings:
                differences.append(f"{label}: {remote}: warnings {db.warnings} where local gives "
                                   f"{expected_warnings}")
    for d in differences:
        print("DIFFERENCE " + d)
    print("compare_remote: statements by kind: " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))
    print(f"compare_remote: {compared} comparisons over {len(ARGS.remote)} remote(s), "
          f"{len(differences)} differences")
    if differences:
        sys.exit(1)
    if compared < ARGS.min_statements:
        sys.exit(f"compare_remote: only {compared} comparisons, fewer than --min-statements "
                 f"{ARGS.min_statements}")


if __name__ == "__main__":
    main()

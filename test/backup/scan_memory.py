"""Spec 085 acceptance 13: every table of a backup scanned whole under DuckDB's limits, its peak memory read.

    python3 -I test/backup/scan_memory.py --duckdb build/release/duckdb --memory-limit 1GB /tmp/huge/huge_full.bak
    python3 -I test/backup/scan_memory.py --duckdb build/release/duckdb --memory-limit 1GB \\
        --scan-args "max_memory := '512MB', max_threads := 2" /tmp/huge/huge_full.bak

For each table mssql_backup_tables says the scan reads: SELECT count(*) and a hash of every row (all
columns the scan returns, _mssql_rid too) with SET memory_limit, profiled, each in a process of its own;
the peak memory DuckDB's buffer manager accounted during the query (the profile's
system.peak_buffer_memory) must stay under the limit, the scan must complete and its count must be the
catalog's row_count. The process's peak resident memory is printed beside it (not checked: it holds
what DuckDB does not account -- the binary, the open backup's indexes, the threads' read windows and
staging capacity, spec 085 § Resources). --hashes writes each table's count and hash as JSON, for
acceptance13.sh to compare the backups of one database with each other; the rows' equality with the
server is compare_rows.py's. Prints the listing's time, one line per table, and exits 1 on a failure.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


def size_bytes(text):
    units = {"KB": 1000, "MB": 1000 ** 2, "GB": 1000 ** 3, "KIB": 1024, "MIB": 1024 ** 2, "GIB": 1024 ** 3}
    t = text.strip().upper()
    for unit, factor in sorted(units.items(), key=lambda u: -len(u[0])):
        if t.endswith(unit):
            return int(float(t[: -len(unit)]) * factor)
    return int(t)


def run(binary, script):
    """The script's exit code, stdout, stderr and the process's peak resident memory in bytes."""
    with tempfile.TemporaryDirectory() as tmp:
        paths = [os.path.join(tmp, n) for n in ("in.sql", "out", "err")]
        with open(paths[0], "w") as f:
            f.write(script)
        with open(paths[0]) as i, open(paths[1], "w") as o, open(paths[2], "w") as e:
            p = subprocess.Popen([binary, "-no-agent", "-bail", "-json"], stdin=i, stdout=o, stderr=e)
            _, status, usage = os.wait4(p.pid, 0)
            p.returncode = os.waitstatus_to_exitcode(status)
        rss = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)  # bytes on macOS, KiB on Linux
        return p.returncode, open(paths[1]).read(), open(paths[2]).read(), rss


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("backup", help="the backup: a file, or its stripes comma-separated")
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--memory-limit", required=True)
    parser.add_argument("--scan-args", default="", help="named arguments of mssql_backup_scan, e.g. max_threads := 2")
    parser.add_argument("--hashes", help="write {schema.table: [count, hash]} to this JSON file")
    args = parser.parse_args()
    paths = "[" + ", ".join(sql_string(f) for f in args.backup.split(",") if f) + "]"
    extra = (", " + args.scan_args) if args.scan_args else ""
    limit = size_bytes(args.memory_limit)

    started = time.time()
    code, out, err, rss = run(args.duckdb, f"SELECT schema_name, table_name, row_count FROM mssql_backup_tables({paths}) "
                                           "WHERE decodable ORDER BY schema_name, table_name;")
    if code != 0:
        sys.exit(f"scan_memory: mssql_backup_tables failed:\n{err}{out}")
    tables = json.loads(out) if out.strip() else []
    print(f"scan_memory: the backup opened and its tables listed in {time.time() - started:.1f} s, "
          f"peak RSS {rss / 2**20:.0f} MiB")
    failures = 0
    hashes = {}
    for t in tables:
        schema, table = t["schema_name"], t["table_name"]
        with tempfile.TemporaryDirectory() as tmp:
            profile = os.path.join(tmp, "profile.json")
            scan = f"mssql_backup_scan({paths}, {sql_string(schema)}, {sql_string(table)}{extra})"
            script = (f"SET memory_limit = {sql_string(args.memory_limit)};\n"
                      f"SET enable_profiling = 'json';\nSET profiling_output = {sql_string(profile)};\n"
                      f"SELECT count(*) AS n, sum(hash(x))::VARCHAR AS h FROM (SELECT *, _mssql_rid FROM {scan}) x;\n")
            started = time.time()
            code, out, err, rss = run(args.duckdb, script)
            seconds = time.time() - started
            if code != 0 or not os.path.exists(profile):
                failures += 1
                print(f"FAIL {schema}.{table}: {(err or out).strip()[:400]}")
                continue
            system = json.load(open(profile)).get("system", {})
            peak = system.get("peak_buffer_memory", -1)
            temp = system.get("peak_temp_dir_size", -1)
            result = json.loads(out)[0] if out.strip() else {}
            rows = result.get("n")
        hashes[f"{schema}.{table}"] = [rows, result.get("h")]
        ok = 0 <= peak < limit and rows is not None and (t["row_count"] is None or rows == t["row_count"])
        failures += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {schema}.{table}: {rows} rows (catalog {t['row_count']}) in {seconds:.1f} s, "
              f"peak buffer memory {peak / 2**20:.0f} MiB of {limit / 2**20:.0f} MiB, peak temp "
              f"{temp / 2**20:.0f} MiB, peak RSS {rss / 2**20:.0f} MiB")
    if args.hashes:
        with open(args.hashes, "w") as f:
            json.dump(hashes, f, indent=1, sort_keys=True)
    print(f"scan_memory: {len(tables)} tables, {failures} failed")
    sys.exit(1 if failures or not tables else 0)


if __name__ == "__main__":
    main()

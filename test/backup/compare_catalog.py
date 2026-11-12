#!/usr/bin/env python3
"""Spec 085 PR 2b: mssql_backup_tables / mssql_backup_views against the server's own catalog.

    python3 -I test/backup/compare_catalog.py --duckdb build/release/duckdb /tmp/bak/2022
    python3 -I test/backup/compare_catalog.py --duckdb build/release/duckdb s3://bucket/oracle/<key>/2019

DIR is compare_info.py's (a directory or an s3:// prefix, a DONE file naming the run's subdirectory
in the lane's layout); the sets compared are those with a golden <set>.tables.txt / .columns.txt /
.views.txt beside them (test/backup/gen/catalog_golden.sql, run by generate.sh right after the set's
backup). Per table: schema, name, object id, shape, partitions, rows, created and modified (to the
millisecond: the server rounds a datetime's 1/300 s ticks to it, TYPE mssql reads them truncated
to the microsecond); per column: id, name, type name (sql_type's base, or an alias type's name),
system type id, length, precision, scale, collation, nullable, identity, computed, sparse; per view:
object id, encrypted, and the definition's length and SHA-256 over UTF-16LE (sys.sql_modules'
nvarchar). mssql_backup_catalog: every base table it lists read whole, its count the rowset's, and the
sysobjvalues rows it serves exactly <set>.modules.txt's (DQ-11: no module source). Partitioning (PR 2c):
per partitioned table its function, scheme, column, parameter type, side, fanout and NEXT USED, every
boundary (as the server prints it, a datetimeoffset as its UTC instant), and every table's partitions
(filegroup, rows, compression). A full set with no
golden (one taken under a workload) is opened too: mssql_backup_tables lists it or refuses by name. Exits 1 on any
difference, or when fewer than --min-fields fields were compared.
"""
import argparse
import hashlib
import importlib.util
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta

_spec = importlib.util.spec_from_file_location(
    "compare_info", os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare_info.py"))
compare_info = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(compare_info)
sql_string, DuckDB = compare_info.sql_string, compare_info.DuckDB


def try_query(self, statement):
    """compare_info's DuckDB.query, but a failure is (False, its message) rather than an exit."""
    script = self.preamble + ".mode json\n" + statement.rstrip().rstrip(";") + ";\n"
    run = subprocess.run([self.binary, "-no-agent", "-bail"], input=script, capture_output=True, text=True)
    return run.returncode == 0, (run.stdout if run.returncode == 0 else run.stderr + run.stdout)


DuckDB.try_query = try_query


def parse_golden(text):
    """compare_info's, with sqlcmd's "(N rows affected)" lines dropped (a golden written without NOCOUNT)."""
    return compare_info.parse_golden(
        "\n".join(line for line in text.splitlines() if not re.fullmatch(r"\(\d+ rows? affected\)", line.strip())))


class Comparison:
    def __init__(self):
        self.fields = 0
        self.differences = []
        self.skipped = []

    def skip(self, what):
        self.skipped.append(what)

    def check(self, where, field, ours, theirs):
        self.fields += 1
        if ours != theirs:
            self.differences.append(f"{where}: {field}: reader {ours!r}, server {theirs!r}")


def millis(ts):
    """A DuckDB TIMESTAMP's text rounded to the millisecond, as CONVERT(varchar(23), d, 121) prints it."""
    if ts is None:
        return None
    t = datetime.strptime(ts if "." in ts else ts + ".0", "%Y-%m-%d %H:%M:%S.%f") + timedelta(microseconds=500)
    return t.strftime("%Y-%m-%d %H:%M:%S.") + f"{t.microsecond // 1000:03d}"


def flag(v):
    return None if v is None else ("1" if v in ("true", "1") else "0")


def keyed(rows, *names):
    out = {}
    for r in rows:
        k = tuple(r[n] for n in names)
        if k in out:
            sys.exit(f"compare_catalog: {k} twice")
        out[k] = r
    return out


def compare_keys(c, what, ours, theirs):
    c.fields += 1
    if set(ours) != set(theirs):
        c.differences.append(f"{what}: reader only {sorted(set(ours) - set(theirs))}, "
                             f"server only {sorted(set(theirs) - set(ours))}")


def fraction7(text):
    """A 'YYYY-MM-DD hh:mm:ss[.f]' or 'hh:mm:ss[.f]' with its fraction padded to 7 digits."""
    head, _, frac = text.partition(".")
    return head + "." + (frac + "0000000")[:7]


def blob_hex(text):
    """DuckDB's BLOB text (printable bytes as themselves, the others as \\xNN) as 0x hex, upper case."""
    out, i = [], 0
    while i < len(text):
        if text.startswith("\\x", i):
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        else:
            out.append(ord(text[i]))
            i += 1
    return "0x" + "".join(f"{b:02X}" for b in out)


def boundary_text(base_type, ours):
    """Our boundary (a VARIANT's text, TimeZone UTC) as catalog_golden.sql prints the server's."""
    if ours is None:
        return None
    if base_type == "datetime":
        return millis(ours)
    if base_type in ("datetime2", "smalldatetime", "time"):
        text = fraction7(ours)
        return text[:23] if base_type == "smalldatetime" else text
    if base_type == "datetimeoffset":
        return ours  # compared as instants below
    if base_type in ("varbinary", "binary"):
        return blob_hex(ours)
    if base_type in ("char", "nchar"):
        return ours.rstrip(" ")  # compared without the padding (the golden's is right-padded)
    if base_type == "bit":
        return {"true": "1", "false": "0"}.get(ours, ours)
    return ours


def utc_instant(text):
    """'YYYY-MM-DD hh:mm:ss[.f] +hh:mm' (the server) or '... +00' / '+00:00' (DuckDB, UTC) as UTC."""
    m = re.fullmatch(r"(\S+ \S+?)\s*([+-])(\d\d)(?::?(\d\d))?", text.strip())
    stamp, sign, hh, mm = m.group(1), m.group(2), int(m.group(3)), int(m.group(4) or 0)
    t = datetime.strptime(fraction7(stamp)[:26], "%Y-%m-%d %H:%M:%S.%f")
    offset = timedelta(hours=hh, minutes=mm) * (1 if sign == "+" else -1)
    return (t - offset).strftime("%Y-%m-%d %H:%M:%S.%f") + fraction7(stamp)[26:]


def declaration(type_name, max_length, precision, scale):
    """The golden's parameter type as SqlTypeText declares it (a system type with its arguments)."""
    n = int(max_length)
    if type_name in ("varbinary", "varchar", "binary", "char"):
        return f"{type_name}({'max' if n < 0 else n})"
    if type_name in ("nvarchar", "nchar"):
        return f"{type_name}({'max' if n < 0 else n // 2})"
    if type_name in ("decimal", "numeric"):
        return f"{type_name}({precision},{scale})"
    if type_name in ("time", "datetime2", "datetimeoffset"):
        return f"{type_name}({scale})"
    return type_name


def compare_partitioning(c, db, name, arg, texts):
    """partitioning / boundaries / partitions against <set>.partitioning / .boundaries / .partitions."""
    ours = keyed(db.query(
        "SELECT schema_name, table_name, partitioning.function_name, partitioning.scheme_name, "
        "partitioning.column_name, partitioning.column_id::VARCHAR AS column_id, partitioning.parameter_type, "
        "partitioning.range_right::VARCHAR AS range_right, partition_count::VARCHAR AS fanout, "
        "partitioning.next_used, partitioning.reason, partitioning.destinations::VARCHAR[] AS destinations "
        f"FROM mssql_backup_tables({arg}) WHERE partitioning IS NOT NULL"), "schema_name", "table_name")
    theirs = keyed(parse_golden(texts[f"{name}.partitioning.txt"]), "schema_name", "table_name")
    compare_keys(c, f"{name}: partitioned tables", ours, theirs)
    for k in sorted(set(ours) & set(theirs)):
        o, t = ours[k], theirs[k]
        where = f"{name}: {'.'.join(k)} partitioning"
        for field in ("function_name", "scheme_name", "column_name", "column_id", "fanout", "next_used"):
            c.check(where, field, o[field], t[field])
        c.check(where, "parameter_type", o["parameter_type"],
                declaration(t["parameter_type"], t["max_length"], t["precision"], t["scale"]))
        c.check(where, "range_right", flag(o["range_right"]), t["range_right"])
        c.check(where, "reason", o["reason"], None)

    # Each partition's destination in the scheme against the server's (sys.destination_data_spaces).
    filegroups = {}
    for r in parse_golden(texts[f"{name}.partitions.txt"]):
        filegroups.setdefault((r["schema_name"], r["table_name"]), {})[int(r["partition_number"])] = r["filegroup"]
    for k in sorted(set(ours) & set(theirs)):
        expected = [filegroups.get(k, {}).get(i) for i in range(1, int(theirs[k]["fanout"]) + 1)]
        c.check(f"{name}: {'.'.join(k)} partitioning", "destinations", ours[k]["destinations"], expected)

    golden = parse_golden(texts[f"{name}.boundaries.txt"])
    base_types = {(r["schema_name"], r["table_name"], r["boundary_id"]): r["base_type"] for r in golden}
    theirs = {(r["schema_name"], r["table_name"], r["boundary_id"]): r["value_text"] for r in golden}
    ours = {}
    for r in db.query(
            "SET TimeZone = 'UTC'; SELECT schema_name, table_name, i::VARCHAR AS i, b::VARCHAR AS b FROM "
            "(SELECT schema_name, table_name, unnest(partitioning.boundaries) AS b, "
            "generate_subscripts(partitioning.boundaries, 1) AS i "
            f"FROM mssql_backup_tables({arg}) WHERE partitioning IS NOT NULL)"):
        key = (r["schema_name"], r["table_name"], r["i"])
        ours[key] = boundary_text(base_types.get(key), r["b"])
    compare_keys(c, f"{name}: boundaries", ours, theirs)
    for k in sorted(set(ours) & set(theirs)):
        o, t = ours[k], theirs[k]
        if base_types[k] == "datetimeoffset":
            o, t = utc_instant(o), utc_instant(t)
        elif base_types[k] in ("char", "nchar"):
            t = t.rstrip(" ")
        elif base_types[k] in ("float", "real"):
            o, t = float(o), float(t)  # the same double, whatever its notation
        c.check(f"{name}: {'.'.join(k[:2])} boundary {k[2]}", base_types[k], o, t)

    ours = keyed(db.query(
        "SELECT schema_name, table_name, p.partition_number::VARCHAR AS partition_number, p.filegroup, "
        "p.rows::VARCHAR AS rows, upper(p.compression) AS compression, shape FROM "
        f"(SELECT schema_name, table_name, shape, unnest(partitions) AS p FROM mssql_backup_tables({arg}))"),
        "schema_name", "table_name", "partition_number")
    theirs = keyed(parse_golden(texts[f"{name}.partitions.txt"]), "schema_name", "table_name", "partition_number")
    compare_keys(c, f"{name}: partitions", ours, theirs)
    for k in sorted(set(ours) & set(theirs)):
        for field in ("filegroup", "rows", "compression"):
            if field == "rows" and ours[k]["shape"] == "columnstore":
                c.skip(f"{name}: {'.'.join(k)} rows (a clustered columnstore: tombstone delta stores, spec 088)")
                continue
            c.check(f"{name}: {'.'.join(k)}", field, ours[k][field], theirs[k][field])


# sys.types' names of the system types by system_type_id: the type an alias is declared over
# (mssql_backup_columns' system_type, task 16d). CLR types (240) are named by their user type, not here.
SYSTEM_TYPE_NAMES = {
    34: "image", 35: "text", 36: "uniqueidentifier", 40: "date", 41: "time", 42: "datetime2",
    43: "datetimeoffset", 48: "tinyint", 52: "smallint", 56: "int", 58: "smalldatetime", 59: "real",
    60: "money", 61: "datetime", 62: "float", 98: "sql_variant", 99: "ntext", 104: "bit", 106: "decimal",
    108: "numeric", 122: "smallmoney", 127: "bigint", 165: "varbinary", 167: "varchar", 173: "binary",
    175: "char", 189: "timestamp", 231: "nvarchar", 239: "nchar", 241: "xml",
}


def compare_set(c, db, name, arg, texts):
    tables = db.query("SELECT schema_name, table_name, object_id::VARCHAR AS object_id, shape, "
                      "partition_count::VARCHAR AS partition_count, row_count::VARCHAR AS row_count, "
                      f"created::VARCHAR AS created, modified::VARCHAR AS modified FROM mssql_backup_tables({arg})")
    columns = db.query("SELECT schema_name, table_name, c.column_id::VARCHAR AS column_id, c.name, c.sql_type, "
                       "c.system_type_id::VARCHAR AS system_type_id, c.max_length::VARCHAR AS max_length, "
                       "c.precision::VARCHAR AS precision, c.scale::VARCHAR AS scale, c.collation, "
                       "c.is_nullable::VARCHAR AS is_nullable, c.is_identity::VARCHAR AS is_identity, "
                       "c.is_computed::VARCHAR AS is_computed, c.is_sparse::VARCHAR AS is_sparse "
                       f"FROM (SELECT schema_name, table_name, unnest(columns) AS c FROM mssql_backup_tables({arg}))")
    listed = db.query("SELECT schema_name, table_name, column_id::VARCHAR AS column_id, column_name, sql_type, "
                      f"system_type FROM mssql_backup_columns({arg})")
    views = db.query("SELECT schema_name, view_name, object_id::VARCHAR AS object_id, "
                     f"is_encrypted::VARCHAR AS is_encrypted, definition, reason FROM mssql_backup_views({arg})")

    ours = keyed(tables, "schema_name", "table_name")
    theirs = keyed(parse_golden(texts[f"{name}.tables.txt"]), "schema_name", "table_name")
    compare_keys(c, f"{name}: tables", ours, theirs)
    for k in sorted(set(ours) & set(theirs)):
        o, t = ours[k], theirs[k]
        where = f"{name}: {'.'.join(k)}"
        for field in ("object_id", "shape", "partition_count", "row_count"):
            # A clustered columnstore's count includes the delta stores the tuple mover has not dropped yet
            # (tombstones): SQL Server 2022's sys.partitions counts them too, 2025's does not, and only the
            # rowgroup metadata (spec 088) tells them apart -- not compared until then (owner, #551).
            if field == "row_count" and o["shape"] == "columnstore":
                c.skip(f"{where} row_count (a clustered columnstore: tombstone delta stores, spec 088)")
                continue
            c.check(where, field, o[field], t[field])
        for field in ("created", "modified"):
            c.check(where, field, millis(o[field]), t[field])

    ours = keyed(columns, "schema_name", "table_name", "column_id")
    theirs = keyed(parse_golden(texts[f"{name}.columns.txt"]), "schema_name", "table_name", "column_id")
    compare_keys(c, f"{name}: columns", ours, theirs)
    for k in sorted(set(ours) & set(theirs), key=lambda k: (k[0], k[1], int(k[2]))):
        o, t = ours[k], theirs[k]
        where = f"{name}: {'.'.join(k)}"
        c.check(where, "name", o["name"], t["column_name"])
        c.check(where, "type_name", re.sub(r"\(.*\)$", "", o["sql_type"]), t["type_name"])
        for field in ("system_type_id", "max_length", "precision", "scale"):
            c.check(where, field, o[field], t[field])
        c.check(where, "collation", o["collation"], t["collation_name"])
        for field in ("is_nullable", "is_identity", "is_computed", "is_sparse"):
            c.check(where, field, flag(o[field]), t[field])

    # mssql_backup_columns: the same columns as mssql_backup_tables' lists, the same sql_type, and the
    # system type underneath (an alias type's base) as the server declares it.
    listing = keyed(listed, "schema_name", "table_name", "column_id")
    compare_keys(c, f"{name}: mssql_backup_columns", listing, theirs)
    for k in sorted(set(listing) & set(theirs), key=lambda k: (k[0], k[1], int(k[2]))):
        o, t = listing[k], theirs[k]
        where = f"{name}: {'.'.join(k)} (mssql_backup_columns)"
        c.check(where, "column_name", o["column_name"], t["column_name"])
        c.check(where, "sql_type", o["sql_type"], ours[k]["sql_type"] if k in ours else None)
        base = SYSTEM_TYPE_NAMES.get(int(t["system_type_id"]))
        if base is None:
            c.skip(f"{where} system_type (system type {t['system_type_id']}: a CLR or unlisted type)")
            continue
        c.check(where, "system_type", o["system_type"],
                declaration(base, t["max_length"], t["precision"], t["scale"]))

    ours = keyed(views, "schema_name", "view_name")
    theirs = keyed(parse_golden(texts[f"{name}.views.txt"]), "schema_name", "view_name")
    compare_keys(c, f"{name}: views", ours, theirs)
    for k in sorted(set(ours) & set(theirs)):
        o, t = ours[k], theirs[k]
        where = f"{name}: view {'.'.join(k)}"
        c.check(where, "object_id", o["object_id"], t["object_id"])
        c.check(where, "is_encrypted", flag(o["is_encrypted"]), t["is_encrypted"])
        c.check(where, "reason", o["reason"], None)
        if o["definition"] is None:
            c.check(where, "definition", (None, None), (t["definition_length"], t["definition_sha256"]))
            continue
        utf16 = o["definition"].encode("utf-16-le")
        c.check(where, "definition length", str(len(utf16) // 2), t["definition_length"])
        c.check(where, "definition SHA-256", hashlib.sha256(utf16).hexdigest().upper(), t["definition_sha256"])

    compare_partitioning(c, db, name, arg, texts)

    # mssql_backup_schemas (PR 2d): every schema sys.schemas lists, by id, with the tables and views
    # counted against the listings above. A set generated before the schemas golden existed has none.
    if f"{name}.schemas.txt" in texts:
        schemas = db.query("SELECT schema_id::VARCHAR AS schema_id, schema_name, table_count::VARCHAR AS table_count, "
                           f"view_count::VARCHAR AS view_count FROM mssql_backup_schemas({arg})")
        ours = keyed(schemas, "schema_id")
        theirs = keyed(parse_golden(texts[f"{name}.schemas.txt"]), "schema_id")
        compare_keys(c, f"{name}: schemas", ours, theirs)
        for k in sorted(set(ours) & set(theirs), key=lambda k: int(k[0])):
            o = ours[k]
            where = f"{name}: schema {k[0]}"
            c.check(where, "schema_name", o["schema_name"], theirs[k]["schema_name"])
            c.check(where, "table_count", o["table_count"],
                    str(sum(1 for t in tables if t["schema_name"] == o["schema_name"])))
            c.check(where, "view_count", o["view_count"],
                    str(sum(1 for v in views if v["schema_name"] == o["schema_name"])))

    # mssql_backup_catalog: every base table it lists reads whole (an xtype a version adds fails here),
    # its count the rowset's; and sysobjvalues serves exactly the rows the server says it may (DQ-11).
    listed = db.query(f"SELECT base_table, row_count::VARCHAR AS row_count FROM mssql_backup_catalog({arg})")
    c.check(f"{name}: mssql_backup_catalog", "lists sysschobjs", any(r["base_table"] == "sysschobjs" for r in listed),
            True)
    # Every table in one statement: one DuckDB process rather than one a table (each call still opens the backup).
    served = {r["base_table"]: r["n"] for r in db.query(" UNION ALL ".join(
        f"SELECT {sql_string(r['base_table'])} AS base_table, count(*)::VARCHAR AS n "
        f"FROM mssql_backup_catalog({arg}, {sql_string(r['base_table'])})" for r in listed))} if listed else {}
    for r in listed:
        if r["row_count"] is not None:
            c.check(f"{name}: mssql_backup_catalog {r['base_table']}", "rows", served[r["base_table"]], r["row_count"])
    # main() requires <set>.modules.txt beside .tables.txt.
    ours = {(r["valclass"], r["objid"], r["subobjid"]) for r in db.query(
        "SELECT DISTINCT valclass::VARCHAR AS valclass, objid::VARCHAR AS objid, subobjid::VARCHAR AS subobjid "
        f"FROM mssql_backup_catalog({arg}, 'sysobjvalues')")}
    theirs = {(r["valclass"], r["objid"], r["subobjid"]) for r in parse_golden(texts[f"{name}.modules.txt"])}
    c.check(f"{name}: mssql_backup_catalog sysobjvalues", "rows served (valclass, objid, subobjid)",
            sorted(ours), sorted(theirs))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dir", help="a directory or an s3:// prefix holding sets.txt")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--min-fields", type=int, default=300)
    parser.add_argument("--require-done", action="store_true",
                        help="the lane's layout: fail, naming DIR, when DIR/DONE is missing")
    args = parser.parse_args()
    base = args.dir.rstrip("/")
    db = DuckDB(args.duckdb, base)

    done = db.query(f"SELECT content FROM read_text({sql_string(base + '/DONE*')})")
    if not done and args.require_done:
        sys.exit(f"compare_catalog: not stored: no DONE under {base} (its generate leg failed or never ran)")
    if done:
        run_dir = done[0]["content"].strip()
        if not run_dir or "/" in run_dir or run_dir.startswith("."):
            sys.exit(f"compare_catalog: {base}/DONE names {run_dir!r}, not a subdirectory")
        base = f"{base}/{run_dir}"
    texts = {os.path.basename(r["filename"]): r["content"]
             for r in db.query(f"SELECT filename, content FROM read_text({sql_string(base + '/*.txt')})")}
    if "sets.txt" not in texts:
        sys.exit(f"compare_catalog: no sets.txt in {base}")
    sets = [line.split("|") for line in texts["sets.txt"].splitlines() if line.strip()]

    c = Comparison()
    compared = 0
    for name, file_list in sets:
        if f"{name}.tables.txt" not in texts:
            # No golden (a backup taken under a workload, a differential, a log): a full's catalog is
            # still opened -- it lists (a page of sysobjvalues / sysseobjvalues that became live while
            # the backup ran named on the objects that need it), or is refused by name for such a page
            # of a base table the model is built from --, never a crash or an unnamed error.
            arg = "[" + ", ".join(sql_string(f"{base}/{f}") for f in file_list.split(",")) + "]"
            kind = db.query(f"SELECT backup_type FROM mssql_backup_info({arg})")[0]["backup_type"]
            if kind == "full":
                for function in ("mssql_backup_tables", "mssql_backup_views"):
                    ok, out = db.try_query(f"SELECT count(*)::VARCHAR AS n FROM {function}({arg})")
                    # A refusal for a page of sysobjvalues / sysseobjvalues would be a regression: the
                    # model names those on the objects that need them.
                    refused = ("became live while the backup ran" in out and "(reading sysobjvalues)" not in out
                               and "(reading sysseobjvalues)" not in out)
                    c.check(f"{name}: {function} (no golden)", "lists or refuses by name", ok or refused, True)
            continue
        for kind in ("columns", "views", "modules", "partitioning", "boundaries", "partitions"):
            if f"{name}.{kind}.txt" not in texts:
                sys.exit(f"compare_catalog: {name}.{kind}.txt missing in {base}")
        files = [f"{base}/{f}" for f in file_list.split(",")]
        compare_set(c, db, name, "[" + ", ".join(sql_string(f) for f in files) + "]", texts)
        compared += 1

    for d in c.differences:
        print(d)
    if c.skipped:
        print(f"compare_catalog: {len(c.skipped)} fields not compared, by rule: " + "; ".join(sorted(set(
            s.split(":", 1)[1].strip() for s in c.skipped))))
    print(f"compare_catalog: {compared} backup sets, {c.fields} fields compared, "
          f"{len(c.differences)} differences ({base})")
    if c.differences:
        sys.exit(1)
    if c.fields < args.min_fields:
        sys.exit(f"compare_catalog: only {c.fields} fields compared, fewer than --min-fields {args.min_fields}")


if __name__ == "__main__":
    main()

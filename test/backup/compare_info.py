#!/usr/bin/env python3
"""Spec 085 acceptance 2: mssql_backup_info against RESTORE HEADERONLY / LABELONLY / FILELISTONLY.

    python3 -I test/backup/compare_info.py --duckdb build/release/duckdb test/data/backup
    python3 -I test/backup/compare_info.py --duckdb build/release/duckdb s3://bucket/oracle/<key>/2019

DIR holds what test/backup/gen/generate.sh writes: the backups, sets.txt (name|file[,file]), server.txt
and the golden <set>.headeronly.txt / .labelonly.txt / .filelistonly.txt (sqlcmd -s '|' -W) -- or,
as the oracle lane stores them, a DONE file naming the subdirectory that holds them (one run's upload,
so two runs storing one version never mix their files). Everything is
read through DuckDB -- the golden text by read_text, the backups by mssql_backup_info -- so a local
directory and an s3:// prefix (httpfs, linked into the oracle build) take the same path. For s3://
the credentials come from MSSQL_R2_ACCESS_KEY, MSSQL_R2_ACCESS_SECRET_KEY and MSSQL_R2_URL, and
reach DuckDB on its stdin, never its command line.

Every field mssql_backup_info reports as located is compared with the server's; the fields it does
not locate yet must be NULL (never guessed). Exits 1 on any difference, or when fewer than
--min-fields fields were compared (a run that compares nothing is not a pass).
"""
import argparse
import json
import os
import subprocess
import sys

BACKUP_TYPES = {"1": "full", "5": "differential", "2": "log"}
# Not located yet (spec 085): must be NULL.
UNLOCATED = ["page_count", "consistent", "open_transactions", "committed_after_checkpoint"]


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


class DuckDB:
    def __init__(self, binary, base):
        self.binary = binary
        self.preamble = ""
        if base.startswith("s3://"):
            try:
                key = os.environ["MSSQL_R2_ACCESS_KEY"]
                secret = os.environ["MSSQL_R2_ACCESS_SECRET_KEY"]
                url = os.environ["MSSQL_R2_URL"]
            except KeyError as e:
                sys.exit(f"compare_info: {base} needs {e.args[0]} in the environment")
            endpoint = url.split("://", 1)[-1].rstrip("/")
            # Its result row goes nowhere, so stdout holds only the statement's JSON.
            self.preamble = (".output /dev/null\n"
                             f"CREATE SECRET backup_oracle (TYPE s3, KEY_ID {sql_string(key)}, "
                             f"SECRET {sql_string(secret)}, ENDPOINT {sql_string(endpoint)}, "
                             f"REGION 'auto', URL_STYLE 'path');\n"
                             ".output\n")

    def query(self, statement):
        """The rows of one statement, every value a string (or None)."""
        script = self.preamble + ".mode json\n" + statement.rstrip().rstrip(";") + ";\n"
        run = subprocess.run([self.binary, "-no-agent", "-bail"], input=script, capture_output=True,
                             text=True)
        if run.returncode != 0:
            sys.exit(f"compare_info: duckdb failed on\n  {statement}\n{run.stderr.strip()}")
        out = run.stdout.strip()
        return json.loads(out) if out else []


def parse_golden(text):
    """sqlcmd -s '|' -W: header line, dashes, rows; 'NULL' is None."""
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) < 2:
        return []
    names = lines[0].split("|")
    rows = []
    for line in lines[2:]:
        values = line.split("|")
        if len(values) != len(names):
            sys.exit(f"compare_info: a golden row of {len(values)} values under {len(names)} columns")
        rows.append({n: (None if v == "NULL" else v) for n, v in zip(names, values)})
    return rows


class Comparison:
    def __init__(self):
        self.fields = 0
        self.differences = []

    def check(self, where, field, ours, theirs):
        self.fields += 1
        if ours != theirs:
            self.differences.append(f"{where}: {field}: mssql_backup_info {ours!r}, RESTORE {theirs!r}")


def boolean(v):
    return None if v is None else ("true" if v == "1" else "false")


def date(v):
    # HEADERONLY prints milliseconds; the MTF dates carry whole seconds.
    return None if v is None else v.split(".")[0]


def upper(v):
    return None if v is None else v.upper()


def compare_set(c, name, files, info, file_rows, header, label, filelist):
    where = name
    h = header[0]
    one = len(files) == 1

    def expect(field, theirs):
        c.check(where, field, info[field], theirs)

    expect("position", h["Position"])
    expect("backup_type", BACKUP_TYPES.get(h["BackupType"], h["BackupType"]))
    expect("is_copy_only", boolean(h["IsCopyOnly"]))
    c.check(where, "backup_set_guid", upper(info["backup_set_guid"]), h["BackupSetGUID"])
    for ours, theirs in [("first_lsn", "FirstLSN"), ("last_lsn", "LastLSN"),
                         ("checkpoint_lsn", "CheckpointLSN"), ("database_backup_lsn", "DatabaseBackupLSN"),
                         ("differential_base_lsn", "DifferentialBaseLSN"), ("fork_point_lsn", "ForkPointLSN")]:
        expect(ours, h[theirs])
    for ours, theirs in [("differential_base_guid", "DifferentialBaseGUID"), ("family_guid", "FamilyGUID"),
                         ("recovery_fork_id", "RecoveryForkID"),
                         ("first_recovery_fork_id", "FirstRecoveryForkID"), ("binding_id", "BindingID")]:
        c.check(where, ours, upper(info[ours]), h[theirs])
    for ours, theirs in [("database_name", "DatabaseName"), ("server_name", "ServerName"),
                         ("user_name", "UserName"), ("machine_name", "MachineName"),
                         ("backup_name", "BackupName"), ("description", "BackupDescription"),
                         ("database_version", "DatabaseVersion"), ("compatibility_level", "CompatibilityLevel"),
                         ("collation_name", "Collation"), ("backup_size", "BackupSize"),
                         ("compressed_size", "CompressedBackupSize")]:
        expect(ours, h[theirs])
    expect("software_version",
           f"{h['SoftwareVersionMajor']}.{h['SoftwareVersionMinor']}.{h['SoftwareVersionBuild']}")
    for ours, theirs in [("database_creation_date", "DatabaseCreationDate"),
                         ("backup_start_date", "BackupStartDate"), ("backup_finish_date", "BackupFinishDate")]:
        expect(ours, date(h[theirs]))
    expect("compressed", boolean(h["Compressed"]))
    # CompressionAlgorithm is a column from 2022; before it, a compressed backup is MS_XPRESS.
    if "CompressionAlgorithm" in h:
        expect("compression_algorithm", h["CompressionAlgorithm"])
    else:
        expect("compression_algorithm", "MS_XPRESS" if h["Compressed"] == "1" else None)
    expect("encrypted", "false" if h.get("KeyAlgorithm") is None else "true")
    expect("encryptor_thumbprint", h.get("EncryptorThumbprint"))
    expect("has_backup_checksums", boolean(h["HasBackupChecksums"]))
    # sys.databases spells it BULK_LOGGED; HEADERONLY BULK-LOGGED.
    expect("recovery_model", None if h["RecoveryModel"] is None else h["RecoveryModel"].replace("-", "_"))
    for field in UNLOCATED:
        expect(field, None)

    lab = label[0]
    c.check(where, "media_set_guid", upper(info["media_set_guid"]), lab["MediaSetId"])
    expect("family_count", lab["FamilyCount"])
    # LABELONLY over a whole set prints its first family; the reader names one only for one file.
    expect("family_sequence_number", lab["FamilySequenceNumber"] if one else None)

    if len(filelist) < 2:  # a database has a data file and a log file at least
        sys.exit(f"compare_info: {name}: {len(filelist)} FILELISTONLY rows")
    c.check(where, "files (count)", len(file_rows), len(filelist))
    by_id = {f["file_id"]: f for f in file_rows}
    for row in filelist:
        f = by_id.get(row["FileId"])
        fw = f"{where} file {row['FileId']}"
        if f is None:
            c.check(fw, "present", None, row["LogicalName"])
            continue
        for ours, theirs in [("logical_name", "LogicalName"), ("physical_name", "PhysicalName"),
                             ("filegroup_name", "FileGroupName"), ("size_bytes", "Size"),
                             ("max_size_bytes", "MaxSize"), ("create_lsn", "CreateLSN"),
                             ("filegroup_id", "FileGroupId")]:
            c.check(fw, ours, f[ours], row[theirs])
        c.check(fw, "type", f["type"], {"D": "data", "L": "log"}.get(row["Type"], row["Type"]))
        c.check(fw, "unique_id", upper(f["unique_id"]), row["UniqueId"])
        c.check(fw, "backup_size_bytes", f["backup_size_bytes"], None)  # not located yet


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dir", help="a directory or an s3:// prefix holding sets.txt")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--min-fields", type=int, default=150)
    parser.add_argument("--require-done", action="store_true",
                        help="the lane's layout: fail, naming DIR, when DIR/DONE is missing")
    args = parser.parse_args()
    base = args.dir.rstrip("/")
    db = DuckDB(args.duckdb, base)

    # The lane's layout: <version>/DONE names the run's subdirectory (read through a glob, which
    # matches nothing in a plain directory rather than failing).
    done = db.query(f"SELECT content FROM read_text({sql_string(base + '/DONE*')})")
    if not done and args.require_done:
        sys.exit(f"compare_info: not stored: no DONE under {base} (its generate leg failed or never ran)")
    if done:
        run_dir = done[0]["content"].strip()
        if not run_dir or "/" in run_dir or run_dir.startswith("."):
            sys.exit(f"compare_info: {base}/DONE names {run_dir!r}, not a subdirectory")
        base = f"{base}/{run_dir}"
    texts = {os.path.basename(r["filename"]): r["content"]
             for r in db.query(f"SELECT filename, content FROM read_text({sql_string(base + '/*.txt')})")}
    if "sets.txt" not in texts:
        sys.exit(f"compare_info: no sets.txt in {base}")
    sets = [line.split("|") for line in texts["sets.txt"].splitlines() if line.strip()]

    c = Comparison()
    for name, file_list in sets:
        files = [f"{base}/{f}" for f in file_list.split(",")]
        arg = "[" + ", ".join(sql_string(f) for f in files) + "]"
        info = db.query(f"SELECT COLUMNS(* EXCLUDE (files))::VARCHAR FROM mssql_backup_info({arg})")
        file_rows = db.query("SELECT COLUMNS(*)::VARCHAR FROM (SELECT unnest(files, recursive := true) "
                             f"FROM mssql_backup_info({arg}))")
        golden = {}
        for kind in ("headeronly", "labelonly", "filelistonly"):
            text = texts.get(f"{name}.{kind}.txt")
            if text is None:
                sys.exit(f"compare_info: {name}.{kind}.txt missing in {base}")
            golden[kind] = parse_golden(text)
        if len(info) != 1 or len(golden["headeronly"]) != 1 or not golden["labelonly"]:
            sys.exit(f"compare_info: {name}: {len(info)} info rows, {len(golden['headeronly'])} HEADERONLY rows")
        compare_set(c, name, files, info[0], file_rows, golden["headeronly"], golden["labelonly"],
                    golden["filelistonly"])
        # The server that made the set (generate.sh's server.txt): catches an image of images.txt
        # labelled with another version, or a set stored under the wrong one.
        if "server.txt" in texts:
            c.check(name, "software_version (server.txt)", info[0]["software_version"],
                    ".".join(texts["server.txt"].strip().split(".")[:3]))

    for d in c.differences:
        print(d)
    print(f"compare_info: {len(sets)} backup sets, {c.fields} fields compared, "
          f"{len(c.differences)} differences ({base})")
    if c.differences:
        sys.exit(1)
    if c.fields < args.min_fields:
        sys.exit(f"compare_info: only {c.fields} fields compared, fewer than --min-fields {args.min_fields}")


if __name__ == "__main__":
    main()

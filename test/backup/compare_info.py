#!/usr/bin/env python3
"""Spec 085 acceptance 2: mssql_backup_info against RESTORE HEADERONLY / LABELONLY / FILELISTONLY.

    python3 -I test/backup/compare_info.py --duckdb build/release/duckdb --expect-major 17 test/data/backup
    python3 -I test/backup/compare_info.py --duckdb build/release/duckdb --expect-major 15 s3://bucket/oracle/<key>/2019

DIR holds what test/backup/gen/generate.sh writes: the backups, sets.txt (name|file[,file]), server.txt
and the golden <set>.headeronly.txt / .labelonly.txt / .filelistonly.txt (sqlcmd -s '|' -W) -- or,
as the oracle lane stores them, a DONE file naming the subdirectory that holds them (one run's upload,
so two runs storing one version never mix their files). Everything is
read through DuckDB -- the golden text by read_text, the backups by mssql_backup_info -- so a local
directory and an s3:// prefix (httpfs, linked into the oracle build) take the same path. For s3://
the credentials come from MSSQL_R2_ACCESS_KEY, MSSQL_R2_ACCESS_SECRET_KEY and MSSQL_R2_URL, and
reach DuckDB on its stdin, never its command line; nothing this script prints carries them.

Every column mssql_backup_info returns is accounted for: compared with the server's field, or listed
in UNLOCATED (not located yet: must be NULL, never guessed) or NOT_COMPARED (with the reason RESTORE
has nothing to compare it with); a column in none of them fails the run by name. --expect-major N
(the version the lane asked for) asserts the backups' software_version, server.txt and the sample
sets a server of that version makes (samples_sets). Exits 1 on any difference, or when fewer than
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
# Returned, and compared with nothing, for the reason given.
NOT_COMPARED = {
    "log_tail_bytes": "not compared: RESTORE prints no such field (HEADERONLY has no log tail length)",
}
# The same for the fields of each `files` entry.
FILE_UNLOCATED = ["backup_size_bytes"]
FILE_NOT_COMPARED = {}

# The sets test/backup/gen/samples.sql makes, in the order generate.sh lists them in sets.txt, each
# with the first major version that makes it: ZSTD is asked for from 2025 (17). The one statement
# of what a version's samples are; generate.sh's sets=(...) is its other copy, and says so.
SAMPLE_SETS = [("full", 0), ("full_xpress", 0), ("full_zstd", 17), ("stripe", 0), ("stripe_zstd", 17),
               ("diff", 0), ("log", 0)]


def samples_sets(major):
    """The sample sets a server of this major version makes, in sets.txt's order."""
    return [name for name, since in SAMPLE_SETS if major >= since]


def sql_string(s):
    return "'" + s.replace("'", "''") + "'"


class DuckDB:
    def __init__(self, binary, base):
        self.binary = binary
        self.preamble = ""
        self.secrets = []
        if base.startswith("s3://"):
            try:
                key = os.environ["MSSQL_R2_ACCESS_KEY"]
                secret = os.environ["MSSQL_R2_ACCESS_SECRET_KEY"]
                url = os.environ["MSSQL_R2_URL"]
            except KeyError as e:
                sys.exit(f"compare_info: {base} needs {e.args[0]} in the environment")
            # Every form a value may take in what DuckDB echoes: as given and as quoted here.
            self.secrets = [v for s in (key, secret) if s for v in {s, s.replace("'", "''")}]
            endpoint = url.split("://", 1)[-1].rstrip("/")
            # Its result row goes nowhere, so stdout holds only the statement's JSON. One option a
            # line: DuckDB's parser error echoes the line it stopped on, which is then not the key's.
            self.preamble = (".output /dev/null\n"
                             "CREATE SECRET backup_oracle (TYPE s3,\n"
                             f"KEY_ID {sql_string(key)},\n"
                             f"SECRET {sql_string(secret)},\n"
                             f"ENDPOINT {sql_string(endpoint)},\n"
                             "REGION 'auto', URL_STYLE 'path');\n"
                             ".output\n")

    def mask(self, text):
        """text with every credential value replaced by ***."""
        for s in sorted(self.secrets, key=len, reverse=True):
            text = text.replace(s, "***")
        return text

    def query(self, statement):
        """The rows of one statement, every value a string (or None)."""
        script = self.preamble + ".mode json\n" + statement.rstrip().rstrip(";") + ";\n"
        run = subprocess.run([self.binary, "-no-agent", "-bail"], input=script, capture_output=True,
                             text=True)
        if run.returncode != 0:
            message = run.stderr.strip()
            if self.secrets:
                # With credentials in the script, only the error's first line, masked: the context
                # lines DuckDB prints under it quote the script, and may cut a value where no mask
                # finds it.
                message = next((line for line in message.splitlines() if line.strip()), "(no message)")
            sys.exit(f"compare_info: duckdb failed on\n  {self.mask(statement)}\n{self.mask(message)}")
        out = run.stdout.strip()
        return json.loads(out) if out else []


def parse_golden(text):
    """sqlcmd -s '|' -W: header line, dashes, rows; 'NULL' is None.

    Only the literal NULL: sqlcmd prints a SQL NULL as NULL and an empty string as an empty field,
    and the two are different answers of the server. The reader maps an empty MTF string to NULL
    (Text() in backup_info_function.cpp), so a golden empty field compared with it is reported as a
    difference -- the server telling apart what the reader folds together -- rather than folded here
    too, which would hide it.
    """
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
        self.columns = set()  # mssql_backup_info's columns a comparison read
        self.file_columns = set()  # and the fields of its `files` entries

    def check(self, where, field, ours, theirs):
        self.fields += 1
        if ours != theirs:
            self.differences.append(f"{where}: {field}: mssql_backup_info {ours!r}, RESTORE {theirs!r}")


def column(row, name, where, seen):
    """row[name], recorded as compared; a column the reader no longer returns fails by name."""
    if name not in row:
        sys.exit(f"compare_info: {where}: mssql_backup_info returns no {name!r} (renamed? update compare_info.py)")
    seen.add(name)
    return row[name]


def check_coverage(kind, returned, compared, unlocated, not_compared):
    """Every returned column compared or listed with its reason; every listed one returned."""
    for name in list(unlocated) + list(not_compared):
        if name not in returned:
            sys.exit(f"compare_info: {name!r} is listed as not compared, but mssql_backup_info returns no such {kind}")
    unaccounted = sorted(set(returned) - compared - set(unlocated) - set(not_compared))
    if unaccounted:
        sys.exit(f"compare_info: {kind}s neither compared nor listed in compare_info.py's UNLOCATED / "
                 f"NOT_COMPARED: {', '.join(unaccounted)} -- compare each with RESTORE's field, or list it "
                 "with the reason it is not")


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

    def ours(field):
        return column(info, field, where, c.columns)

    def expect(field, theirs):
        c.check(where, field, ours(field), theirs)

    expect("position", h["Position"])
    expect("backup_type", BACKUP_TYPES.get(h["BackupType"], h["BackupType"]))
    expect("is_copy_only", boolean(h["IsCopyOnly"]))
    c.check(where, "backup_set_guid", upper(ours("backup_set_guid")), h["BackupSetGUID"])
    for field, theirs in [("first_lsn", "FirstLSN"), ("last_lsn", "LastLSN"),
                          ("checkpoint_lsn", "CheckpointLSN"), ("database_backup_lsn", "DatabaseBackupLSN"),
                          ("differential_base_lsn", "DifferentialBaseLSN"), ("fork_point_lsn", "ForkPointLSN")]:
        expect(field, h[theirs])
    for field, theirs in [("differential_base_guid", "DifferentialBaseGUID"), ("family_guid", "FamilyGUID"),
                          ("recovery_fork_id", "RecoveryForkID"),
                          ("first_recovery_fork_id", "FirstRecoveryForkID"), ("binding_id", "BindingID")]:
        c.check(where, field, upper(ours(field)), h[theirs])
    for field, theirs in [("database_name", "DatabaseName"), ("server_name", "ServerName"),
                          ("user_name", "UserName"), ("machine_name", "MachineName"),
                          ("backup_name", "BackupName"), ("description", "BackupDescription"),
                          ("database_version", "DatabaseVersion"), ("compatibility_level", "CompatibilityLevel"),
                          ("collation_name", "Collation"), ("backup_size", "BackupSize"),
                          ("compressed_size", "CompressedBackupSize")]:
        expect(field, h[theirs])
    expect("software_version",
           f"{h['SoftwareVersionMajor']}.{h['SoftwareVersionMinor']}.{h['SoftwareVersionBuild']}")
    for field, theirs in [("database_creation_date", "DatabaseCreationDate"),
                          ("backup_start_date", "BackupStartDate"), ("backup_finish_date", "BackupFinishDate")]:
        expect(field, date(h[theirs]))
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
    c.check(where, "media_set_guid", upper(ours("media_set_guid")), lab["MediaSetId"])
    expect("family_count", lab["FamilyCount"])
    # LABELONLY over a whole set prints its first family; the reader names one only for one file.
    expect("family_sequence_number", lab["FamilySequenceNumber"] if one else None)

    if len(filelist) < 2:  # a database has a data file and a log file at least
        sys.exit(f"compare_info: {name}: {len(filelist)} FILELISTONLY rows")
    c.check(where, "files (count)", len(file_rows), len(filelist))
    # file_id is the join key: a FILELISTONLY row with no file of its id is a difference.
    by_id = {column(f, "file_id", where, c.file_columns): f for f in file_rows}
    for row in filelist:
        f = by_id.get(row["FileId"])
        fw = f"{where} file {row['FileId']}"
        if f is None:
            c.check(fw, "present", None, row["LogicalName"])
            continue

        def of_file(field):
            return column(f, field, fw, c.file_columns)

        for field, theirs in [("logical_name", "LogicalName"), ("physical_name", "PhysicalName"),
                              ("filegroup_name", "FileGroupName"), ("size_bytes", "Size"),
                              ("max_size_bytes", "MaxSize"), ("create_lsn", "CreateLSN"),
                              ("filegroup_id", "FileGroupId")]:
            c.check(fw, field, of_file(field), row[theirs])
        c.check(fw, "type", of_file("type"), {"D": "data", "L": "log"}.get(row["Type"], row["Type"]))
        c.check(fw, "unique_id", upper(of_file("unique_id")), row["UniqueId"])
        for field in FILE_UNLOCATED:
            c.check(fw, field, of_file(field), None)


def read_sets(text, base):
    """sets.txt's (name, [file, ...]), each a plain name in base: no path, no glob, nothing hidden."""
    sets = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split("|")
        if len(fields) != 2:
            sys.exit(f"compare_info: {base}/sets.txt line {number}: {line!r} is not name|file[,file]")
        name, file_list = fields
        for part in [name] + file_list.split(","):
            if not part or "/" in part or "\\" in part or part.startswith(".") or any(ch in part for ch in "*?[]{}"):
                sys.exit(f"compare_info: {base}/sets.txt line {number}: {part!r} is not a plain name in {base}")
        if any(name == n for n, _ in sets):
            sys.exit(f"compare_info: {base}/sets.txt line {number}: set {name!r} named twice")
        sets.append((name, file_list.split(",")))
    return sets


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dir", help="a directory or an s3:// prefix holding sets.txt")
    parser.add_argument("--duckdb", default="build/release/duckdb")
    parser.add_argument("--min-fields", type=int, default=150)
    parser.add_argument("--require-done", action="store_true",
                        help="the lane's layout: fail, naming DIR, when DIR/DONE is missing")
    parser.add_argument("--expect-major", type=int, default=None,
                        help="the server version the backups must come from (14 = 2017, 15 = 2019, "
                             "16 = 2022, 17 = 2025): asserted on software_version, server.txt and the "
                             "sample sets of that version")
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
    sets = read_sets(texts["sets.txt"], base)

    if args.expect_major is not None:
        # The sample sets of that version, in order; the other fixtures' sets may stand beside them.
        sample_names = {n for n, _ in SAMPLE_SETS}
        found = [n for n, _ in sets if n in sample_names]
        expected = samples_sets(args.expect_major)
        if found != expected:
            sys.exit(f"compare_info: {base}/sets.txt names the sample sets {found}; a server of major version "
                     f"{args.expect_major} makes {expected}")
        # The server the lane started (server.txt, written by the generator): absent only in the
        # committed samples, which predate it.
        if "server.txt" in texts:
            server_major = texts["server.txt"].strip().split(".")[0]
            if server_major != str(args.expect_major):
                sys.exit(f"compare_info: {base}/server.txt says {texts['server.txt'].strip()!r}, not major "
                         f"version {args.expect_major}: the image is another version's")
        elif done:
            sys.exit(f"compare_info: no server.txt in {base} (the generator writes one)")

    c = Comparison()
    returned_columns = set()
    returned_file_columns = set()
    for name, file_list in sets:
        files = [f"{base}/{f}" for f in file_list]
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
        returned_columns |= set(info[0])
        for f in file_rows:
            returned_file_columns |= set(f)
        compare_set(c, name, files, info[0], file_rows, golden["headeronly"], golden["labelonly"],
                    golden["filelistonly"])
        # The backup's trailer and the server that made it (server.txt) agree: a check of the
        # reader's version field. That the server is the version asked for is --expect-major's.
        if "server.txt" in texts:
            c.check(name, "software_version (server.txt)", info[0]["software_version"],
                    ".".join(texts["server.txt"].strip().split(".")[:3]))
        if args.expect_major is not None:
            c.check(name, "software_version major (--expect-major)",
                    (info[0]["software_version"] or "").split(".")[0], str(args.expect_major))

    if sets:
        check_coverage("column", returned_columns, c.columns, UNLOCATED, NOT_COMPARED)
        if returned_file_columns:
            check_coverage("files field", returned_file_columns, c.file_columns, FILE_UNLOCATED,
                           FILE_NOT_COMPARED)

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

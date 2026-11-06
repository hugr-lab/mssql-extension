#!/usr/bin/env python3
"""Spec 091 D2: ask a SQL Server how it reads every byte of every code page it stores
varchar in, and how each collation maps to a code page.

The answers are the source of truth for src/codec/code_page/ (emit_tables.py turns them
into C++). Nothing here comes from iconv, ICU or Python's codecs, which differ from the
server in places (110 single bytes, every invalid double-byte sequence, 249 cp950 pairs).

    MSSQL_TEST_DSN='Server=...;...' python3 tools/code_pages/generate.py
    make code-pages-generate          # the same, with the Makefile's test DSN

How a byte is read under a code page: a varchar made from binary takes the DATABASE's
default collation, so each code page gets a scratch database whose default collation
uses it, and the probe runs in that database's context (EXEC <db>.sys.sp_executesql).
A COLLATE clause would not do: it CONVERTS the characters from the source page
(measured on SQL Server 2025: 0x82A0 under Japanese_CI_AS, made in a 1252 database,
reads '??', and 0xE4 under Cyrillic_General_CI_AS reads 'a').

Each page is probed under two collations of different kinds or versions (Windows and
SQL_, _90 / _100 / _140), each in its own scratch database: the emitter asserts that
they read every byte and pair alike, i.e. that the reading belongs to the code page and
not to the collation the tables are later looked up by. The scratch databases are
dropped when the run ends, failed or not (KEEP_DATABASES=1 keeps them).

Output in tools/code_pages/data/, sorted and without timestamps, so a rerun against the
same server build gives an empty diff in every data file; provenance.txt alone records
the run. The run writes into a temporary directory and moves the files into place only
when every one has the rows it should, so a failed run leaves the committed data as it was:
  cp<NNN>.psv          in_hex|out_utf16le_hex: every single byte and, for the double-byte
                       pages, every pair whose first byte is >= 0x80
  cp<NNN>.alt.psv      the same under the page's second collation (evidence: the emitter
                       compares it with cp<NNN>.psv and emits nothing from it; not
                       committed, so provenance.txt records the comparison)
  cp<NNN>.strings.psv  double-byte pages: ~400 pseudo-random strings of 3-12 bytes
                       (seeded, the same on every run), so the unit test checks the kernel
                       against whole strings the server read, not only against the model
                       of pairs the emitter derives
  collations.psv       name|collation_id|lcid|code_page|version, from sys.fn_helpcollations()
  provenance.txt       the run, kept out of the tables: the server build, the date, the
                       generator's commit, how many pages' second collation read alike,
                       and each page's two collations
"""
import datetime
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]

# Two collations per code page that can be a database default (Unicode-only ones cannot):
# the primary one, and a second of another kind (Windows vs SQL_) or version.
PAGES = {
    437: ("SQL_Latin1_General_CP437_CI_AS", "SQL_Latin1_General_CP437_BIN"),
    850: ("SQL_Latin1_General_CP850_CI_AS", "SQL_Scandinavian_CP850_CI_AS"),
    874: ("Thai_CI_AS", "Thai_100_CI_AS"),
    932: ("Japanese_CI_AS", "Japanese_XJIS_140_CI_AS"),
    936: ("Chinese_PRC_CI_AS", "Chinese_PRC_90_CI_AS"),
    949: ("Korean_Wansung_CI_AS", "Korean_90_CI_AS"),
    950: ("Chinese_Taiwan_Stroke_CI_AS", "Chinese_Taiwan_Stroke_90_CI_AS"),
    1250: ("Czech_CI_AS", "SQL_Czech_CP1250_CI_AS"),
    1251: ("Cyrillic_General_CI_AS", "SQL_Latin1_General_CP1251_CI_AS"),
    1252: ("Danish_Norwegian_CI_AS", "SQL_Latin1_General_CP1_CI_AS"),
    1253: ("Greek_CI_AS", "SQL_Latin1_General_CP1253_CI_AS"),
    1254: ("SQL_Latin1_General_CP1254_CI_AS", "Turkish_100_CI_AS"),
    1255: ("SQL_Latin1_General_CP1255_CI_AS", "Hebrew_100_CI_AS"),
    1256: ("SQL_Latin1_General_CP1256_CI_AS", "Arabic_100_CI_AS"),
    1257: ("SQL_Latin1_General_CP1257_CI_AS", "Lithuanian_100_CI_AS"),
    1258: ("Vietnamese_CI_AS", "Vietnamese_100_CI_AS"),
}
DOUBLE_BYTE = {932, 936, 949, 950}
STRINGS_PER_PAGE = 400

DIGITS = "WITH d AS (SELECT TOP 256 ROW_NUMBER() OVER (ORDER BY (SELECT 1)) - 1 AS v FROM sys.all_objects)"


def read_as(binary_expr, n=4):
    """T-SQL: the binary (n bytes at most) read as varchar under the database's collation,
    as UTF-16LE hex."""
    return (f"CONVERT(varchar({5 * n}), CONVERT(varbinary({2 * n}), CONVERT(nvarchar({n}), "
            f"CONVERT(varchar({n}), {binary_expr}))), 2)")


SINGLE = ("SELECT CONVERT(varchar(8), CONVERT(binary(1), v), 2) AS i, "
          f"{read_as('CONVERT(binary(1), v)')} AS o FROM d")
PAIRS = ("SELECT CONVERT(varchar(8), CONVERT(binary(2), l.v * 256 + t.v), 2) AS i, "
         f"{read_as('CONVERT(binary(2), l.v * 256 + t.v)')} AS o "
         "FROM d l CROSS JOIN d t WHERE l.v >= 128")
COLLATIONS = ("SELECT name, CONVERT(int, COLLATIONPROPERTY(name, 'CollationId')) AS collation_id, "
              "CONVERT(int, COLLATIONPROPERTY(name, 'LCID')) AS lcid, "
              "CONVERT(int, COLLATIONPROPERTY(name, 'CodePage')) AS code_page, "
              "CONVERT(int, COLLATIONPROPERTY(name, 'Version')) AS version "
              "FROM sys.fn_helpcollations()")
PROVENANCE = ("SELECT CONVERT(nvarchar(64), SERVERPROPERTY('ProductVersion')) AS version, "
              "CONVERT(nvarchar(128), SERVERPROPERTY('Edition')) AS edition")
# The script writes the server's row here; it goes into provenance.txt, not into data/.
SERVER_ROW = "server.psv"


def splitmix64(seed):
    """A fixed PRNG, so the strings are the same on every Python version."""
    mask = (1 << 64) - 1
    state = seed & mask
    while True:
        state = (state + 0x9E3779B97F4A7C15) & mask
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
        yield z ^ (z >> 31)


def random_strings(cp):
    """STRINGS_PER_PAGE distinct strings of 3-12 bytes, weighted toward what a double-byte
    page reads specially: bytes of the lead range 0x81-0xFE, trail bytes 0x40-0x7E, 0x00
    (after a lead byte it reads on its own), and the edges 0x80 / 0xA0 / 0xFD-0xFF."""
    rng = splitmix64(0x091 * 1000003 + cp)
    edges = (0x80, 0xA0, 0xFD, 0xFE, 0xFF)
    out = set()
    while len(out) < STRINGS_PER_PAGE:
        n = 3 + next(rng) % 10
        s = bytearray()
        for _ in range(n):
            r = next(rng) % 100
            v = next(rng)
            if r < 40:
                s.append(0x81 + v % 0x7E)
            elif r < 60:
                s.append(0x40 + v % 0x3F)
            elif r < 70:
                s.append(0)
            elif r < 80:
                s.append(edges[v % len(edges)])
            else:
                s.append(v % 256)
        out.add(bytes(s))
    return sorted(out)


def strings_probe(strings):
    values = ", ".join(f"(0x{s.hex().upper()})" for s in strings)
    return f"SELECT CONVERT(varchar(32), v, 2) AS i, {read_as('v', 16)} AS o FROM (VALUES {values}) AS s(v)"


def q(text):
    """Single-quote escaping, the same in T-SQL and DuckDB."""
    return text.replace("'", "''")


def redact(text, dsn):
    """Error output without the DSN (as given and as quoted into the script) or any
    password in it."""
    for secret in sorted({dsn, q(dsn)}, key=len, reverse=True):
        if secret:
            text = text.replace(secret, "<dsn>")
    text = re.sub(r"(?i)\b(password|pwd)(\s*=\s*)[^;\n]*", r"\1\2<redacted>", text)
    return re.sub(r"(://[^:/@\s]*:)[^@\s]*@", r"\1<redacted>@", text)


def database(cp, alt):
    return f"mssql_cpgen_{cp}" + ("_alt" if alt else "")


def attach(dsn):
    return f"ATTACH '{q(dsn)}' AS m (TYPE mssql);"


def exec_line(sql):
    return f"SELECT mssql_exec('m', '{q(sql)}');"


def copy_line(inner, path):
    return (f"COPY (SELECT i, o FROM mssql_scan('m', '{q(inner)}') ORDER BY i) "
            f"TO '{q(str(path))}' (HEADER false, DELIMITER '|', QUOTE '');")


def build_script(dsn, out):
    """The probes, and what each output file must hold: {file name: rows}."""
    lines, expect = [attach(dsn)], {}
    for cp, collations in PAGES.items():
        probe = f"{DIGITS} {SINGLE}" + (f" UNION ALL {PAIRS}" if cp in DOUBLE_BYTE else "")
        for alt, collation in enumerate(collations):
            db = database(cp, alt)
            name = f"cp{cp}" + (".alt" if alt else "") + ".psv"
            lines.append(exec_line(f"IF DB_ID('{db}') IS NULL CREATE DATABASE {db} COLLATE {collation}"))
            lines.append(copy_line(f"EXEC {db}.sys.sp_executesql N'{q(probe)}'", out / name))
            expect[name] = 256 + (128 * 256 if cp in DOUBLE_BYTE else 0)
        if cp in DOUBLE_BYTE:
            strings = strings_probe(random_strings(cp))
            lines.append(copy_line(f"EXEC {database(cp, False)}.sys.sp_executesql N'{q(strings)}'",
                                   out / f"cp{cp}.strings.psv"))
            expect[f"cp{cp}.strings.psv"] = STRINGS_PER_PAGE
    lines.append(f"COPY (SELECT * FROM mssql_scan('m', '{q(COLLATIONS)}') ORDER BY name) "
                 f"TO '{q(str(out / 'collations.psv'))}' (HEADER false, DELIMITER '|', QUOTE '');")
    lines.append(f"COPY (SELECT * FROM mssql_scan('m', '{q(PROVENANCE)}')) "
                 f"TO '{q(str(out / SERVER_ROW))}' (HEADER false, DELIMITER '|', QUOTE '');")
    expect["collations.psv"] = None
    expect[SERVER_ROW] = 1
    return "\n".join(lines) + "\n", expect


def cleanup_script(dsn):
    lines = [attach(dsn)]
    for cp in PAGES:
        for alt in (False, True):
            db = database(cp, alt)
            lines.append(exec_line(f"IF DB_ID('{db}') IS NOT NULL DROP DATABASE {db}"))
    return "\n".join(lines) + "\n"


def check_collations(path):
    """Every collation PAGES names exists with that code page, per the committed data."""
    if not path.exists():
        return
    known = {}
    for line in path.read_text().splitlines():
        name, _, _, cp, _ = line.split("|")
        known[name] = int(cp)
    for cp, collations in PAGES.items():
        for collation in collations:
            if known.get(collation) != cp:
                sys.exit(f"{collation} is not a code page {cp} collation in {path.name} "
                         f"(it has {known.get(collation)}): fix PAGES")


def run_duckdb(duckdb, script, bail=True):
    # The script carries the password: it goes to stdin only, never to argv or a file,
    # and nothing below prints it unredacted.
    argv = [str(duckdb), "-batch", "-noheader", "-list"] + (["-bail"] if bail else [])
    return subprocess.run(argv, input=script, text=True, capture_output=True)


def generator_commit():
    """The commit this ran from, `+modified` when the generator or emitter differs from it."""
    def git(*args):
        return subprocess.run(["git", "-C", str(REPO)] + list(args), capture_output=True, text=True)

    # Runs after the server's answers are in: no git must not throw them away.
    try:
        head = git("rev-parse", "HEAD")
        if head.returncode != 0:
            return "unknown"
        tools = ("tools/code_pages/generate.py", "tools/code_pages/emit_tables.py")
        modified = git("diff", "--quiet", "HEAD", "--", *tools).returncode != 0
    except OSError:
        return "unknown"
    return head.stdout.strip() + ("+modified" if modified else "")


def provenance(staging):
    """provenance.txt: the record of this run. The .alt.psv probes are not committed, so
    this is the only trace their check leaves in the repository (review of #527)."""
    alike = [cp for cp in PAGES
             if (staging / f"cp{cp}.alt.psv").read_bytes() == (staging / f"cp{cp}.psv").read_bytes()]
    differ = " ".join(str(cp) for cp in PAGES if cp not in alike)
    lines = [
        "server|" + (staging / SERVER_ROW).read_text().strip(),
        "date|" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "generator|" + generator_commit(),
        f"second_collation|{len(alike)} of {len(PAGES)} pages: cp<NNN>.alt.psv byte-identical "
        "to cp<NNN>.psv" + (f"; differ: {differ}" if differ else ""),
    ]
    lines += [f"page|{cp}|{first}|{second}" for cp, (first, second) in PAGES.items()]
    return "\n".join(lines) + "\n"


def main():
    dsn = os.environ.get("MSSQL_TEST_DSN")
    if not dsn:
        sys.exit("set MSSQL_TEST_DSN (the Makefile's code-pages-generate target does)")
    duckdb = pathlib.Path(os.environ.get("DUCKDB", REPO / "build/release/duckdb"))
    if not duckdb.exists():
        sys.exit(f"{duckdb} not found; run 'make' first")
    out = pathlib.Path(os.environ.get("OUT", REPO / "tools/code_pages/data"))
    check_collations(out / "collations.psv")
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cpgen-") as tmp:
        staging = pathlib.Path(tmp)
        script, expect = build_script(dsn, staging)
        try:
            run = run_duckdb(duckdb, script)
            if run.returncode != 0:
                sys.exit("generation failed:\n" + redact(run.stderr or run.stdout, dsn))
            # The CLI can exit 0 on a runtime error, so the files themselves are the check.
            for name, rows in expect.items():
                path = staging / name
                got = sum(1 for _ in path.open()) if path.exists() else 0
                if got == 0 or (rows is not None and got != rows):
                    sys.exit(f"generation failed: {name} has {got} rows, not {rows or 'some'}:\n"
                             + redact(run.stderr or run.stdout, dsn))
        finally:
            if os.environ.get("KEEP_DATABASES") != "1":
                # No -bail: every DROP is tried even when one fails.
                clean = run_duckdb(duckdb, cleanup_script(dsn), bail=False)
                if clean.returncode != 0 or clean.stderr.strip():
                    print("dropping the mssql_cpgen_* databases failed; drop them by hand:\n"
                          + redact(clean.stderr or clean.stdout, dsn), file=sys.stderr)
        (staging / "provenance.txt").write_text(provenance(staging))
        written = [name for name in expect if name != SERVER_ROW] + ["provenance.txt"]
        for name in written:
            shutil.move(str(staging / name), str(out / name))
    for name in written:
        with open(out / name) as fh:
            print(f"{name:<20} {sum(1 for _ in fh):>7} rows")


if __name__ == "__main__":
    main()

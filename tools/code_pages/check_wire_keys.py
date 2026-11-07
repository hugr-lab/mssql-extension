#!/usr/bin/env python3
"""Spec 092 D1: does the wire's collation lead to the page the collation's name does?

The tables' wire keys (LCID, UTF-8 flag, SortId -> code page) are derived from
COLLATIONPROPERTY, not read off a wire (spec 091 D9). A raw scan decodes by them, so this
asks a live server about every collation with a code page: a text made of what that page
can hold is stored under the collation, then read bare, through the wire's key, and
compared with the server's own CAST. A key that misses keeps the bytes and warns; a key
naming another page reads other characters. The text is the page's high half, so no
other page reads it alike: a single character would not tell 1250 from 1252, where the
euro sign sits at one byte.

    MSSQL_TEST_DSN='Server=...;...' python3 tools/code_pages/check_wire_keys.py
    make code-pages-check-wire

Asks nothing of the server's data and creates nothing. Exits 1 on any difference.
"""
import os
import pathlib
import sys

from generate import REPO, attach, q, redact, run_duckdb

DATA = REPO / "tools/code_pages/data"
COLUMNS_PER_STATEMENT = 200
CHARACTERS_PER_PROBE = 64


def probe_text(page):
    """Characters only this page's high half reads, as a T-SQL N'' literal's body."""
    seen, text = set(), []
    for line in (DATA / f"cp{page}.psv").read_text().splitlines():
        _, out = line.split("|")
        answer = bytes.fromhex(out).decode("utf-16-le")
        code = ord(answer) if len(answer) == 1 else 0
        # Not the C1 controls or the private use area: 1252 0x81 -> U+0081 and the like
        # read alike on every page that leaves the byte undefined.
        if code >= 0xA0 and not 0xE000 <= code <= 0xF8FF and answer not in seen and answer != "'":
            seen.add(answer)
            text.append(answer)
    # Evenly over the page, so a double-byte page's 20,000 characters are sampled.
    step = max(1, len(text) // CHARACTERS_PER_PROBE)
    return "".join(text[::step][:CHARACTERS_PER_PROBE])


def script(dsn):
    rows = [line.split("|") for line in (DATA / "collations.psv").read_text().splitlines()]
    named = [(name, int(page)) for name, _, _, page, _ in rows if int(page) not in (0, 65001)]
    texts = {page: probe_text(page) for page in {page for _, page in named}}
    lines = [attach(dsn), "SET logging_storage = 'memory';", "SET enable_logging = true;",
             "SET logging_level = 'WARNING';"]
    for start in range(0, len(named), COLUMNS_PER_STATEMENT):
        part = named[start:start + COLUMNS_PER_STATEMENT]
        select, differs = [], []
        for k, (name, page) in enumerate(part):
            stored = f"CAST(N'{texts[page]}' COLLATE {name} AS varchar(400))"
            select += [f"{stored} c{k}", f"CAST({stored} AS nvarchar(400)) n{k}"]
            differs.append(f"c{k} IS DISTINCT FROM n{k} OR n{k} IS DISTINCT FROM '{texts[page]}'")
        lines.append(f"SELECT 'differ', count(*) FROM mssql_scan('m', '{q('SELECT ' + ', '.join(select))}') "
                     f"WHERE {' OR '.join(differs)};")
    lines.append("SELECT 'warned', count(*) FROM duckdb_logs WHERE log_level = 'WARNING' AND "
                 "message LIKE '%non-UTF-8 collation%';")
    return "\n".join(lines) + "\n", len(named), len(texts)


def main():
    dsn = os.environ.get("MSSQL_TEST_DSN")
    if not dsn:
        sys.exit("set MSSQL_TEST_DSN (the Makefile's code-pages-check-wire target does)")
    duckdb = pathlib.Path(os.environ.get("DUCKDB", REPO / "build/release/duckdb"))
    if not duckdb.exists():
        sys.exit(f"{duckdb} not found; run 'make' first")
    text, collations, pages = script(dsn)
    run = run_duckdb(duckdb, text)
    answers = [line.split("|") for line in run.stdout.splitlines() if line.startswith(("differ|", "warned|"))]
    expected = (collations + COLUMNS_PER_STATEMENT - 1) // COLUMNS_PER_STATEMENT + 1
    # The CLI can exit 0 on a runtime error, so the answers themselves are the check.
    if run.returncode != 0 or len(answers) != expected:
        sys.exit(f"check failed: {len(answers)} answers, not {expected}:\n" + redact(run.stderr or run.stdout, dsn))
    bad = sum(int(count) for _, count in answers)
    print(f"{collations} collations on {pages} code pages: {bad} differ or warned")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

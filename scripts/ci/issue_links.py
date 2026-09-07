#!/usr/bin/env python3
"""Index and validate the issue -> regression-test linkage.

A test that exists to keep a fixed bug fixed declares the issue it guards in
its header:

    # name: test/sql/copy/copy_varchar_length.test
    # description: ...
    # group: [mssql]
    # issue: 181

C++ tests use the same field in their leading comment block:

    // issue: 189

Why a declared field and not a grep for "issue #NNN": a test may MENTION an
issue without guarding it, and the difference is the whole point. The clearest
example is test/sql/query/legacy_lob_types.test, a test for #197 whose prose
says of #224 "deliberately not pinned here" -- a grep-built index reports that
issue as covered when the file explicitly says it is not. A declaration cannot
be produced by accident; a prose mention can.

Modes:
    (default)     validate declarations, print a summary
    --index       print the issue -> tests table
    --issue N     print the tests guarding issue N
    --backlog     print files that mention an issue in prose but declare none

Offline by design: it never asks GitHub whether an issue exists or is closed,
so it is safe on any runner and in any sandbox.
"""
import os
import re
import sys

ROOTS = ("test/sql", "test/cpp")
SQL_FIELD = re.compile(r"^#\s*issue:\s*(.+?)\s*$", re.I)
CPP_FIELD = re.compile(r"^//\s*issue:\s*(.+?)\s*$", re.I)
NUMBERS = re.compile(r"#?(\d{1,5})")
PROSE = re.compile(r"issue\s*#?(\d{2,5})", re.I)


def header_lines(path):
    """Yield the file's header block, where a declaration must live.

    The header block is the run of comment lines at the very top of the file,
    ending at the first blank line or first non-comment line. For a .test file
    that is exactly sqllogictest's own `# name:` / `# description:` /
    `# group:` triple; for a .cpp test it is the leading banner comment.

    Deliberately narrow. If the block extended through every leading comment,
    a declaration could sit forty lines down inside a prose preamble, which is
    both easy to miss when reading and easy to add by accident when editing --
    and the whole point of a declaration is that it cannot happen by accident.
    """
    comment = "//" if path.endswith((".cpp", ".hpp")) else "#"
    with open(path, errors="replace") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or not stripped.startswith(comment):
                return
            yield line.rstrip("\n")


def declared(path):
    """Issue numbers this file declares it guards.

    Returns (numbers, errors). A present-but-unparseable field is an ERROR, not
    an absent declaration: `# issue: nonsense` would otherwise fall through to
    "declares nothing", land silently in the backlog, and read as if the author
    had never tried -- which is the failure a typo actually produces.
    """
    field = CPP_FIELD if path.endswith((".cpp", ".hpp")) else SQL_FIELD
    found, errors = [], []
    for line in header_lines(path):
        m = field.match(line.strip())
        if not m:
            continue
        nums = NUMBERS.findall(m.group(1))
        if not nums:
            errors.append("issue field names no number: %r" % line.strip())
            continue
        found.extend(int(n) for n in nums)
    return found, errors


def mentioned(path):
    """Issue numbers named anywhere in the file, declaration or not."""
    with open(path, errors="replace") as fh:
        return {int(n) for n in PROSE.findall(fh.read())}


def walk():
    for root in ROOTS:
        for dirpath, _, files in os.walk(root):
            for fn in sorted(files):
                if fn.endswith((".test", ".cpp")):
                    yield os.path.join(dirpath, fn)


def collect():
    index, backlog, bad = {}, [], []
    for path in walk():
        try:
            nums, errors = declared(path)
        except OSError as exc:
            bad.append((path, str(exc)))
            continue
        for err in errors:
            bad.append((path, err))
        for n in nums:
            if not 1 <= n <= 99999:
                bad.append((path, "issue number out of range: %d" % n))
                continue
            index.setdefault(n, []).append(path)
        if not nums and mentioned(path):
            backlog.append(path)
    return index, backlog, bad


def main(argv):
    index, backlog, bad = collect()

    if "--index" in argv:
        for n in sorted(index):
            print("#%-6d %s" % (n, ", ".join(index[n])))
        return 0

    if "--issue" in argv:
        try:
            want = int(argv[argv.index("--issue") + 1])
        except (IndexError, ValueError):
            print("--issue needs a number", file=sys.stderr)
            return 2
        hits = index.get(want)
        if not hits:
            print("no test declares itself a guard for #%d" % want)
            return 1
        for path in hits:
            print(path)
        return 0

    if "--backlog" in argv:
        for path in backlog:
            print(path)
        return 0

    for path, why in bad:
        print("ERROR %s: %s" % (path, why))
    if bad:
        return 1

    print("%d issue(s) guarded by %d declared test(s)"
          % (len(index), sum(len(v) for v in index.values())))
    if backlog:
        print("%d file(s) mention an issue without declaring one "
              "(see --backlog; not an error)" % len(backlog))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

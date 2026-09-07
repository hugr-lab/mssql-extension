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
FIELD_SHAPE = re.compile(r"#?\d+(?:\s*,\s*#?\d+)*\Z")
NUMBERS = re.compile(r"#?(\d+)")
# Plural, and the separators this tree actually uses: "issues #90",
# "issue-#89", "issue: 224". Still requires the word, which usefully
# excludes "PR #213".
PROSE = re.compile(r"issues?[\s#:,-]*(\d{2,5})", re.I)


def header_lines(path):
    """Yield the file's header block, where a declaration must live.

    The header block is the run of comment lines at the very top of the file,
    ending at the first blank line or first non-comment line. For a .test file
    that is exactly sqllogictest's own `# name:` / `# description:` /
    `# group:` triple; for a .cpp test it is the leading banner comment.

    Note this is narrower than "every leading comment" but NOT as narrow as it
    may sound: .test files in this tree routinely run their prose preamble
    straight on from the header with a bare `#` rather than a blank line, so
    the block can be long -- legacy_lob_scan.test's is 21 lines, and
    copy_nvarchar_length_validation.test's was 28 before it was normalised. The
    rule is chosen because it is simple and matches where the back-fill puts
    the field, not because it bounds the block to a few lines.
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
        value = m.group(1)
        # Validate the WHOLE field, not just scan it for digits. Scanning let
        # `# issue: 181 (see spec 057)` index 181 AND 57, and `# issue: 123456`
        # split into 12345 and 6 -- both inside the range check -- registering
        # guards for issues nobody wrote. That is precisely the "cannot be
        # produced by accident" property this field exists to have.
        if not FIELD_SHAPE.match(value):
            errors.append("issue field is not a comma-separated number list: %r" % line.strip())
            continue
        found.extend(int(n) for n in NUMBERS.findall(value))
    return found, errors


def mentioned(path):
    """Issue numbers named anywhere in the file, declaration or not."""
    with open(path, errors="replace") as fh:
        return {int(n) for n in PROSE.findall(fh.read())}


def missing_roots():
    """Roots that do not exist.

    os.walk on a missing directory yields nothing and raises nothing, so a
    moved test tree would make this print "0 issue(s) guarded" and exit 0 --
    a CI gate passing because it indexed nothing at all.
    """
    return [r for r in ROOTS if not os.path.isdir(r)]


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
    gone = missing_roots()
    if gone:
        for root in gone:
            print("ERROR missing docs/test root: %s" % root, file=sys.stderr)
        return 1

    index, backlog, bad = collect()
    # Printed in EVERY mode. Reporting them only in the default mode meant
    # `--index` after a mistyped field showed a table quietly missing that file
    # and exited 0.
    for path, why in bad:
        print("ERROR %s: %s" % (path, why), file=sys.stderr)

    if bad and ("--index" in argv or "--issue" in argv or "--backlog" in argv):
        return 1

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
            # 3, not 1: "no guard" is an answer, and a caller must be able to
            # tell it from "the index is invalid", which returns 1.
            return 3
        for path in hits:
            print(path)
        return 0

    if "--backlog" in argv:
        for path in backlog:
            print(path)
        return 0

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

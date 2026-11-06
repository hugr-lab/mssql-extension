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
import contextlib
import io
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
# `\b` so "reissue 1234" / "tissue 99" do not match, and [ \t] rather than \s
# so a line ending in the word "issue" cannot join the next line's "# 226 rows"
# into issue 226. `+` rather than `*` because a separator is what distinguishes
# a reference from a number that merely follows the word (roborev 1431).
PROSE = re.compile(r"\bissues?[ \t#:,-]+(\d{2,5})", re.I)


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


# ---------------------------------------------------------------- self-test

#: (label, the file's text, the numbers it DECLARES, how many errors)
#
# This file exists for one property -- "a declaration cannot be produced by
# accident" -- and every past defect was that property leaking. So each case is
# a leak that happened or could.
DECLARE_TESTS = [
    ("a plain declaration", "# name: x\n# issue: 181\n", [181], 0),
    ("two issues, comma separated", "# issue: 90, 91\n", [90, 91], 0),
    ("hash prefixes", "# issue: #90,#91\n", [90, 91], 0),
    ("upper case field name", "# ISSUE: 181\n", [181], 0),
    # The field is validated WHOLE, not scanned for digits. Scanning indexed
    # 181 AND 57 here, registering a guard for an issue nobody wrote.
    ("a trailing parenthetical", "# issue: 181 (see spec 057)\n", [], 1),
    ("prose instead of a number", "# issue: nonsense\n", [], 1),
    # Pins CURRENT behaviour, which is NOT what declared()'s docstring
    # promises: `(.+?)` needs a character, so a bare `# issue:` matches no
    # field at all and is treated as an ABSENT declaration rather than the
    # "present but unparseable" ERROR the docstring describes. Left as it is
    # because this PR must not change behaviour; worth deciding separately.
    ("an empty field is (today) treated as no field", "# issue:\n", [], 0),
    ("a field of only blanks, likewise", "# issue:   \n", [], 0),
    ("a range rather than a list", "# issue: 90-91\n", [], 1),
    # Not a declaration at all: no field, so nothing and no error.
    ("a prose mention in the header", "# name: x\n# guards issue #181\n", [], 0),
    ("no header at all", "SELECT 1;\n", [], 0),
]

#: (label, text, numbers PROSE finds)
#
# The prose matcher feeds the backlog, which is advisory -- but a false hit
# there sends someone to annotate a file that guards nothing.
PROSE_TESTS = [
    ("issue #181", "-- see issue #181 for why\n", {181}),
    # Only the number ADJACENT to the word is taken: "#91" has no "issue"
    # before it. Enough for the backlog, whose job is to flag the FILE.
    ("plural, the adjacent number only", "-- issues #90 and #91\n", {90}),
    ("a hyphen separator", "-- issue-#89 again\n", {89}),
    ("a colon separator", "-- issue: 224\n", {224}),
    ("a PR reference is not an issue", "-- see PR #213\n", set()),
    ("reissue is not issue", "-- reissue 1234 the token\n", set()),
    ("tissue is not issue", "-- tissue 99 paper\n", set()),
    # Without the [ \t] restriction the word could join the NEXT line's number.
    ("the word at end of line does not reach the next line",
     "-- closes the issue\n-- 226 rows\n", set()),
    ("a one-digit number is not an issue reference", "-- issue 7\n", set()),
]


def _line(ok, label, detail):
    print("  [%s] %-54s -> %s" % ("ok" if ok else "FAIL", label, detail))
    return 0 if ok else 1


def _write(path, text):
    with open(path, "w") as fh:
        fh.write(text)


def _check_declarations(tmp):
    print("what counts as a DECLARATION:")
    failures = 0
    for label, text, want_nums, want_errors in DECLARE_TESTS:
        path = os.path.join(tmp, "a.test")
        _write(path, text)
        nums, errors = declared(path)
        ok = nums == want_nums and len(errors) == want_errors
        failures += _line(ok, label, "%s, %d error(s)" % (nums or "none", len(errors)))
    return failures


def _check_cpp(tmp):
    """A .cpp test declares with `//`, and `#` must not work there."""
    print("\nthe C++ comment form:")
    failures = 0
    path = os.path.join(tmp, "a.cpp")
    _write(path, "// test_x.cpp\n// issue: 189\n")
    failures += _line(declared(path)[0] == [189], "// issue: 189 in a .cpp", "189")
    _write(path, "# issue: 189\n")
    failures += _line(declared(path)[0] == [],
                      "a # field in a .cpp is not a declaration", "none")
    return failures


def _check_header_block(tmp):
    """The block ends at the first blank or non-comment line.

    A field below that point is not a declaration, which is what stops a
    reference buried in a test's body from registering as a guard.
    """
    print("\nwhere a declaration may live:")
    failures = 0
    path = os.path.join(tmp, "a.test")
    _write(path, "# name: x\n#\n# prose on and on\n# issue: 181\n")
    failures += _line(declared(path)[0] == [181],
                      "a long header block reaches the field", "181")
    _write(path, "# name: x\n\n# issue: 181\n")
    failures += _line(declared(path)[0] == [],
                      "a blank line ends the block", "none")
    _write(path, "# name: x\nSELECT 1;\n# issue: 181\n")
    failures += _line(declared(path)[0] == [],
                      "a statement ends the block", "none")
    return failures


def _check_prose(tmp):
    print("\nwhat the PROSE matcher finds (it feeds --backlog):")
    failures = 0
    path = os.path.join(tmp, "a.test")
    for label, text, want in PROSE_TESTS:
        _write(path, text)
        got = mentioned(path)
        failures += _line(got == want, label, str(sorted(got)) if got else "none")
    return failures


def _check_collect(tmp):
    """collect()'s own layer: the range check and the backlog rule."""
    print("\ncollect() over a tree:")
    failures = 0
    global ROOTS
    keep = ROOTS
    root = os.path.join(tmp, "tree")
    os.makedirs(root, exist_ok=True)
    ROOTS = (root,)
    try:
        _write(os.path.join(root, "guard.test"), "# issue: 181\n")
        _write(os.path.join(root, "huge.test"), "# issue: 123456\n")
        _write(os.path.join(root, "prose.test"), "# name: x\n-- see issue #224\n")
        _write(os.path.join(root, "nothing.test"), "SELECT 1;\n")
        index, backlog, bad = collect()
        failures += _line(index == {181: [os.path.join(root, "guard.test")]},
                          "only the declared issue is indexed", str(sorted(index)))
        # 123456 passes FIELD_SHAPE, so the RANGE check is the layer that
        # catches it -- and it must be an error, not a silent omission.
        failures += _line(len(bad) == 1 and "out of range" in bad[0][1],
                          "a number past 99999 is an error, not a silent drop",
                          bad[0][1][:40] if bad else "NOTHING")
        failures += _line(backlog == [os.path.join(root, "prose.test")],
                          "a prose-only mention lands in the backlog", str(len(backlog)))
        failures += _line(mode_summary({181: ["x"]}, [], []) == 0,
                          "a clean summary exits 0", "0")
        # The PRINTED figure, not just the status. `declaring` counts DISTINCT
        # files, so a file declaring two issues must read "2 issue(s) guarded
        # by 1 declared test(s)" -- the roborev-1431 fix, which the first cut
        # of these fixtures claimed to pin and did not (roborev on 840952c).
        _write(os.path.join(root, "two.test"), "# issue: 90, 91\n")
        two_index, _, _ = collect()
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            mode_summary({n: p for n, p in two_index.items() if n in (90, 91)}, [], [])
        printed = buffer.getvalue().strip()
        failures += _line(printed == "2 issue(s) guarded by 1 declared test(s)",
                          "a file declaring two issues counts as ONE test", printed)
        failures += _line(mode_summary({}, [], [("f", "why")]) == 1,
                          "a bad declaration fails the summary", "1")
        failures += _line(mode_issue({181: ["x"]}, ["--issue", "999"]) == 3,
                          "an unguarded issue exits 3, not 1", "3")
        failures += _line(mode_issue({}, ["--issue"]) == 2,
                          "--issue without a number exits 2", "2")
    finally:
        ROOTS = keep
    return failures


def _check_missing_roots(tmp):
    print("\nmissing roots (a moved test tree must not pass):")
    global ROOTS
    keep = ROOTS
    ROOTS = (os.path.join(tmp, "not-there"),)
    try:
        return _line(require_roots() is True,
                     "a missing root is reported and fails", "fails")
    finally:
        ROOTS = keep


def self_test():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        failures = _check_declarations(tmp)
        failures += _check_cpp(tmp)
        failures += _check_header_block(tmp)
        failures += _check_prose(tmp)
        failures += _check_collect(tmp)
        failures += _check_missing_roots(tmp)
    print("\n%s: self-test, %d failure(s)" % ("FAIL" if failures else "PASS", failures))
    return failures


def _answerers(index, backlog, argv):
    """flag -> handler, in the old if-chain's PRECEDENCE order.

    ONE source of truth. Listing the flags in a QUERY_MODES tuple and again as
    the dispatch dict's keys meant a mode added to one and not the other either
    raised KeyError or was silently never dispatched (roborev on 840952c).
    `dict` preserves insertion order, so `next(iter(...))` still picks
    `--index` over `--backlog` exactly as the if-chain did.
    """
    return {"--index": lambda: mode_index(index),
            "--issue": lambda: mode_issue(index, argv),
            "--backlog": lambda: mode_backlog(backlog)}


def require_roots():
    """Complain about missing roots. -> True when any is gone."""
    gone = missing_roots()
    for root in gone:
        # ROOTS are RELATIVE, so the overwhelmingly likely cause is being
        # invoked from somewhere other than the repo root -- which the old
        # wording ("docs/test root") did not say, and there is no docs root
        # in ROOTS to begin with (roborev 1431).
        print("ERROR test root not found: %s (run from the repository root)" % root, file=sys.stderr)
    return bool(gone)


def report_errors(bad):
    """Printed in EVERY mode.

    Reporting them only in the default mode meant `--index` after a mistyped
    field showed a table quietly missing that file and exited 0.
    """
    for path, why in bad:
        print("ERROR %s: %s" % (path, why), file=sys.stderr)


def mode_index(index):
    for n in sorted(index):
        print("#%-6d %s" % (n, ", ".join(index[n])))
    return 0


def mode_issue(index, argv):
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


def mode_backlog(backlog):
    for path in backlog:
        print(path)
    return 0


def mode_summary(index, backlog, bad):
    if bad:
        return 1
    # DISTINCT files, not pairs: `sum(len(v) for v in index.values())` counted a
    # file declaring two issues twice, so the figure overstated how many tests
    # carry a declaration (roborev 1431).
    declaring = {path for paths in index.values() for path in paths}
    print("%d issue(s) guarded by %d declared test(s)" % (len(index), len(declaring)))
    if backlog:
        print("%d file(s) mention an issue without declaring one "
              "(see --backlog; not an error)" % len(backlog))
    return 0


def main(argv):
    if "--self-test" in argv:
        return 1 if self_test() else 0
    if require_roots():
        return 1

    index, backlog, bad = collect()
    report_errors(bad)

    answer = _answerers(index, backlog, argv)
    asked = next((mode for mode in answer if mode in argv), None)
    if asked:
        # A bad declaration makes the index incomplete, so a mode that ANSWERS
        # from it refuses rather than answering wrongly. Reporting the errors
        # only in the default mode meant `--index` after a mistyped field
        # printed a table quietly missing that file and exited 0.
        return 1 if bad else answer[asked]()
    return mode_summary(index, backlog, bad)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

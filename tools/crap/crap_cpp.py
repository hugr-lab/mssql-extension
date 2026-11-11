#!/usr/bin/env python3
"""CRAP scores for this repository's C++, from lizard's complexity and an LCOV report.

CRAP = CC² × (1 − coverage)³ + CC, the metric `crapper` applies to the languages it
reads. It does not read C++ -- no tool does, for CRAP: crap4j is Java-only -- so this
joins the two halves that do exist:

  complexity   `lizard` (github.com/terryyin/lizard), cyclomatic complexity per
               function for C/C++, read from its `--csv` output
  coverage     an LCOV report, as `llvm-cov export -format=lcov` or `gcovr --lcov`
               writes one; a function's coverage is the share of its own DA lines
               that were hit, which is how crapper scores a function elsewhere

Without `--lcov` the score is complexity only: coverage and CRAP are N/A, and the
report says what CRAP would be at 0% coverage (CC² + CC), which is the floor until
this repository grows an instrumented build.

Usage:

    python3 tools/crap/crap_cpp.py src                       # complexity only
    python3 tools/crap/crap_cpp.py --lcov coverage.lcov src  # the real thing
    python3 tools/crap/crap_cpp.py --selftest                # no lizard, no lcov

An instrumented build, for the coverage half (clang; the numbers are only as good as
the suite that runs, and `make test` alone leaves every TDS path at zero):

    cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
    cmake --build build/coverage -j
    LLVM_PROFILE_FILE="build/coverage/%p.profraw" <run the tests>
    xcrun llvm-profdata merge -sparse build/coverage/*.profraw -o build/coverage/all.profdata
    xcrun llvm-cov export -format=lcov -instr-profile=build/coverage/all.profdata \
        build/coverage/<binary> > build/coverage/coverage.lcov
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys

#: lizard's `--csv` has no header. Its column order, verified against lizard
#: 1.26.0, read here by position and then validated per row -- so a lizard that
#: changes it is a named error rather than silent nonsense:
CSV_COLUMNS = ("nloc", "ccn", "tokens", "params", "length", "location",
               "file", "function", "long_name", "start", "end")
CC, FILE, FUNCTION, START, END = 1, 6, 7, 9, 10

#: C and C++ only. lizard reads 20-odd languages; this tool exists for the one
#: crapper cannot.
SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".h", ".ipp")


def crap(cc, coverage):
    """CRAP for one function. `coverage` is a fraction, or None when unknown."""
    if coverage is None:
        return None
    uncovered = 1.0 - coverage
    return cc * cc * uncovered ** 3 + cc


def parse_lizard_csv(text):
    """lizard --csv -> [{file, function, cc, start, end}], validated row by row.

    A row that does not fit the documented shape is refused by name: the column
    order is positional, so a lizard that changed it must not be read as if it
    had not.
    """
    out = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        fields = next(__import__("csv").reader([line]))
        if len(fields) < len(CSV_COLUMNS):
            sys.exit(f"crap_cpp: lizard csv line {number} has {len(fields)} columns, "
                     f"expected {len(CSV_COLUMNS)} ({', '.join(CSV_COLUMNS)}). "
                     f"Check the lizard version.")
        try:
            cc, start, end = int(fields[CC]), int(fields[START]), int(fields[END])
        except ValueError:
            sys.exit(f"crap_cpp: lizard csv line {number}: ccn/start/end are not "
                     f"numbers ({fields[CC]!r}, {fields[START]!r}, {fields[END]!r}). "
                     f"Check the lizard version.")
        if end < start:
            sys.exit(f"crap_cpp: lizard csv line {number}: end {end} before start {start}")
        out.append({"file": fields[FILE], "function": fields[FUNCTION],
                    "cc": cc, "start": start, "end": end})
    return out


def parse_lcov(text):
    """LCOV -> {source path: {line number: hit count}} from its DA records."""
    files, current = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("SF:"):
            current = files.setdefault(line[3:], {})
        elif line.startswith("DA:") and current is not None:
            number, _, count = line[3:].partition(",")
            try:
                current[int(number)] = int(count.split(",")[0])
            except ValueError:
                continue
        elif line == "end_of_record":
            current = None
    return files


def lines_for(coverage, path):
    """The DA lines of one source file, matched by the longest common suffix.

    An LCOV report names files as the compiler saw them -- absolute, or relative
    to the build directory -- and lizard names them as it was asked to. Matching
    on the longest shared suffix joins them without either side having to agree
    on a root.
    """
    if path in coverage:
        return coverage[path]
    wanted = pathlib.PurePath(path).parts
    best, best_score = None, 0
    for candidate, lines in coverage.items():
        parts = pathlib.PurePath(candidate).parts
        score = 0
        for a, b in zip(reversed(wanted), reversed(parts)):
            if a != b:
                break
            score += 1
        if score > best_score:
            best, best_score = lines, score
    return best


def coverage_for(function, coverage):
    """The share of a function's own DA lines that were hit, or None."""
    if coverage is None:
        return None
    lines = lines_for(coverage, function["file"])
    if lines is None:
        return None
    inside = [count for number, count in lines.items()
              if function["start"] <= number <= function["end"]]
    if not inside:
        return None
    return sum(1 for count in inside if count > 0) / len(inside)


def score(functions, coverage):
    rows = []
    for f in functions:
        fraction = coverage_for(f, coverage)
        rows.append({**f,
                     "coverage": fraction,
                     "crap": crap(f["cc"], fraction),
                     "crap_at_zero": f["cc"] * f["cc"] + f["cc"]})
    rows.sort(key=lambda r: (-(r["crap"] if r["crap"] is not None else r["crap_at_zero"]),
                             -r["cc"], r["file"], r["function"]))
    return rows


def run_lizard(paths, binary):
    """lizard over `paths`, or exit saying how to get it."""
    command = shutil.which(binary)
    if command:
        argv = [command, "--csv"]
    elif subprocess.run([sys.executable, "-c", "import lizard"],
                        capture_output=True).returncode == 0:
        argv = [sys.executable, "-m", "lizard", "--csv"]
    else:
        sys.exit("crap_cpp: lizard is not installed. It is the complexity half of this\n"
                 "  tool and the only part that reads C++:\n"
                 "    uv tool install lizard   (or: pipx install lizard, pip install lizard)\n"
                 "  Then re-run. `--lizard-csv <file>` reads a csv you already have.")
    argv += ["-l", "cpp"] + [str(p) for p in paths]
    run = subprocess.run(argv, capture_output=True, text=True)
    # lizard exits non-zero when a threshold is exceeded, which is not our failure;
    # empty output is.
    if not run.stdout.strip():
        sys.exit(f"crap_cpp: lizard produced no rows for {', '.join(str(p) for p in paths)}"
                 + (f"\n{run.stderr.strip()}" if run.stderr.strip() else ""))
    return run.stdout


def report(rows, threshold, limit):
    known = [r for r in rows if r["crap"] is not None]
    print("CRAP Report (C++)")
    print("=================")
    print(f"{len(rows)} function(s)" + ("" if known else
          "; coverage N/A, so the CRAP column is what it WOULD be at 0% coverage (CC² + CC)"))
    print()
    print(f"{'function':<44} {'file':<46} {'CC':>4} {'Cov%':>7} {'CRAP':>9}")
    print("-" * 114)
    for row in rows[:limit]:
        cov = "N/A" if row["coverage"] is None else f"{row['coverage'] * 100:.1f}%"
        value = row["crap"] if row["crap"] is not None else row["crap_at_zero"]
        print(f"{row['function'][:44]:<44} {row['file'][-46:]:<46} "
              f"{row['cc']:>4} {cov:>7} {value:>9.1f}")
    if len(rows) > limit:
        print(f"... {len(rows) - limit} more; --limit 0 for all, --json for the lot")
    over = [r for r in rows
            if (r["crap"] if r["crap"] is not None else r["crap_at_zero"]) >= threshold]
    print()
    print(f"{len(over)} function(s) at or above CRAP {threshold}"
          + (" (at 0% coverage)" if not known else ""))
    return over


def selftest():
    """The joins and the arithmetic, with no lizard and no lcov on disk."""
    failures = 0

    def check(label, ok, detail):
        nonlocal failures
        print(f"  [{'ok' if ok else 'FAIL'}] {label:<58} -> {detail}")
        failures += 0 if ok else 1

    print("the CRAP formula:")
    check("CC 1 fully covered is 1", crap(1, 1.0) == 1.0, crap(1, 1.0))
    check("CC 10 fully covered is 10", crap(10, 1.0) == 10.0, crap(10, 1.0))
    check("CC 10 uncovered is 110", crap(10, 0.0) == 110.0, crap(10, 0.0))
    check("CC 3 uncovered is over the gate", crap(3, 0.0) == 12.0, crap(3, 0.0))
    check("CC 3 covered is under it", crap(3, 1.0) == 3.0, crap(3, 1.0))
    check("half-covered CC 10 is 22.5", abs(crap(10, 0.5) - 22.5) < 1e-9, crap(10, 0.5))
    check("unknown coverage scores nothing", crap(10, None) is None, crap(10, None))

    print("\nlizard's csv:")
    # Verbatim in the shape lizard 1.26.0 writes, `location` included:
    #   nloc,ccn,tokens,params,length,location,file,function,long_name,start,end
    csv_text = ('12,4,44,1,12,"Foo::bar@100-120@src/backup/backup_lob.cpp",'
                '"src/backup/backup_lob.cpp","Foo::bar","Foo::bar( int n)",100,120\n'
                '3,1,12,0,3,"plain@5-8@src/x.cpp","src/x.cpp","plain","plain()",5,8\n')
    rows = parse_lizard_csv(csv_text)
    check("two rows, by position", len(rows) == 2, f"{len(rows)} rows")
    check("the complexity is the CCN column", rows[0]["cc"] == 4, rows[0]["cc"])
    check("the span is start..end", (rows[0]["start"], rows[0]["end"]) == (100, 120),
          f"{rows[0]['start']}..{rows[0]['end']}")
    check("the name is the function, not the signature", rows[0]["function"] == "Foo::bar",
          rows[0]["function"])
    try:
        parse_lizard_csv("1,2,3\n")
        why = "ACCEPTED"
    except SystemExit as exc:
        why = str(exc)
    check("a short row is refused, naming lizard", "lizard version" in why, why[:46])

    print("\nlcov, and the file join:")
    lcov = ("SF:/build/elsewhere/src/x.cpp\n"
            "DA:5,1\nDA:6,0\nDA:7,3\nDA:8,0\n"
            "end_of_record\n")
    coverage = parse_lcov(lcov)
    check("one record, four DA lines", len(coverage) == 1 and len(next(iter(coverage.values()))) == 4,
          f"{len(coverage)} file(s)")
    fraction = coverage_for({"file": "src/x.cpp", "start": 5, "end": 8}, coverage)
    check("a build-dir path joins by its suffix", fraction == 0.5, fraction)
    check("a function outside the lines scores nothing",
          coverage_for({"file": "src/x.cpp", "start": 90, "end": 99}, coverage) is None, "None")
    check("an unmentioned file scores nothing",
          coverage_for({"file": "src/other.cpp", "start": 5, "end": 8}, coverage) is None, "None")
    scored = score(parse_lizard_csv(csv_text), coverage)
    check("the worst function is first", scored[0]["function"] == "Foo::bar", scored[0]["function"])
    # backup_lob.cpp is not in the lcov above, so it scores nothing; x.cpp's
    # `plain` is CC 1 over four DA lines of which two were hit, so its CRAP is
    # 1 x 0.5³ + 1 = 1.125 -- the uncovered term is CUBED, which is the whole
    # point of the metric and the easiest thing to get wrong by hand.
    check("a function whose file the report omits scores nothing",
          scored[0]["crap"] is None, str(scored[0]["crap"]))
    check("and a half-covered CC 1 is 1.125, not 1",
          abs(scored[1]["crap"] - 1.125) < 1e-9, scored[1]["crap"])
    print(f"\n{'FAIL' if failures else 'PASS'}: self-test, {failures} failure(s)")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=["src"], help="files or directories (default: src)")
    ap.add_argument("--lcov", action="append", default=[], help="an LCOV report; may be repeated")
    ap.add_argument("--lizard-csv", help="read lizard's csv from here instead of running it")
    ap.add_argument("--lizard", default="lizard", help="the lizard binary (default: lizard)")
    ap.add_argument("--threshold", type=float, default=10.0, help="the gate (default: 10)")
    ap.add_argument("--limit", type=int, default=30, help="rows to print; 0 for all")
    ap.add_argument("--json", help="write every row here")
    ap.add_argument("--selftest", action="store_true", help="check this tool's own joins")
    args = ap.parse_args()
    if args.selftest:
        return selftest()

    if args.lizard_csv:
        text = pathlib.Path(args.lizard_csv).read_text()
    else:
        paths = [pathlib.Path(p) for p in args.paths]
        missing = [str(p) for p in paths if not p.exists()]
        if missing:
            sys.exit(f"crap_cpp: no such path: {', '.join(missing)}")
        text = run_lizard(paths, args.lizard)
    functions = [f for f in parse_lizard_csv(text)
                 if pathlib.PurePath(f["file"]).suffix in SUFFIXES]
    if not functions:
        sys.exit("crap_cpp: no C or C++ functions in lizard's output")

    coverage = None
    for path in args.lcov:
        text = pathlib.Path(path).read_text()
        parsed = parse_lcov(text)
        if not parsed:
            sys.exit(f"crap_cpp: {path} holds no SF/DA records -- is it LCOV?")
        coverage = {**(coverage or {}), **parsed}

    rows = score(functions, coverage)
    over = report(rows, args.threshold, args.limit or len(rows))
    if args.json:
        pathlib.Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.json).write_text(json.dumps(rows, indent=2) + "\n")
        print(f"wrote {args.json}")
    return 2 if over else 0


if __name__ == "__main__":
    sys.exit(main())

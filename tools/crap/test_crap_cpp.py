"""Tests of crap_cpp.py, the scorer shared across the three repositories: `uvx pytest tools/crap`.
Neither lizard nor an LCOV report on disk is needed."""

import subprocess

import pytest

import crap_cpp

# One lizard row and one LCOV record for the same file: `f` spans lines 5..8, of which 5 and 7 ran.
LIZARD_CSV = '3,2,12,0,3,"f@5-8@src/x.c","src/x.c","f","f()",5,8\n'
LCOV = "SF:/build/src/x.c\nDA:5,1\nDA:6,0\nDA:7,3\nDA:8,0\nend_of_record\n"


def test_crap_cpp_selftest_passes(capsys):
    assert crap_cpp.selftest() == 0
    assert "PASS: self-test, 0 failure(s)" in capsys.readouterr().out


def test_checks_count_failures(capsys):
    checks = crap_cpp.Checks()
    checks.check("fine", True, 1)
    checks.check("broken", False, 2)
    assert checks.failures == 1
    assert "[FAIL] broken" in capsys.readouterr().out


def test_refusal_reports_acceptance_too():
    assert crap_cpp.refusal(LIZARD_CSV) == "ACCEPTED"
    assert "lizard version" in crap_cpp.refusal("1,2,3\n")


def test_main_scores_a_csv_against_lcov_and_writes_json(tmp_path, capsys):
    csv_path, lcov_path, json_path = tmp_path / "l.csv", tmp_path / "c.lcov", tmp_path / "out" / "rows.json"
    csv_path.write_text(LIZARD_CSV)
    lcov_path.write_text(LCOV)
    # CC 2 at 50%: 4 * 0.125 + 2 = 2.5, under the gate.
    assert crap_cpp.main(["--lizard-csv", str(csv_path), "--lcov", str(lcov_path), "--json", str(json_path)]) == 0
    out = capsys.readouterr().out
    assert "1 function(s)" in out and "50.0%" in out and "0 function(s) at or above CRAP 10.0" in out
    assert json_path.exists()
    # With the gate at 2 the same row fails; with no report it is scored at 0% (CC² + CC = 6) and says so.
    assert crap_cpp.main(["--lizard-csv", str(csv_path), "--lcov", str(lcov_path), "--threshold", "2"]) == 2
    assert crap_cpp.main(["--lizard-csv", str(csv_path), "--threshold", "6"]) == 2
    assert "WOULD be at 0% coverage" in capsys.readouterr().out


def test_main_runs_the_selftest():
    assert crap_cpp.main(["--selftest"]) == 0


def test_main_refuses_a_missing_path_an_empty_csv_and_a_non_lcov_file(tmp_path):
    with pytest.raises(SystemExit, match="no such path"):
        crap_cpp.main([str(tmp_path / "nowhere")])
    empty = tmp_path / "empty.csv"
    empty.write_text('1,1,1,0,1,"g@1-2@README.md","README.md","g","g()",1,2\n')
    with pytest.raises(SystemExit, match="no C or C\\+\\+ functions"):
        crap_cpp.main(["--lizard-csv", str(empty)])
    not_lcov = tmp_path / "x.txt"
    not_lcov.write_text("hello\n")
    csv_path = tmp_path / "l.csv"
    csv_path.write_text(LIZARD_CSV)
    with pytest.raises(SystemExit, match="is it LCOV"):
        crap_cpp.main(["--lizard-csv", str(csv_path), "--lcov", str(not_lcov)])


def test_print_rows_truncates_and_says_so(capsys):
    rows = crap_cpp.score(crap_cpp.parse_lizard_csv(LIZARD_CSV * 3), None)
    crap_cpp.print_rows(rows, 2)
    out = capsys.readouterr().out
    assert out.count("src/x.c") == 2 and "... 1 more" in out


def test_lizard_command_prefers_path_then_module_then_exits(monkeypatch):
    monkeypatch.setattr(crap_cpp.shutil, "which", lambda name: "/opt/bin/lizard")
    assert crap_cpp.lizard_command("lizard") == ["/opt/bin/lizard", "--csv"]
    monkeypatch.setattr(crap_cpp.shutil, "which", lambda name: None)
    monkeypatch.setattr(crap_cpp.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 0))
    assert crap_cpp.lizard_command("lizard")[1:] == ["-m", "lizard", "--csv"]
    monkeypatch.setattr(crap_cpp.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 1))
    with pytest.raises(SystemExit, match="uv tool install lizard"):
        crap_cpp.lizard_command("lizard")


def test_run_lizard_returns_the_csv_and_refuses_empty_output(monkeypatch):
    calls = []

    def fake_run(argv, **kwargs):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, stdout=LIZARD_CSV if len(calls) == 1 else "", stderr="boom")

    monkeypatch.setattr(crap_cpp.shutil, "which", lambda name: "/opt/bin/lizard")
    monkeypatch.setattr(crap_cpp.subprocess, "run", fake_run)
    assert crap_cpp.run_lizard(["src"], "lizard") == LIZARD_CSV
    assert calls[0] == ["/opt/bin/lizard", "--csv", "-l", "cpp", "src"]
    with pytest.raises(SystemExit, match="no rows for src\nboom"):
        crap_cpp.run_lizard(["src"], "lizard")

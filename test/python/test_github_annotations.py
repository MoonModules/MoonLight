"""A failing test and a check's error reach a pull request as workflow-command annotations, and the counts as a summary table, with no token."""

import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "ci"))
import github_annotations as ga  # noqa: E402


def test_a_failing_test_is_annotated_where_its_failure_points_and_counted(tmp_path):
    junit = tmp_path / "junit-unit.xml"
    junit.write_text('<testsuites><testsuite name="all">'
                     '<testcase classname="test/unit/core/unit_A.cpp" name="adds, then subtracts"/>'
                     '<testcase classname="test/unit/core/unit_A.cpp" name="divides, by zero: refused"><failure message="CHECK( a == b )">test/unit/core/unit_A.cpp:42: ERROR: 100% off\nsecond line</failure></testcase>'
                     '<testcase classname="t" name="later"><skipped/></testcase>'
                     '</testsuite></testsuites>')
    out = io.StringIO()
    rows = ga.junit([str(junit)], out)
    assert rows == [("junit-unit.xml", {"passed": 1, "failed": 1, "skipped": 1})]
    assert out.getvalue().splitlines() == ["::error file=test/unit/core/unit_A.cpp,line=42,title=divides%2C by zero%3A refused::CHECK( a == b ) test/unit/core/unit_A.cpp:42: ERROR: 100%25 off%0Asecond line"]


def test_a_checks_errors_are_annotated_only_in_the_files_a_pull_request_changes(tmp_path):
    record = tmp_path / "findings"
    ga._findings.write("check_x", {"r": {"reason": "why"}},
                       [{"rule": "r", "level": "error", "message": "bad", "file": "src/a.h", "line": 3},
                        {"rule": "r", "level": "error", "message": "old", "file": "src/b.h", "line": 9},
                        {"rule": "r", "message": "a warning", "file": "src/a.h"}], record)
    out = io.StringIO()
    rows = ga.findings({"src/a.h"}, out, record)
    assert out.getvalue().splitlines() == ["::error file=src/a.h,line=3,title=check_x::bad"]
    assert rows == [("check_x", {"errors": 2, "warnings": 1})]
    assert "| check_x | 2 | 1 |" in ga.summary("Findings", rows)


def test_a_forks_generated_files_are_warned_about_not_failed(monkeypatch):
    """The metrics whole and an edited scenario each get a warning, since the merge regenerates them; a new scenario is written by hand."""
    import io
    def changed(base, *filters):
        return ["docs/reference/metrics/code.json"] if "docs/reference/metrics" in filters else ["test/scenarios/core/old.json"]
    monkeypatch.setattr(ga, "changed_files", changed)
    out = io.StringIO()
    assert ga.generated("main", out) == ["docs/reference/metrics/code.json", "test/scenarios/core/old.json"]
    assert out.getvalue().count("::warning file=") == 2


def test_a_base_git_does_not_know_stops_with_one_line_not_a_traceback():
    """A clone without the remote a step names, such as `upstream`, is told which ref to pass."""
    import pytest
    with pytest.raises(SystemExit) as stop:
        ga.changed_files("no-such-remote/main")
    assert "--base" in str(stop.value)


def test_the_branch_range_comes_before_the_paths_and_a_name_with_spaces_stays_whole(tmp_path):
    """git reads a range after `--` as a path, which named no change at all; and a plan's name holds spaces."""
    import subprocess
    git = lambda *a: subprocess.run(["git", *a], cwd=tmp_path, check=True, capture_output=True)
    git("init", "-q", "-b", "main")
    git("config", "user.email", "t@t")
    git("config", "user.name", "t")
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "a.md").write_text("one")
    git("add", "."); git("commit", "-qm", "base")
    git("switch", "-qc", "topic")
    (tmp_path / "docs" / "a plan (1 of 2).md").write_text("two")
    (tmp_path / "other.txt").write_text("three")
    git("add", "."); git("commit", "-qm", "change")
    assert ga.changed_files("main", "--", "docs", root=tmp_path) == ["docs/a plan (1 of 2).md"]

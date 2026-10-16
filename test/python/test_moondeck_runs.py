"""MoonDeck's last runs, what MoonWalk marks a contributor's step done by: each script's log names the arguments that ran on its `# args:` line and ends with its exit code.

A step matches a run by script and arguments, so these pin the log's shape that `doneByRuns` relies on. Run: `uv run moondeck/test/test_host.py --python`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

from moondeck import SCRIPTS, last_runs  # noqa: E402

SCRIPT = next(s for s in SCRIPTS if s.get("script"))


def log(tmp_path: Path, text: str) -> list:
    (tmp_path / f"{SCRIPT['id']}.log").write_text(text, encoding="utf-8")
    return [r for r in last_runs(tmp_path) if r["script"] == f"moondeck/{SCRIPT['script']}"]


def test_a_run_is_the_script_and_the_arguments_that_ran_and_whether_it_passed(tmp_path):
    assert log(tmp_path, "# x\n# args: --tests\nbuilding\n[exit code: 0]\n") == [
        {"script": f"moondeck/{SCRIPT['script']}", "args": "--tests", "passed": True}]


def test_a_failed_run_is_reported_failed(tmp_path):
    assert log(tmp_path, "# x\n# args: \n[exit code: 1]\n")[0]["passed"] is False


def test_a_run_still_going_or_a_log_without_its_arguments_vouches_for_nothing(tmp_path):
    assert log(tmp_path, "# x\n# args: --tests\nstill building\n") == []
    assert log(tmp_path, "# x\nbuilt before the args line existed\n[exit code: 0]\n") == []

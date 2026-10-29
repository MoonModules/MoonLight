"""Running a process's script steps from the terminal: which steps run and which are skipped, and why. Run: `uv run moondeck/test/test_host.py --python`."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "run"))
from run_process import plan  # noqa: E402


def step(sid, triggered=True, fresh=False):
    return {"id": sid, "process": "ci-on-pr", "script": "moondeck/x.py", "args": "", "triggered": triggered, "fresh": fresh}


def test_a_step_the_change_does_not_touch_is_skipped_unless_every_step_runs():
    assert plan([step("a", triggered=False)], every=False, rerun=False)[0][1].startswith("skip: the change")
    assert plan([step("a", triggered=False)], every=True, rerun=False)[0][1] == "run"


def test_a_holding_pass_is_skipped_unless_rerun():
    assert plan([step("a", fresh=True)], every=False, rerun=False)[0][1] == "skip: its last pass still holds"
    assert plan([step("a", fresh=True)], every=False, rerun=True)[0][1] == "run"


def test_a_triggered_step_without_a_holding_pass_runs():
    assert [w for _, w in plan([step("a"), step("b")], every=False, rerun=False)] == ["run", "run"]

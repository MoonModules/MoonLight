"""The in-process scenario runner says what it does not run, rather than passing it in silence.

Each test hands the built `mm_scenarios` a scenario written for it. The binary is built by `build_desktop.py --tests`; without it the tests skip, as the JS lane does without node, since a host-test run in CI builds no C++."""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))

from run_scenario import RUNNER  # noqa: E402

pytestmark = pytest.mark.skipif(not RUNNER.exists(), reason="mm_scenarios is not built: run build_desktop.py --tests")


def run(tmp_path: Path, scenario: dict) -> subprocess.CompletedProcess:
    """Run one scenario through the runner, with its filesystem in the test's own folder."""
    path = tmp_path / f"{scenario['name']}.json"
    path.write_text(json.dumps(scenario), encoding="utf-8")
    env = {**os.environ, "MM_DATA_DIR": str(tmp_path / "fs")}
    return subprocess.run([str(RUNNER), str(path)], cwd=ROOT, env=env, capture_output=True, text=True, encoding="utf-8", timeout=120)


def test_an_op_the_runner_does_not_run_is_reported_as_skipped(tmp_path):
    scenario = {"name": "scenario_T_unknown_op", "module": "FileManagerModule", "mode": "mutate", "description": "t",
                "top": ["FileManagerModule"], "steps": [{"name": "misspelt", "op": "expect_flie", "path": "/x"}]}
    assert "SKIP  misspelt (op not run in-process: expect_flie)" in run(tmp_path, scenario).stdout


def test_a_mutate_scenario_with_nothing_to_build_fails_unless_it_says_live_only(tmp_path):
    bare = {"name": "scenario_T_bare", "module": "WiFiModule", "mode": "mutate", "description": "t", "steps": []}
    failed = run(tmp_path, bare)
    assert failed.returncode == 1 and 'mark it "live_only": true' in failed.stdout
    skipped = run(tmp_path, {**bare, "live_only": True})
    assert skipped.returncode == 2 and "SKIP (live_only)" in skipped.stdout

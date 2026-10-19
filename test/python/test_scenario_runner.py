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


def test_a_live_op_is_skipped_and_an_op_neither_runner_knows_fails(tmp_path):
    scenario = {"name": "scenario_T_ops", "module": "FileManagerModule", "mode": "mutate", "description": "t",
                "top": ["FileManagerModule"], "steps": [{"name": "wrote", "op": "write_file", "path": "/x.txt", "value": "x"},
                                                        {"name": "read", "op": "expect_file", "path": "/x.txt", "contains": "x"},
                                                        {"name": "online", "op": "expect_http", "path": "/x"}]}
    live = run(tmp_path, scenario)
    assert live.returncode == 0 and "SKIP  online (op not run in-process: expect_http)" in live.stdout
    misspelled = run(tmp_path, {**scenario, "steps": scenario["steps"][:2] + [{"name": "misspelled", "op": "expect_flie", "path": "/x"}]})
    assert misspelled.returncode == 1 and "FAIL  misspelled (unknown op: expect_flie)" in misspelled.stdout


def test_an_unknown_mode_fails_even_on_a_live_only_scenario(tmp_path):
    scenario = {"name": "scenario_T_mode", "module": "WiFiModule", "mode": "mutat", "live_only": True, "description": "t", "steps": []}
    assert run(tmp_path, scenario).returncode == 1


def test_a_mutate_scenario_with_nothing_to_build_fails_unless_it_says_live_only(tmp_path):
    bare = {"name": "scenario_T_bare", "module": "WiFiModule", "mode": "mutate", "description": "t", "steps": []}
    failed = run(tmp_path, bare)
    assert failed.returncode == 1 and 'mark it "live_only": true' in failed.stdout
    skipped = run(tmp_path, {**bare, "live_only": True})
    assert skipped.returncode == 2 and "SKIP (live_only)" in skipped.stdout

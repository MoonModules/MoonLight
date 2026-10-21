# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""A scenario's observations: the sample window stays on the machine, and a scenario file takes only a summary."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "scenario"))
import _observed  # noqa: E402


def test_a_measurement_goes_into_the_local_window_and_the_next_run_builds_on_it(tmp_path):
    store = tmp_path / "observations.json"
    first = _observed.record("scenario_X", "step", "desktop", {"tick_us": 10, "free_heap": 0, "max_alloc_block": 0}, "2026-10-20", path=store)
    second = _observed.record("scenario_X", "step", "desktop", {"tick_us": 30, "free_heap": 0, "max_alloc_block": 0}, "2026-10-21", path=store)
    assert first["tick_us"]["n"] == 1 and second["tick_us"]["n"] == 2 and second["tick_us"]["samples"] == [10, 30]
    assert "free_heap" not in second   # 0 is no measurement: the desktop has no ceiling to report
    assert second["last_updated"] == "2026-10-21"


def test_a_renegotiated_contract_starts_the_window_over(tmp_path):
    store = tmp_path / "observations.json"
    _observed.record("s", "step", "t", {"tick_us": 10}, "d", path=store)
    block = _observed.record("s", "step", "t", {"tick_us": 99}, "d", reset_window=True, path=store)
    assert block["tick_us"]["samples"] == [99]


def test_the_summary_a_scenario_file_publishes_has_the_statistics_and_no_samples():
    block = {"tick_us": {"p50": 12, "p95": 20, "min": 10, "max": 21, "n": 3, "samples": [10, 12, 21]}, "last_updated": "2026-10-20"}
    assert _observed.summary(block) == {"tick_us": {"p50": 12, "p95": 20, "min": 10, "max": 21, "n": 3}, "last_updated": "2026-10-20"}


def test_a_window_of_each_scenario_step_and_target_is_kept_apart(tmp_path):
    store = tmp_path / "observations.json"
    _observed.record("a", "step", "desktop", {"tick_us": 1}, "d", path=store)
    other = _observed.record("a", "step", "esp32", {"tick_us": 2}, "d", path=store)
    assert other["tick_us"]["samples"] == [2]

"""run_scenario.py's whole run keeps its output for the KPI only when it is safe to reuse (moondeck/scenario/run_scenario.py). No scenario runs."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

import run_scenario


def _run(monkeypatch, tmp_path, codes, fingerprint=lambda exe, paths: "key"):
    paths = []
    for i, _ in enumerate(codes):
        p = tmp_path / f"s{i}.json"
        p.write_text(json.dumps({}))
        paths.append(p)
    results = iter(codes)
    kept = []
    monkeypatch.setattr(run_scenario, "_run_one", lambda *a: next(results))
    monkeypatch.setattr(run_scenario.test_runs, "record", lambda r: None)
    monkeypatch.setattr(run_scenario._suite_output, "fingerprint", fingerprint)
    monkeypatch.setattr(run_scenario._suite_output, "keep", lambda exe, out, key: kept.append(key))

    class Args:
        update_contract = reason = update_observed = None
    run_scenario._run_many(paths, Args(), whole=True)
    return kept


def test_a_whole_run_with_every_scenario_passing_or_skipped_is_kept_for_the_kpi(monkeypatch, tmp_path):
    assert _run(monkeypatch, tmp_path, [0, run_scenario.SKIPPED]) == ["key"]


def test_a_whole_run_with_a_failed_scenario_keeps_no_output(monkeypatch, tmp_path):
    assert _run(monkeypatch, tmp_path, [0, 1]) == []


def test_an_unreadable_fingerprint_still_runs_the_scenarios_and_keeps_nothing(monkeypatch, tmp_path):
    def unreadable(exe, paths):
        raise PermissionError("execute-only runner")
    assert _run(monkeypatch, tmp_path, [0], unreadable) == []

"""_suite_output.py: a suite's whole output kept with its binary's hash, read back only for that same binary."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))
import _suite_output  # noqa: E402


def test_a_kept_run_is_read_back_for_the_same_binary_and_dropped_once_it_is_rebuilt(tmp_path):
    exe = tmp_path / "mm_tests"
    exe.write_bytes(b"binary one")
    assert _suite_output.kept(exe) is None                 # before a whole run: the reader runs the suite itself
    _suite_output.keep(exe, "[doctest] test cases: 3394 | 3394 passed\n")
    assert "3394 passed" in _suite_output.kept(exe)
    exe.write_bytes(b"binary two")                         # rebuilt: the kept run describes another binary
    assert _suite_output.kept(exe) is None


def test_a_run_whose_output_cannot_be_saved_still_finishes_and_a_vanished_binary_reads_as_nothing_kept(tmp_path, capsys):
    exe = tmp_path / "mm_tests"
    exe.write_bytes(b"binary")
    (tmp_path / "mm_tests.output.json").mkdir()            # the sidecar's place is taken: the write fails
    _suite_output.keep(exe, "output")                      # said on stderr, not raised: the caller still records the suite's result
    assert "not kept" in capsys.readouterr().err
    (tmp_path / "mm_tests.output.json").rmdir()
    _suite_output.keep(exe, "output")
    exe.unlink()                                           # a rebuild removed the binary after its path was chosen
    assert _suite_output.kept(exe) is None


def test_scenario_definitions_are_part_of_the_key_so_an_edited_workload_is_run_again(tmp_path):
    exe = tmp_path / "mm_scenarios"
    exe.write_bytes(b"binary")
    scenario = tmp_path / "scenario_a.json"
    scenario.write_text('{"ticks": 10}', encoding="utf-8")
    key = _suite_output.fingerprint(exe, [scenario])       # taken before the run
    scenario.write_text('{"ticks": 20}', encoding="utf-8") # the run rewrote its own definition
    _suite_output.keep(exe, "ran ticks 10", key)
    assert _suite_output.kept(exe, [scenario]) is None     # the output describes a definition that is gone from disk
    scenario.write_text('{"ticks": 10}', encoding="utf-8")
    assert _suite_output.kept(exe, [scenario]) == "ran ticks 10"


def test_a_scenario_run_keeps_its_output_only_when_no_definition_changed_while_it_ran(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
    import run_scenario
    exe = tmp_path / "mm_scenarios"
    exe.write_bytes(b"binary")
    a, b = tmp_path / "scenario_a.json", tmp_path / "scenario_b.json"
    for p in (a, b):
        p.write_text('{"ticks": 1}', encoding="utf-8")
    monkeypatch.setattr(run_scenario, "RUNNER", exe)
    monkeypatch.setattr(run_scenario.test_runs, "record", lambda runs: None)
    edited = []

    def run_one(p, *rest):
        if edited and p == a:
            b.write_text('{"ticks": 2}', encoding="utf-8")
        return 0

    monkeypatch.setattr(run_scenario, "_run_one", run_one)
    args = type("A", (), {"update_contract": False, "reason": None, "update_observed": False})()
    assert run_scenario._run_many([a, b], args, whole=True) == 0               # untouched: the KPI reads it
    assert _suite_output.kept(exe, [a, b]) is not None
    exe.with_name("mm_scenarios.output.json").unlink()
    edited.append(1)                                                           # scenario B is edited while A runs
    assert run_scenario._run_many([a, b], args, whole=True) == 0
    b.write_text('{"ticks": 1}', encoding="utf-8")                             # restored: it must not pass as the run of its old text
    assert _suite_output.kept(exe, [a, b]) is None


def test_a_host_run_is_kept_only_with_its_case_count_and_for_the_binary_that_started_it(tmp_path):
    exe = tmp_path / "mm_tests"
    exe.write_bytes(b"binary one")
    key = _suite_output.fingerprint(exe)                          # taken as the run starts
    _suite_output.keep_complete(exe, "crashed before the summary\n", key, "test cases:")
    assert _suite_output.kept(exe) is None                        # no case count: the KPI runs the suite itself
    exe.write_bytes(b"binary two")                                # rebuilt while the run went
    _suite_output.keep_complete(exe, "[doctest] test cases: 3 | 3 passed\n", key, "test cases:")
    assert _suite_output.kept(exe) is None
    key = _suite_output.fingerprint(exe)
    _suite_output.keep_complete(exe, "[doctest] test cases: 3 | 3 passed\n", key, "test cases:")
    assert "3 passed" in _suite_output.kept(exe)

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

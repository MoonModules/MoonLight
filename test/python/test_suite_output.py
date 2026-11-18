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

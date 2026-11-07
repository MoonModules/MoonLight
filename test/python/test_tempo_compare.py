# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""Comparing two tempo runs: the exact McNemar test, the last two runs on the same files, and tracks paired by their audio."""

import json

import pytest

import tempo_compare


def test_mcnemar_is_the_exact_two_sided_binomial_test_on_the_discordant_tracks():
    assert tempo_compare.mcnemar(0, 0) == 1.0
    assert tempo_compare.mcnemar(3, 3) == 1.0
    assert tempo_compare.mcnemar(0, 5) == 0.0625
    assert round(tempo_compare.mcnemar(2, 12), 4) == 0.0129


def run_file(out, when, rows, cut_short=False):
    text = "".join(json.dumps(r) + "\n" for r in rows) + ('{"path": "cut' if cut_short else "")
    (out / f"run-{when}.jsonl").write_text(text)


def track(h, est, right, path=None):
    return {"path": path or h, "hash": h, "name": h, "genre": "g", "ref": 120, "est": est,
            "judge": {"acc1_2": right, "verdict": "right" if right else "half"}}


def test_the_last_pair_is_the_two_newest_runs_on_the_same_files(tmp_path):
    run_file(tmp_path, "20260101-000001", [track("x", 120, True, "a/x")])
    run_file(tmp_path, "20260101-000002", [track("y", 120, True, "b/y")])
    run_file(tmp_path, "20260101-000003", [track("x", 60, False, "a/x")], cut_short=True)
    older, newer = tempo_compare.last_pair(tmp_path)
    assert (older.name, newer.name) == ("run-20260101-000001.jsonl", "run-20260101-000003.jsonl")


def test_the_last_pair_knows_a_moved_or_renamed_file_by_its_audio(tmp_path):
    run_file(tmp_path, "20260101-000001", [track("x", 120, True, "a/x.wav")])
    run_file(tmp_path, "20260101-000002", [track("x", 60, False, "b/renamed.wav")])
    older, newer = tempo_compare.last_pair(tmp_path)
    assert (older.name, newer.name) == ("run-20260101-000001.jsonl", "run-20260101-000002.jsonl")


def test_with_no_two_runs_on_the_same_files_the_last_pair_says_so(tmp_path):
    run_file(tmp_path, "20260101-000001", [track("x", 120, True, "a/x")])
    with pytest.raises(SystemExit, match="no two runs on the same files"):
        tempo_compare.last_pair(tmp_path)


def test_a_comparison_pairs_tracks_by_their_audio_and_leaves_a_failed_decode_out(tmp_path, capsys):
    run_file(tmp_path, "a", [track("x", 120, True), track("y", 60, False), {"path": "z", "hash": "", "judge": {"acc1_2": False}}])
    run_file(tmp_path, "b", [track("x", 120, True), track("y", 120, True), {"path": "z", "hash": "", "judge": {"acc1_2": True}}])
    text = tempo_compare.compare(tmp_path / "run-a.jsonl", tmp_path / "run-b.jsonl", "acc1_2", False, tmp_path)
    assert "2 tracks scored in both" in text
    assert "| Right in this run only | 0 | 1 |" in text
    assert "| y | g | 120 | 60 half | 120 right |" in text
    assert (tmp_path / "view.md").read_text() == text

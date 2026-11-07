# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The generated set: the tracks and labels it claims, silence and noise left unlabeled, the version that makes --generated write it again, and the readings it gives through the shipped analysis."""

import csv
import json
import shutil
import wave

import pytest

import bench_tempo


def test_the_generated_set_writes_its_tracks_its_labels_and_its_version(tmp_path):
    bench_tempo.generate(tmp_path)
    rows = list(csv.DictReader((tmp_path / "labels.csv").open()))
    assert len(rows) == 19
    assert sum(1 for r in rows if r["bpm"]) == 17
    assert {r["path"] for r in rows if not r["bpm"]} == {"silence.wav", "noise.wav"}
    assert all(r["source"] == "generated" for r in rows)
    with wave.open(str(tmp_path / "click-120.wav")) as w:
        assert w.getframerate() == bench_tempo.RATE and w.getnframes() == 30 * bench_tempo.RATE
    assert (tmp_path / "version.txt").read_text().strip() == bench_tempo.GENERATED_VERSION


def test_a_generated_set_from_an_older_generator_is_written_again(tmp_path, monkeypatch):
    monkeypatch.setattr(bench_tempo, "OUT_DIR", tmp_path)
    gen = tmp_path / "generated"
    gen.mkdir()
    (gen / "version.txt").write_text("0\n")
    assert bench_tempo.generated_set() == gen.resolve()
    assert (gen / "labels.csv").exists() and (gen / "version.txt").read_text().strip() == bench_tempo.GENERATED_VERSION


def test_the_generated_set_runs_whole_while_other_folders_take_a_sample(tmp_path):
    bench_tempo.generate(tmp_path / "generated")
    music = tmp_path / "music"
    music.mkdir()
    for i in range(30):
        (music / f"{i:02d}.wav").write_bytes(b"")
    files = bench_tempo.collect([tmp_path / "generated", music], [], 5, frozenset({tmp_path / "generated"}))
    assert sum(1 for p, _ in files if p.parent.name == "generated") == 19
    assert sum(1 for p, _ in files if p.parent.name == "music") == 5


@pytest.mark.skipif(not bench_tempo.binary().exists() or shutil.which("ffmpeg") is None,
                    reason="needs mm_tempo (build_desktop.py --tests or bench_tempo.py builds it) and FFmpeg")
def test_the_generated_set_reads_through_the_shipped_analysis_as_pinned(tmp_path):
    # The whole path: FFmpeg, mm_tempo's AudioService, its header, the baseline. From 170 BPM the prior reads half.
    bench_tempo.generate(tmp_path / "generated")
    bench_tempo.run(bench_tempo.collect([tmp_path / "generated"], [], 0), tmp_path / "out", no_build=True)
    rows = {r["name"]: r for r in map(json.loads, next((tmp_path / "out").glob("run-*.jsonl")).read_text().splitlines())}
    expected = {f"click-{bpm}": "right" for bpm in (60, 72, 85, 90, 100, 110, 120, 128, 135, 140, 150, 160)}
    expected |= {"click-174": "half", "click-185": "half", "click-200": "half", "swing-120": "right", "triplet-90": "right"}
    assert {name: rows[name]["judge"]["verdict"] for name in expected} == expected
    assert rows["silence"]["est"] is None and not rows["silence"]["error"]
    assert rows["noise"]["est"] is None and not rows["noise"]["error"]

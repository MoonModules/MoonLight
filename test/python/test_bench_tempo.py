# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The tempo harness's runner: labels and their source, the probe and its tags, decoding through damaged and stopped pipes, the arguments, the report card, and a first look that samples each folder."""

import argparse
import json
import os
import shutil
import subprocess
import sys
import threading

import pytest

import bench_tempo


def test_a_label_keeps_its_source_and_an_unknown_source_reads_as_a_tag_of_unknown_origin(tmp_path):
    (tmp_path / "labels.csv").write_text("path,bpm,source,genre,id\na.wav,128,store,House,7\nb.wav,90,guess,,\nc.wav,,tapped,,\n")
    labels = bench_tempo.read_labels(tmp_path / "labels.csv", tmp_path)
    assert labels[(tmp_path / "a.wav").resolve()] == {"bpm": 128.0, "source": "store", "genre": "House", "id": "7"}
    assert labels[(tmp_path / "b.wav").resolve()]["source"] == "tag"
    assert labels[(tmp_path / "c.wav").resolve()]["bpm"] is None


def test_a_labels_file_a_spreadsheet_saved_with_a_byte_order_mark_still_reads(tmp_path):
    (tmp_path / "labels.csv").write_bytes("\ufeffpath,bpm,source\na.wav,128,tapped\n".encode("utf-8"))
    assert bench_tempo.read_labels(tmp_path / "labels.csv", tmp_path)[(tmp_path / "a.wav").resolve()]["bpm"] == 128.0


def test_a_labels_file_without_a_path_column_says_so_rather_than_reading_nothing_silently(tmp_path, capsys):
    (tmp_path / "labels.csv").write_text("file,bpm\na.wav,128\n")
    assert bench_tempo.read_labels(tmp_path / "labels.csv", tmp_path) == {}
    assert "no path column" in capsys.readouterr().err


def test_a_first_look_spreads_through_a_folder_instead_of_taking_its_first_files():
    assert bench_tempo.sample(list(range(100)), 4) == [0, 25, 50, 75]
    assert bench_tempo.sample(list(range(3)), 10) == [0, 1, 2]
    assert bench_tempo.sample(list(range(5)), 0) == list(range(5))


def test_the_limit_applies_to_each_folder_and_labels_join_their_files(tmp_path):
    for folder, count in (("one", 6), ("two", 2)):
        (tmp_path / folder).mkdir()
        for i in range(count):
            (tmp_path / folder / f"{i}.wav").write_bytes(b"")
    (tmp_path / "one" / "labels.csv").write_text("path,bpm,source\n0.wav,100,annotated\n")
    files = bench_tempo.collect([tmp_path / "one", tmp_path / "two"], [], 3)
    assert len(files) == 3 + 2
    labeled = [label for _, label in files if label]
    assert labeled == [{"bpm": 100.0, "source": "annotated", "genre": "", "id": ""}]


def test_a_bpm_that_is_not_a_number_leaves_its_row_out_and_the_rest_in(tmp_path):
    (tmp_path / "labels.csv").write_text("path,bpm,source\na.wav,128bpm,store\nb.wav,90,store\n")
    labels = bench_tempo.read_labels(tmp_path / "labels.csv", tmp_path)
    assert list(labels) == [(tmp_path / "b.wav").resolve()]


def write_wav(path, seconds):
    import wave
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(24000)
        w.writeframes(b"\0\0" * int(24000 * seconds))


@pytest.mark.skipif(shutil.which("ffprobe") is None, reason="ffprobe is not installed")
def test_a_file_is_checked_before_decoding_for_audio_it_can_read_and_length(tmp_path):
    write_wav(tmp_path / "long.wav", 10)
    write_wav(tmp_path / "short.wav", 3)
    (tmp_path / "text.mp3").write_text("not audio")
    assert bench_tempo.probe(tmp_path / "long.wav")[0] == ""
    assert bench_tempo.probe(tmp_path / "short.wav")[0].startswith("too short")
    assert bench_tempo.probe(tmp_path / "text.mp3")[0] == "unreadable"


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="FFmpeg is not installed")
def test_the_same_probe_reads_a_file_s_bpm_and_genre_tags(tmp_path):
    write_wav(tmp_path / "plain.wav", 10)
    tagged = tmp_path / "tagged.flac"
    subprocess.run(["ffmpeg", "-v", "error", "-i", str(tmp_path / "plain.wav"), "-metadata", "BPM=128",
                    "-metadata", "GENRE=House; Deep House", str(tagged)], check=True)
    assert bench_tempo.probe(tagged) == ("", 128.0, "House")


def fake_run(monkeypatch, tmp_path, analyze):
    """run() with the build, FFmpeg and the probe stood in for, each file decoded by `analyze(path)`."""
    exe = tmp_path / "mm_tempo"
    exe.write_text("")
    monkeypatch.setattr(bench_tempo, "binary", lambda: exe)
    monkeypatch.setattr(bench_tempo.shutil, "which", lambda name: name)
    monkeypatch.setattr(bench_tempo, "probe", lambda path: ("", None, "tag-genre"))
    monkeypatch.setattr(bench_tempo, "analyze", lambda path, exe: analyze(path))


def frames(n):
    return lambda path: {"columns": ["flux"], "fps": 24000 / 512, "frames": [[0]] * n, "hash": "h"}


def test_a_file_shorter_once_decoded_than_its_header_says_is_skipped_not_scored(tmp_path, monkeypatch):
    # An MP3 cut off by an interrupted copy keeps the length in its header; under eight seconds decoded, it is not scored.
    fake_run(monkeypatch, tmp_path, frames(100))
    label = {"bpm": 120.0, "source": "tapped", "genre": "x"}
    bench_tempo.run([(tmp_path / "cut.mp3", label)], tmp_path / "out", no_build=True)
    report = next((tmp_path / "out").glob("report-*.md")).read_text()
    assert "## Skipped and damaged files" in report and "| cut | too short after decoding (2.1 s) |" in report


def test_a_file_that_breaks_the_analysis_is_listed_and_the_run_still_reports_the_rest(tmp_path, monkeypatch):
    def analyze(path):
        if path.stem == "broken":
            raise ValueError("garbled output")
        return frames(1000)(path)
    fake_run(monkeypatch, tmp_path, analyze)
    label = {"bpm": 120.0, "source": "tapped", "genre": "x"}
    bench_tempo.run([(tmp_path / "broken.mp3", label), (tmp_path / "fine.mp3", label)], tmp_path / "out", no_build=True)
    report = next((tmp_path / "out").glob("report-*.md")).read_text()
    assert "| broken | failed: ValueError: garbled output |" in report and "| **tapped** | 1 |" in report


def test_a_track_s_genre_comes_from_its_label_or_its_tag_never_its_folder(tmp_path, monkeypatch):
    fake_run(monkeypatch, tmp_path, frames(1000))
    album = tmp_path / "Some Artist" / "Some Album"
    bench_tempo.run([(album / "a.mp3", {"bpm": 120.0, "source": "tapped", "genre": "Techno"}), (album / "b.mp3", None)],
                    tmp_path / "out", no_build=True)
    rows = [json.loads(line) for line in next((tmp_path / "out").glob("run-*.jsonl")).read_text().splitlines()]
    assert sorted(r["genre"] for r in rows) == ["Techno", "tag-genre"]


@pytest.mark.skipif(sys.platform == "win32", reason="the fake decoder is a script, which Windows does not run as a program")
def test_a_decoder_writing_many_errors_marks_the_file_damaged_instead_of_stalling(tmp_path, monkeypatch):
    # A damaged MP3 made FFmpeg write 277 KB of errors; read from a pipe only after the decode, they stalled the run.
    tools = tmp_path / "bin"
    tools.mkdir()
    ffmpeg = tools / "ffmpeg"
    ffmpeg.write_text(f"#!{sys.executable}\nimport sys\nsys.stderr.write('bad frame\\n' * 30000)\nsys.stdout.buffer.write(bytes(4 * 512 * 4))\n")
    fake_tempo = tools / "mm_tempo"
    fake_tempo.write_text(f"#!{sys.executable}\nimport sys\nsys.stdin.buffer.read()\nprint('# rate=24000 block=512 columns=ms,level,flux,onset')\n")
    for f in (ffmpeg, fake_tempo):
        f.chmod(0o755)
    monkeypatch.setenv("PATH", f"{tools}{os.pathsep}{os.environ['PATH']}")
    done = {}
    worker = threading.Thread(target=lambda: done.update(bench_tempo.analyze(tmp_path / "x.mp3", fake_tempo)), daemon=True)
    worker.start()
    worker.join(30)
    assert not worker.is_alive(), "the decode stalled on its own error messages"
    assert done["error"] == "damaged: 30000 decode errors"


def test_show_points_the_view_pane_at_the_latest_report_and_starts_nothing(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(bench_tempo, "OUT_DIR", tmp_path)
    monkeypatch.setattr(bench_tempo, "run", lambda *a, **k: pytest.fail("--show started a run"))
    monkeypatch.setattr(sys, "argv", ["bench_tempo.py", "--show"])
    with pytest.raises(SystemExit, match="No tempo report yet"):
        bench_tempo.main()
    (tmp_path / "view.md").write_text("# latest")
    bench_tempo.main()
    assert capsys.readouterr().out.strip() == "MOONDECK_VIEW: /api/tempo-view"


def test_a_limit_below_0_or_jobs_below_1_is_refused_with_a_message():
    for parse, text, says in ((bench_tempo.at_least(0), "-1", "below 0"), (bench_tempo.at_least(1), "0", "below 1"),
                              (bench_tempo.at_least(0), "x", "not a whole number")):
        with pytest.raises(argparse.ArgumentTypeError, match=says):
            parse(text)
    assert bench_tempo.at_least(1)("4") == 4


@pytest.mark.skipif(sys.platform == "win32", reason="the fake programs are scripts, which Windows does not run as programs")
def test_a_decoder_killed_because_mm_tempo_stopped_reading_is_not_reported_as_a_decode_failure(tmp_path, monkeypatch):
    tools = tmp_path / "tools"
    tools.mkdir()
    ffmpeg = tools / "ffmpeg"
    ffmpeg.write_text(f"#!{sys.executable}\nimport sys\nwhile True: sys.stdout.buffer.write(bytes(1 << 16))\n")
    fake_tempo = tools / "mm_tempo"
    fake_tempo.write_text(f"#!{sys.executable}\nprint('# rate=24000 block=512 columns=ms,level,flux,onset')\n")
    for f in (ffmpeg, fake_tempo):
        f.chmod(0o755)
    monkeypatch.setenv("PATH", f"{tools}{os.pathsep}{os.environ['PATH']}")
    res = bench_tempo.analyze(tmp_path / "x.mp3", fake_tempo)
    assert not res.get("error", "").startswith("decode failed"), res


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="FFmpeg is not installed")
def test_the_probe_reads_an_mp3_s_id3_bpm_tag(tmp_path):
    write_wav(tmp_path / "plain.wav", 10)
    tagged = tmp_path / "tagged.mp3"
    run = subprocess.run(["ffmpeg", "-v", "error", "-i", str(tmp_path / "plain.wav"), "-metadata", "TBPM=126",
                          "-metadata", "genre=Techno", str(tagged)], capture_output=True)
    if run.returncode != 0:
        pytest.skip("this FFmpeg has no MP3 encoder")
    assert bench_tempo.probe(tagged) == ("", 126.0, "Techno")

"""The note harness: each stage's frames become notes as a transcription counts them, the sets a folder holds are read with their notes, the score is mir_eval's with the invented notes named by kind, and the generated set reads through the shipped analysis and the note effects as pinned."""

import json
import shutil

import mido
import pytest

import bench_notes


def frames(cells: list[str], struck: dict[int, set[int]] | None = None) -> list[tuple[float, float, int]]:
    """One stage's notes from these cells, one per 20 ms frame, the strikes given per frame."""
    struck = struck or {}
    return bench_notes.frames_to_notes([20 * i for i in range(len(cells))], cells, [struck.get(i, set()) for i in range(len(cells))], 20.0)


def test_a_key_kept_on_consecutive_frames_is_one_note_from_its_first_frame_to_the_end_of_its_last():
    assert frames(["-", "39", "39", "39", "-"]) == [(0.02, 0.08, 60)]


def test_a_short_gap_is_the_same_note_still_sounding_and_a_longer_one_ends_it():
    assert frames(["39", "-", "-", "39"]) == [(0.0, 0.08, 60)]
    assert frames(["39", "-", "-", "-", "39"]) == [(0.0, 0.02, 60), (0.08, 0.1, 60)]


def test_a_strike_while_the_key_sounds_starts_a_new_note():
    # The attack's flash lit the key a frame early; the strike two frames on is the note's real start.
    assert frames(["39", "39", "39", "39"], {2: {39}}) == [(0.0, 0.04, 60), (0.04, 0.08, 60)]


def test_a_guitarset_annotation_names_each_note_with_its_string(tmp_path):
    doc = {"annotations": [
        {"namespace": "note_midi", "annotation_metadata": {"data_source": "0"}, "data": [{"time": 1.0, "duration": 0.5, "value": 40.2, "confidence": None}]},
        {"namespace": "note_midi", "annotation_metadata": {"data_source": "5"}, "data": [{"time": 0.5, "duration": 0.25, "value": 64.0, "confidence": None}]},
        {"namespace": "chord", "data": [{"time": 0, "duration": 1, "value": "C:maj"}]}]}
    (tmp_path / "x.jams").write_text(json.dumps(doc))
    assert bench_notes.jams_notes(tmp_path / "x.jams") == [(0.5, 0.75, 64, 5), (1.0, 1.5, 40, 0)]


def test_a_urmp_note_file_gives_its_pitches_in_hertz_as_midi(tmp_path):
    (tmp_path / "Notes_1_vn_01_X.txt").write_text("0.50\t440.0\t0.25\n1.00\t261.63\t0.50\n")
    assert bench_notes.urmp_notes(tmp_path / "Notes_1_vn_01_X.txt") == [(0.5, 0.75, 69), (1.0, 1.5, 60)]


def test_a_midi_file_gives_its_notes_and_leaves_the_drums_out(tmp_path):
    mid = mido.MidiFile(ticks_per_beat=480)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track += [mido.Message("note_on", note=60, velocity=90, time=0), mido.Message("note_on", note=36, velocity=90, channel=9, time=0),
              mido.Message("note_off", note=60, time=480), mido.Message("note_on", note=64, velocity=0, time=0)]
    mid.save(tmp_path / "x.mid")
    assert bench_notes.midi_notes(tmp_path / "x.mid") == [(0.0, 0.5, 60)]   # 480 ticks at 120 BPM


def test_a_folder_is_read_by_the_set_its_files_name(tmp_path):
    (tmp_path / "audio_mono-mic").mkdir()
    (tmp_path / "annotation").mkdir()
    (tmp_path / "audio_mono-mic" / "00_A_mic.wav").write_bytes(b"")
    (tmp_path / "annotation" / "00_A.jams").write_text(json.dumps({"annotations": []}))
    (tmp_path / "piece.wav").write_bytes(b"")
    mido.MidiFile().save(tmp_path / "piece.midi")
    (tmp_path / "lonely.wav").write_bytes(b"")   # no notes beside it: not scored
    names = {p.name for p, _ in bench_notes.collect(tmp_path, 0)}
    assert names == {"00_A_mic.wav", "piece.wav"}


def test_a_stage_that_plays_the_score_scores_whole_and_its_inventions_are_named_by_kind():
    ref = [(1.0, 1.5, 60), (2.0, 2.5, 64)]
    perfect = bench_notes.score(ref, list(ref))
    assert perfect["p"] == perfect["r"] == perfect["f"] == perfect["f_off"] == 1.0
    noisy = bench_notes.score(ref, ref + [(1.0, 1.5, 72), (2.0, 2.4, 65), (1.0, 1.03, 90), (3.0, 3.5, 50)])
    assert (noisy["overtone"], noisy["leakage"], noisy["flash"], noisy["other"]) == (1, 1, 1, 1)
    assert noisy["r"] == 1.0 and noisy["p"] == pytest.approx(2 / 6)


def test_the_generated_set_writes_its_tracks_its_notes_and_its_version(tmp_path):
    bench_notes.generate(tmp_path)
    items = dict((p.name, notes) for p, notes in bench_notes.collect(tmp_path, 0))
    assert set(items) == {"scale-c-major.wav", "triads.wav", "solo-over-chord.wav", "melody-over-bass.wav"}
    assert [n[2] for n in items["scale-c-major.wav"]] == [60, 62, 64, 65, 67, 69, 71, 72]
    assert (tmp_path / "version.txt").read_text().strip() == bench_notes.GENERATED_VERSION


@pytest.mark.skipif(not bench_notes.bench_tempo.binary().exists() or shutil.which("ffmpeg") is None,
                    reason="needs mm_tempo (build_desktop.py --tests or bench_notes.py builds it) and FFmpeg")
def test_the_generated_set_reads_through_the_shipped_analysis_and_the_note_effects_as_pinned(tmp_path):
    bench_notes.generate(tmp_path / "generated")
    rows = {r["name"]: r["scores"] for r in bench_notes.run(bench_notes.collect(tmp_path / "generated", 0), tmp_path / "out", no_build=True)}
    # The engine hears seven of the scale's eight notes: F4 comes a semitone above E4, whose leakage already lights its key, so F4's start marks no onset.
    assert rows["scale-c-major"]["sounding"]["r"] == pytest.approx(7 / 8)
    assert rows["triads"]["sieve"]["f"] > rows["triads"]["sounding"]["f"]                  # the sieve takes overtones out of held chords
    assert rows["triads"]["sieve"]["overtone"] < rows["triads"]["sounding"]["overtone"]
    assert rows["scale-c-major"]["strings"]["f"] > rows["scale-c-major"]["sounding"]["f"]  # one note a string keeps the melody, not the attack's flash

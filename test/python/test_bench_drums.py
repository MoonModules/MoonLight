"""The drum harness: MDB Drums' tracks are read with their labeled hits, each frame field stands for its drums, and a field's hits are scored with mir_eval's onset measure and their lag."""

import shutil

import pytest

import bench_drums


def track(root, name: str, lines: str, mix: bool = False) -> None:
    """One MDB Drums track: its class annotation and its audio, as the set lays them out."""
    ann = root / "annotations" / "class"
    ann.mkdir(parents=True, exist_ok=True)
    (ann / f"{name}_class.txt").write_text(lines)
    audio = root / "audio" / ("full_mix" if mix else "drum_only")
    audio.mkdir(parents=True, exist_ok=True)
    (audio / f"{name}_{'MIX' if mix else 'Drum'}.wav").write_bytes(b"")


def test_a_track_is_read_with_its_hits_by_drum_and_the_resource_forks_are_not(tmp_path):
    track(tmp_path, "Song", "0.010000 \t KD \n0.570000 \t SD \n0.570000 \t KD \n")
    (tmp_path / "annotations" / "class" / "._Song_class.txt").write_bytes(b"\x00")
    items = bench_drums.collect(tmp_path, mix=False, limit=0)
    assert [p.name for p, _ in items] == ["Song_Drum.wav"]
    assert items[0][1] == {"KD": [0.01, 0.57], "SD": [0.57]}
    assert bench_drums.collect(tmp_path, mix=True, limit=0) == []   # no full mix beside it


def test_each_field_stands_for_its_drums_and_a_kick_and_snare_struck_together_are_one_hit_for_onset():
    hits = {"KD": [0.0, 1.0], "SD": [1.005], "HH": [0.5], "CY": [2.0], "TT": [3.0]}
    ref = lambda field: bench_drums.reference(hits, bench_drums.FIELDS[field])
    assert ref("onsetLow") == [0.0, 1.0]
    assert ref("onsetMid") == [1.005, 3.0]
    assert ref("onsetHigh") == [0.5, 2.0]
    assert ref("onset") == [0.0, 0.5, 1.0, 2.0, 3.0]


def test_a_field_hit_is_a_frame_above_zero_and_scores_within_50_ms_with_its_lag():
    frames = [[0, 0], [20, 90], [40, 0], [1000, 0], [1020, 60]]   # ms, the field
    est = bench_drums.found(frames, 1)
    assert est == [0.02, 1.02]
    s = bench_drums.score([0.0, 1.0, 2.0], est)
    assert s["p"] == 1.0 and s["r"] == pytest.approx(2 / 3)
    assert s["lags"] == [20, 20]
    assert bench_drums.score([0.0], [0.2])["r"] == 0.0   # 200 ms late is a miss


def test_drumkits_toms_are_any_of_its_four_tom_bits():
    frames = [[0, 0x01], [20, 0x10], [40, 0x40], [60, 0x02]]   # ms, kit: the kick, the low rack tom, the high rack tom, the snare
    mask, _ = bench_drums.KIT["toms"]
    assert bench_drums.found(frames, 1, mask) == [0.02, 0.04]
    assert bench_drums.found(frames, 1, bench_drums.KIT["kick"][0]) == [0.0]


@pytest.mark.skipif(not bench_drums.bench_tempo.binary().exists() or shutil.which("ffmpeg") is None,
                    reason="needs mm_tempo (build_desktop.py --tests or bench_drums.py builds it) and FFmpeg")
def test_on_a_drum_input_the_walks_kick_lights_the_bass_and_not_the_mids_and_its_snare_the_mids_and_not_the_bass(tmp_path):
    bench_drums.generate(tmp_path)
    res = bench_drums.bench_tempo.analyze(tmp_path / "drum-walk.wav", bench_drums.bench_tempo.binary(), ("--drums", "--drum-input"))
    low, mid = res["columns"].index("onsetLow"), res["columns"].index("onsetMid")
    def lights(at: float, column: int) -> bool:
        return any(f[column] for f in res["frames"] if at - 0.03 <= f[0] / 1000 <= at + 0.09)
    (kick, _, _), (snare, _, _) = bench_drums.walk()[:2]
    assert lights(kick, low) and not lights(kick, mid)
    assert lights(snare, mid) and not lights(snare, low)

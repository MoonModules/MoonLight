# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The tempo harness's verdicts: each reading is named by the ratio it sits at, and the report keeps unscored files out of its percentages."""

import bench_tempo


def verdict(est, ref):
    return bench_tempo.judge(est, ref)["verdict"]


def test_each_reading_is_named_by_the_ratio_of_the_label_it_sits_at():
    assert verdict(128.5, 128) == "right"
    assert verdict(132, 128) == "within 4%"
    assert verdict(256, 128) == "double"
    assert verdict(64.2, 128) == "half"
    assert verdict(384, 128) == "triple" and verdict(42.7, 128) == "third"
    assert verdict(192, 128) == "3:2" and verdict(85.4, 128) == "3:2"
    assert verdict(122.86, 92) == "4:3" and verdict(96, 128) == "4:3"
    assert verdict(136, 109) == "wrong"   # 5:4, a ratio the verdicts do not name
    assert verdict(None, 128) == "no tempo"


def test_acc2_counts_exactly_the_readings_named_right_within_4_percent_double_half_triple_or_third():
    # Every ratio is judged at Acc2's 4%, so a reading 3% off half is named half, not wrong, and the Misses agree with Acc2.
    assert verdict(66, 128) == "half"
    for est in [x / 4 for x in range(80, 1600)]:
        j = bench_tempo.judge(est, 128)
        assert j["acc2_4"] == (j["verdict"] in ("right", "within 4%", "double", "half", "triple", "third")), est


def test_a_track_without_a_label_is_not_judged():
    assert bench_tempo.judge(120, None) == {}


def test_acc1_takes_2_and_4_percent_and_acc2_forgives_double_half_and_thirds():
    j = bench_tempo.judge(132, 128)
    assert not j["acc1_2"] and j["acc1_4"]
    j = bench_tempo.judge(384, 128)
    assert not j["acc1_4"] and j["acc2_2"] and j["acc2_4"]
    assert not bench_tempo.judge(192, 128)["acc2_4"]   # 3:2 is not an Acc2 factor


def row(name, est, ref, error=""):
    return {"name": name, "id": "", "hash": name, "genre": "g", "source": "store", "est": est, "ref": ref,
            "error": error, "judge": bench_tempo.judge(est, ref) if not error else {}}


def test_a_file_that_failed_to_decode_is_listed_apart_and_left_out_of_every_percentage():
    rows = [row("a", 128, 128), row("b", None, 128, error="decode: bad")]
    text = bench_tempo.report(rows, "baseline", private=False)
    assert "| store | 1 | 100.0% |" in text
    assert "## Skipped and damaged files" in text and "| b | decode: bad |" in text


def test_a_private_report_names_tracks_by_their_hash():
    rows = [row("Artist - Song", 100, 128)]
    rows[0]["hash"] = "0123456789abcdef"
    text = bench_tempo.report(rows, "baseline", private=True)
    assert "Artist - Song" not in text and "0123456789abcdef" in text


def test_per_genre_figures_never_mix_label_sources_and_misses_name_their_source():
    rows = [dict(row(f"t{i}", 64 if i % 2 else 128, 128), source="tapped", genre="House") for i in range(2)]
    rows += [dict(row(f"s{i}", 128, 128), source="store", genre="House") for i in range(8)]
    text = bench_tempo.report(rows, "baseline", private=False)
    assert "## Per genre, tapped labels" in text and "## Per genre, store labels" in text
    assert "| House | 2 | 50.0% |" in text and "| House | 8 | 100.0% |" in text and "| House | 10 |" not in text
    assert "| tapped | 128 | 64 |" in text   # the miss names its label's source

# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The tempo harness's baseline reads a pulse train's tempo, and nothing from silence or a short curve."""

from _tempo_baseline import baseline_bpm

FPS = 24000 / 512.0


def pulses(bpm: float, seconds: float = 30.0) -> list[int]:
    """A flux curve with one block-wide peak on every beat, the shape a kick gives the onset function."""
    n = int(seconds * FPS)
    period = 60.0 * FPS / bpm
    return [200 if (i % period) < 1.0 else 0 for i in range(n)]


def test_the_baseline_reads_a_pulse_train_within_half_a_percent_from_70_to_165_bpm():
    for bpm in (70, 90, 120, 128, 140, 165):
        est = baseline_bpm(pulses(bpm), FPS)
        assert abs(est / bpm - 1) < 0.005, (bpm, est)


def test_the_baseline_reads_half_from_170_bpm_where_its_prior_favors_the_slower_octave():
    # The prior is a log-normal at 120 BPM, one octave wide, so it weighs 170 and 85 alike (120 * sqrt(2) is 169.7); the half wins above.
    for bpm in (170, 174, 185, 200):
        est = baseline_bpm(pulses(bpm), FPS)
        assert abs(est / (bpm / 2) - 1) < 0.005, (bpm, est)


def test_the_baseline_reads_nothing_from_silence_or_from_a_curve_shorter_than_eight_seconds():
    assert baseline_bpm([0] * 2000, FPS) is None
    assert baseline_bpm(pulses(120, seconds=5), FPS) is None



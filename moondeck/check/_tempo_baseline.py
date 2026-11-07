"""The tempo harness's baseline: Ellis's autocorrelation tempo from AudioService's flux, until AudioFrame carries its own.

``baseline_bpm`` reads one tempo from a whole track's flux curve, as ``mm_tempo`` prints it, a frame a block.

@attribution Ellis, librosa
"""

import math


def baseline_bpm(flux: list[int], fps: float, lo: float = 60.0, hi: float = 200.0) -> float | None:
    """Ellis (2007) with librosa's prior: the onset curve's autocorrelation under a log-normal prior at 120 BPM.

    The whole-track autocorrelation of the onset curve is Ellis's; the prior, one octave wide, is librosa's default (Ellis's paper uses 1.4 octaves). The curve is the flux less its one-second running mean, half-wave rectified and smoothed, and the prior picks the metrical level from whole-frame lags. The lag is then refined finer than a frame by a parabola through the peak at each of its first four multiples, the longer ones weighing more: neither source does this, but at 24 kHz the nearest whole-frame lag can be 2.8% off at 160 BPM, wider than the 2% the report heads with.
    """
    n = len(flux)
    if n < fps * 8:
        return None
    w = max(1, int(fps))
    run, acc = [], 0.0
    for i, v in enumerate(flux):
        acc += v
        if i >= w:
            acc -= flux[i - w]
        run.append(acc / min(i + 1, w))
    rect = [max(0.0, v - m) for v, m in zip(flux, run)]
    if not any(rect):
        return None
    # A triangle over five frames, so a beat period between two whole lags still lines its peaks up.
    kernel = (1.0, 2.0, 3.0, 2.0, 1.0)
    env = [sum(k * rect[i + j - 2] for j, k in enumerate(kernel) if 0 <= i + j - 2 < n) for i in range(n)]
    max_lag = int(math.ceil(4 * 60.0 * fps / (lo * 0.96))) + 2   # four multiples of the slowest refined lag
    acf = [sum(env[i] * env[i + lag] for i in range(n - lag)) for lag in range(max_lag + 1)]   # the plain sum, as Ellis and librosa take it

    def prior(bpm: float) -> float:
        return math.exp(-0.5 * math.log2(bpm / 120.0) ** 2)

    lags = range(max(1, math.floor(60.0 * fps / hi)), math.ceil(60.0 * fps / lo) + 1)   # inclusive at both ends
    level = max(lags, key=lambda lag: acf[lag] * prior(60.0 * fps / lag))
    if acf[level] <= 0:
        return None

    def peak(x: float) -> float | None:
        """The autocorrelation peak nearest lag x, placed between frames by a parabola through it and its neighbors."""
        lo_m, hi_m = max(1, round(x) - 1), min(max_lag - 1, round(x) + 1)
        if lo_m > hi_m:
            return None
        m = max(range(lo_m, hi_m + 1), key=lambda j: acf[j])
        a, b, c = acf[m - 1], acf[m], acf[m + 1]
        den = a - 2 * b + c
        return m + (max(-0.5, min(0.5, 0.5 * (a - c) / den)) if den < 0 else 0.0)

    # Each multiple k places a peak near k lags; the lag is their sum over the sum of k, so the longer ones weigh more.
    tau, lag_sum, k_sum = float(level), 0.0, 0
    for k in range(1, 5):
        p = peak(k * tau)
        if p is None:
            break
        lag_sum, k_sum = lag_sum + p, k_sum + k
        tau = lag_sum / k_sum
    return round(60.0 * fps / tau, 2)

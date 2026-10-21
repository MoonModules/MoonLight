"""Shared observation update for observed.<target> blocks: a rolling sample window
plus the statistics derived from it.

Both runners (moondeck/scenario/run_scenario.py and run_live_scenario.py) persist
per-target measurements for each scalar, kept in a local store (see `record`), with a summary published into the scenario file only on request.

WHY NOT [min, max]. The previous shape was a widen-only range, and widen-only is the
flaw: nothing ever narrows it, so the max converges on the worst thing that ever
happened on any machine rather than on what the code costs. One contended run pushed a
step from 156 to 2131 us permanently (2026-08-28, a desktop build streaming while the
scenarios measured). A number that only ever grows is not a signal, and comparing
against it hides the regressions it exists to catch.

WHY NOT mean and standard deviation. Tick times are not normally distributed: there is
a hard floor (the real work) and a one-sided tail of excursions (scheduler, cache,
contention), which is roughly log-normal. The mean is dragged by that tail, so it moves
when the machine is busy rather than when the code changes, and "outside 1 or 2 sigma"
has no stable meaning on a skewed distribution -- 2 sigma is ~5% of samples only if the
data is normal, and this data is not.

SO: keep a bounded window of raw samples and derive order statistics from it.
  - p50 (median): what it normally costs. Outliers cannot drag it.
  - p95: the tail, replacing max as the regression indicator. A real regression moves
    it; a single bad run does not.
  - min: the floor, i.e. the uncontended cost, which is genuinely informative.
  - max: the outlier tell.
  - n: how many samples back the numbers. n=1 is a first impression, not a baseline,
    and the reader can see which they are looking at.

Shape, in the local store; the scenario file's summary is the same without `samples`:
    observed.<target> = {
        "tick_us":         {"p50":…, "p95":…, "min":…, "max":…, "n":…, "samples":[…]},
        "free_heap":       {…},
        "max_alloc_block": {…},
        "last_updated":    iso_date,
    }

`last_updated` is the date this block last took a sample: the question a reader has is
whether the numbers are fresh, and after kWindow runs a first-seen date describes
samples that have long aged out.

The window is bounded (kWindow) so the store cannot grow without limit and so an old
contended sample eventually ages out.

The "what to watch" mapping is a property of the contract direction (tick contract =
ceiling, so p95 is the failure indicator; heap/block contract = floor, so min is).
See docs/reference/testing.md § Persistent observations.
"""
from __future__ import annotations

import json
import os
from pathlib import Path


_FIELDS = ("tick_us", "free_heap", "max_alloc_block")

# 0 is never a measurement, in any field, so it never enters a window.
#
# A tick of 0 us means the step ran below the host clock's resolution, not that it was free; a
# window holding those reports a median of 0, a step that looks infinitely fast. A free_heap or
# max_alloc_block of 0 is the desktop platform saying "no meaningful ceiling", which is a
# CONSTANT: the value never varies (verified across every scenario file: desktop has exactly one
# distinct value, 0), so a 32-sample window of it is 32 copies of a fact that could not change.
# Recording nothing leaves n=0, which the report already renders as "not measured here" -- the
# honest answer for a target that has no such limit. Every ESP32 target reports real varying
# numbers and is untouched by this.

# Samples kept per field. Enough for a p95 to mean something (the 95th percentile of 32
# samples is the second-worst, which a one-off cannot reach), small enough that the JSON
# stays readable and a stale measurement ages out within a few dozen runs.
kWindow = 32


def _pct(sorted_vals: list[int], q: float) -> int:
    """The q-quantile by nearest-rank, the definition that returns an ACTUAL observed
    sample rather than an interpolation between two. A measured number is what a reader
    can go and reproduce; an interpolated one never happened."""
    if not sorted_vals:
        return 0
    import math
    rank = max(1, math.ceil(q * len(sorted_vals)))
    return int(sorted_vals[min(rank, len(sorted_vals)) - 1])


def _stats(samples: list[int]) -> dict:
    """Derive the reported statistics from a sample window."""
    if not samples:
        return {"p50": 0, "p95": 0, "min": 0, "max": 0, "n": 0, "samples": []}
    s = sorted(int(v) for v in samples)
    return {
        "p50": _pct(s, 0.50),
        "p95": _pct(s, 0.95),
        "min": s[0],
        "max": s[-1],
        "n": len(s),
        # Stored in ARRIVAL order, not sorted: the window is a history, and dropping the
        # oldest is only meaningful if order is preserved.
        "samples": [int(v) for v in samples],
    }


def _window_of(block: dict, field: str) -> list[int]:
    """The existing sample window for `field`, empty where there is none yet."""
    cur = block.get(field)
    w = cur.get("samples") if isinstance(cur, dict) else None
    return [int(v) for v in w] if isinstance(w, list) else []


def widen(existing: dict | None, sample: dict, today: str) -> tuple[dict, bool]:
    """Return (new_observed_block, changed) for a fresh measurement.

    It appends to a rolling window; `record` calls it on every measured step of both runners.

    `changed` is True whenever a sample was recorded, because the sample IS the state:
    the window has to be persisted for the next run to build on it, and a caller that
    skips the write drops the measurement entirely.

    The window is returned for the caller to keep: `record` keeps it in the machine's own store, and only a summary
    of it is ever published into a scenario file, on request.
    """
    block = dict(existing) if isinstance(existing, dict) else {}
    recorded = False
    for f in _FIELDS:
        if f not in sample:
            continue
        value = int(sample[f])
        if value == 0:
            continue          # not a measurement: see the note above _FIELDS
        window = _window_of(block, f)
        window.append(value)
        if len(window) > kWindow:
            window = window[-kWindow:]          # drop the oldest, keep arrival order
        block[f] = _stats(window)
        recorded = True

    if not recorded:
        return (existing if isinstance(existing, dict) else {}), False

    block["last_updated"] = today
    return block, True


def reset(sample: dict, today: str) -> dict:
    """Build a fresh observed block from a single measurement -- called when the contract
    is renegotiated and the previous window described the PREVIOUS contract."""
    block = {f: _stats([] if int(sample[f]) == 0 else [int(sample[f])])
             for f in _FIELDS if f in sample}
    block["last_updated"] = today
    return block


# Where the sample windows live: on the machine that measured them, under the build folder, never committed. A window is a
# machine's own history, and writing it into the scenario file made every passing run rewrite a tracked file. The scenario
# file keeps the contract it is compared against and, when a bench run is published with --update-observed, a summary.
STORE = Path(__file__).resolve().parents[2] / "build" / "moondeck-logs" / "observations.json"


def _read_store(path: Path = STORE) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def record(scenario: str, step: str, target: str, sample: dict, today: str, reset_window: bool = False, path: Path = STORE) -> dict:
    """Take one measurement into this machine's window for a scenario step on a target, and return the block, samples included.

    `reset_window` starts the window over from this sample, as a renegotiated contract does, since the old window described the old promise."""
    store = _read_store(path)
    key = f"{scenario}/{step}/{target}"
    if reset_window:
        block, changed = reset(sample, today), True
    else:
        block, changed = widen(store.get(key), sample, today)
    if changed:
        store[key] = block
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(json.dumps(store, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(tmp, path)
    return block


def summary(block: dict) -> dict:
    """The block a scenario file publishes: the statistics without the samples behind them, so it stays small and a diff shows numbers, not windows."""
    out = {f: {k: v for k, v in block[f].items() if k != "samples"} for f in _FIELDS if isinstance(block.get(f), dict)}
    if "last_updated" in block:
        out["last_updated"] = block["last_updated"]
    return out


def save_scenario(path, scenario) -> None:
    """Write a scenario JSON the one way both runners write it, so a file's shape never depends on which runner touched it last."""
    import json

    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps(scenario, indent=2, ensure_ascii=False) + "\n")

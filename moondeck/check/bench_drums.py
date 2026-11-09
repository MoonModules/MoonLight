#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["mir_eval>=0.7"]
# ///
"""Drum harness: run drum recordings whose hits are labeled through the shipped analysis on the desktop, and score each onset the frame carries against the drums it stands for.

Builds and runs the ``mm_tempo`` target in its ``--drums`` mode (``test/bench/bench_tempo.cpp``), which feeds decoded audio through the boards' block analysis and prints each frame's ``onset``, ``onsetLow``, ``onsetMid`` and ``onsetHigh``. A frame whose field is above 0 is a hit of that field, scored with mir_eval's onset measure, a hit counting within 50 ms of a labeled one: ``onsetLow`` against the kick, ``onsetMid`` against the snare and the toms, ``onsetHigh`` against the hi-hat and the cymbals, ``onset`` against every drum. DrumKit's own drums are scored too, through its `hitsOf`: its kick, snare and hi-hat, and its four toms together against the toms. The latency is the median lag from a labeled hit to the frame that found it.

The set is MDB Drums (Southall et al., ISMIR 2017, CC BY-NC-SA 4.0), 23 MedleyDB tracks with every hit labeled by drum, read from its ``annotations/class/`` and its ``audio/drum_only/`` or, with ``--mix``, ``audio/full_mix/``. The data stays where it is, given by ``--dir`` and only read; runs and reports go to ``--out`` (default ``build/drums/``). A report, not a check: nothing passes or fails.

Usage:
    uv run moondeck/check/bench_drums.py --dir <MDB Drums folder> --instrument drums   # the drums alone, as a drum input
    uv run moondeck/check/bench_drums.py --dir <MDB Drums folder> --mix    # the drums inside the band
"""

import argparse
import concurrent.futures
import csv
import json
import math
import os
import random
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_tempo  # noqa: E402  the one home of the build, the decode and the run's plumbing

OUT_DIR = bench_tempo.ROOT / "build" / "drums"
WINDOW_S = 0.05       # mir_eval's onset window and the field's
SAME_HIT_S = 0.02     # labels this close are one hit for `onset`, a kick and a snare struck together
FIELDS = {"onsetLow": ("KD",), "onsetMid": ("SD", "TT"), "onsetHigh": ("HH", "CY"), "onset": ("KD", "SD", "TT", "HH", "CY", "OT")}
FIELD_NAMES = {"onsetLow": "onsetLow, the kick", "onsetMid": "onsetMid, snare and toms", "onsetHigh": "onsetHigh, hi-hat and cymbals", "onset": "onset, every drum"}
KIT = {"kick": (0x01, ("KD",)), "snare": (0x02, ("SD",)), "hi-hat": (0x04, ("HH", "CY")), "toms": (0x78, ("TT",))}   # DrumKit's drums as `kit` bits, and the labels each stands for


# ---------------------------------------------------------------- the generated drum walk

GENERATED_VERSION = "6"   # raise it when generate() changes, so --generated writes the walk again
RATES = (4, 8, 12, 16, 20)   # the speed ramps, hits a second
KEPT_APART = 0.9             # a rate a drum keeps apart: this share of its hits lit
RATE = bench_tempo.RATE
# Each drum's rise per third-octave center, in dB over the room, as MDB Drums' isolated hits rise in their first two frames (2026-11-08): a kick at 40 to 125 Hz,
# a snare at 160 to 200 Hz, a tom at its own pitch; the kick's beater click, the snare's wires, the hi-hat and the cymbal are noise above them.
KICK = {40: 14, 50: 34, 63: 40, 80: 39, 100: 35, 125: 29, 160: 22, 200: 13, 250: 4}
SNARE = {100: 2, 125: 5, 160: 13, 200: 16, 250: 6, 315: 4}
TOM = {80: 4, 100: 13, 125: 19, 160: 26, 200: 22, 250: 12, 315: 8, 400: 3}
THIRDS = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000]
TOM_SHIFTS = (-2, -1, 1, 3)   # the floor tom up to the high rack tom, in thirds from the measured tom's pitch: DrumKit's four


def shifted(profile: dict, thirds: int) -> dict:
    """A profile moved by `thirds` third octaves, a tom tuned higher or lower."""
    return {THIRDS[THIRDS.index(hz) + thirds]: db for hz, db in profile.items() if 0 <= THIRDS.index(hz) + thirds < len(THIRDS)}


def hit(buf: list[float], at: float, drum: str, rng: random.Random) -> None:
    """One drum's hit at `at` seconds into `buf`: its profile's tones, or its noise, attacked in 2 ms and decaying."""
    start = int(at * RATE)
    tones = {"KD": (KICK, 0.08), "SD": (SNARE, 0.08)}.get(drum) or (shifted(TOM, TOM_SHIFTS[int(drum[2]) - 1]), 0.25) if drum.startswith("TT") or drum in ("KD", "SD") else None
    if tones:
        profile, decay = tones
        parts = [(hz, 0.01 * 10 ** (db / 20), rng.uniform(0, 2 * math.pi)) for hz, db in profile.items()]
        for i in range(start, min(len(buf), start + int(5 * decay * RATE))):
            t = (i - start) / RATE
            env = min(1.0, t / 0.002) * math.exp(-t / decay)
            buf[i] += env * sum(a * math.sin(2 * math.pi * hz * t + ph) for hz, a, ph in parts)
    noise = {"KD": (2.5, 0.003, 2), "SD": (0.15, 0.06, 2), "HH": (0.25, 0.03, 3), "CY": (0.2, 0.8, 3)}.get(drum)   # decays as real drums die away: a kick or a snare within 100 ms, the kick's beater click within 10, 19 dB under its body as MDB Drums' kicks click
    if noise:   # white noise differenced `order` times, rising 6 dB an octave each, so it sounds high and leaves the bass alone
        gain, decay, order = noise
        last = [0.0] * (order + 1)
        for i in range(start, min(len(buf), start + int(5 * decay * RATE))):
            t = (i - start) / RATE
            v = rng.uniform(-1, 1)
            for k in range(order):
                v, last[k] = v - last[k], v
            buf[i] += gain * min(1.0, t / 0.001) * math.exp(-t / decay) * v


def walk() -> list[tuple[float, str, str]]:
    """The walk's hits with their section: each drum alone half a second apart, twice; the same at a quarter second; kick and hi-hat together; then kick, snare and hi-hat each at RATES hits a second for a second, the section naming the drum and its rate."""
    order = ["KD", "SD", "TT1", "TT2", "TT3", "TT4", "HH", "CY"]
    hits, t = [], 0.5
    for gap in (0.5, 0.5, 0.25):
        for d in order:
            hits.append((t, d, "alone"))
            t += gap
    t += 0.5
    for _ in range(4):
        hits += [(t, "KD", "together"), (t, "HH", "together")]
        t += 0.5
    for d in ("KD", "SD", "HH"):
        for rate in RATES:
            t += 0.5
            for k in range(rate):
                hits.append((t + k / rate, d, f"{d} {rate}"))
            t += 1.0
    return hits


def generate(target: Path) -> None:
    """Write the drum walk, its labeled hits in drums.csv, and the generator's version."""
    target.mkdir(parents=True, exist_ok=True)
    hits = walk()
    rng = random.Random(7)   # the same walk every time
    buf = [0.0005 * rng.uniform(-1, 1) for _ in range(int((hits[-1][0] + 1.5) * RATE))]   # the room's hiss
    for at, drum, _ in hits:
        hit(buf, at, drum, rng)
    bench_tempo.write_wav(target / "drum-walk.wav", buf)
    with (target / "drums.csv").open("w", newline="", encoding="utf-8") as fh:
        out = csv.writer(fh)
        out.writerow(["path", "time", "class", "section"])
        out.writerows(["drum-walk.wav", f"{at:.4f}", "TT" if d.startswith("TT") else d, section] for at, d, section in hits)
    (target / "version.txt").write_text(GENERATED_VERSION + "\n", encoding="utf-8")
    print(f"Wrote the drum walk, {len(hits)} hits, to {target}")


# ---------------------------------------------------------------- reading the sets


def read_class(path: Path) -> dict[str, list[float]]:
    """An MDB Drums class annotation, `time <tab> class` a line, as each class's hit times in seconds."""
    hits: dict[str, list[float]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        cols = line.split()
        if len(cols) >= 2:
            hits.setdefault(cols[1], []).append(float(cols[0]))
    return {k: sorted(v) for k, v in hits.items()}


def collect(folder: Path, mix: bool, limit: int) -> list[tuple[Path, dict[str, list[float]]]]:
    """Each track with its labeled hits: `annotations/class/<name>_class.txt` beside `audio/drum_only/<name>_Drum.wav`, or `audio/full_mix/<name>_MIX.wav` with `mix`."""
    items = []
    labels = folder / "drums.csv"   # the generated walk: path, time, class
    if labels.exists():
        by_file: dict[str, dict[str, list[float]]] = {}
        for row in csv.DictReader(labels.open(encoding="utf-8")):
            by_file.setdefault(row["path"], {}).setdefault(row["class"], []).append(float(row["time"]))
            if row.get("section", "").count(" "):   # a speed ramp, "KD 12": its hits kept apart, scored on their own
                by_file[row["path"]].setdefault("@" + row["section"], []).append(float(row["time"]))
        items += [(folder / name, {c: sorted(ts) for c, ts in hits.items()}) for name, hits in sorted(by_file.items())]
    for ann in sorted(p for p in folder.rglob("annotations/class/*_class.txt") if not p.name.startswith("._")):
        name = ann.name[: -len("_class.txt")]
        audio = ann.parent.parent.parent / "audio" / ("full_mix" if mix else "drum_only") / f"{name}_{'MIX' if mix else 'Drum'}.wav"
        if audio.exists():
            items.append((audio, read_class(ann)))
    return bench_tempo.sample(items, limit) if limit else items


def reference(hits: dict[str, list[float]], classes: tuple[str, ...]) -> list[float]:
    """The labeled hits of `classes`, labels within SAME_HIT_S of each other one hit."""
    times = sorted(t for c in classes for t in hits.get(c, []))
    merged: list[float] = []
    for t in times:
        if not merged or t - merged[-1] > SAME_HIT_S:
            merged.append(t)
    return merged


def found(frames: list[list[int]], column: int, mask: int = -1) -> list[float]:
    """The times in seconds of the frames whose `column` is above 0, or has a bit of `mask`."""
    return [f[0] / 1000.0 for f in frames if f[column] & mask]


def score(ref: list[float], est: list[float]) -> dict:
    """mir_eval's onset precision, recall and F within WINDOW_S, and each matched hit's lag in milliseconds."""
    import numpy as np
    import mir_eval
    if not ref:
        return {"n_ref": 0, "n_est": len(est)}
    r, e = np.array(ref), np.array(est)
    pairs = mir_eval.util.match_events(r, e, WINDOW_S) if len(e) else []
    p = len(pairs) / len(e) if len(e) else 0.0
    rc = len(pairs) / len(r)
    return {"n_ref": len(r), "n_est": len(e), "p": p, "r": rc, "f": 2 * p * rc / (p + rc) if p + rc else 0.0,
            "lags": [round(1000 * (e[j] - r[i])) for i, j in pairs]}


def analyze(path: Path, hits: dict[str, list[float]], exe: Path, instrument: str = "mix") -> dict:
    """One track's row: its frames through mm_tempo --drums, the Audio card's `instrument` set, and each field's score; a track that fails is a row naming why."""
    try:
        res = bench_tempo.analyze(path, exe, ("--drums", "--drum-input") if instrument == "drums" else ("--drums",))
        if "error" in res:
            return {"name": path.stem, "error": res["error"]}
        frames, columns = res["frames"], res["columns"]
        scores = {f: score(reference(hits, FIELDS[f]), found(frames, columns.index(f))) for f in FIELDS}
        scores |= {d: score(reference(hits, classes), found(frames, columns.index("kit"), mask)) for d, (mask, classes) in KIT.items()}
        kit_of = {"KD": "kick", "SD": "snare", "HH": "hi-hat"}
        speed = {k[1:]: score(sorted(v), found(frames, columns.index("kit"), KIT[kit_of[k[1:].split()[0]]][0]))["r"] for k, v in hits.items() if k.startswith("@")}
        return {"name": path.stem, "scores": scores, "speed": speed}
    except Exception as e:   # one damaged file must not end the run
        return {"name": path.stem, "error": f"{type(e).__name__}: {e}"}


def pct(x) -> str:
    return "" if x is None else f"{100 * x:.0f}%"


def report(rows: list[dict], mix: bool, instrument: str = "mix") -> str:
    """The run's page: each field over every track, the counts summed, then each track's F per field."""
    good = [r for r in rows if "scores" in r]
    lines = ["# Drums report", "", f"{len(good)} tracks scored, the drums {'inside the band' if mix else 'alone'}, the Audio card's instrument at {instrument}; a hit counts within "
             f"{WINDOW_S * 1000:.0f} ms of a labeled one, and the latency is the median lag from a labeled hit to the frame that found it.", "",
             "| Field | Hits | Found | Precision | Recall | F | Latency |", "|---|---|---|---|---|---|---|"]
    def row(key: str, name: str) -> str:
        s = [r["scores"][key] for r in good if r["scores"][key].get("n_ref")]
        n_ref, n_est = sum(x["n_ref"] for x in s), sum(x["n_est"] for x in s)
        matched = sum(round(x["r"] * x["n_ref"]) for x in s)
        p, rc = (matched / n_est if n_est else 0.0), (matched / n_ref if n_ref else 0.0)
        lags = sorted(lag for x in s for lag in x["lags"])
        return (f"| {name} | {n_ref} | {n_est} | {pct(p)} | {pct(rc)} | {pct(2 * p * rc / (p + rc) if p + rc else 0.0)} | "
                f"{f'{lags[len(lags) // 2]} ms' if lags else ''} |")
    lines += [row(f, FIELD_NAMES[f]) for f in FIELDS]
    lines += ["", "What DrumKit lights, each drum against the labels it stands for:", "", "| DrumKit's drum | Hits | Lit | Precision | Recall | F | Latency |",
              "|---|---|---|---|---|---|---|"] + [row(d, d) for d in KIT]
    for r in good:
        if r.get("speed"):
            lines += ["", f"How fast DrumKit keeps a drum's hits apart in {r['name']}: the share of hits it lights at each rate, and the fastest rate it lights {KEPT_APART:.0%} of.", "",
                      "| Drum | " + " | ".join(f"{n}/s" for n in RATES) + " | Kept apart up to |", "|---|" + "---|" * (len(RATES) + 1)]
            for d, name in (("KD", "kick"), ("SD", "snare"), ("HH", "hi-hat")):
                shares = [r["speed"].get(f"{d} {n}") for n in RATES]
                kept = [n for n, x in zip(RATES, shares) if x is not None and x >= KEPT_APART]
                lines.append(f"| {name} | " + " | ".join(pct(x) for x in shares) + f" | {f'{max(kept)}/s' if kept else 'none'} |")
    lines += ["", "| Track | " + " | ".join(FIELDS) + " |", "|---|" + "---|" * len(FIELDS)]
    for r in rows:
        if "error" in r:
            lines.append(f"| {r['name']} | {r['error']} |" + " |" * (len(FIELDS) - 1))
        else:
            lines.append(f"| {r['name']} | " + " | ".join(pct(r["scores"][f].get("f")) for f in FIELDS) + " |")
    return "\n".join(lines) + "\n"


def run(items: list, out: Path, mix: bool = False, jobs: int = 4, no_build: bool = False, instrument: str = "mix") -> list[dict]:
    """Analyze and score the tracks, write the run file and the report, and print the report."""
    if not shutil.which("ffmpeg"):
        sys.exit("The harness decodes with FFmpeg: install FFmpeg first")
    if not no_build:
        bench_tempo.build()
    exe = bench_tempo.binary()
    if not exe.exists():
        sys.exit(f"mm_tempo not built: {exe}")
    with concurrent.futures.ProcessPoolExecutor(jobs) as ex:   # processes: the scoring is Python
        rows = list(ex.map(analyze, [p for p, _ in items], [h for _, h in items], [exe] * len(items), [instrument] * len(items)))
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    (out / f"run-{stamp}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
    page = report(rows, mix, instrument)
    (out / "view.md").write_text(page, encoding="utf-8")
    print(page)
    return rows


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, help="the MDB Drums folder, read only; without it, the generated drum walk")
    ap.add_argument("--mix", action="store_true", help="the full mix, the drums inside the band, in place of the drums alone")
    ap.add_argument("--instrument", choices=("mix", "drums"), default="mix", help="the Audio card's instrument: a full mix, or the drums alone")
    ap.add_argument("--out", type=Path, default=OUT_DIR, help="where runs and reports go (default build/drums)")
    ap.add_argument("--limit", type=bench_tempo.at_least(0), default=0, help="at most N tracks, spread through the set")
    ap.add_argument("--jobs", type=bench_tempo.at_least(1), default=os.cpu_count() or 4, help="tracks analyzed at once")
    ap.add_argument("--no-build", action="store_true", help="run the already-built binary")
    args = ap.parse_args()
    folder = args.dir.expanduser().resolve() if args.dir else bench_tempo.generated_set(OUT_DIR, GENERATED_VERSION, generate)
    if not folder.is_dir():
        sys.exit(f"{folder}: not a folder (is the volume mounted?)")
    items = collect(folder, args.mix, args.limit)
    if not items:
        sys.exit("No MDB Drums tracks found: annotations/class/*_class.txt beside audio/drum_only/ and audio/full_mix/.")
    run(items, args.out, mix=args.mix, jobs=args.jobs, no_build=args.no_build, instrument=args.instrument)


if __name__ == "__main__":
    main()

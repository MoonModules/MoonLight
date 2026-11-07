#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["mir_eval>=0.7", "mido>=1.3"]
# ///
"""Note harness: run music whose notes are known through the note effects on the desktop, and score each stage against the score.

Builds and runs the ``mm_tempo`` target in its ``--notes`` mode (``test/bench/bench_tempo.cpp``), which feeds decoded audio through the shipped block analysis and through the Keyboard and Strings effects' own ``keep``, and prints, a frame at a time, the keys every stage keeps: the engine's sounding keys, the sieve and peak picking (``no overtones``), Keyboard's hands and Strings' fretting hand, each with and without ``chord``, and the strings and frets the hand plays. A report, not a check: nothing passes or fails.

Each stage's frames become notes, a key kept on consecutive frames being one note and a new strike starting the next, and are scored with mir_eval's transcription measure (Raffel et al. 2014): a note counts when its pitch matches and its start falls within 50 ms, and again with its end within a fifth of its length. A note the stage invents is named by its kind: a flash, shorter than 70 ms, the burst a note's attack spreads over the keys; an overtone, an octave, a twelfth or two octaves from a played one; leakage, a semitone or two away; the errors the sieve and the peak picking target. On a set that labels strings, GuitarSet, the share of notes the fretting hand puts on the labeled string is reported too.

The data stays where it is, given by ``--dir`` and only read; runs and reports go to ``--out`` (default ``build/notes/``), and the **Notes Report** card shows the latest (``--show``). The sets a folder can hold, each told by its files:

- audio beside a MIDI file of the same name (``.mid`` or ``.midi``): MIDI rendered through a sound font, or MAESTRO (Google Magenta, CC BY-NC-SA), whose performances come with their MIDI;
- GuitarSet (Xi et al. 2018, CC BY 4.0): ``audio_mono-mic/*_mic.wav`` beside ``annotation/*.jams``, each note labeled with its string;
- URMP (Li et al. 2018, CC BY-NC-SA): each part's ``AuSep_*.wav`` beside its ``Notes_*.txt``;
- the generated set (``--generated``), tones whose notes are exact by construction, written once to ``build/notes/generated``.

@attribution Raffel, Xi, Li and Hawthorne

Usage:
    uv run moondeck/check/bench_notes.py --generated                    # the exact-note set, written once
    uv run moondeck/check/bench_notes.py --dir <GuitarSet folder> --limit=20
    uv run moondeck/check/bench_notes.py --dir <folder> --instrument cello   # the Strings instrument the hand plays
    uv run moondeck/check/bench_notes.py --show                         # the latest report, in MoonDeck's View pane
"""

import argparse
import concurrent.futures
import csv
import json
import math
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_tempo  # noqa: E402  the one home of the build, the decode and the run's plumbing

ROOT = bench_tempo.ROOT
OUT_DIR = ROOT / "build" / "notes"
VIEW_URL = "/api/notes-view"   # moondeck.py serves OUT_DIR/view.md there, into the View pane
RATE = bench_tempo.RATE

STAGES = ("sounding", "sieve", "keys", "keysChord", "strings", "stringsChord")   # mm_tempo --notes's columns after the time
STAGE_NAMES = {"sounding": "all (the engine)", "sieve": "no overtones", "keys": "Keyboard hands", "keysChord": "Keyboard hands + chord",
               "strings": "Strings hands", "stringsChord": "Strings hands + chord"}
INSTRUMENTS = {"guitar": 0, "bass": 1, "violin": 2, "cello": 3}   # StringsEffect's Instrument order
OPEN_STRINGS = {"guitar": (40, 45, 50, 55, 59, 64), "bass": (28, 33, 38, 43), "violin": (55, 62, 69, 76), "cello": (36, 43, 50, 57)}
ONSET_TOLERANCE = 0.05   # seconds: mir_eval's default and the field's
OFFSET_RATIO = 0.2       # an end within a fifth of the note's length, mir_eval's default
GAP_FRAMES = 2           # a key missing for this many frames or fewer is the same note still sounding
FLASH_SECONDS = 0.07     # an invented note shorter than this is a flash, the attack's broadband burst lighting keys for a frame or three
OVERTONES = (12, 19, 24)
A0 = 21                  # MIDI note of key 0, the frame's numbering


# ---------------------------------------------------------------- frames to notes


def parse_keys(cell: str) -> dict[int, bool]:
    """A stage's cell: each key it keeps, and whether it was struck on this frame (`*`, the sounding column alone marks it)."""
    if cell == "-":
        return {}
    out = {}
    for tok in cell.split(","):
        key = tok.split(":")[0]
        out[int(key)] = tok.endswith("*")
    return out


def frames_to_notes(times_ms: list[int], cells: list[str], struck: list[set[int]], block_ms: float) -> list[tuple[float, float, int]]:
    """A stage's frames as notes (onset s, offset s, MIDI): a key kept on consecutive frames is one note, bridged over gaps of GAP_FRAMES or fewer, and a strike starts a new note, a repeated one or the real start after an attack's flash."""
    open_notes: dict[int, list] = {}   # key -> [onset ms, last kept frame index, first frame index]
    notes = []

    def close(key: int) -> None:
        on, last, _ = open_notes.pop(key)
        notes.append((on / 1000.0, (times_ms[last] + block_ms) / 1000.0, key + A0))

    for i, (t, cell) in enumerate(zip(times_ms, cells)):
        kept = parse_keys(cell)
        for key in list(open_notes):
            if key not in kept and i - open_notes[key][1] > GAP_FRAMES:
                close(key)
        for key in kept:
            if key in open_notes and key in struck[i] and open_notes[key][2] < i - 1:
                close(key)   # struck while it sounds: a new note
            if key in open_notes:
                open_notes[key][1] = i
            else:
                open_notes[key] = [t, i, i]
    for key in list(open_notes):
        close(key)
    return sorted(notes)


def stage_notes(frames: list[list[str]], block_ms: float) -> dict[str, list]:
    """Every stage's notes from mm_tempo --notes's frames, the strikes read from the sounding column."""
    times = [int(f[0]) for f in frames]
    struck = [{k for k, hit in parse_keys(f[1]).items() if hit} for f in frames]
    return {stage: frames_to_notes(times, [f[1 + j] for f in frames], struck, block_ms) for j, stage in enumerate(STAGES)}


def placements(frames: list[list[str]]) -> list[tuple[int, dict[int, int]]]:
    """The fretting hand's strings a frame: (time ms, {string: fret})."""
    out = []
    for f in frames:
        cell = f[1 + len(STAGES)]
        out.append((int(f[0]), {} if cell == "-" else {int(a): int(b) for a, b in (tok.split(":") for tok in cell.split(","))}))
    return out


# ---------------------------------------------------------------- reference notes


def midi_notes(path: Path) -> list[tuple[float, float, int]]:
    """A MIDI file's notes, (onset s, offset s, pitch), every track and channel but drums, the sustain pedal ignored as mir_eval's references do."""
    import mido
    notes, active = [], {}
    t = 0.0
    for msg in mido.MidiFile(str(path)):
        t += msg.time
        if msg.type not in ("note_on", "note_off") or getattr(msg, "channel", 0) == 9:
            continue
        key = (msg.channel, msg.note)
        if msg.type == "note_on" and msg.velocity > 0:
            if key in active:
                notes.append((active.pop(key), t, msg.note))
            active[key] = t
        elif key in active:
            notes.append((active.pop(key), t, msg.note))
    return sorted(notes)


def jams_notes(path: Path) -> list[tuple[float, float, int, int]]:
    """A GuitarSet annotation's notes, (onset s, offset s, pitch, string), the string its `note_midi` annotation's data source, the low E first."""
    doc = json.loads(path.read_text(encoding="utf-8"))
    notes = []
    for ann in doc.get("annotations", []):
        if ann.get("namespace") != "note_midi":
            continue
        string = int(ann.get("annotation_metadata", {}).get("data_source", 0))
        for obs in ann.get("data", []):
            notes.append((obs["time"], obs["time"] + obs["duration"], int(round(obs["value"])), string))
    return sorted(notes)


def urmp_notes(path: Path) -> list[tuple[float, float, int]]:
    """A URMP part's notes, one a line: onset s, pitch (Hz in the set's own files, MIDI accepted), duration s."""
    notes = []
    for line in path.read_text(encoding="utf-8").splitlines():
        cols = line.split()
        if len(cols) < 3:
            continue
        on, pitch, dur = float(cols[0]), float(cols[1]), float(cols[2])
        midi = int(round(69 + 12 * math.log2(pitch / 440.0))) if pitch > 127 else int(round(pitch))
        notes.append((on, on + dur, midi))
    return sorted(notes)


def collect(folder: Path, limit: int) -> list[tuple[Path, list]]:
    """Each audio file in `folder` with its reference notes, by the set its files name: GuitarSet, URMP, audio beside MIDI, or the generated set's notes.csv."""
    items = []
    for jams in sorted(folder.rglob("annotation/*.jams")):
        mic = next(iter(jams.parent.parent.glob(f"audio_mono-mic/{jams.stem}_mic.wav")), None)
        if mic:
            items.append((mic, jams_notes(jams)))
    for notes in sorted(folder.rglob("Notes_*.txt")):
        audio = notes.with_name("AuSep_" + notes.name[len("Notes_"):]).with_suffix(".wav")
        if audio.exists():
            items.append((audio, urmp_notes(notes)))
    for audio in sorted(p for p in folder.rglob("*") if p.suffix.lower() in bench_tempo.AUDIO_EXT):
        mid = next((audio.with_suffix(ext) for ext in (".mid", ".midi") if audio.with_suffix(ext).exists()), None)
        if mid:
            items.append((audio, midi_notes(mid)))
    labels = folder / "notes.csv"
    if labels.exists():
        by_file: dict[str, list] = {}
        for row in csv.DictReader(labels.open(encoding="utf-8")):
            by_file.setdefault(row["path"], []).append((float(row["onset"]), float(row["offset"]), int(row["midi"])))
        items += [(folder / name, sorted(notes)) for name, notes in sorted(by_file.items())]
    return bench_tempo.sample(items, limit) if limit else items


# ---------------------------------------------------------------- scoring


def hz(midi: int) -> float:
    return 440.0 * 2 ** ((midi - 69) / 12)


def score(ref: list, est: list) -> dict:
    """mir_eval's note precision, recall and F-measure, onset only and with offsets, and the unmatched notes the stage invents, by kind."""
    import numpy as np
    import mir_eval
    if not ref:
        return {"n_ref": 0, "n_est": len(est)}
    ri = np.array([[a, max(b, a + 1e-3)] for a, b, *_ in ref])
    rp = np.array([hz(n[2]) for n in ref])
    ei = np.array([[a, max(b, a + 1e-3)] for a, b, *_ in est]) if est else np.zeros((0, 2))
    ep = np.array([hz(n[2]) for n in est]) if est else np.zeros(0)
    p, r, f, _ = mir_eval.transcription.precision_recall_f1_overlap(ri, rp, ei, ep, onset_tolerance=ONSET_TOLERANCE, offset_ratio=None)
    _, _, f_off, _ = mir_eval.transcription.precision_recall_f1_overlap(ri, rp, ei, ep, onset_tolerance=ONSET_TOLERANCE, offset_ratio=OFFSET_RATIO)
    matched = {j for _, j in mir_eval.transcription.match_notes(ri, rp, ei, ep, onset_tolerance=ONSET_TOLERANCE, offset_ratio=None)}
    kinds = {"flash": 0, "overtone": 0, "leakage": 0, "other": 0}
    for j, (on, off, midi, *_) in enumerate(est):
        if j in matched:
            continue
        if off - on < FLASH_SECONDS:
            kinds["flash"] += 1
            continue
        near = [n[2] for n in ref if abs(n[0] - on) <= ONSET_TOLERANCE * 2 or n[0] <= on <= n[1]]
        diffs = {midi - m for m in near}
        kinds["overtone" if diffs & set(OVERTONES) else "leakage" if diffs & {-2, -1, 1, 2} else "other"] += 1
    return {"n_ref": len(ref), "n_est": len(est), "p": p, "r": r, "f": f, "f_off": f_off, **kinds}


def placement_share(ref: list, frames: list[list[str]], instrument: str) -> float | None:
    """The share of labeled notes the fretting hand plays on the labeled string at the note's start, where the reference names strings."""
    if not ref or len(ref[0]) < 4:
        return None
    opens = OPEN_STRINGS[instrument]
    placed = placements(frames)
    right = 0
    for on, off, midi, string in ref:
        frame = next((p for t, p in placed if t / 1000.0 >= on + 0.03), {})   # a frame into the note, past the attack's spread
        right += any(s == string and opens[s] + fret == midi for s, fret in frame.items())
    return right / len(ref)


# ---------------------------------------------------------------- the generated set


GENERATED_VERSION = "1"   # raise it when generate() changes, so --generated writes the set again


def tone(buf: list[float], on: float, off: float, midi: int, gain: float, partials: int = 6, decay: float = 1.5) -> None:
    """A note with its harmonics at 1/n, struck and decaying with `decay` seconds, ending at `off`: a plucked or struck string's shape."""
    f0 = hz(midi)
    start, end = int(on * RATE), min(len(buf), int(off * RATE))
    for i in range(start, end):
        t = (i - start) / RATE
        env = math.exp(-t / decay) * min(1.0, t / 0.005) * min(1.0, (end - i) / (0.01 * RATE))
        buf[i] += gain * env * sum(math.sin(2 * math.pi * f0 * k * t) / k for k in range(1, partials + 1) if f0 * k < RATE / 2)


def generate(target: Path) -> None:
    """Write tracks whose notes are exact by construction, with a notes.csv and the generator's version beside them."""
    target.mkdir(parents=True, exist_ok=True)
    rows = []

    def track(name: str, seconds: float, notes: list[tuple[float, float, int, float]]) -> None:
        buf = [0.0] * int(seconds * RATE)
        for on, off, midi, gain in notes:
            tone(buf, on, off, midi, gain)
        bench_tempo.write_wav(target / f"{name}.wav", buf)
        rows.extend({"path": f"{name}.wav", "onset": f"{on:.3f}", "offset": f"{off:.3f}", "midi": midi} for on, off, midi, _ in notes)

    scale = [60, 62, 64, 65, 67, 69, 71, 72]
    track("scale-c-major", 6.0, [(1.0 + 0.5 * i, 1.0 + 0.5 * i + 0.45, m, 1.0) for i, m in enumerate(scale)])
    track("triads", 8.0, [(1.0 + 2.0 * i, 2.8 + 2.0 * i, m, 1.0) for i, chord in enumerate(((48, 52, 55), (53, 57, 60), (55, 59, 62))) for m in chord])
    # A solo high on the neck over a held E minor chord low on it, the chord loud: what a hand following the loudest sound loses.
    held = [(0.5, 7.5, m, 1.0) for m in (40, 47, 52, 55)]
    solo = [(1.0 + 0.4 * i, 1.0 + 0.4 * i + 0.35, m, 0.5) for i, m in enumerate((76, 78, 79, 81, 83, 81, 79, 78, 76, 79, 83, 81, 79, 76))]
    track("solo-over-chord", 8.0, held + solo)
    track("melody-over-bass", 8.0, [(0.5 + 1.0 * i, 1.4 + 1.0 * i, m, 1.0) for i, m in enumerate((36, 43, 41, 38, 36, 43, 41))]
          + [(0.5 + 0.5 * i, 0.95 + 0.5 * i, m, 0.6) for i, m in enumerate((72, 74, 76, 77, 79, 77, 76, 74, 72, 71, 72, 74, 76))])
    with (target / "notes.csv").open("w", newline="", encoding="utf-8") as fh:
        out = csv.DictWriter(fh, fieldnames=["path", "onset", "offset", "midi"])
        out.writeheader()
        out.writerows(rows)
    (target / "version.txt").write_text(GENERATED_VERSION + "\n", encoding="utf-8")
    print(f"Wrote {len({r['path'] for r in rows})} tracks and notes.csv to {target}")


# ---------------------------------------------------------------- running


def analyze(path: Path, ref: list, exe: Path, instrument: str) -> dict:
    """One file's row: its frames through mm_tempo --notes, each stage's notes and score, and the hand's placement share."""
    res = bench_tempo.analyze(path, exe, ("--notes", "--instrument", str(INSTRUMENTS[instrument])), cast=str)
    if "error" in res:
        return {"name": path.stem, "error": res["error"]}
    block_ms = 1000.0 / res["fps"]
    notes = stage_notes(res["frames"], block_ms)
    return {"name": path.stem, "scores": {stage: score(ref, notes[stage]) for stage in STAGES},
            "placement": placement_share(ref, res["frames"], instrument)}


def pct(x) -> str:
    return "" if x is None else f"{100 * x:.0f}%"


def report(rows: list[dict], instrument: str) -> str:
    """The run's page: each stage over every file, then each file's F per stage, the errors named apart."""
    good = [r for r in rows if "scores" in r and r["scores"]["sounding"].get("n_ref")]
    lines = [f"# Notes report", "", f"{len(good)} files scored, Strings as a {instrument}; onset within {ONSET_TOLERANCE * 1000:.0f} ms, "
             f"offset within {OFFSET_RATIO:.0%} of the note.", "",
             "| Stage | Precision | Recall | F | F with offsets | Flashes | Overtones | Leakage | Other |", "|---|---|---|---|---|---|---|---|---|"]
    for stage in STAGES:
        s = [r["scores"][stage] for r in good]
        n = sum(x["n_ref"] for x in s) or 1
        mean = lambda k: sum(x[k] * x["n_ref"] for x in s) / n   # weighted by the notes each file has
        lines.append(f"| {STAGE_NAMES[stage]} | {pct(mean('p'))} | {pct(mean('r'))} | {pct(mean('f'))} | {pct(mean('f_off'))} | "
                     f"{sum(x['flash'] for x in s)} | {sum(x['overtone'] for x in s)} | {sum(x['leakage'] for x in s)} | {sum(x['other'] for x in s)} |")
    placed = [r["placement"] for r in good if r["placement"] is not None]
    if placed:
        lines += ["", f"The fretting hand puts {pct(sum(placed) / len(placed))} of the labeled notes on their labeled string."]
    lines += ["", "| File | Notes | " + " | ".join(STAGE_NAMES[s] for s in STAGES) + " |", "|---|---|" + "---|" * len(STAGES)]
    for r in rows:
        if "error" in r:
            lines.append(f"| {r['name']} | {r['error']} |" + " |" * len(STAGES))
        else:
            lines.append(f"| {r['name']} | {r['scores']['sounding'].get('n_ref', 0)} | " + " | ".join(pct(r["scores"][s].get("f")) for s in STAGES) + " |")
    return "\n".join(lines) + "\n"


def run(items: list[tuple[Path, list]], out: Path, instrument: str = "guitar", jobs: int = 4, no_build: bool = False) -> list[dict]:
    """Analyze and score the files, write the run file and the report, and show the report."""
    if not no_build:
        bench_tempo.build()
    exe = bench_tempo.binary()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        rows = list(ex.map(lambda item: analyze(item[0], item[1], exe, instrument), items))
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    (out / f"run-{stamp}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
    page = report(rows, instrument)
    (out / "view.md").write_text(page, encoding="utf-8")
    print(page)
    if out.resolve() == OUT_DIR.resolve():
        print(f"MOONDECK_VIEW: {VIEW_URL}")
    return rows


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, action="append", default=[], help="a data folder or mount, read only (repeatable)")
    ap.add_argument("--generated", action="store_true", help="add the generated set, written to build/notes/generated once")
    ap.add_argument("--show", action="store_true", help="show the latest report in build/notes, the folder MoonDeck's card reads, and stop")
    ap.add_argument("--instrument", choices=sorted(INSTRUMENTS), default="guitar", help="the Strings instrument the fretting hand plays")
    ap.add_argument("--out", type=Path, default=OUT_DIR, help="where runs and reports go (default build/notes)")
    ap.add_argument("--limit", type=bench_tempo.at_least(0), default=0, help="at most N files a folder, spread through it")
    ap.add_argument("--jobs", type=bench_tempo.at_least(1), default=os.cpu_count() or 4, help="files analyzed at once")
    ap.add_argument("--no-build", action="store_true", help="run the already-built binary")
    args = ap.parse_args()
    if args.show:
        bench_tempo.show_latest(OUT_DIR, VIEW_URL, "notes", "uv run moondeck/check/bench_notes.py --generated")
        return
    dirs = [d.expanduser().resolve() for d in args.dir]
    if args.generated or not dirs:
        dirs.insert(0, bench_tempo.generated_set(OUT_DIR, GENERATED_VERSION, generate))
    items = []
    for d in dirs:
        if not d.is_dir():
            sys.exit(f"{d}: not a folder (is the volume mounted?)")
        items += collect(d, args.limit if d.name != "generated" else 0)
    if not items:
        sys.exit("No audio with known notes found: see --help for the sets a folder can hold.")
    run(items, args.out, instrument=args.instrument, jobs=args.jobs, no_build=args.no_build)


if __name__ == "__main__":
    main()

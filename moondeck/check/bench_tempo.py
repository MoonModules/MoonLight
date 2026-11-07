#!/usr/bin/env python3
"""Tempo harness: run audio files through AudioService on the desktop and score the tempo against labels.

Builds and runs the ``mm_tempo`` target (``test/bench/bench_tempo.cpp``), which feeds decoded audio through the shipped block analysis faster than real time. A report, not a check: nothing passes or fails, and the figures are read per label source, since a score against a tapped label is accuracy and a score against another program's estimate is agreement.

The data stays where it is: a folder or a mounted volume, given by ``--dir``, is only ever read. Runs and reports go to ``--out`` (default ``build/tempo/``). Runs come from the command line, a person's or an agent's; the **Tempo Report** card only shows the latest report (``--show``) and starts nothing.

The tempo scored is a baseline: Ellis's autocorrelation (2007) of AudioService's flux under librosa's log-normal prior at 120 BPM. It is a whole-track reading of files near full scale, the analysis's first seconds of learning the levels included, so it measures the method on music, not a board's live accuracy through a microphone.

@attribution Gouyon, Schreiber and Müller

Usage:
    uv run moondeck/check/bench_tempo.py --generated                  # the exact-tempo set, written once
    uv run moondeck/check/bench_tempo.py --dir <folder> --limit=20    # a first look at a folder or mount
    uv run moondeck/check/bench_tempo.py --generate <folder>          # write the exact-tempo set elsewhere
    uv run moondeck/check/bench_tempo.py --show                       # the latest report, in MoonDeck's View pane
"""

import argparse
import concurrent.futures
import csv
import hashlib
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_desktop import build_target, test_binary  # noqa: E402  the one home of the build tree and its binaries
from _tempo_baseline import baseline_bpm  # noqa: E402

OUT_DIR = ROOT / "build" / "tempo"
VIEW_URL = "/api/tempo-view"   # moondeck.py serves OUT_DIR/view.md there, into the View pane
RATE = 24000   # every board's rate, AudioService's default
AUDIO_EXT = {".wav", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".aif", ".aiff", ".wma"}


# Label sources, most trusted first: the report gives a figure per source and never merges them.
SOURCES = {
    "tapped": "a trained listener tapping beats and downbeats: accuracy, the gold standard",
    "generated": "made by --generate at this tempo: accuracy, exactly",
    "metronome": "the performer played to a click: accuracy",
    "annotated": "people tapped or checked it for a dataset: accuracy against human judgment",
    "uploader": "whoever shared the file typed it: approximate",
    "store": "a shop's catalog value: approximate, whole numbers, the shop's conventions",
    "dj-software": "an estimate by DJ software: agreement with another tracker",
    "tag": "a tag of unknown origin: agreement, as dj-software",
}
ACCURACY_SOURCES = ("tapped", "generated", "metronome", "annotated")


def show(markdown: str, out: Path) -> None:
    """Hand a markdown page to MoonDeck's View pane."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "view.md").write_text(markdown, encoding="utf-8")
    if out.resolve() == OUT_DIR.resolve():
        print(f"MOONDECK_VIEW: {VIEW_URL}")


# ---------------------------------------------------------------- the binary


def binary() -> Path:
    return test_binary("mm_tempo")


def build() -> None:
    if build_target("mm_tempo", quiet=True) != 0:
        sys.exit(1)


MIN_SECONDS = 8.0   # the shortest audio a tempo reading needs (the baseline's own floor)


def probe(path: Path) -> tuple[str, float | None, str]:
    """One ffprobe before any decoding: why the file cannot be analyzed ("" when it can), its BPM tag and its genre tag.

    The BPM tag is ID3 TBPM or a BPM Vorbis comment, and `tmpo`, an MP4 atom's name, should ffprobe report it under that key; the genre is the tag's first entry.
    """
    run = subprocess.run(["ffprobe", "-v", "error", "-show_entries",
                          "stream=codec_type:stream_tags:format=duration:format_tags", "-of", "json", str(path)],
                         capture_output=True, encoding="utf-8", errors="replace")
    if run.returncode != 0:
        return "unreadable", None, ""
    data = json.loads(run.stdout or "{}")
    if not any(s.get("codec_type") == "audio" for s in data.get("streams", [])):
        return "no audio stream", None, ""
    try:
        seconds = float(data.get("format", {}).get("duration", "nan"))
    except ValueError:
        seconds = float("nan")
    if not seconds >= MIN_SECONDS:
        return ("no duration" if math.isnan(seconds) else f"too short ({seconds:.1f} s)"), None, ""
    tags = {}
    for s in data.get("streams", []):
        tags.update({k.lower(): v for k, v in s.get("tags", {}).items()})
    tags.update({k.lower(): v for k, v in data.get("format", {}).get("tags", {}).items()})
    bpm = None
    for key in ("tbpm", "bpm", "tmpo"):
        try:
            value = float(str(tags[key]).strip())
            bpm = value if 20 <= value <= 400 else None
            break
        except (KeyError, ValueError):
            continue
    return "", bpm, str(tags.get("genre", "")).replace(";", ",").split(",")[0].strip()


def analyze(path: Path, exe: Path, flags: tuple = (), cast=int) -> dict:
    """Decode one file with FFmpeg, run it through mm_tempo with `flags`, and return its frames, each column through `cast`, and its audio hash.

    FFmpeg's messages go to a file, not a pipe: a damaged file can write hundreds of kilobytes of them, which would fill a pipe and stall the decode. Good files decode with none, so any message marks the file damaged.
    """
    with tempfile.TemporaryFile() as errors:
        dec = subprocess.Popen(["ffmpeg", "-v", "error", "-nostdin", "-i", str(path), "-ac", "1", "-ar", str(RATE),
                                "-f", "s32le", "-"], stdout=subprocess.PIPE, stderr=errors)
        run = subprocess.Popen([str(exe), "--rate", str(RATE), *flags], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
        digest = hashlib.sha256()
        # The hash names the track in a private report; it is taken from the decoded audio, never the path.
        killed = False   # a kill's exit code differs by platform (-9 here, 1 on Windows), so the flag says it
        with concurrent.futures.ThreadPoolExecutor(1) as ex:
            reader = ex.submit(run.stdout.read)
            try:
                while chunk := dec.stdout.read(1 << 16):
                    digest.update(chunk)
                    run.stdin.write(chunk)
                run.stdin.close()
            except OSError:   # mm_tempo stopped reading: a broken pipe, or EINVAL on Windows
                dec.kill()
                killed = True
                try:
                    run.stdin.close()
                except OSError:
                    pass
            out = reader.result().decode()
        run.stdout.close()
        dec.wait()
        dec.stdout.close()
        run.wait()
        errors.seek(0)
        lines = errors.read().decode(errors="replace").strip().splitlines()
    if dec.returncode != 0 and not killed and run.returncode == 0:   # a decoder killed above is not the fault
        return {"error": "decode failed: " + (lines[-1] if lines else f"exit {dec.returncode}")}
    if lines:
        return {"error": f"damaged: {len(lines)} decode errors"}
    if run.returncode != 0:
        return {"error": f"mm_tempo: exit {run.returncode}"}
    header, frames = {}, []
    for line in out.splitlines():
        if line.startswith("#"):
            header = dict(field.split("=", 1) for field in line[1:].split())
            continue
        frames.append([cast(v) for v in line.split("\t")])
    # The frame rate is the binary's own rate over its own block, as its header states them.
    return {"columns": header["columns"].split(","), "fps": int(header["rate"]) / int(header["block"]),
            "frames": frames, "hash": digest.hexdigest()[:16]}


# ---------------------------------------------------------------- labels


def read_labels(path: Path, base: Path) -> dict[Path, dict]:
    """labels.csv: path (relative to the data folder), bpm, source, and optionally genre and the set's own id."""
    labels = {}
    with path.open(newline="", encoding="utf-8-sig") as fh:   # -sig: Excel and Numbers open the file with a byte-order mark
        reader = csv.DictReader(fh)
        if "path" not in (reader.fieldnames or []):
            print(f"{path}: no path column, so no labels read from it", file=sys.stderr)
        for row in reader:
            if not row.get("path"):
                continue
            src = (row.get("source") or "").strip() or "tag"
            if src not in SOURCES:
                print(f"labels: unknown source {src!r} for {row['path']}, read as 'tag'", file=sys.stderr)
                src = "tag"
            text = (row.get("bpm") or "").strip()
            try:
                bpm = float(text) if text else None
            except ValueError:
                print(f"labels: bpm {text!r} for {row['path']} is not a number, row left out", file=sys.stderr)
                continue
            labels[(base / row["path"]).resolve()] = {"bpm": bpm, "source": src,
                                                      "genre": (row.get("genre") or "").strip(),
                                                      "id": (row.get("id") or "").strip()}
    return labels


# ---------------------------------------------------------------- scoring


FACTORS = (1.0, 2.0, 3.0, 0.5, 1.0 / 3.0)


def judge(est: float | None, ref: float | None) -> dict:
    """Acc1 and Acc2 at 2% and at Gouyon et al.'s 4% (2006), and a verdict naming the ratio a miss sits at.

    A miss at 4:3 or 3:2 is the beat's subdivision, an offbeat hat or a dotted note, read as the pulse: a level error, like double and half, rather than a lost beat. Only a reading at none of these ratios is wrong. Every ratio but the label's own is judged at Acc2's 4%, so Acc2 counts exactly the readings named right, within 4%, double, half, triple or third.
    """
    if not ref:
        return {}
    keys = ("acc1_2", "acc1_4", "acc2_2", "acc2_4", "octave", "r32", "r43")
    if est is None:
        return {**{k: False for k in keys}, "verdict": "no tempo"}
    r = est / ref
    def near(f: float, tol: float) -> bool:
        return abs(r / f - 1.0) <= tol
    verdict = ("right" if near(1.0, 0.02) else "within 4%" if near(1.0, 0.04)
               else "double" if near(2.0, 0.04) else "half" if near(0.5, 0.04)
               else "triple" if near(3.0, 0.04) else "third" if near(1.0 / 3.0, 0.04)
               else "3:2" if near(1.5, 0.04) or near(2.0 / 3.0, 0.04)
               else "4:3" if near(4.0 / 3.0, 0.04) or near(0.75, 0.04) else "wrong")
    return {
        "acc1_2": near(1.0, 0.02), "acc1_4": near(1.0, 0.04),
        "acc2_2": any(near(f, 0.02) for f in FACTORS), "acc2_4": any(near(f, 0.04) for f in FACTORS),
        "octave": verdict in ("double", "half"), "r32": verdict == "3:2", "r43": verdict == "4:3", "verdict": verdict,
    }


def pct(rows: list[dict], key: str) -> str:
    scored = [r for r in rows if key in r["judge"]]
    return f"{100.0 * sum(r['judge'][key] for r in scored) / len(scored):.1f}%" if scored else "-"


def report(rows: list[dict], method: str, private: bool) -> str:
    """The shared shape: right, the metrical readings per group, then every miss with its ratio and verdict."""
    labeled = [r for r in rows if r["ref"] and r["judge"]]   # a file that failed to decode is listed apart, unscored
    lines = [f"# Tempo harness, {datetime.now():%Y-%m-%d %H:%M}", "",
             f"Method: {method}. {len(rows)} files, {len(labeled)} with a label. The terms are in MoonDeck.md, "
             "Reading the tempo report.", ""]

    def table(title: str, column: str, groups: dict[str, list[dict]]) -> None:
        lines.extend([f"## {title}", "",
                      f"| {column} | Tracks | Right | Right at 4% | Acc2 | Double or half | 3:2 | 4:3 |",
                      "|---|---:|---:|---:|---:|---:|---:|---:|"])
        for name, m in groups.items():
            lines.append(f"| {name} | {len(m)} | " + " | ".join(
                pct(m, k) for k in ("acc1_2", "acc1_4", "acc2_4", "octave", "r32", "r43")) + " |")
        lines.append("")

    sources = {s: [r for r in labeled if r["source"] == s] for s in SOURCES}
    table("Per label source", "Source", {(f"**{s}**" if s in ACCURACY_SOURCES else s): m for s, m in sources.items() if m})
    lines.append("Sources in bold give accuracy; the others give agreement with whoever wrote the label.\n")
    # One genre table per label source, so a percentage never mixes accuracy with agreement.
    for source, members in sources.items():
        genres = sorted({r["genre"] for r in members if r["genre"]})
        if genres:
            table(f"Per genre, {source} labels", "Genre", {g: [r for r in members if r["genre"] == g] for g in genres})

    misses = sorted((r for r in labeled if r["judge"]["verdict"] != "right"), key=lambda r: (r["genre"], r["name"]))
    if misses:
        lines.extend(["## Misses", "", "| Id | Track | Genre | Source | Label | Estimate | Ratio | Verdict |",
                      "|---:|---|---|---|---:|---:|---:|---|"])
        for r in misses:
            track = r["hash"] if private else r["name"].replace("|", "/")
            est = f"{r['est']:g}" if r["est"] is not None else "-"
            ratio = f"{r['est'] / r['ref']:.3f}" if r["est"] is not None else "-"
            lines.append(f"| {r['id']} | {track} | {r['genre']} | {r['source']} | {r['ref']:g} | {est} | {ratio} | {r['judge']['verdict']} |")
        lines.append("")
    unlabeled = sorted((r for r in rows if not r["ref"] and not r.get("error")), key=lambda r: (r["genre"], r["name"]))
    if unlabeled:
        lines.extend(["## Without a label", "",
                      "Estimates to check by ear.", "",
                      "| Track | Genre | Estimate |", "|---|---|---:|"])
        for r in unlabeled:
            track = r["hash"] if private else r["name"].replace("|", "/")
            est = f"{r['est']:g}" if r["est"] is not None else "no tempo"
            lines.append(f"| {track} | {r['genre']} | {est} |")
        lines.append("")
    errors = sorted((r for r in rows if r.get("error")), key=lambda r: r["name"])
    if errors:
        lines.extend(["## Skipped and damaged files", "", "Checked before decoding, or damaged in decoding; none is scored.", "",
                      "| Track | Why |", "|---|---|"])
        lines += [f"| {(r['hash'] or '-') if private else r['name'].replace('|', '/')} | {r['error']} |" for r in errors]
        lines.append("")
    lines.extend(["## The figures' limits", "",
                  "- The tempo as a board reads it live: this is one reading of each whole file.",
                  "- A microphone's level and noise: files arrive near full scale, the levels' first seconds of learning included.",
                  "- Whether a shop's or a program's label is right: those score agreement, not accuracy.",
                  "- The beat's phase or the bar: only the tempo is scored.", ""])
    return "\n".join(lines)


# ---------------------------------------------------------------- the generated set


def click(buf: list[float], at: float, gain: float, freq: float, length: float) -> None:
    """A decaying sine burst with a noise transient, the shape of a kick or a hat."""
    start = int(at * RATE)
    rnd = random.Random(start)
    for i in range(int(length * RATE)):
        if start + i >= len(buf):
            break
        t = i / RATE
        env = math.exp(-t / (length / 5))
        buf[start + i] += gain * env * (math.sin(2 * math.pi * freq * t) + (0.3 * rnd.uniform(-1, 1) if t < 0.004 else 0))


def write_wav(path: Path, buf: list[float]) -> None:
    peak = max(1e-9, max(abs(v) for v in buf))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(32000 * v / peak)) for v in buf))


GENERATED_VERSION = "1"   # raise it when generate() changes, so --generated writes the set again


def generate(target: Path) -> None:
    """Write tracks whose tempo is exact by construction, with a labels.csv and the generator's version beside them."""
    target.mkdir(parents=True, exist_ok=True)
    seconds = 30.0
    rows = []

    def bar_track(bpm: float, swing: float = 0.0, triplet: bool = False) -> list[float]:
        buf = [0.0] * int(seconds * RATE)
        beat = 60.0 / bpm
        n = int(seconds / beat)
        for i in range(n):
            t = i * beat
            click(buf, t, 1.0 if i % 4 == 0 else 0.7, 55.0, 0.15)                 # kick on every beat, the one louder
            if i % 4 in (1, 3):
                click(buf, t, 0.4, 180.0, 0.08)                                    # snare on two and four
            subs = (0.0, 1 / 3, 2 / 3) if triplet else (0.0, 0.5 + swing)
            for s in subs:
                click(buf, t + s * beat, 0.15, 7000.0, 0.03)                       # hats
        return buf

    for bpm in (60, 72, 85, 90, 100, 110, 120, 128, 135, 140, 150, 160, 174, 185, 200):
        name = f"click-{bpm}.wav"
        write_wav(target / name, bar_track(bpm))
        rows.append({"path": name, "bpm": bpm, "source": "generated", "genre": "click"})
    write_wav(target / "swing-120.wav", bar_track(120, swing=0.17))
    rows.append({"path": "swing-120.wav", "bpm": 120, "source": "generated", "genre": "swing"})
    write_wav(target / "triplet-90.wav", bar_track(90, triplet=True))
    rows.append({"path": "triplet-90.wav", "bpm": 90, "source": "generated", "genre": "triplet"})
    # Silence and this noise read no tempo, their flux never rising above its mean; neither carries a label, so both are analyzed and left out of the scores.
    write_wav(target / "silence.wav", [0.0] * int(seconds * RATE))
    rnd = random.Random(1)
    write_wav(target / "noise.wav", [rnd.uniform(-1, 1) for _ in range(int(seconds * RATE))])
    rows += [{"path": "silence.wav", "bpm": "", "source": "generated", "genre": "no-tempo"},
             {"path": "noise.wav", "bpm": "", "source": "generated", "genre": "no-tempo"}]
    with (target / "labels.csv").open("w", newline="", encoding="utf-8") as fh:
        out = csv.DictWriter(fh, fieldnames=["path", "bpm", "source", "genre"])
        out.writeheader()
        out.writerows(rows)
    (target / "version.txt").write_text(GENERATED_VERSION + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} tracks and labels.csv to {target}")


def generated_set(out: Path | None = None, version: str | None = None, write=None) -> Path:
    """A generated set's folder under `out`, written again by `write` when missing or made by an older generator than `version`; this harness's own by default, bench_notes passing its."""
    gen = ((out or OUT_DIR) / "generated").resolve()
    stamp = gen / "version.txt"
    if not stamp.exists() or stamp.read_text(encoding="utf-8").strip() != (version or GENERATED_VERSION):
        (write or generate)(gen)
    return gen


def show_latest(out: Path, view_url: str, what: str, how: str) -> None:
    """Point MoonDeck's View pane at the latest `what` report in `out`, or stop saying `how` a run makes one."""
    if not (out / "view.md").exists():
        sys.exit(f"No {what} report yet: a run from the command line makes one, {how}")
    print(f"MOONDECK_VIEW: {view_url}")


# ---------------------------------------------------------------- running


def sample(items: list, limit: int) -> list:
    """At most `limit` items spread evenly through the list, so a first look at a sorted library is not one artist."""
    if not limit or len(items) <= limit:
        return items
    return [items[i * len(items) // limit] for i in range(limit)]


def collect(dirs: list[Path], labels_files: list[Path], limit: int, whole: frozenset[Path] = frozenset()) -> list[tuple[Path, dict]]:
    """Each folder's audio with its label, `limit` files a folder at most, except the folders in `whole`."""
    labels: dict[Path, dict] = {}
    for d in dirs:
        if (d / "labels.csv").exists():
            labels.update(read_labels(d / "labels.csv", d))
    for f in labels_files:
        labels.update(read_labels(f, f.parent))
    files = []
    for d in dirs:
        found = [(p.resolve(), labels.get(p.resolve())) for p in sorted(d.rglob("*"))
                 if p.suffix.lower() in AUDIO_EXT and p.is_file() and not p.name.startswith("._")]
        files += found if d in whole else sample(found, limit)
    return files


def score(path: Path, label: dict | None, exe: Path, tag_source: str) -> dict:
    """One file's row: probed, decoded, read by the baseline and judged against its label, or the reason it was not."""
    t0 = time.monotonic()
    problem, tag_bpm, tag_genre = probe(path)
    res = {"error": problem} if problem else analyze(path, exe)
    if "frames" in res and len(res["frames"]) < MIN_SECONDS * res["fps"]:   # a file cut short can claim its whole length
        res = {"error": f"too short after decoding ({len(res['frames']) / res['fps']:.1f} s)"}
    if label is None:
        label = {"bpm": tag_bpm, "source": tag_source if tag_bpm else "", "genre": ""}
    est = None
    if not res.get("error"):
        col = res["columns"].index("flux")
        flux = [f[col] for f in res["frames"]]
        est = baseline_bpm(flux, res["fps"])
    return {"path": str(path), "hash": res.get("hash", ""), "error": res.get("error", ""), "name": path.stem,
            "id": label.get("id", ""), "est": est, "ref": label["bpm"], "source": label["source"],
            "genre": label["genre"] or tag_genre, "judge": judge(est, label["bpm"]) if not res.get("error") else {},
            "seconds": round(time.monotonic() - t0, 2)}


def run(files: list[tuple[Path, dict | None]], out: Path, *, private: bool = False, tag_source: str = "tag",
        jobs: int = os.cpu_count() or 4, no_build: bool = False) -> None:
    """Analyze the files, score them, write the run file and the report, and show the report."""
    if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
        sys.exit("The harness decodes with FFmpeg and reads lengths and tags with ffprobe: install FFmpeg first")
    if not no_build:
        build()
    exe = binary()
    if not exe.exists():
        sys.exit(f"mm_tempo not built: {exe}")
    rows = []
    print(f"{len(files)} files, {sum(1 for _, label in files if label)} with a labels.csv entry", flush=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    run_path = out / f"run-{stamp}.jsonl"
    began = time.monotonic()

    def one(item: tuple[Path, dict | None]) -> dict:
        try:
            return score(*item, exe, tag_source)
        except Exception as e:   # any input: a file that breaks the analysis is its own row, listed apart, never the run's end
            return {"path": str(item[0]), "hash": "", "error": f"failed: {type(e).__name__}: {e}", "name": item[0].stem,
                    "id": "", "est": None, "ref": None, "source": "", "genre": "", "judge": {}, "seconds": 0.0}

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool, run_path.open("w", encoding="utf-8") as fh:
        for i, row in enumerate(pool.map(one, files), 1):
            rows.append(row)
            fh.write(json.dumps(row) + "\n")
            fh.flush()   # each file's result on disk at once, kept if the run stops
            if i % 25 == 0 or i == len(files):
                print(f"  {i}/{len(files)}  {time.monotonic() - began:.0f} s", flush=True)

    text = report(rows, "baseline: Ellis 2007 autocorrelation of AudioService's flux", private)
    report_path = out / f"report-{stamp}.md"
    report_path.write_text(text, encoding="utf-8")
    print("\n" + text)
    print(f"Run file: {run_path}\nReport:   {report_path}")
    show(text, out)


def at_least(low: int):
    """An argparse type: a whole number no lower than `low`, refused with a message rather than a traceback."""
    def parse(text: str) -> int:
        try:
            value = int(text)
        except ValueError:
            raise argparse.ArgumentTypeError(f"{text!r} is not a whole number") from None
        if value < low:
            raise argparse.ArgumentTypeError(f"{value} is below {low}")
        return value
    return parse


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")   # track names print as they are, whatever the console's code page
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, action="append", default=[], help="a data folder or mount, read only (repeatable)")
    ap.add_argument("--generated", action="store_true", help="add the generated set, written to build/tempo/generated once")
    ap.add_argument("--generate", type=Path, metavar="DIR", help="write the exact-tempo test set into DIR, and stop")
    ap.add_argument("--show", action="store_true", help="show the latest report in build/tempo, the folder MoonDeck's card reads, and stop")
    ap.add_argument("--labels", type=Path, action="append", default=[], help="a labels.csv beyond the folders' own")
    ap.add_argument("--private", action="store_true", help="name tracks in the report by a hash of their audio, for your own music")
    ap.add_argument("--tag-source", choices=sorted(SOURCES), default="tag", help="who wrote the files' BPM tags")
    ap.add_argument("--out", type=Path, default=OUT_DIR, help="where runs and reports go (default build/tempo, the folder MoonDeck's card reads)")
    ap.add_argument("--limit", type=at_least(0), default=0, help="at most N files a folder, spread through it, for a first look")
    ap.add_argument("--jobs", type=at_least(1), default=os.cpu_count() or 4, help="files analyzed at once")
    ap.add_argument("--no-build", action="store_true", help="run the already-built binary")
    args = ap.parse_args()

    if args.show:
        show_latest(OUT_DIR, VIEW_URL, "tempo", "uv run moondeck/check/bench_tempo.py --generated")
        return
    if args.generate:
        generate(args.generate)
        return
    dirs = [d.expanduser().resolve() for d in args.dir]
    whole: set[Path] = set()
    if args.generated or not dirs:
        # The generated set runs whole and first: it is the yardstick every run is read against, and the run with no folder.
        gen = generated_set()
        dirs.insert(0, gen)
        whole.add(gen)
    for d in dirs:
        if not d.is_dir():
            sys.exit(f"{d}: not a folder (is the volume mounted?)")
    run(collect(dirs, args.labels, args.limit, frozenset(whole)), args.out,
        private=args.private, tag_source=args.tag_source, jobs=args.jobs, no_build=args.no_build)


if __name__ == "__main__":
    main()

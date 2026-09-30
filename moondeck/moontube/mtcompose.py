#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Cut published clips into one video, on the beat, with a music track.

A PROJECT file (moontube/projects/<name>.json) is the edit: which clips, in what order,
how long each gets, and what plays underneath. It is the video editor's save file,
kept as JSON so a cut is reviewable and re-runnable rather than a sequence of manual
drags nobody can reproduce.

ffmpeg's concat demuxer does the joining, which is its standard mechanism for exactly
this: a listing of `file '...'` lines, no re-encode per clip beyond the normalization
pass every source needs to share a timebase. There is no richer project format inside
ffmpeg (EDL and OTIO belong to other tools), so the JSON generates the listing.

Timing is in BARS, not seconds. A cut lands on the music or it does not, and a bar is
the unit that makes that true: `bars: 8` at 112.35 BPM is 17.1 seconds, and changing
the track changes every duration correctly with one number.

    uv run moondeck/moontube/mtcompose.py --project moontube/projects/getting-started.json
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLIPS_DIR = ROOT / "docs" / "assets" / "moontube"


def run_ffmpeg(args: list[str], what: str) -> bool:
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", *args],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{what} failed:\n{r.stderr.strip()[:600]}", file=sys.stderr)
        return False
    return True


def encode_webm(src: Path, dst: Path, vf: str, crf: int,
                seconds: float | None = None, what: str = "encoding",
                keep_audio: bool = False) -> bool:
    """The one VP9 recipe, for every clip this tooling writes.

    Written once because both callers want the same thing: VP9 at a given quality, with a
    filter chain the caller composes. Audio is dropped unless the caller keeps it, because a
    time-stretched segment would pitch-shift any speech in it. Two copies drifted apart on crf
    the first time this was duplicated.
    """
    cmd = ["-i", str(src), "-vf", vf,
           "-c:v", "libvpx-vp9", "-crf", str(crf), "-b:v", "0", "-row-mt", "1"]
    # A NARRATED clip carries the point in its voice, so a cut that drops it keeps the
    # pictures and throws away what they were explaining. Silent by default still, because
    # a timed segment is time-stretched and stretched speech is worse than none.
    cmd += ["-c:a", "libopus", "-b:a", "96k"] if keep_audio else ["-an"]
    if seconds is not None:
        cmd += ["-t", f"{seconds:.3f}"]
    return run_ffmpeg(cmd + [str(dst)], what)


def probe_duration(path: Path) -> float:
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "default=noprint_wrappers=1:nokey=1", str(path)],
                       capture_output=True, text=True)
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def resolve_source(entry: dict) -> Path | None:
    """A clip entry names either a published clip by name, or any file by path."""
    if "clip" in entry:
        return CLIPS_DIR / f"{entry['clip']}.webm"
    if "source" in entry:
        p = Path(entry["source"])
        return p if p.is_absolute() else ROOT / p
    return None


def fit(src: Path, dst: Path, want: float, width: int, title: str | None,
        subtitle: str | None) -> bool:
    """Make one segment exactly `want` seconds wide, at the project's width.

    A clip shorter than its slot is SLOWED to fill it and a longer one is sped up,
    rather than cut: these are whole interactions, and truncating one mid-gesture
    leaves the viewer watching an action that never completes. The speed change is
    small by construction, because the bar count is chosen to suit the clip.
    """
    have = probe_duration(src)
    if have <= 0:
        print(f"  cannot read {src}", file=sys.stderr)
        return False
    # WHOLE means the clip keeps its own length and its narration: a `want` of zero says the
    # edit is not timing this one to the music. Stretching a narrated clip would pitch-shift
    # the voice, and cutting it to a bar count ends the sentence mid-word.
    whole = want <= 0
    ratio = 1.0 if whole else have / want     # >1 = speed up, <1 = slow down
    vf = [f"scale={width}:-2", "fps=24", "format=yuv420p"] if whole else \
         [f"setpts={1/ratio:.5f}*PTS", f"scale={width}:-2", "fps=24", "format=yuv420p"]

    if title:
        # drawtext over the first seconds: the section's name, so a viewer joining
        # mid-video knows what they are looking at.
        #
        # From a FILE, not an inline string. drawtext's parser treats ' : \ and %
        # as syntax, and hand-escaping them covered two of the four: a title carrying
        # a percentage silently became something else. textfile has no such parsing.
        t_file = dst.with_suffix(".title.txt")
        t_file.write_text(title)
        vf.append(f"drawtext=textfile='{t_file}':fontsize=44:fontcolor=white:"
                  f"x=(w-text_w)/2:y=h*0.80:box=1:boxcolor=black@0.55:boxborderw=18:"
                  f"enable='between(t,0.4,3.4)'")
        if subtitle:
            s_file = dst.with_suffix(".subtitle.txt")
            s_file.write_text(subtitle)
            vf.append(f"drawtext=textfile='{s_file}':fontsize=26:fontcolor=white@0.85:"
                      f"x=(w-text_w)/2:y=h*0.80+62:box=1:boxcolor=black@0.45:"
                      f"boxborderw=12:enable='between(t,0.6,3.4)'")

    return encode_webm(src, dst, ",".join(vf), crf=34,
                       seconds=None if whole else want,
                       what=f"{'keeping' if whole else 'fitting'} {src.name}",
                       keep_audio=whole)


def build_inset(folder: Path, work: Path, total: float, width: int,
                box_w: int) -> Path | None:
    """One video of the example footage, cycling, long enough to sit over the whole cut.

    The cut shows the INTERFACE, and a viewer watching someone set a pin has no idea what it
    does to real lights. This is that answer, in the corner, for the length of the film.

    The sources are rough: 640x480 to 2048x1536, three seconds to a minute, and some of them
    are near-duplicates of each other. So each is letterboxed into one box rather than cropped,
    which keeps a tall phone clip whole instead of slicing its middle out, and they are
    concatenated in name order and looped to reach the cut's length. Picking and trimming them
    is an edit, and an edit belongs in the project file rather than in a folder listing.
    """
    srcs = sorted(f for f in folder.iterdir()
                  if f.suffix.lower() in (".mp4", ".mov", ".m4v", ".webm"))
    if not srcs:
        print(f"  no example videos in {folder}", file=sys.stderr)
        return None
    box_h = int(box_w * 9 / 16) // 2 * 2
    parts: list[Path] = []
    for i, s in enumerate(srcs):
        d = work / f"inset-{i:02d}.webm"
        # Letterboxed, not cropped: these are whole shots of a finished wall, and cropping a
        # 4:3 or a portrait one to 16:9 cuts the installation in half.
        vf = (f"scale={box_w}:{box_h}:force_original_aspect_ratio=decrease,"
              f"pad={box_w}:{box_h}:(ow-iw)/2:(oh-ih)/2:black,fps=24,format=yuv420p")
        if not encode_webm(s, d, vf, crf=36, what=f"inset {s.name}"):
            print(f"  skipping {s.name}", file=sys.stderr)
            continue
        parts.append(d)
    if not parts:
        return None
    listing = work / "inset.txt"
    listing.write_text("".join(f"file '{q.name}'\n" for q in parts))
    joined = work / "inset.webm"
    if not run_ffmpeg(["-f", "concat", "-safe", "0", "-i", str(listing),
                       "-c", "copy", str(joined)], "inset concat"):
        return None
    # LOOPED to the cut's length: the footage is minutes and the cut is longer, so it runs out
    # partway through and the corner would go black for the rest.
    looped = work / "inset-loop.webm"
    if not run_ffmpeg(["-stream_loop", "-1", "-i", str(joined), "-t", f"{total:.3f}",
                       "-c", "copy", str(looped)], "inset loop"):
        return None
    return looped


def clean_work(work: Path, keep: bool) -> None:
    """Delete the intermediates once the cut exists.

    They are a means, not an output: one normalized segment per clip plus the joined
    concat, worth several times the finished file and regenerated by the next run.
    Kept only on request, or when the cut FAILED, where they are the evidence of what
    went wrong.
    """
    if keep or not work.exists():
        return
    shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--project", required=True, help="the project file to cut")
    ap.add_argument("--out", default=None, help="output file (default: media/<name>.mp4)")
    ap.add_argument("--no-audio", action="store_true", help="cut the picture only")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep the per-segment intermediates (to inspect a bad cut)")
    args = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            print(f"{tool} is not on PATH (brew install ffmpeg)", file=sys.stderr)
            return 1

    proj = json.loads(Path(args.project).read_text())
    bpm = float(proj.get("bpm", 120.0))
    if not math.isfinite(bpm) or bpm <= 0:
        print(f"bpm must be a positive number, got {bpm}", file=sys.stderr)
        return 1
    bar = 4 * 60 / bpm                       # one bar, in seconds, at 4/4
    width = int(proj.get("width", 1280))
    # media/video/ holds finished video: the clips and the cuts made from them. The
    # project that describes the cut lives with the clips it names, under
    # moontube/, because it is a source rather than an output.
    out = Path(args.out) if args.out else ROOT / "media" / "video" / f"{proj['name']}.mp4"
    work = ROOT / "media" / f".{proj['name']}-work"
    work.mkdir(parents=True, exist_ok=True)
    out.parent.mkdir(parents=True, exist_ok=True)

    print(f"{proj['name']}: {bpm} BPM, bar = {bar:.3f}s")
    segments: list[Path] = []
    t = 0.0
    for i, entry in enumerate(proj.get("clips", [])):
        src = resolve_source(entry)
        if not src or not src.exists():
            print(f"  MISSING: {entry.get('clip') or entry.get('source')}",
                  file=sys.stderr)
            continue
        # `bars: 0` means WHOLE: the clip runs at its own length, with its narration.
        bars = float(entry.get("bars", 8))
        want = bars * bar
        dst = work / f"{i:02d}.webm"
        label = entry.get("clip") or Path(entry["source"]).stem
        if bars <= 0:
            have = probe_duration(src)
            print(f"  {t:7.1f}s  {label:24}  whole = {have:5.1f}s")
        else:
            print(f"  {t:7.1f}s  {label:24} {bars:>3.0f} bars = {want:5.1f}s")
        if not fit(src, dst, want, width, entry.get("title"),
                   entry.get("subtitle")):
            # STOP. Concatenating the rest would produce a video that looks finished
            # and is silently missing a section, which is worse than no video at all.
            print(f"  failed to fit {label}; not cutting a partial video",
                  file=sys.stderr)
            return 1
        segments.append(dst)
        t += probe_duration(dst) if bars <= 0 else want

    if not segments:
        print("nothing to cut", file=sys.stderr)
        return 1

    listing = work / "concat.txt"
    listing.write_text("".join(f"file '{p.name}'\n" for p in segments))

    joined = work / "picture.webm"
    if not run_ffmpeg(["-f", "concat", "-safe", "0", "-i", str(listing),
                       "-c", "copy", str(joined)], "concat"):
        return 1

    # The example window, over the whole cut. A project that names no folder gets no window.
    inset = proj.get("inset")
    if inset:
        folder = ROOT / inset.get("folder", "media/examplevideos")
        box_w = int(inset.get("width", width // 4)) // 2 * 2
        margin = int(inset.get("margin", 24))
        if not folder.is_dir():
            print(f"  inset folder not found: {folder}", file=sys.stderr)
            return 1
        strip = build_inset(folder, work, t, width, box_w)
        if not strip:
            return 1
        framed = work / "picture-inset.webm"
        # TOP RIGHT, with a hairline so the window reads as a window rather than as part of
        # the interface behind it. `shortest=0`: the cut decides the length, not the loop.
        if not run_ffmpeg([
                "-i", str(joined), "-i", str(strip),
                "-filter_complex",
                f"[1:v]pad=iw+4:ih+4:2:2:white@0.65[ins];"
                f"[0:v][ins]overlay=W-w-{margin}:{margin}:shortest=0[v]",
                "-map", "[v]", "-map", "0:a?",
                "-c:v", "libvpx-vp9", "-crf", "34", "-b:v", "0", "-row-mt", "1",
                "-c:a", "copy", str(framed)], "inset overlay"):
            return 1
        joined = framed

    if args.no_audio or not proj.get("audio"):
        dst = out.with_suffix(".webm")
        shutil.copy(joined, dst)
        clean_work(work, args.keep_work)
        print(f"\n{dst}  ({t:.0f}s, no music track)")
        return 0

    audio = ROOT / proj["audio"] if not Path(proj["audio"]).is_absolute() \
        else Path(proj["audio"])
    if not audio.exists():
        print(f"audio not found: {audio}", file=sys.stderr)
        return 1

    # The music starts at its FIRST BEAT, so bar 1 of the cut is bar 1 of the track.
    # Faded at both ends: a hard cut into music reads as a mistake, and a track that
    # stops dead at the last frame reads as a file that ran out.
    gain = float(proj.get("audio_gain", 0.6))
    fade = 2.5
    ok = run_ffmpeg([
        "-i", str(joined),
        "-ss", str(proj.get("first_beat", 0.0)), "-i", str(audio),
        "-filter_complex",
        f"[1:a]volume={gain},afade=t=in:st=0:d=1.5,"
        f"afade=t=out:st={max(0.0, t - fade):.2f}:d={fade}[a]",
        "-map", "0:v", "-map", "[a]",
        "-c:v", "libx264", "-preset", "medium", "-crf", "21", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k", "-shortest", str(out)], "scoring")
    if not ok:
        print(f"the intermediates are in {work} for inspection", file=sys.stderr)
        return 1

    clean_work(work, args.keep_work)
    size = out.stat().st_size // 1024
    mins, secs = divmod(int(t), 60)
    print(f"\n{out}  ({mins}:{secs:02d}, {size} KB, scored)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

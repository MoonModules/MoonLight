#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["piper-tts", "requests"]   # requests: imported transitively through mtrun
# ///
"""Lay a spoken track over a recorded clip, one line per caption.

A clip already says what it is showing, in its captions. This speaks them, so the clip can be
watched rather than read. The words are the captions themselves: a second script would drift from
what is on screen the first time either is edited.

Timing comes from the run file rather than from guesswork. Each caption is held for a known number
of seconds, so a line starts when its caption appears and the silence between lines is the rest of
that shot. A line longer than its hold is reported rather than overlapped, because the fix belongs
in the run file (hold the shot longer) and not in the mix.

Piper speaks the lines, in the voice the run file names. Luna presents the slides as Alba, and each
clip is spoken by one of her team, so the series sounds like the several agents that made it.

Usage:
  uv run moondeck/moontube/mtvoiceover.py --run moontube/clips/05-layouts.json \\
      [--voice alan] [--clip docs/assets/moontube/05-layouts.webm]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mtnarrate import VOICE_PATHS, _duration, speak, voice_model, voice_of   # noqa: E402
import mtrun                                                       # noqa: E402  DEFAULT_SPEED: one home for the run format's defaults

ROOT = Path(__file__).resolve().parent.parent.parent


def _speed(run: dict) -> float:
    """The clip is rendered at this playback rate, so a step's seconds are divided by it.

    The default comes from `mtrun`, which owns the run format: a second copy here read 1.0 where the
    recorder read 2.0, so a run file that named no speed was voiced at half the rate it was filmed.
    """
    return float(run.get("speed", mtrun.DEFAULT_SPEED)) or 1.0


def timeline(run: dict, voice: str, work: Path, clip: Path) -> list[tuple[float, Path, str]]:
    """Every caption as (when it starts, its audio, its text), in clip time.

    The offsets come from the recording itself, written beside the take as `<clip>.captions.json`.
    They were computed from the run file's holds before, which is what a step is ASKED to dwell
    rather than what the device took to do it: a clip whose holds summed to forty-nine seconds ran
    for a hundred and sixty-six, so every line landed further behind the picture than the one before.
    """
    marks_file = clip.with_suffix(".captions.json")
    if not marks_file.exists():
        raise SystemExit(
            f"no caption marks beside {clip.name}: record it again so the offsets are measured "
            f"rather than assumed (expected {marks_file.name})")
    marks = json.loads(marks_file.read_text())

    model = voice_model(voice)
    _, speaker = VOICE_PATHS[voice]
    # The marks are measured on the RAW take; the published clip is rendered at `speed`, so a
    # caption at twenty seconds of recording is at twenty over speed seconds of the clip.
    speed = _speed(run)
    out: list[tuple[float, Path, str]] = []
    for i, mark in enumerate(marks):
        wav = work / f"line{i:03d}.wav"
        speak(model, speaker, mark["text"], wav)
        out.append((float(mark["at"]) / speed, wav, mark["text"]))
    return out


def _start_time(path: Path, kind: str) -> float:
    """A stream's start timestamp, which decides whether the mux lands on the negative side."""
    out = subprocess.run(
        ["ffprobe", "-v", "quiet", "-select_streams", kind, "-show_entries", "stream=start_time",
         "-of", "csv=p=0", str(path)], capture_output=True, text=True).stdout.strip()
    try:
        return float(out.split("\n")[0])
    except (ValueError, IndexError):
        return 0.0


def mix(clip: Path, lines: list[tuple[float, Path, str]], work: Path, out: Path) -> Path:
    """Build one continuous track the length of the clip, then mux it onto the video.

    The track is assembled as a WAV first, at one sample rate, rather than as N delayed streams the
    muxer sums as it writes. Mixing at mux time produced a stream players listed and then failed to
    decode: the clip looked voiced and played silent.
    """
    total = _duration(clip)

    # A silent bed the length of the clip, so every line lands at its own offset and the track ends
    # with the video rather than with the last word.
    bed = work / "bed.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                    "-i", f"anullsrc=r=48000:cl=mono", "-t", f"{total:.3f}",
                    "-c:a", "pcm_s16le", str(bed)], check=True)

    inputs: list[str] = ["-i", str(bed)]
    for _, wav, _ in lines:
        inputs += ["-i", str(wav)]

    parts = [f"[{n}:a]aresample=48000,adelay={int(offset * 1000)}[d{n}]"
             for n, (offset, _, _) in enumerate(lines, start=1)]
    chain = ";".join(parts)
    mixed = "[0:a]" + "".join(f"[d{n}]" for n in range(1, len(lines) + 1))
    chain += f";{mixed}amix=inputs={len(lines) + 1}:dropout_transition=0:normalize=0[out]"

    track = work / "track.wav"
    subprocess.run(["ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", chain,
                    "-map", "[out]", "-c:a", "pcm_s16le", str(track)], check=True)

    out.parent.mkdir(parents=True, exist_ok=True)
    # Staged beside the work files: ffmpeg cannot read a file it is writing, and the usual case here
    # is narrating a clip in place.
    staged = work / f"mixed{out.suffix}"
    # The recorded video's first frame lands a few milliseconds after zero, and a track starting at
    # zero beside it is written on a NEGATIVE timestamp: ffmpeg decodes it, VLC reports no audio.
    # Offsetting the track by that much puts both streams on the positive side of the timeline.
    video_start = _start_time(clip, "v")
    offset = max(0.0, -video_start) if video_start < 0 else 0.007
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(clip),
                    "-itsoffset", f"{offset:.3f}", "-i", str(track),
                    "-map", "0:v", "-map", "1:a",
                    # The video is RE-ENCODED rather than copied. A copy keeps the recording's few
                    # milliseconds of start offset while the new track starts at zero, which lands
                    # the audio on a negative timestamp: ffmpeg decodes it anyway, VLC reports no
                    # audio at all. The generation costs a little quality and buys a clip that plays.
                    "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "34", "-pix_fmt", "yuv420p",
                    "-c:a", "libopus", "-b:a", "96k",
                    "-avoid_negative_ts", "make_zero",
                    "-shortest", str(staged)], check=True)
    shutil.move(str(staged), str(out))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Speak a clip's captions over the clip.")
    ap.add_argument("--run", required=True, help="the run file the clip was recorded from")
    ap.add_argument("--voice",
                    help="override the run file's own voice, one of: " + ", ".join(VOICE_PATHS))
    ap.add_argument("--clip", help="the clip to narrate (default: the tracked one for this run)")
    ap.add_argument("--out", help="where the narrated clip lands (default: over the tracked one)")
    args = ap.parse_args()

    run = json.loads(Path(args.run).read_text())
    name = run.get("name", Path(args.run).stem)
    clip = Path(args.clip) if args.clip else ROOT / "docs" / "assets" / "moontube" / f"{name}.webm"
    if not clip.exists():
        print(f"no clip at {clip}: record it first")
        return 1

    # The marks sit beside the raw take rather than the published clip, because that is where the
    # recorder wrote them.
    raw = ROOT / "media" / "video" / f"{name}-raw.webm"

    work = ROOT / "build" / "voiceover" / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    voice = voice_of(run, args.voice)
    print(f"Voicing [{name}]: {clip.name}, voice {voice}")
    lines = timeline(run, voice, work, raw)
    if not lines:
        print("no captions to speak")
        return 1

    # REPORT a line that outlasts its shot rather than letting it bleed into the next one: the run
    # file decides how long a shot is held, so that is where a too-short hold is fixed.
    speed = _speed(run)
    over = 0
    for n, (offset, aiff, text) in enumerate(lines):
        spoken = _duration(aiff)
        nxt = lines[n + 1][0] if n + 1 < len(lines) else _duration(clip)
        room = nxt - offset
        flag = ""
        if spoken > room:
            over += 1
            flag = f"  OVER by {spoken - room:.1f}s"
        print(f"  {offset:6.1f}s  {spoken:4.1f}s  {text[:58]}{flag}")
    if over:
        print(f"\n{over} line(s) outlast their shot: raise `hold` in {Path(args.run).name} "
              f"and record again.")

    out = Path(args.out) if args.out else clip
    result = mix(clip, lines, work, out)
    print(f"\n{result}  ({result.stat().st_size // 1024} KB, {_duration(result):.0f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

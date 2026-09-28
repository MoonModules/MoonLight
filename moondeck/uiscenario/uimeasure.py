#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["piper-tts"]
# ///
"""Measure each caption's spoken length and write it into the run file as `speech`.

The recording holds a shot until its narration has finished, which needs the length of a line that
has not been spoken yet. So it is measured here, once, before the camera rolls: the same voice
speaks each caption, ffprobe reads the result, and the number lands in the step.

Sizing the dwell by eye instead left captions vanishing mid-sentence and each line starting over the
one before it, because the hold was a guess and the voice was laid on afterwards.

The voice MATTERS: two voices do not speak a line at the same speed, so a clip measured with one and
voiced with another drifts. Alba is the default here and in `uivoiceover`, which is what keeps the
two in agreement without anyone having to remember.

Usage:
  uv run moondeck/uiscenario/uimeasure.py --run test/uiscenarios/clips/05-layouts.json [--voice alba]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uinarrate import VOICE_PATHS, speak, voice_model            # noqa: E402

# A beat after the words, so a shot does not cut on the last syllable, plus margin for the
# synthesiser's own variance between runs of the same line.
TAIL_SECONDS = 1.0
MARGIN = 1.25


def _duration(path: Path) -> float:
    out = subprocess.run(
        ["ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
        capture_output=True, text=True).stdout.strip()
    return float(out) if out else 0.0


def measure(run_path: Path, voice: str) -> int:
    run = json.loads(run_path.read_text())
    steps = run.get("steps")
    if not steps:
        print(f"{run_path.name} has no steps: a slide script is rendered by uinarrate instead.",
              file=sys.stderr)
        return 1

    model = voice_model(voice)
    _, speaker = VOICE_PATHS[voice]
    work = Path(tempfile.mkdtemp())
    try:
        measured = []
        for i, step in enumerate(steps):
            caption = step.get("caption")
            if not caption:
                # No line to wait for, so any `speech` left over from an earlier caption would hold
                # the shot for words nobody says.
                step.pop("speech", None)
                continue
            wav = work / f"{i:03d}.wav"
            speak(model, speaker, caption, wav)
            seconds = round(_duration(wav) * MARGIN + TAIL_SECONDS, 1)
            if step.get("action") == "chapter":
                # A chapter card holds for `seconds` rather than a hold, so the title's own line
                # sets it: the card sat on screen for two seconds saying nothing.
                step["seconds"] = seconds
            else:
                step["speech"] = seconds
                # `hold` is what the step is ASKED to dwell and `speech` supersedes it, so leaving
                # both invites the two to disagree.
                step.pop("hold", None)
            measured.append(seconds)
        run_path.write_text(json.dumps(run, indent=2, ensure_ascii=False) + "\n")
        print(f"{len(measured)} line(s) measured with {voice}: {measured}")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Write measured caption durations into a run file.")
    ap.add_argument("--run", required=True, help="the run file to measure")
    ap.add_argument("--voice", default="alba",
                    help="the voice the clip will be narrated with: " + ", ".join(VOICE_PATHS))
    args = ap.parse_args()
    return measure(Path(args.run), args.voice)


if __name__ == "__main__":
    sys.exit(main())

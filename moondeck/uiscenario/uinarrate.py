#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright", "piper-tts"]
# ///
"""Render a narrated slide video from a script: slides as HTML, narration from Piper.

The spoken introduction is the product owner's own words, and the clips that follow are its proof.
Until it is recorded to camera, this stands in for it: each beat becomes a slide, the slide is
screenshotted through the same browser the UI clips use, Piper narrates the beat, and the slide is
held for exactly as long as its narration takes. Nothing guesses the timing, because the audio's own
duration decides it.

Piper is a neural synthesiser that runs offline, on a model fetched once into `media/voices/`. It is
built on a workstation rather than in CI, like the UI clips it sits beside.

Usage:
  uv run moondeck/uiscenario/uinarrate.py --script <slides.json> [--voice Daniel] [--out media/video]
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
WIDTH, HEIGHT = 1280, 720

# The MoonLight character, with the logo's paper background removed, and where it stands.
PRESENTER = Path(__file__).resolve().parent / "presenter.png"
FACE, FACE_X, FACE_Y = 200, 980, 360
# The trace under it, in the slides' accent color.
WAVE_W, WAVE_H, WAVE_X, WAVE_Y = 300, 80, 930, 575
ACCENT = "0xc9a5ff"

# Piper, a neural text-to-speech that runs offline. macOS `say` is a concatenative synthesiser and
# sounds it; this is the difference between a voice you tolerate and one you listen to.
VOICES_DIR = ROOT / "media" / "voices"
PIPER_BASE = "https://huggingface.co/rhasspy/piper-voices/resolve/main"
# Some models hold several speakers, so a voice names the model AND which speaker of it to use.
VOICE_PATHS = {
    "alba":     ("en/en_GB/alba/medium/en_GB-alba-medium", None),
    "jenny":    ("en/en_GB/jenny_dioco/medium/en_GB-jenny_dioco-medium", None),
    "prudence": ("en/en_GB/semaine/medium/en_GB-semaine-medium", 0),
}


def _duration(path: Path) -> float:
    out = subprocess.run(
        ["ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)],
        capture_output=True, text=True).stdout.strip()
    return float(out) if out else 0.0


def slide_html(slide: dict, index: int, total: int) -> str:
    """One slide, styled like the app so the intro sits beside the UI clips rather than beside a deck."""
    bullets = "".join(f"<li>{b}</li>" for b in slide.get("bullets", []))
    # `credits` is a two-column name/contribution list, for a slide whose job is to BE the list.
    credits = "".join(f"<div><b>{n}</b> <span>{w}</span></div>"
                      for n, w in slide.get("credits", []))
    kicker = slide.get("kicker", "")
    return f"""<!doctype html><html><head><meta charset="utf-8"><style>
  :root {{ --bg:#141a2e; --card:#1b2340; --ink:#e8ecff; --dim:#8b95bf; --accent:#c9a5ff; }}
  * {{ box-sizing:border-box; margin:0; padding:0; }}
  body {{ width:{WIDTH}px; height:{HEIGHT}px; background:var(--bg); color:var(--ink);
         font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
         display:flex; align-items:center; justify-content:center; }}
  .slide {{ width:1060px; }}
  .kicker {{ color:var(--accent); font-size:18px; letter-spacing:.14em; text-transform:uppercase;
             margin-bottom:18px; }}
  h1 {{ font-size:52px; line-height:1.15; font-weight:600; letter-spacing:-.01em; }}
  h1 .soft {{ color:var(--dim); font-weight:400; }}
  ul {{ margin-top:34px; list-style:none; }}
  li {{ font-size:27px; color:var(--ink); padding:11px 0 11px 30px; position:relative; }}
  li::before {{ content:""; position:absolute; left:0; top:23px; width:10px; height:10px;
                border-radius:50%; background:var(--accent); }}
  .credits {{ margin-top:26px; column-count:2; column-gap:48px; }}
  .credits div {{ break-inside:avoid; font-size:19px; line-height:1.42; padding:3px 0; }}
  .credits b {{ color:var(--accent); font-weight:600; }}
  .credits span {{ color:var(--dim); }}
  .foot {{ position:fixed; left:110px; bottom:44px; color:var(--dim); font-size:15px; }}
  .num {{ position:fixed; right:110px; bottom:44px; color:var(--dim); font-size:15px;
          font-variant-numeric:tabular-nums; }}
</style></head><body>
  <div class="slide">
    {f'<div class="kicker">{kicker}</div>' if kicker else ''}
    <h1>{slide.get("title","")}</h1>
    {f'<ul>{bullets}</ul>' if bullets else ''}
    {f'<div class="credits">{credits}</div>' if credits else ''}
  </div>
  <div class="foot">MoonLight</div>
  <div class="num">{index} / {total}</div>
</body></html>"""


def render_slides(slides: list[dict], work: Path) -> list[Path]:
    """Screenshot each slide through the browser the UI clips already use."""
    from playwright.sync_api import sync_playwright

    shots: list[Path] = []
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page(viewport={"width": WIDTH, "height": HEIGHT},
                                device_scale_factor=2)
        for i, slide in enumerate(slides, 1):
            html = work / f"slide{i:02d}.html"
            html.write_text(slide_html(slide, i, len(slides)))
            page.goto(html.as_uri())
            page.wait_for_timeout(120)          # let the font land before the shot
            shot = work / f"slide{i:02d}.png"
            page.screenshot(path=str(shot))
            shots.append(shot)
            print(f"  slide {i:02d}  {slide.get('title','')[:54]}")
        browser.close()
    return shots


def speak(model: Path, speaker: int | None, text: str, out: Path) -> None:
    """Say one line with Piper, retrying once.

    The synthesiser aborts now and then, on a line it renders perfectly on the next attempt, so a
    crash is retried rather than failing a whole render that is minutes of work.
    """
    cmd = [sys.executable, "-m", "piper", "-m", str(model), "-f", str(out)]
    if speaker is not None:
        cmd += ["-s", str(speaker)]
    for attempt in range(1, 6):
        try:
            subprocess.run(cmd, input=text, text=True, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            # A crashed run can leave a truncated file behind, which reads as success to anything
            # that only checks the exit code, so the output is checked for content too.
            if out.exists() and out.stat().st_size > 1024:
                return
            raise subprocess.CalledProcessError(0, cmd)
        except subprocess.CalledProcessError:
            out.unlink(missing_ok=True)
            if attempt == 5:
                raise
            # Backing off rather than retrying at once: the aborts come in bursts, and a second
            # attempt in the same instant hits the same state the first one died in.
            time.sleep(2.0 * attempt)


def voice_model(name: str) -> Path:
    """The Piper model for a voice, fetched on first use.

    A model is tens of megabytes, so it lives under `media/` rather than in the tree: it is a tool
    the intro is built with, like a compiler, not a source the repository carries.
    """
    path, _ = VOICE_PATHS[name]
    model = VOICES_DIR / f"{name}.onnx"
    if model.exists():
        return model
    VOICES_DIR.mkdir(parents=True, exist_ok=True)
    base = f"{PIPER_BASE}/{path}"
    print(f"  fetching the {name} voice, once")
    for url, dest in ((f"{base}.onnx", model), (f"{base}.onnx.json", model.with_suffix(".onnx.json"))):
        # `--fail` and a temporary file, together: curl exits 0 on a 404 and writes the error page,
        # which would cache an HTML document as a model and fail every render until it is deleted
        # by hand. Nothing lands at the real path until the download has succeeded.
        part = dest.with_suffix(dest.suffix + ".part")
        try:
            subprocess.run(["curl", "-sSL", "--fail", "-o", str(part), url], check=True)
        except subprocess.CalledProcessError:
            part.unlink(missing_ok=True)
            raise SystemExit(f"could not fetch the {name} voice from {url}")
        part.replace(dest)
    return model


def narrate(slides: list[dict], voice: str, work: Path) -> list[Path]:
    """One audio file per slide. Its length is what the slide is held for."""
    model = voice_model(voice)
    _, speaker = VOICE_PATHS[voice]
    audio: list[Path] = []
    for i, slide in enumerate(slides, 1):
        wav = work / f"say{i:02d}.wav"
        # Piper reads the line on stdin, which keeps punctuation out of the argument list.
        speak(model, speaker, slide.get("say", ""), wav)
        audio.append(wav)
        print(f"  audio {i:02d}  {_duration(wav):5.1f}s")
    return audio


def build(slides: list[dict], shots: list[Path], audio: list[Path], work: Path, out: Path) -> Path:
    """Cut each slide to its narration, then concatenate. One ffmpeg pass per slide, one to join."""
    parts: list[Path] = []
    for i, (shot, aiff) in enumerate(zip(shots, audio), 1):
        # A beat of silence after the words, so a slide does not cut on the last syllable.
        seconds = _duration(aiff) + 0.9
        part = work / f"part{i:02d}.webm"

        # The waveform is rendered first, as its own file: composing it inside the slide's filter
        # graph consumed the stream the slide needed, and a separate pass is easier to read anyway.
        wave = work / f"wave{i:02d}.mkv"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-i", str(aiff),
            "-filter_complex",
            f"[0:a]showwaves=s={WAVE_W}x{WAVE_H}:mode=p2p:colors={ACCENT}:draw=full:rate=30[v]",
            "-map", "[v]", "-c:v", "ffv1", str(wave)], check=True)

        subprocess.run([
            "ffmpeg", "-v", "error", "-y",
            "-loop", "1", "-i", str(shot),
            "-i", str(aiff),
            "-loop", "1", "-i", str(PRESENTER),
            "-i", str(wave),
            # The presenter stands in the corner and the trace moves under it, so a viewer can see
            # that something is speaking rather than reading a still slide for half a minute.
            "-filter_complex",
            # The slide is shot at 2x for crisp text, so it comes back to size BEFORE anything is
            # placed on it: the overlay coordinates below are in output pixels, not screenshot ones.
            f"[0:v]scale={WIDTH}:{HEIGHT}[slide];"
            f"[2:v]scale={FACE}:{FACE}[face];"
            f"[3:v]format=rgba,lumakey=threshold=0.03:tolerance=0.05[wav];"
            f"[slide][face]overlay=x={FACE_X}:y={FACE_Y}:shortest=1[a];"
            f"[a][wav]overlay=x={WAVE_X}:y={WAVE_Y}[v]",
            "-map", "[v]", "-map", "1:a",
            # VP9 and Opus, the codecs the UI clips carry, so joining them below is a stream copy.
            "-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "34", "-t", f"{seconds:.2f}",
            "-pix_fmt", "yuv420p",
            "-c:a", "libopus", "-b:a", "96k",
            # The audio is shorter than the video by that beat, so it is padded rather than looped.
            "-af", f"apad=whole_dur={seconds:.2f}",
            "-r", "30", str(part)], check=True)
        parts.append(part)

    listing = work / "parts.txt"
    listing.write_text("".join(f"file '{p}'\n" for p in parts))
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0",
                    "-i", str(listing), "-c", "copy", str(out)], check=True)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Render a narrated slide video from a script.")
    ap.add_argument("--script", required=True, help="the slides JSON")
    ap.add_argument("--voice", default="alba",
                    help="a Piper voice: " + ", ".join(VOICE_PATHS))
    ap.add_argument("--out", default="media/video", help="where the video lands")
    args = ap.parse_args()

    script = json.loads(Path(args.script).read_text())
    slides = script["slides"]
    name = script.get("name", Path(args.script).stem)

    work = ROOT / "build" / "narrate" / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    print(f"Narrating [{name}]: {len(slides)} slides, voice {args.voice}")
    shots = render_slides(slides, work)
    audio = narrate(slides, args.voice, work)
    out = build(slides, shots, audio, work, ROOT / args.out / f"{name}.webm")

    total = _duration(out)
    size = out.stat().st_size // 1024
    print(f"\n{out}  ({size} KB, {total:.0f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

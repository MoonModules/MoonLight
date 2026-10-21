#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright"]
# ///
"""Record a catalog's card deck as a looping GIF: every card animating at once, the picture README and the home page open on.

A catalog page draws its modules as a deck of cards, each playing its module's preview; at a small card size the whole catalog fits one screen. This records that screen with Playwright and cuts a few seconds of it into a GIF with ffmpeg, cropped to the deck and scaled to the width GitHub shows README at. It reads the docs server, so the deck shows the catalog as it is: run it after an effect is added.

    uv run moondeck/docs/build_docs.py --serve      # the docs server, in another terminal
    uv run moondeck/moontube/mtdeck.py              # docs/assets/light/effects-deck.gif
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def gif_filter(box: dict, width: int, fps: int) -> str:
    """The ffmpeg filter that crops the recording to the deck and turns it into a GIF: one palette for the whole loop, dithered, and only the changed rectangle stored per frame."""
    return (f"crop={int(box['w'])}:{int(box['h'])}:{int(box['x'])}:{int(box['y'])},fps={fps},scale={width}:-1:flags=lanczos,"
            "split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];"
            "[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle")


def record(url: str, card_size: float, folder: Path) -> tuple[Path, dict]:
    """Record the deck at `card_size` for a few seconds; the video and the deck's box in it."""
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        browser = p.chromium.launch()
        ctx = browser.new_context(viewport={"width": 1600, "height": 1500}, record_video_dir=str(folder),
                                  record_video_size={"width": 1600, "height": 1500})
        page = ctx.new_page()
        page.goto(url)
        page.wait_for_timeout(2500)
        page.evaluate(f"""(() => {{ const r = document.querySelector('.mm-deck-bar input[type=range]');
            r.value = '{card_size}'; r.dispatchEvent(new Event('input', {{bubbles: true}})); }})()""")
        page.wait_for_timeout(1500)
        # Below the sticky header, which would otherwise cover the deck's top row.
        page.evaluate("document.querySelector('.mm-deck').scrollIntoView({block: 'start'}); window.scrollBy(0, -72)")
        page.wait_for_timeout(1500)
        box = page.evaluate("""(() => { const r = document.querySelector('.mm-deck').getBoundingClientRect();
            return {x: r.x, y: r.y, w: r.width, h: Math.min(r.height, innerHeight - r.y)}; })()""")
        page.wait_for_timeout(5000)
        ctx.close()
        browser.close()
    return next(folder.glob("*.webm")), box


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--url", default="http://localhost:8422/MoonLight/moonmodules/light/effects.html", help="the catalog page on the docs server")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "assets" / "light" / "effects-deck.gif")
    ap.add_argument("--card-size", type=float, default=0.3, help="the deck's card size, small enough for the whole catalog to fit one screen")
    ap.add_argument("--width", type=int, default=900, help="the GIF's width: README's width on GitHub")
    ap.add_argument("--seconds", type=float, default=4)
    ap.add_argument("--fps", type=int, default=10)
    args = ap.parse_args()
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not on PATH")
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        video, box = record(args.url, args.card_size, Path(tmp))
        length = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(video)],
                                      capture_output=True, text=True, check=True).stdout)
        # The last seconds, once every card has loaded and is playing.
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{max(0.0, length - args.seconds - 0.2):.2f}", "-t", f"{args.seconds:g}",
                        "-i", str(video), "-vf", gif_filter(box, args.width, args.fps), str(args.out)], check=True)
    print(f"{args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out}: {args.out.stat().st_size // 1024} KB, deck {json.dumps({k: round(v) for k, v in box.items()})}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

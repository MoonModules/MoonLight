#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright", "requests"]
# ///
"""Record a run file as video, with its captions and cursor.

Same engine and same run file as the UI tests: the steps are the steps, and this
caller adds a camera. Playwright's own Screencast API draws the cursor, highlights
what each action touches and renders the captions as overlays, so there is no second
pass burning text into frames and no hand-drawn pointer to keep in sync.

    uv run moondeck/uiscenario/uivideo.py --run test/uiscenarios/clips/95-add-a-layer.json

Prerequisites:
    1. A running MoonLight:   uv run moondeck/run/run_desktop.py
    2. Playwright's chromium: uv run --with playwright playwright install chromium
"""

from __future__ import annotations

import argparse
import math
import shutil
import subprocess
import sys
from pathlib import Path

import requests
from playwright.sync_api import sync_playwright

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[2]      # moondeck/uiscenario/ -> repo root

import uirun  # noqa: E402

VIEWPORT = {"width": 1280, "height": 720}


def publish(raw: Path, out: Path, speed: float, width: int, crf: int) -> bool:
    """Re-encode a take into the clip that ships with the docs.

    Three levers, measured on a 49s take (4.8 MB of Playwright's VP8):

        1280 1x  3.3 MB     960 1x  2.3 MB
        1280 2x  2.3 MB     960 2x  872 KB

    Speed and scale each take about a third off and they multiply; the CODEC barely
    matters (VP9 crf30 saved 10%), because a 3D preview is constantly-changing pixels
    rather than a static page. 960 keeps every label and control value legible.

    Speeding up is honest here: it changes the VIDEO, not what the run did, so the
    test and the clip still describe the same interaction. A UI demo at recording
    pace is slower than anyone wants to watch.
    """
    if shutil.which("ffmpeg") is None:
        print("ffmpeg not on PATH: keeping the raw take, no published clip")
        return False
    out.parent.mkdir(parents=True, exist_ok=True)
    if not math.isfinite(speed) or speed <= 0:
        print(f"speed must be a positive number, got {speed}")
        return False
    vf = f"setpts={1/speed:.4f}*PTS,scale={width}:-2,fps=24"
    cmd = ["ffmpeg", "-v", "error", "-y", "-i", str(raw), "-vf", vf,
           "-c:v", "libvpx-vp9", "-crf", str(crf), "-b:v", "0",
           "-row-mt", "1", "-an", str(out)]
    if subprocess.run(cmd, check=False).returncode != 0:
        print("ffmpeg failed: keeping the raw take, no published clip")
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", required=True,
                    help="the run file to perform (test/uiscenarios/clips/<name>.json)")
    ap.add_argument("--host", default="localhost:8080",
                    help="MoonLight to drive (default: localhost:8080)")
    ap.add_argument("--out", default=None,
                    help="where the clip lands (default: media/video/)")
    ap.add_argument("--speed", type=float, default=None,
                    help="override the run file's playback speed")
    ap.add_argument("--width", type=int, default=None,
                    help="override the run file's clip width")
    ap.add_argument("--crf", type=int, default=36,
                    help="VP9 quality, lower is better (default: 36)")
    ap.add_argument("--no-publish", action="store_true",
                    help="record the raw take only, skip the docs clip")
    ap.add_argument("--keep", action="store_true",
                    help="expect leftovers: do not report what the run did not delete")
    args = ap.parse_args()

    host = args.host.replace("http://", "").replace("https://", "").rstrip("/")
    try:
        requests.get(f"http://{host}/api/state", timeout=3)
    except requests.RequestException:
        print(f"No MoonLight answering on {host}.\n"
              f"Start one with: uv run moondeck/run/run_desktop.py", file=sys.stderr)
        return 1

    run = uirun.load_run(Path(args.run))
    # A run needing particular hardware resolves its address from the bench registry,
    # so the run file states the need and moondeck.json states where that board is.
    explicit_host = args.host != ap.get_default("host")
    if run.requires and not explicit_host:
        found = uirun.device_for(run.requires)
        if not found:
            print(f"This run needs a device with {run.requires!r}, and none is "
                  f"answering. Check moondeck.json and the board.", file=sys.stderr)
            return 1
        host = found

    # The run file's own host wins unless --host was given explicitly: a run that
    # drives the installer knows which port that is, and the caller should not have to.
    elif run.host and not explicit_host:
        host = run.host.replace("http://", "").replace("https://", "").rstrip("/")
        try:
            requests.get(f"http://{host}/", timeout=3)
        except requests.RequestException:
            print(f"This run drives {host}, which is not answering.\n"
                  f"Start it (the installer preview: "
                  f"uv run moondeck/run/preview_installer.py)", file=sys.stderr)
            return 1
    # One flat folder of clips, named after the run. A folder per run bought nothing:
    # a run produces ONE take, overwritten next time, so the folder only ever held a
    # single file. media/ splits by KIND instead (audio, video), which is the division
    # that earns its keep once compositions cut video against a music track.
    out_dir = Path(args.out) if args.out else ROOT / "media" / "video"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f"{run.name}.webm"

    print(f"Recording [{run.name}]: {len(run.steps)} steps")

    # RAISES rather than degrading to an empty set. An empty set from a failed probe is
    # indistinguishable from a device with no modules, and either reading disables the
    # leftover check: a failure before the run makes a device look like another surface,
    # and one after it computes an empty leftover set, so a dirty take publishes.
    def module_names() -> set:
        return uirun.all_names(uirun.state(host).get("modules", []))

    # The leftover report only means something against a MoonLight DEVICE. A run may
    # drive another surface entirely (the web installer), which has no module tree and
    # answers /api/state with HTML, so the probe decides rather than the caller. Only
    # THIS probe is forgiving: it runs before the recording, where an unreachable device
    # is still a surface question rather than a verdict on the take.
    try:
        before: set = module_names()
        is_device = bool(before)
    except Exception:
        before, is_device = set(), False

    with sync_playwright() as p:
        # The test id contract, same as the UI tests use.
        p.selectors.set_test_id_attribute(uirun.TEST_ID_ATTRS)
        # A run that drives real hardware needs a granted Web Serial port, and a grant lives
        # in a browser PROFILE rather than in a flag: the API hands out port objects and never
        # device paths, so there is nothing a command line could name. `.playwright-profile`
        # holds one granted once by a human, which every later take reuses with nothing to
        # click. Runs that touch no hardware are unaffected by using it.
        profile = ROOT / ".playwright-profile"
        if profile.is_dir():
            context = p.chromium.launch_persistent_context(
                str(profile), headless=True, channel="chrome", viewport=VIEWPORT)
            browser = context.browser
            page = context.pages[0] if context.pages else context.new_page()
        else:
            browser = p.chromium.launch()
            context = browser.new_context(viewport=VIEWPORT)
            page = context.new_page()
        driver = uirun.Driver(page, host, screencast=page.screencast)
        # `requires` resolves to a real DEVICE, which renders cards like any other; only
        # `host` means another app (the installer), which has none. Folding the two
        # together made a hardware run skip the card wait it needs.
        driver.open_app(cards=not run.host)

        with page.screencast.start(path=str(out), size=VIEWPORT):
            # show_actions draws the pointer and names each action on screen: it
            # animates from the previous action's point to the next, which is the
            # continuity a viewer needs to see cause before effect.
            #
            # `duration` is paid PER ACTION and BLOCKS: a move under show_actions costs
            # almost exactly this (measured: 269ms at 260, 717ms at 700), because the
            # call waits for its own animation. So it is the pace of the video, not a
            # decoration on it.
            #
            # 620ms is the travel time for a pointer crossing the window between two
            # controls, which is the move a viewer has to FOLLOW to see what is being
            # changed. Drags set their own, much shorter, duration for the samples
            # along a slider's track, where the pointer is already where it belongs.
            with page.screencast.show_actions(cursor="pointer", duration=620,
                                              position="top-left", font_size=20):
                failures = driver.run_all(run)
                page.wait_for_timeout(1500)

        context.close()
        browser.close()

    # NO REST CLEANUP. This tool writes NOTHING over the API: a run tidies up through
    # its own delete steps, which is the interface a person would use, and anything it
    # leaves behind is REPORTED rather than quietly removed. A DELETE here would be the
    # one place the tool sets state instead of observing it, and it would also hide a
    # run whose cleanup steps are broken.
    #
    # This probe is NOT rescued: a device that answered before the run and not after it
    # is exactly the case the leftover check exists for, so the failure aborts rather
    # than publishing an unverified take.
    leftover = sorted(module_names() - before) if is_device else []
    if args.keep:
        leftover = []              # the flag says leftovers are expected, so they are not a fault
    if leftover:
        print("\nThe run left these behind (its own delete steps did not remove them):")
        for name in leftover:
            print(f"  {name}")

    if failures:
        print("\nThe take has problems (the device disagreed with the UI):")
        for f in failures:
            print(f"  {f}")
    size = out.stat().st_size // 1024 if out.exists() else 0
    print(f"\n{out}  ({size} KB, raw take)")

    # PUBLISH ONLY A GOOD TAKE. The published clip is the tracked artifact a doc page
    # embeds, so overwriting it from a run the device disagreed with ships a video of
    # the UI not working. The exit code alone did not prevent that: the file was
    # already written by the time anyone read it.
    published = False
    if failures or leftover:
        # NAME the reason. "Not published" alone sent me reading a 17 MB take to work
        # out what had gone wrong, when the run already knew.
        why = []
        if failures:
            why.append(f"{len(failures)} step(s) failed")
        if leftover:
            why.append(f"left behind: {', '.join(leftover)}")
        print(f"Not published ({'; '.join(why)}): fix the run and record again "
              f"(--no-publish to record a raw take deliberately).")
    elif not args.no_publish and out.exists():
        pub = ROOT / "docs" / "assets" / "uiscenarios" / f"{run.name}.webm"
        # The CLI overrides the run file, which overrides the default: a flag is for
        # trying a value, and the run file is where the chosen one is kept.
        speed = args.speed if args.speed is not None else run.speed
        width = args.width if args.width is not None else run.width
        if publish(out, pub, speed, width, args.crf):
            published = True
            ksize = pub.stat().st_size // 1024
            print(f"{pub.relative_to(ROOT)}  ({ksize} KB, {speed:g}x "
                  f"{width}px - tracked, embed this one)")
    # NON-ZERO whenever a clip was wanted and none was written. A leftover-only refusal
    # still wrote no clip, so exiting 0 turned a refused take into a green card with
    # nothing behind it. --no-publish is the one case where no clip is the requested
    # outcome, and a raw take that recorded cleanly is a success.
    if args.no_publish:
        return 1 if failures or leftover else 0
    return 0 if published else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Check that a firmware binary exists and is newer than the sources that go into it.

The full `idf.py build` is the strongest check, but on a cold build directory it costs
minutes — enough that the gate list stops being run, which is worse than a weaker check
run often. This is the cheap alternative: it does not compile, it asks whether the binary
on disk was built *after* every source file that feeds it.

Freshness is measured against the SOURCES, not the clock. A wall-clock rule ("built within
the last hour") passes a binary that predates an edit made twenty minutes ago — precisely
the stale-artifact trap that costs a debugging session chasing a fix that was never in the
running image. Comparing timestamps answers the question the gate actually means: does this
binary include what is on disk right now?

Usage:
  uv run moondeck/check/check_esp32_built.py --firmware esp32s3-n16r8
  uv run moondeck/check/check_esp32_built.py --firmware esp32 --max-age-hours 24

Exit codes: 0 fresh · 1 missing or stale (the message says which, and what to run).
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REPO = ROOT   # this repository, which a test that moves ROOT keeps for the UI globs

sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_esp32 import APP_BIN, FIRMWARES, MOONBASE_BIN  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
import _findings  # noqa: E402

RULES = {
    "missing binary": {"reason": "No firmware image exists to flash or measure.", "fix": "Build it with build_esp32.py."},
    "stale binary": {"reason": "A source that feeds the image is newer than the image, so it lacks what is on disk.", "fix": "Rebuild with build_esp32.py."},
    "stale moonbase image": {"reason": "A moonbase/ source is newer than the MoonBase image.", "fix": "Rebuild with build_esp32.py."},
    "old binary": {"reason": "The image is older than the age the run asked for.", "fix": "Rebuild with build_esp32.py."},
}


def record(firmware, found):
    """Keep this run's verdict on the one firmware it looked at, beside the others' last verdicts: a run checks one firmware, the record holds every firmware's."""
    import json
    try:
        kept = json.loads((_findings.DIR / "check_esp32_built.json").read_text(encoding="utf-8"))["findings"]
    except (OSError, ValueError, KeyError):
        kept = []
    _findings.write("check_esp32_built", RULES, [f for f in kept if f.get("firmware") != firmware] + [dict(f, firmware=firmware) for f in found])

# What feeds an ESP32 image. Kept in step with this check's trigger in
# CLAUDE.md § Commit: both answer "could this change alter the firmware?".
SOURCE_DIRS = ("src", "esp32")
SOURCE_FILES = ("CMakeLists.txt", "library.json")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".cmake", ".json", ".txt", ".py", ".js",
                   ".html", ".css", ".defaults", ".csv",  # .csv: partition tables feed the image
                   ".yml"}                                 # .yml: idf_component.yml pins components

# Build outputs and caches live under the source dirs; they are products, not inputs, and
# including them would compare the binary against itself.
SKIP_PARTS = {"build", "managed_components", "__pycache__", ".git"}

# Generated headers that sit in the source tree but are build PRODUCTS (both gitignored).
# A desktop build regenerates them after an ESP32 build, so treating them as inputs makes
# the firmware look stale forever, one minute after it was built. What actually feeds them
# — src/ui/*.js, library.json, the git hash — is already covered by the scan.
SKIP_FILES = {"src/ui/ui_embedded.h", "src/core/util/build_id.h"}

# The desktop-only platform never compiles into an ESP32 image, so an edit there cannot
# stale the firmware. Kept in step with this check's trigger in CLAUDE.md § Commit,
# which excludes the same path.
SKIP_PREFIXES = ("src/platform/desktop/",)


def newest_source(only=None):
    """The most recently modified source file that feeds a firmware image."""
    newest_path, newest_mtime = None, 0.0

    for rel in SOURCE_DIRS:
        base = ROOT / rel
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            rel_posix = path.relative_to(ROOT).as_posix()
            if SKIP_PARTS.intersection(path.relative_to(ROOT).parts):
                continue
            if rel_posix in SKIP_FILES or rel_posix.startswith(SKIP_PREFIXES):
                continue
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            if only is not None and rel_posix not in only:
                continue   # this firmware does not compile it, so it cannot make it stale
            mtime = path.stat().st_mtime
            if mtime > newest_mtime:
                newest_path, newest_mtime = path, mtime

    for rel in SOURCE_FILES:
        if only is not None and rel not in only:
            continue
        path = ROOT / rel
        if path.exists() and path.stat().st_mtime > newest_mtime:
            newest_path, newest_mtime = path, path.stat().st_mtime

    return newest_path, newest_mtime


# The UI reaches the firmware as one generated header, whose inputs no compiler reads, so ninja never lists them. They are every file the builds glob as its dependencies, bundled by embed_ui.cmake.
UI_HEADER = "src/ui/ui_embedded.h"
UI_GLOBS = ("src/ui/*.js", "src/ui/*.html", "src/ui/*.css", "src/ui/*.png", "src/ui/vendor/*.js", "src/ui/embed_ui.cmake", "moondeck/build/bundle_ui.py")


def ui_inputs() -> set:
    """The files the embedded header is generated from, the same globs the three builds depend on, read in this repository."""
    return {str(p.relative_to(REPO)) for g in UI_GLOBS for p in REPO.glob(g)}


def with_ui(deps: set) -> set:
    """A build's dependencies, with the UI's own files added when it reads the embedded header, so a UI edit marks the firmware stale."""
    return deps | ui_inputs() if UI_HEADER in deps else deps


def compiled_sources(firmware):
    """The files this firmware's build actually reads, from ninja's own dependency database.

    A firmware compiles a SUBSET of src/: a driver gated behind a per-firmware flag (MM_PANEL_CARDS)
    is not in the S3 build at all. Comparing every source against that binary flags it stale forever
    over a file it cannot compile, and no rebuild can clear it — the build correctly does nothing, so
    the binary's timestamp never moves. Asking ninja what it depends on is the authoritative answer.

    Returns None when the database cannot be read, and the caller then falls back to scanning
    everything: over-reporting staleness is the safe direction for a freshness gate.
    """
    build_dir = ROOT / "build" / f"esp32-{firmware}"
    if not (build_dir / ".ninja_deps").exists():
        return None
    try:
        out = subprocess.run(["ninja", "-t", "deps"], cwd=build_dir,
                             capture_output=True, text=True, timeout=60)
    except (subprocess.SubprocessError, OSError):
        return None
    if out.returncode != 0:
        return None
    deps = set()
    for line in out.stdout.splitlines():
        line = line.strip()
        if not line.startswith(("output", "deps")) and line:
            path = (build_dir / line).resolve()
            try:
                deps.add(path.relative_to(ROOT).as_posix())
            except ValueError:
                pass   # outside the repo (IDF, toolchain) — not ours to watch
    return with_ui(deps) or None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--firmware", required=True,
                        help="Firmware variant, e.g. esp32s3-n16r8 (its build dir is "
                             "build/esp32-<firmware>/).")
    parser.add_argument("--max-age-hours", type=float, default=0,
                        help="Also fail if the binary is older than this many hours, even "
                             "when no source is newer. 0 (default) disables the age rule — "
                             "source freshness is the check that matters.")
    args = parser.parse_args()

    binary = ROOT / "build" / f"esp32-{args.firmware}" / APP_BIN
    build_cmd = f"uv run moondeck/build/build_esp32.py --firmware {args.firmware}"

    if not binary.exists():
        record(args.firmware, [{"rule": "missing binary", "message": f"{args.firmware}: no firmware binary", "file": binary.relative_to(ROOT).as_posix(), "level": "error"}])
        print(f"No firmware binary for {args.firmware}.")
        print(f"  expected: {binary.relative_to(ROOT)}")
        print(f"  build it: {build_cmd}")
        return 1

    built = binary.stat().st_mtime
    newest_path, newest_mtime = newest_source(compiled_sources(args.firmware))
    age_h = (time.time() - built) / 3600

    if newest_path is not None and newest_mtime > built:
        stale_by = (newest_mtime - built) / 60
        record(args.firmware, [{"rule": "stale binary", "message": f"{args.firmware}: {newest_path.relative_to(ROOT)} is {stale_by:.0f} min newer than the binary", "file": newest_path.relative_to(ROOT).as_posix(), "level": "error"}])
        print(f"Firmware for {args.firmware} is STALE: a source file is newer than the binary.")
        print(f"  binary : {binary.relative_to(ROOT)} (built {age_h:.1f}h ago)")
        print(f"  newer  : {newest_path.relative_to(ROOT)} ({stale_by:.0f} min after the build)")
        print(f"  rebuild: {build_cmd}")
        return 1

    # MoonBase firmwares carry a second image (built into build/moonbase-<chip>/ by the same
    # build run): it must exist and be newer than every moonbase/ source, by the same
    # sources-not-clock rule as the app image.
    spec = FIRMWARES.get(args.firmware, {})
    if spec.get("moonbase"):
        mb_bin = ROOT / "build" / f"moonbase-{spec['chip']}" / MOONBASE_BIN
        if not mb_bin.exists():
            record(args.firmware, [{"rule": "missing binary", "message": f"{args.firmware}: no MoonBase image", "file": mb_bin.relative_to(ROOT).as_posix(), "level": "error"}])
            print(f"No MoonBase image for {args.firmware}.")
            print(f"  expected: {mb_bin.relative_to(ROOT)}")
            print(f"  build it: {build_cmd}")
            return 1
        mb_built = mb_bin.stat().st_mtime
        mb_newest = max((f.stat().st_mtime for f in (ROOT / "moonbase").rglob("*")
                         if f.is_file() and f.suffix in SOURCE_SUFFIXES
                         and not SKIP_PARTS.intersection(f.parts)), default=0)
        if mb_newest > mb_built:
            record(args.firmware, [{"rule": "stale moonbase image", "message": f"{args.firmware}: a moonbase/ source is newer than the MoonBase image", "file": mb_bin.relative_to(ROOT).as_posix(), "level": "error"}])
            print(f"MoonBase image for {args.firmware} is STALE: a moonbase/ source is newer.")
            print(f"  rebuild: {build_cmd}")
            return 1

    if args.max_age_hours and age_h > args.max_age_hours:
        record(args.firmware, [{"rule": "old binary", "message": f"{args.firmware}: built {age_h:.1f}h ago, over the {args.max_age_hours}h limit", "file": binary.relative_to(ROOT).as_posix(), "level": "error"}])
        print(f"Firmware for {args.firmware} is older than {args.max_age_hours}h "
              f"({age_h:.1f}h) — no source is newer, but the age rule asks for a rebuild.")
        print(f"  rebuild: {build_cmd}")
        return 1

    record(args.firmware, [])
    print(f"Firmware for {args.firmware} is up to date "
          f"(built {age_h:.1f}h ago, newer than every source).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

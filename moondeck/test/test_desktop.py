#!/usr/bin/env python3
"""Run desktop unit tests from the per-host build dir.

By default runs every TEST_CASE. Optional filters narrow the run:
  --module <Name>   only TEST_CASEs from files where @module == <Name>
                    (or where <Name> appears in @also)
  --test <pattern>  doctest wildcard pattern matched against TEST_CASE names
"""

import argparse
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Reuse the shared test-metadata parser instead of re-implementing @module /
# @case discovery here. Same parser feeds the doc generator and MoonDeck.
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import _test_metadata as test_meta  # noqa: E402


def _host_build_dir() -> Path:
    """Match build_desktop.py / run_desktop.py — one dir per host."""
    return ROOT / "build" / {
        "Darwin":  "macos",
        "Linux":   "linux",
        "Windows": "windows",
    }.get(platform.system(), platform.system().lower())


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--module", help="Filter to the named module (matches @module + @also).")
    p.add_argument("--test", help="doctest test-case pattern (wildcards allowed).")
    args = p.parse_args()

    build_dir = _host_build_dir()
    if not build_dir.exists():
        print(f"Build directory not found: {build_dir.relative_to(ROOT)}. "
              f"Run build_desktop.py first.")
        sys.exit(1)

    # MSVC multi-config drops mm_tests under test/Release/; everyone else
    # puts it at test/. Pick whichever exists.
    candidates = [
        build_dir / "test" / "mm_tests.exe",
        build_dir / "test" / "Release" / "mm_tests.exe",
        build_dir / "test" / "mm_tests",
    ]
    test_exe = next((c for c in candidates if c.exists()), None)
    if test_exe is None:
        print(f"Test executable not found under "
              f"{build_dir.relative_to(ROOT)}/test/.")
        print("Compile it first: uv run moondeck/build/build_desktop.py --tests (MoonDeck → desktop → Compile Tests).")
        sys.exit(1)

    # Default: summary-only output (`-s` dumps every passing assertion, ~11k lines
    # for the full suite, which floods the MoonDeck log). Filtered runs (--module
    # or --test) opt in to `-s` so a single case's assertions stay visible.
    cmd = [str(test_exe)]
    filtered = False

    if args.module and args.module.lower() != "all":
        cases = [c["name"] for c in test_meta.cases_for_module(args.module)]
        if not cases:
            print(f"No unit tests found for module: {args.module}")
            sys.exit(1)
        # doctest -tc accepts comma-separated patterns; escape commas in case names
        # (shouldn't appear, but defensive).
        joined = ",".join(c.replace(",", "?") for c in cases)
        cmd.append(f"-tc={joined}")
        print(f"Module filter: {args.module} ({len(cases)} test case(s))")
        filtered = True

    if args.test:
        cmd.append(f"-tc={args.test}")
        filtered = True

    if filtered:
        cmd.append("-s")  # show passing assertions for the small filtered set

    print(f"Running tests from {build_dir.relative_to(ROOT)}/ ...")
    sys.stdout.flush()
    # Stream output line-by-line so the MoonDeck log updates as the test runs
    # instead of dumping everything at the end. Without bufsize=1 + iter(), the
    # full ~11k-line output buffers internally and only appears at exit.
    proc = subprocess.Popen(
        cmd, cwd=build_dir,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        bufsize=1, text=True, encoding="utf-8", errors="replace",
    )
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
    sys.exit(proc.wait())


if __name__ == "__main__":
    main()

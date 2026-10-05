#!/usr/bin/env python3
"""Move library.json to the version development works toward, at the two moments a release changes it.

Between releases library.json holds the NEXT version with a `-dev` suffix, and tagging `vX.Y.Z` releases it: verify_version.py compares the tag with library.json's core, so `v6.1.0` releases `6.1.0-dev` with no edit.
Two moments change the file, and this script makes each one command; it never touches git, since the commit and the tag stay the product owner's.

    uv run moondeck/build/release_version.py next    # after releasing vX.Y.Z: X.(Y+1).0-dev
    uv run moondeck/build/release_version.py major   # the coming release is a major: (X+1).0.0-dev, then tag v(X+1).0.0
"""

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compute_version as cv  # noqa: E402  (the version rules, one home)


def set_version(version: str, path: Path = None) -> None:
    """Rewrite library.json's version field in place, the rest of the file as it is."""
    path = path or cv.LIBRARY_JSON
    text = path.read_text(encoding="utf-8")
    new, n = re.subn(r'("version"\s*:\s*")[^"]*(")', rf"\g<1>{version}\g<2>", text, count=1)
    if n != 1:
        raise SystemExit(f"no version field in {path}")
    path.write_text(new, encoding="utf-8")


def target(step: str, current: str, last: str | None) -> str:
    """The development version a step moves to: after the last release (`next`), or toward a major (`major`)."""
    core = cv.core_version(current)
    if step == "next":
        return cv.next_minor(last or core) + "-dev"
    return cv.next_major(last or core) + "-dev"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=["next", "major"], help="next: after a release; major: the coming release is a major")
    args = ap.parse_args()
    current = json.loads(cv.LIBRARY_JSON.read_text(encoding="utf-8"))["version"]
    version = target(args.step, current, cv.last_release_core())
    set_version(version)
    print(f"library.json: {current} -> {version}. Commit it; the release tag will be v{cv.core_version(version)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compute the semver string a build should carry, per build type.

`library.json` holds the in-development version with a `-dev` prerelease suffix
(e.g. `2.1.0-dev`). That's right for a local build, but a *published* build needs
a precise, comparable version:

  - Stable release (a `vX.Y.Z` tag): the CORE semver, suffix dropped: `2.1.0`.
    (The release ritual already bumps library.json to the core before tagging;
    this just normalises in case the suffix lingers.)
  - Moving `latest` build (every push to main): a MONOTONIC prerelease:
    `<next>-dev.<N>` where N = commits since the last stable `vX.Y.Z` tag
    (git-describe style). Successive `latest` builds get `…-dev.6`, `…-dev.7`, …
    which semver §11 orders numerically, so a device can tell it's behind the
    newest `latest`. Without N, every `latest` build claims the same version.

N counts since the last `v*` tag specifically, NOT `git describe`'s nearest tag,
which would pick the moving `latest` tag and give a meaningless count. With no
`v*` tag yet, N falls back to the total commit count from the root.

The same string is burned into the binary (-DMM_VERSION), the release asset names
(firmware-<F>-v<V>.bin), and the ESP Web Tools manifest, so all three agree.

Usage:
  compute_version.py --tag latest              # auto → latest → <next>-dev.<N>, e.g. 6.1.0-dev.6 after v6.0.0
  compute_version.py --tag v2.1.0              # auto → stable → core, e.g. 2.1.0
  compute_version.py --tag v2.1.0-rc1          # auto → stable → the RC semver, e.g. 2.1.0-rc1
  compute_version.py --channel local           # <next>-dev, e.g. 6.1.0-dev after v6.0.0

The release workflow passes only `--tag`; the channel is derived from it (the one
place that mapping lives), so the build and release jobs can't disagree.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"


def core_version(version: str) -> str:
    """Strip any -prerelease / +build suffix → the MAJOR.MINOR.PATCH core."""
    return version.split("-")[0].split("+")[0]


def commits_since_last_stable() -> int:
    """Commits since the last `vX.Y.Z` tag (not the moving `latest` tag).

    Falls back to the total commit count when no `v*` tag exists yet.
    """
    try:
        last = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0", "--match", "v*"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.strip()
        rng = f"{last}..HEAD"
    except subprocess.CalledProcessError:
        rng = "HEAD"  # no v* tag yet: count from the root
    out = subprocess.run(
        ["git", "rev-list", "--count", rng],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout.strip()
    return int(out)


def last_release_core() -> str | None:
    """The core of the last `vX.Y.Z` tag, or None before the first release."""
    try:
        tag = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*"],
            cwd=ROOT, capture_output=True, text=True, check=True,
        ).stdout.strip()
    except subprocess.CalledProcessError:
        return None
    return core_version(tag[1:] if tag.startswith("v") else tag)


def _as_tuple(core: str) -> tuple:
    return tuple(int(p) for p in core.split(".")[:3])


def next_minor(core: str) -> str:
    """The version development moves on to after releasing `core`: X.Y.Z → X.(Y+1).0."""
    major, minor, _ = _as_tuple(core)
    return f"{major}.{minor + 1}.0"


def next_major(core: str) -> str:
    """The next major after `core`: X.Y.Z → (X+1).0.0."""
    return f"{_as_tuple(core)[0] + 1}.0.0"


def development_core(version: str) -> str:
    """The core a build after the last release carries: library.json's when it is ahead of that release, else the next minor.

    Without this, a build after v6.0.0 read 6.0.0-dev.N, which semver ranks BELOW 6.0.0, and a board on `latest` was offered the older release as an update.
    """
    core = core_version(version)
    last = last_release_core()
    if last is None or _as_tuple(core) > _as_tuple(last):
        return core
    return next_minor(last)


def channel_for_tag(tag: str) -> str:
    """Derive the build channel from the release tag, the single source of that mapping.

    `latest` → the moving prerelease channel; any other tag (a `vX.Y.Z` / `vX.Y.Z-rcN`,
    or empty) → stable (which handles an -rc suffix inside `compute`). Centralised here so
    the workflow's two Compute-version steps can't disagree on the channel.
    """
    return "latest" if tag == "latest" else "stable"


def compute(channel: str, tag: str = "") -> str:
    version = json.loads(LIBRARY_JSON.read_text(encoding="utf-8"))["version"]
    core = core_version(version)
    if channel == "stable":
        # A vX.Y.Z-rcN tag is itself a precise prerelease semver: carry it through
        # verbatim (minus the leading v) rather than collapsing to the core X.Y.Z, or
        # the RC binary/manifest would lie about being the stable release. A plain
        # vX.Y.Z (or no tag) yields the core.
        if tag:
            t = tag[1:] if tag.startswith("v") else tag
            if "-" in t:  # has a prerelease identifier (rc, beta, …)
                return t
        return core
    if channel == "latest":
        return f"{development_core(version)}-dev.{commits_since_last_stable()}"
    return f"{development_core(version)}-dev"   # local: never passes for a release


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--channel", choices=["stable", "latest", "local", "auto"], default="auto",
        help="auto (default) → derive from --tag (latest tag → latest, else stable); "
             "stable → core semver (or the tag's prerelease semver for an -rc tag); "
             "latest → <next>-dev.<N>; local → <next>-dev, where <next> is library.json's core when "
             "ahead of the last vX.Y.Z tag, else the next minor",
    )
    parser.add_argument(
        "--tag", default="",
        help="The release tag (e.g. latest, v2.1.0, v2.1.0-rc1). With --channel auto it "
             "selects the channel; for a prerelease tag on the stable channel, its semver "
             "is used verbatim instead of the core.",
    )
    args = parser.parse_args()
    channel = channel_for_tag(args.tag) if args.channel == "auto" else args.channel
    print(compute(channel, args.tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())

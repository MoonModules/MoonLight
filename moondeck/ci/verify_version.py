#!/usr/bin/env python3
"""Verify that the git tag matches `library.json["version"]`.

Runs as the first job in release.yml. Fails the workflow when the tag and
the in-tree version disagree, so a release never publishes binaries whose
filenames lie about the version they ship.

Tag → version mapping: strip a leading 'v'. Both `v1.0.0` and `1.0.0` are
accepted as the tag, but the in-tree version is the bare semver.

Release ritual (develop-on-a-prerelease, the standard semver flow): between
releases, library.json carries the NEXT version with a `-dev` prerelease suffix
(e.g. `2.1.0-dev`), so a moving/`latest` build self-reports a clean prerelease
semver. Cutting the stable release drops the suffix: tag `v2.1.0` releases what
was `2.1.0-dev`. So the check compares the tag's semver to library.json's CORE
version (the part before any `-prerelease` suffix) — `v2.1.0` ↔ `2.1.0-dev`
passes (same core), while a wrong core like `v2.2.0` ↔ `2.1.0-dev` still fails.

The `latest` tag is the moving prerelease channel published on every merge to
main, not a semver release — the script accepts it and skips the equality check.

Inputs:
  GITHUB_REF_NAME  — the tag, set by GitHub Actions on a `push: tags` event
                     (or the workflow_dispatch input forwarded to it).
  --tag <name>     — explicit override, useful for local checks.
"""

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck"))
from _console import utf8_stdio  # noqa: E402
utf8_stdio()   # the OK summary prints a ↔ through Windows CI's pipe
LIBRARY_JSON = ROOT / "library.json"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", help="Tag to verify (defaults to $GITHUB_REF_NAME)")
    args = parser.parse_args()

    import os
    tag = args.tag or os.environ.get("GITHUB_REF_NAME")
    if not tag:
        print("verify_version: no tag supplied (--tag or $GITHUB_REF_NAME).")
        return 2

    # `latest` is a moving prerelease — no in-tree version to match against.
    if tag == "latest":
        print(f"verify_version: '{tag}' tag, skipping library.json match.")
        return 0

    tag_version = tag[1:] if tag.startswith("v") else tag

    try:
        meta = json.loads(LIBRARY_JSON.read_text())
    except FileNotFoundError:
        print(f"verify_version: {LIBRARY_JSON} not found.")
        return 2
    except json.JSONDecodeError as e:
        print(f"verify_version: {LIBRARY_JSON} is not valid JSON: {e}")
        return 2

    in_tree = meta.get("version")
    if not in_tree:
        print(f"verify_version: 'version' missing from {LIBRARY_JSON}.")
        return 2

    # Compare the CORE semver (drop any `-prerelease` / `+build` suffix from the
    # in-tree version) so the release ritual works: cutting `v2.1.0` releases the
    # in-development `2.1.0-dev`. A mismatched core (e.g. `v2.2.0` vs `2.1.0-dev`)
    # still fails. The tag itself is a real release, so it carries no suffix.
    in_tree_core = in_tree.split("-")[0].split("+")[0]
    if tag_version != in_tree_core:
        print(
            f"verify_version: tag '{tag}' (version {tag_version}) does not match "
            f"library.json core version '{in_tree_core}' (from '{in_tree}').\n"
            f"To release, set library.json version to {tag_version} (drop the "
            f"-dev suffix), commit, retag, push."
        )
        return 1

    print(f"verify_version: OK (tag {tag} ↔ library.json {in_tree}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

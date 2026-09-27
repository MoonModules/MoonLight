# /// script
# dependencies = ["pytest"]
# ///
"""verify_version release-ritual tests.

Pins the develop-on-a-prerelease flow: between releases library.json carries the NEXT
version with a `-dev` suffix (e.g. `2.1.0-dev`), and cutting the stable tag drops the
suffix. So `verify_version.py` must accept `v2.1.0` against `library.json 2.1.0-dev`
(same core) while still rejecting a wrong core. The `latest` moving tag skips the check.

Run: `uv run pytest test/python`.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "moondeck" / "ci" / "verify_version.py"


def run(tag, version, tmp_path):
    """Run verify_version against a throwaway library.json holding `version`."""
    lib = tmp_path / "library.json"
    lib.write_text(json.dumps({"name": "MoonLight", "version": version}))
    # verify_version reads ROOT/library.json; point it at the temp copy via cwd is not enough
    # (it resolves ROOT from __file__), so patch by running a tiny wrapper that swaps the path.
    code = (
        f"import runpy, sys; "
        f"import moondeck.ci.verify_version as v; "
        f"v.LIBRARY_JSON = __import__('pathlib').Path(r'{lib}'); "
        f"sys.argv = ['verify_version', '--tag', r'{tag}']; "
        f"sys.exit(v.main())"
    )
    return subprocess.run(
        [sys.executable, "-c", code], cwd=ROOT, capture_output=True, text=True
    ).returncode


def test_stable_tag_matches_dev_library(tmp_path):
    # The release ritual: tag v2.1.0 releases the in-development 2.1.0-dev.
    assert run("v2.1.0", "2.1.0-dev", tmp_path) == 0


def test_stable_tag_matches_clean_library(tmp_path):
    assert run("v2.0.0", "2.0.0", tmp_path) == 0


def test_wrong_core_fails(tmp_path):
    # A tag whose CORE differs from library.json must still fail.
    assert run("v2.2.0", "2.1.0-dev", tmp_path) == 1


def test_latest_skips(tmp_path):
    # The moving prerelease channel has no in-tree version to match.
    assert run("latest", "2.1.0-dev", tmp_path) == 0

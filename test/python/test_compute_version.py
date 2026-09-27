# /// script
# dependencies = ["pytest"]
# ///
"""compute_version tests — pins the per-channel version string.

A stable build carries the core semver; a `latest` build carries a monotonic
`<core>-dev.<N>` so successive latest builds are orderable; a local build is
library.json verbatim. N counts since the last `v*` tag, with a tag-less fallback.

Run: `uv run pytest test/python`.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "build"))
import compute_version as cv  # noqa: E402


def _lib(tmp_path, version):
    """Write a throwaway library.json holding `version`, return its path."""
    f = tmp_path / "library.json"
    f.write_text(json.dumps({"name": "MoonLight", "version": version}))
    return f


def test_core_strips_prerelease_and_build():
    assert cv.core_version("2.1.0-dev") == "2.1.0"
    assert cv.core_version("2.1.0-dev.6") == "2.1.0"
    assert cv.core_version("2.1.0+build.7") == "2.1.0"
    assert cv.core_version("2.0.0") == "2.0.0"


def test_stable_is_core(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    assert cv.compute("stable") == "2.1.0"


def test_latest_is_core_dev_n(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    monkeypatch.setattr(cv, "commits_since_last_stable", lambda: 6)
    assert cv.compute("latest") == "2.1.0-dev.6"


def test_local_is_verbatim(monkeypatch, tmp_path):
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    assert cv.compute("local") == "2.1.0-dev"


def test_channel_for_tag_centralizes_the_mapping():
    # The release workflow passes only --tag; the channel derives from it here, so
    # the build job and release job can't disagree.
    assert cv.channel_for_tag("latest") == "latest"
    assert cv.channel_for_tag("v2.1.0") == "stable"
    assert cv.channel_for_tag("v2.1.0-rc1") == "stable"   # -rc handled inside compute(), still stable channel
    assert cv.channel_for_tag("") == "stable"


def test_stable_rc_tag_keeps_prerelease(monkeypatch, tmp_path):
    # An -rc tag is itself a precise prerelease semver — carried through verbatim
    # (minus the leading v), not collapsed to the core, so the RC binary doesn't
    # masquerade as the stable release.
    monkeypatch.setattr(cv, "LIBRARY_JSON", _lib(tmp_path, "2.1.0-dev"))
    assert cv.compute("stable", "v2.1.0-rc1") == "2.1.0-rc1"
    assert cv.compute("stable", "v2.1.0") == "2.1.0"      # plain stable tag → core
    assert cv.compute("stable", "") == "2.1.0"            # no tag → core


def test_no_tag_fallback_counts_from_root(monkeypatch, tmp_path):
    # First-release case: with no v* tag, `git describe --match v*` fails and the
    # helper falls back to counting all commits from the root (rev-list HEAD).
    import subprocess
    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        if "describe" in cmd:
            raise subprocess.CalledProcessError(128, cmd)   # no v* tag
        # rev-list --count HEAD → some count
        return subprocess.CompletedProcess(cmd, 0, stdout="42\n", stderr="")

    monkeypatch.setattr(cv.subprocess, "run", fake_run)
    assert cv.commits_since_last_stable() == 42
    # The fallback range must be HEAD (not <tag>..HEAD).
    rev_list = next(c for c in calls if "rev-list" in c)
    assert rev_list[-1] == "HEAD"

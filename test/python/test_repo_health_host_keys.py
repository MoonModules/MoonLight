"""A desktop metric belongs to the host that measured it, not to `desktop`.

macOS, Windows and CI Linux differ by more than the code does between two commits: one Windows run
rewrote a committed tick from 2us to 5us, fps from 500,000 to 200,000, and a binary size from a
macOS build to a Windows `.exe`, none of which any diff caused. With a flat `desktop` key the
per-commit trend records whichever machine ran last rather than what changed, and two contributors
overwrite each other silently, in both directions.

The carry-forward half is already pinned in `test_repo_health_baseline.py`, which asserts that a
non-`esp32*` key survives both the merge and the ghost-row prune. What was missing, and what these
add, is the half that was actually wrong: the spelling of the key at the point it is written.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import _host  # noqa: E402


def test_the_key_names_the_host_and_never_lumps_two_together(monkeypatch):
    """Each OS gets its own key, derived from the one map that already names the build directory.

    The fallback names the platform rather than sharing a `desktop-unknown` bucket, so a machine
    nobody has added yet still cannot overwrite a named host's numbers.
    """
    for build_dir, expected in (("build/macos", "desktop-macos"),
                                ("build/linux", "desktop-linux"),
                                ("build/windows", "desktop-windows"),
                                ("build/freebsd", "desktop-freebsd")):
        monkeypatch.setattr(_host, "host_build_dir", lambda d=build_dir: d)
        assert _host.desktop_target() == expected


def test_the_measurement_itself_is_written_under_the_host_key(monkeypatch, tmp_path):
    """The control for the actual defect: the WRITE must not spell the key `desktop`.

    The merge was never wrong. Both hosts simply wrote to the same `desktop` key, so the merge did
    its job and the later run won. Pinning the merge passes against the old code; only pinning what
    `measure_flash` writes fails against it, which is what makes this the control and not a
    restatement of the baseline tests.
    """
    import repo_health

    # A tmp ROOT with a `build` dir, because measure_flash returns early when none exists. Pointing
    # it at the repo would make the test pass or fail on whether this machine happens to have built
    # anything, which is how the first version of this test passed on Windows and failed on CI.
    (tmp_path / "build").mkdir()
    monkeypatch.setattr(repo_health, "ROOT", tmp_path)

    binary = tmp_path / "projectMM.exe"
    binary.write_bytes(b"x" * 1024)
    monkeypatch.setattr(repo_health, "desktop_binary", lambda: binary)
    monkeypatch.setattr(repo_health, "desktop_target", lambda: "desktop-windows")
    repo_health.MEASURED_THIS_RUN.clear()
    repo_health.MEASURED_DATES.clear()

    flash = repo_health.measure_flash()

    assert "desktop" not in flash, "a flat `desktop` key is what let two hosts overwrite each other"
    assert flash["desktop-windows"] == 1024
    assert "desktop-windows" in repo_health.MEASURED_THIS_RUN
    assert "desktop" not in repo_health.MEASURED_DATES

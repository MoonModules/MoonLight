"""build_esp32.find_idf_python() must select the venv by IDF *version*, not mtime.

ESP-IDF makes one Python venv per IDF version (`idf<major.minor>_py<X.Y>_env`) under
`~/.espressif/python_env/`. With two IDFs installed — e.g. a 5.5 kept alongside 6.1 for
a version-fallback experiment — sourcing either one's `export.sh` touches its venv, so
the last-activated venv is the newest by mtime. The old logic picked newest-by-mtime,
which meant a 6.1 build could be handed the 5.5 venv (its esptool is too old → idf.py
aborts with "The following Python requirements are not satisfied"). This pins that the
selection follows the *target* IDF version, so it's a function of what we build, not of
what was last sourced.

Imports the real function from moondeck/build/build_esp32.py (no ESP-IDF needed).
Run: `uv run --with pytest pytest test/python -q`.
"""

import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

import build_esp32  # noqa: E402


def _make_venvs(home: Path, names_by_age: list[str]) -> None:
    """Create fake venvs under <home>/.espressif/python_env/, each with a python
    executable. `names_by_age` is oldest-first; later entries get a newer mtime so
    the LAST name is the newest (the mtime a naive picker would choose)."""
    venv_root = home / ".espressif" / "python_env"
    for i, name in enumerate(names_by_age):
        d = venv_root / name / build_esp32._VENV_BIN
        d.mkdir(parents=True)
        exe = d / build_esp32._PYTHON_EXE
        exe.write_text("#!fake python\n")
        # Stagger mtimes deterministically by index so the last name is newest.
        ts = 1_000_000 + i  # strictly increasing → last name is newest
        os.utime(venv_root / name, (ts, ts))


def _make_idf(idf_path: Path, version_line: str) -> Path:
    idf_path.mkdir(parents=True, exist_ok=True)
    (idf_path / "version.txt").write_text(version_line)
    return idf_path


def test_picks_matching_version_even_when_other_is_newer(tmp_path, monkeypatch):
    """The regression: a 6.1 build with a NEWER-mtime 5.5 venv present must still
    pick the 6.1 venv."""
    home = tmp_path / "home"
    # 6.1 venv created first (older), 5.5 venv created last (newest mtime).
    _make_venvs(home, ["idf6.1_py3.12_env", "idf5.5_py3.12_env"])
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))

    idf61 = _make_idf(tmp_path / "esp-idf", "v6.1-dev-5215-g0d928780081")
    picked = build_esp32.find_idf_python(idf61)
    assert picked is not None
    assert picked.name == "idf6.1_py3.12_env", (
        "must match the target IDF version (6.1), not the newest-mtime venv (5.5)"
    )


def test_picks_matching_version_for_the_older_idf(tmp_path, monkeypatch):
    """Symmetric case: a 5.5 build must pick the 5.5 venv even if the 6.1 venv is
    newer — proves it's version-driven, not just 'prefer 6.1'."""
    home = tmp_path / "home"
    _make_venvs(home, ["idf5.5_py3.12_env", "idf6.1_py3.12_env"])  # 6.1 newest
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))

    idf55 = _make_idf(tmp_path / "esp-idf-v5.5", "v5.5.4")
    picked = build_esp32.find_idf_python(idf55)
    assert picked is not None
    assert picked.name == "idf5.5_py3.12_env"


def test_newest_python_minor_wins_within_a_version(tmp_path, monkeypatch):
    """If several Python-minor venvs exist for the SAME IDF version, the newest one
    wins (mtime is the tie-breaker only within the matched version)."""
    home = tmp_path / "home"
    _make_venvs(home, ["idf6.1_py3.12_env", "idf6.1_py3.13_env"])  # 3.13 newest
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))

    idf61 = _make_idf(tmp_path / "esp-idf", "v6.1-dev-5215-g0d928780081")
    picked = build_esp32.find_idf_python(idf61)
    assert picked is not None
    assert picked.name == "idf6.1_py3.13_env"


def test_falls_back_to_newest_when_no_version_match(tmp_path, monkeypatch):
    """Unknown/absent version match → keep the old newest-mtime behavior so a
    single-IDF setup (the common case) is unaffected."""
    home = tmp_path / "home"
    _make_venvs(home, ["idf6.1_py3.12_env", "idf6.1_py3.13_env"])
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))

    # An IDF whose version doesn't match any venv prefix (e.g. a 5.4 with no venv).
    idf_other = _make_idf(tmp_path / "esp-idf-x", "v5.4.1")
    picked = build_esp32.find_idf_python(idf_other)
    assert picked is not None
    assert picked.name == "idf6.1_py3.13_env", "newest overall when nothing matches"


def test_version_prefix_does_not_collide_across_minor(tmp_path, monkeypatch):
    """A 6.1 target must NOT match an idf6.10 venv. The prefix carries a trailing
    '_' ('idf6.1_') precisely so 'idf6.1' can't prefix-match 'idf6.10_py...'; this
    pins that boundary so a refactor that drops the separator regresses loudly.
    With no genuine idf6.1 venv present, it falls back to newest-overall (idf6.10)
    rather than wrongly treating idf6.10 as a 6.1 match."""
    home = tmp_path / "home"
    _make_venvs(home, ["idf6.1_py3.12_env", "idf6.10_py3.12_env"])  # 6.10 newest
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))

    idf61 = _make_idf(tmp_path / "esp-idf", "v6.1-dev-5215-g0d928780081")
    picked = build_esp32.find_idf_python(idf61)
    assert picked is not None
    # Must pick the exact idf6.1 venv, NOT idf6.10 (even though idf6.10 is newer).
    assert picked.name == "idf6.1_py3.12_env", (
        "the trailing '_' must keep idf6.1 from matching idf6.10"
    )


def test_none_when_no_venvs(tmp_path, monkeypatch):
    home = tmp_path / "home"
    (home / ".espressif" / "python_env").mkdir(parents=True)
    monkeypatch.setattr(build_esp32.Path, "home", staticmethod(lambda: home))
    idf61 = _make_idf(tmp_path / "esp-idf", "v6.1")
    assert build_esp32.find_idf_python(idf61) is None

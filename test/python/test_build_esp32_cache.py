"""A build directory whose cached version disagrees with this build is reconfigured, not reused."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from build_esp32 import fragment_paths, stale_feature_cache  # noqa: E402
import build_esp32  # noqa: E402
import sdkconfig_fragments  # noqa: E402


def test_a_plain_build_after_a_versioned_one_reconfigures(tmp_path):
    """CMake keeps a -D value a later configure omits, so a plain build would otherwise report the version an earlier build was given."""
    cache = tmp_path / "CMakeCache.txt"
    cache.write_text("IDF_TARGET:STRING=esp32\nMM_VERSION:UNINITIALIZED=5.9.0\n")
    assert "MM_VERSION" in (stale_feature_cache(tmp_path, [], "esp32") or "")
    cache.write_text("IDF_TARGET:STRING=esp32\n")
    assert stale_feature_cache(tmp_path, [], "esp32") is None


def test_a_value_changed_inside_a_fragment_regenerates_the_sdkconfig(tmp_path, monkeypatch):
    """IDF seeds sdkconfig from the fragments once, so a changed value inside one is caught by their content, not their list."""
    monkeypatch.setattr(build_esp32, "ESP32_DIR", tmp_path)
    (tmp_path / "sdkconfig.defaults").write_text("CONFIG_A=y\n")
    (tmp_path / "sdkconfig.defaults.board").write_text("CONFIG_B=y\n")
    paths = fragment_paths(["-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.board", "-DMM_FIRMWARE_NAME=x"])
    assert paths == [tmp_path / "sdkconfig.defaults", tmp_path / "sdkconfig.defaults.board"]
    build = tmp_path / "build"
    build.mkdir()
    value = sdkconfig_fragments.digest(paths)
    assert sdkconfig_fragments.changed(build, value)       # no record: nothing says what seeded it
    sdkconfig_fragments.record(build, value)
    assert not sdkconfig_fragments.changed(build, value)
    (tmp_path / "sdkconfig.defaults").write_text("CONFIG_A=y\nCONFIG_C=y\n")
    assert sdkconfig_fragments.changed(build, sdkconfig_fragments.digest(paths))


def test_the_fragments_merge_order_is_part_of_what_they_say(tmp_path):
    """The last fragment setting an option wins, so the same files in another order are another config."""
    (tmp_path / "a").write_text("CONFIG_X=1\n")
    (tmp_path / "b").write_text("CONFIG_X=2\n")
    a, b = tmp_path / "a", tmp_path / "b"
    assert sdkconfig_fragments.digest([a, b]) != sdkconfig_fragments.digest([b, a])


def test_an_unchanged_dir_is_not_reconfigured(tmp_path, monkeypatch):
    """refresh runs idf.py only when the record disagrees, so an ordinary build stays as fast as before."""
    ran = []
    monkeypatch.setattr(sdkconfig_fragments.subprocess, "run", lambda *a, **k: ran.append(a) or type("R", (), {"returncode": 0})())
    frag = tmp_path / "sdkconfig.defaults"
    frag.write_text("CONFIG_A=y\n")
    build = tmp_path / "build"
    build.mkdir()
    (build / "sdkconfig").write_text("CONFIG_A=n\n")
    value = sdkconfig_fragments.refresh(["idf.py"], build, [frag], tmp_path, {})
    assert len(ran) == 1 and not (build / "sdkconfig").exists()   # changed: deleted and reconfigured
    sdkconfig_fragments.record(build, value)
    sdkconfig_fragments.refresh(["idf.py"], build, [frag], tmp_path, {})
    assert len(ran) == 1

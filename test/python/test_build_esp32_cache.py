"""A build directory whose cached version disagrees with this build is reconfigured, not reused."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from build_esp32 import stale_feature_cache  # noqa: E402


def test_a_plain_build_after_a_versioned_one_reconfigures(tmp_path):
    """CMake keeps a -D value a later configure omits, so a plain build would otherwise report the version an earlier build was given."""
    cache = tmp_path / "CMakeCache.txt"
    cache.write_text("IDF_TARGET:STRING=esp32\nMM_VERSION:UNINITIALIZED=5.9.0\n")
    assert "MM_VERSION" in (stale_feature_cache(tmp_path, [], "esp32") or "")
    cache.write_text("IDF_TARGET:STRING=esp32\n")
    assert stale_feature_cache(tmp_path, [], "esp32") is None

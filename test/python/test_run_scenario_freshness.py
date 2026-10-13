"""The scenario runner's freshness check reads only the sources the desktop runner is built from."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
import run_scenario  # noqa: E402


def test_an_edit_to_a_platform_the_desktop_never_links_does_not_make_the_runner_stale():
    # A Teensy edit once reported the runner stale, and rebuilding could never clear it: the desktop does not link that file.
    assert run_scenario._other_platform("src/platform/teensy/platform_teensy_ota.cpp")
    assert run_scenario._other_platform("src/platform/esp32/platform_esp32.cpp")


def test_the_desktop_platform_and_the_shared_platform_headers_still_count():
    assert not run_scenario._other_platform("src/platform/desktop/platform_desktop.cpp")
    assert not run_scenario._other_platform("src/platform/platform.h")
    assert not run_scenario._other_platform("src/core/system/HttpServerModule.cpp")

"""The ESP32 freshness check marks a firmware stale when anything it is built from changed, the UI included: the UI reaches the image as a generated header whose own inputs no compiler reads."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import check_esp32_built as check  # noqa: E402


def test_the_ui_inputs_are_the_files_the_header_is_generated_from():
    inputs = check.ui_inputs()
    assert {"src/ui/app.js", "src/ui/help.js", "src/ui/index.html", "src/ui/style.css"} <= inputs
    assert all((ROOT / f).exists() for f in inputs)


def test_a_build_reading_the_ui_header_depends_on_the_ui_files():
    deps = check.with_ui({"src/main.cpp", "src/ui/ui_embedded.h"})
    assert "src/ui/app.js" in deps


def test_a_build_without_the_ui_header_gains_nothing():
    assert check.with_ui({"src/main.cpp"}) == {"src/main.cpp"}

"""The ESP32 freshness check marks a firmware stale when anything it is built from changed, the UI included: the UI reaches the image as a generated header whose own inputs no compiler reads."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import check_esp32_built as check  # noqa: E402


def test_the_ui_inputs_are_the_files_the_header_is_bundled_from():
    inputs = check.ui_inputs()
    # A module only the bundle reads, the vendored script and the bundler itself count as much as the page.
    assert {"src/ui/app.js", "src/ui/preview3d.js", "src/ui/index.html", "src/ui/style.css",
            "src/ui/vendor/prism.js", "moondeck/build/bundle_ui.py"} <= inputs
    assert all((ROOT / f).exists() for f in inputs)


def test_a_build_reading_the_ui_header_depends_on_the_ui_files():
    deps = check.with_ui({"src/main.cpp", "src/ui/ui_embedded.h"})
    assert "src/ui/app.js" in deps


def test_the_sources_a_firmware_reads_include_the_ui_files_its_header_embeds(tmp_path, monkeypatch):
    build = tmp_path / "build" / "esp32-x"
    build.mkdir(parents=True)
    (build / ".ninja_deps").write_bytes(b"")
    ninja = type("Ran", (), {"returncode": 0, "stdout": "output: x.o\n  ../../src/main.cpp\n  ../../src/ui/ui_embedded.h\n"})()
    monkeypatch.setattr(check, "ROOT", tmp_path)
    monkeypatch.setattr(check.subprocess, "run", lambda *a, **k: ninja)
    deps = check.compiled_sources("x")
    assert "src/main.cpp" in deps and "src/ui/app.js" in deps   # app.js is embedded through the header, which ninja lists


def test_a_build_without_the_ui_header_gains_nothing():
    assert check.with_ui({"src/main.cpp"}) == {"src/main.cpp"}

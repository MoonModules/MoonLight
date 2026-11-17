"""The desktop rows of the firmware registry: one per platform package_host.py builds, projected to firmwares.json and to the install picker."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from firmwares import FIRMWARES, chip_family  # noqa: E402
from generate_firmwares import PICKER, build_doc, picker_source  # noqa: E402

DESKTOP = {name: spec for name, spec in FIRMWARES.items() if spec["platform"] == "host"}


def test_every_desktop_platform_the_release_builds_is_a_row():
    assert sorted(DESKTOP) == ["host-android-arm64", "host-linux-arm64", "host-linux-x64", "host-macos-arm64", "host-windows-x64"]


def test_each_desktop_row_matches_the_assets_its_platform_ships():
    shipped = {
        "host-macos-arm64": ["MoonLight-macos-arm64-v1.2.3.dmg", "MoonLight-macos-arm64-v1.2.3.tar.gz"],
        "host-windows-x64": ["MoonLight-windows-x64-v1.2.3-setup.exe", "MoonLight-windows-x64-v1.2.3.zip"],
        "host-linux-x64": ["moonlight_1.2.3_amd64.deb", "MoonLight-linux-x64-v1.2.3.tar.gz"],
        "host-linux-arm64": ["moonlight_1.2.3_arm64.deb", "MoonLight-linux-arm64-v1.2.3.tar.gz"],
        "host-android-arm64": ["MoonLight-android-arm64-v1.2.3.apk"],
    }
    for name, names in shipped.items():
        patterns = [re.compile(p) for p in DESKTOP[name]["assets"]]
        for asset in names:
            assert any(p.search(asset) for p in patterns), f"{name} misses {asset}"
        assert not any(p.search("moonlight_1.2.3_armhf.deb") for p in patterns)


def test_a_desktop_row_has_a_family_label_for_the_manifest_vocabulary():
    for name, spec in DESKTOP.items():
        assert chip_family(name) == spec["label"]


def test_the_picker_carries_the_registry_desktop_rows():
    picker = PICKER.read_text(encoding="utf-8")
    assert picker == picker_source(picker, build_doc()), "run generate_firmwares.py: install-picker.js HOST_BUILDS is stale"

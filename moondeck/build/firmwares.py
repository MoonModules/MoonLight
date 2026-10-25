#!/usr/bin/env python3
"""The firmware registry: every compiled binary variant, on every platform, as one dict.

Each platform's build script owns its slice, with that platform's build details beside the shared fields (`chip`, `eth_only`, `ships`, `description`, `installable`); this module joins the slices and tags each row with its `platform`, so the installer list, the release, CI and MoonDeck read one registry rather than a list per platform.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_esp32  # noqa: E402
import build_teensy  # noqa: E402


def _desktop(label: str, assets: list[str]) -> dict:
    """A desktop build, downloaded rather than flashed. `assets` are the release-asset name patterns (regexes) package_desktop.py writes, the installer's own form first."""
    return {"chip": label.lower().replace(" ", "-"), "label": label, "eth_only": False, "ships": True, "assets": assets,
            "description": f"MoonLight for {label}, downloaded and run on the computer."}


# The desktop slice: one row per platform package_desktop.py builds (the release's desktop jobs). The name is `desktop-<chip>`, the key the installer's download list carries.
_DESKTOP = {
    f"desktop-{spec['chip']}": spec
    for spec in (
        _desktop("macOS arm64", [r"^MoonLight-macos-arm64-v.+\.dmg$", r"^MoonLight-macos-arm64-v.+\.tar\.gz$"]),
        _desktop("Windows x64", [r"^MoonLight-windows-x64-v.+-setup\.exe$", r"^MoonLight-windows-x64-v.+\.zip$"]),
        _desktop("Linux x64", [r"^moonlight_.+_amd64\.deb$", r"^MoonLight-linux-x64-v.+\.tar\.gz$"]),
        _desktop("Linux arm64", [r"^moonlight_.+_arm64\.deb$", r"^MoonLight-linux-arm64-v.+\.tar\.gz$"]),
    )
}

# A platform's slice, in the order the installer lists them.
_SLICES = {"esp32": build_esp32.FIRMWARES, "teensy": build_teensy.FIRMWARES, "desktop": _DESKTOP}

FIRMWARES: dict[str, dict] = {
    name: {**spec, "platform": platform}
    for platform, slice_ in _SLICES.items()
    for name, spec in slice_.items()
}

# Every chip family the installer knows, keyed by a registry row's `chip`: the vocabulary of a manifest's `chipFamily` and a catalog entry's `chip`.
TARGET_TO_FAMILY: dict[str, str] = {**build_esp32.TARGET_TO_FAMILY, **build_teensy.TARGET_TO_FAMILY,
                                    **{spec["chip"]: spec["label"] for spec in _DESKTOP.values()}}


def chip_family(firmware: str) -> str:
    """The chip family a firmware installs onto."""
    return TARGET_TO_FAMILY[FIRMWARES[firmware]["chip"]]

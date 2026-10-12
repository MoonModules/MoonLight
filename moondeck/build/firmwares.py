#!/usr/bin/env python3
"""The firmware registry: every compiled binary variant, on every platform, as one dict.

Each platform's build script owns its slice, with that platform's build details beside the shared fields (`chip`, `eth_only`, `ships`, `description`, `installable`); this module joins the slices and tags each row with its `platform`, so the installer list, the release, CI and MoonDeck read one registry rather than a list per platform.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_esp32  # noqa: E402
import build_teensy  # noqa: E402

# A platform's slice, in the order the installer lists them.
_SLICES = {"esp32": build_esp32.FIRMWARES, "teensy": build_teensy.FIRMWARES}

FIRMWARES: dict[str, dict] = {
    name: {**spec, "platform": platform}
    for platform, slice_ in _SLICES.items()
    for name, spec in slice_.items()
}

# Every chip family the installer knows, keyed by a registry row's `chip`: the vocabulary of a manifest's `chipFamily` and a catalog entry's `chip`.
TARGET_TO_FAMILY: dict[str, str] = {**build_esp32.TARGET_TO_FAMILY, **build_teensy.TARGET_TO_FAMILY}


def chip_family(firmware: str) -> str:
    """The chip family a firmware installs onto."""
    return TARGET_TO_FAMILY[FIRMWARES[firmware]["chip"]]

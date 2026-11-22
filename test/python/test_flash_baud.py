"""flash_esp32.py baud resolution (_catalog_flash_baud).

MoonDeck / CLI flashing defaults FAST (921600 — the DIY bench assumes a modern USB
bridge). A deviceModel with a flaky bridge pins its own lower `flashBaud` in the catalog
(the LOLIN D32's CH340 → 460800). The catch: `flashBaud` keys off the deviceModel, but
several models share a firmware — so resolving by firmware alone lets one board's opt-down
leak to its siblings (the LOLIN's 460800 wrongly slowing the Dig-Uno, both on `esp32`).
Resolving by the EXACT deviceModel (which MoonDeck maps from the port) fixes that.

Pins the resolution so those two real bugs (fast default; sibling leakage) can't regress.
Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

from flash_esp32 import _catalog_flash_baud, DEFAULT_FLASH_BAUD  # noqa: E402


def test_default_is_fast():
    # The DIY-bench default is 921600 (~2x faster than the installer's 460800).
    assert DEFAULT_FLASH_BAUD == 921600


def test_exact_model_with_optdown_is_slow():
    # A board that pins a lower flashBaud (LOLIN D32's flaky CH340) gets it exactly.
    assert _catalog_flash_baud("esp32", "LOLIN D32") == 460800


def test_exact_model_without_flashbaud_gets_fast_default_not_sibling():
    # THE FIX: a known model with no flashBaud of its own gets the fast default — it must
    # NOT inherit a firmware-sibling's opt-down. The Dig-Uno shares `esp32` with the LOLIN
    # but has a fine bridge, so it flashes at 921600, not the LOLIN's 460800.
    assert _catalog_flash_baud("esp32", "QuinLED Dig-Uno V3") == 921600


def test_firmware_only_falls_back_to_lowest_sibling():
    # No deviceModel known (a plain --firmware flash): take the lowest flashBaud among
    # models sharing the firmware, so an opt-down still protects an unidentified board.
    assert _catalog_flash_baud("esp32-lite") == 460800     # LOLIN's 460800 protects
    # esp32-ethonly carries the Olimex's opt-down (its USB bridge dies at 921600), so it
    # protects too; the S3 firmwares have no opted-down sibling and stay fast.
    assert _catalog_flash_baud("esp32-ethonly") == 460800
    assert _catalog_flash_baud("esp32s3-opi") == 921600    # no sibling opt-down, fast


def test_unknown_model_falls_through_to_firmware():
    # A device_model not in the catalog behaves like no model given (firmware resolution).
    assert _catalog_flash_baud("esp32", "Nonexistent Model") == 460800


def test_unknown_firmware_is_fast_default():
    assert _catalog_flash_baud("nonexistent-fw") == DEFAULT_FLASH_BAUD
    assert _catalog_flash_baud("nonexistent-fw", "Nonexistent Model") == DEFAULT_FLASH_BAUD

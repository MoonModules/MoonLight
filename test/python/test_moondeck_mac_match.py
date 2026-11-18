"""A board's MAC as esptool or a USB serial reads it, matched to the MAC its device reports (`_usb.mac_matches`).

Most chips report their efuse MAC itself, so an exact compare works. The ESP32-S31's `esp_efuse_mac_get_default()` returns the EUI-64
encoding of the efuse MAC (FF:FE inserted after the OUI) cut to six bytes, so the device reports 30:ED:A0:FF:FE:F3 for an efuse MAC of
30:ED:A0:F3:D4:68; both forms match, or the S31 would never be found on its port nor pass the flash's MAC guard.
Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import _usb  # noqa: E402
from _usb import mac_matches as _mac_matches  # noqa: E402


def test_exact_match_case_insensitive():
    assert _mac_matches("AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff")
    assert _mac_matches("aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF")


def test_s31_eui64_truncated_derivation_matches():
    # The real S31 pair seen on the bench: esptool efuse MAC vs the device's
    # esp_efuse_mac_get_default() report. OUI (30:ED:A0) kept, FF:FE inserted,
    # then truncated to 6 bytes — so only the first post-OUI efuse byte (F3) survives.
    assert _mac_matches("30:ED:A0:F3:D4:68", "30:ED:A0:FF:FE:F3")


def test_unrelated_macs_do_not_match():
    assert not _mac_matches("30:ED:A0:F3:D4:68", "11:22:33:44:55:66")
    # Same OUI but a different chip — OUI overlap must NOT be treated as a match.
    assert not _mac_matches("30:ED:A0:F3:D4:68", "30:ED:A0:AA:BB:CC")


def test_derivation_is_directional_efuse_to_device():
    # The efuse side is what esptool or the USB serial reads; the device holds the derived one.
    # Matching is computed forward (efuse -> EUI-64-trunc), so an already-derived MAC in the efuse
    # slot matches itself and its own forward derivation, and only derives forward, as the efuse MAC cannot be rebuilt from the EUI-64 form.
    assert _mac_matches("30:ED:A0:FF:FE:F3", "30:ED:A0:FF:FE:F3")   # exact still fine
    assert _mac_matches("30:ED:A0:FF:FE:F3", "30:ED:A0:FF:FE:FF")   # the forward derivation of the slot's own bytes
    assert not _mac_matches("30:ED:A0:FF:FE:F3", "30:ED:A0:F3:D4:68")  # can't go backward


def test_empty_never_matches():
    assert not _mac_matches("", "30:ED:A0:FF:FE:F3")
    assert not _mac_matches("30:ED:A0:F3:D4:68", "")
    assert not _mac_matches(None, None)


def test_malformed_only_matches_itself_never_derives():
    # A too-short MAC can't drive the EUI-64 derivation (needs 6 octets), so it
    # only ever matches an identical string — never a derived one. Exact-match still
    # holds for identical inputs (harmless: same source), but no derivation is attempted.
    assert _mac_matches("30:ED:A0", "30:ED:A0")             # identical → exact branch
    assert not _mac_matches("30:ED:A0", "30:ED:A0:FF:FE")   # can't derive from 3 octets


def test_a_board_reports_itself_by_its_efuse_mac_or_its_eui64_form_the_map_joins_its_usb_by_either():
    assert _usb.device_macs("30:ED:A0:F3:D4:68") == ["30:ed:a0:f3:d4:68", "30:ed:a0:ff:fe:f3"]
    assert _usb.device_macs("30:ED:A0") == [] and _usb.device_macs("") == [] and _usb.device_macs(None) == []

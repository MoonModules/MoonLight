"""MoonDeck port identity — the three levels of "which board is on this port?".

The serial port PATH drifts between sessions (macOS renumbers cu.usbserial-* on
replug), so the dropdown resolves up to three levels that degrade gracefully:
  1. path  — always
  2. chip  — ESP32 family, from the USB descriptor (native-USB Espressif) or the
             registry firmware (external UART bridge)
  3. board — the specific device, from a registry match: native-USB Espressif
             chips report their MAC as the USB serial (match by MAC); external
             adapters key on the stable serial embedded in the port name.

These pin the pure resolvers (_chip_from_usb, _firmware_to_chip, _port_serial,
_resolve_port) that back describe_serial_ports — the ioreg read is the only I/O
and is mocked out of scope here. Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

from moondeck import (  # noqa: E402
    _apply_probe_results, _chip_from_usb, _firmware_to_chip, _normalize_chip,
    _parse_esptool_probe, _port_serial, _resolve_port,
)


# --- level 2: chip from the USB descriptor (native-USB Espressif only) --------

def test_chip_from_usb_pid_table_then_generic():
    # PID 0x1001 is the SHARED USB-JTAG PID (S3/C3/C6/H2/P4), so it does NOT name the chip — it
    # resolves to the generic native-USB family, and the specific chip comes from a registry match.
    assert _chip_from_usb(0x303A, 0x1001) == "esp32 (native-usb)"
    # Only 0x1002 (the S2's own PID) names a specific chip.
    assert _chip_from_usb(0x303A, 0x1002) == "esp32-s2"
    # Any other Espressif PID → still known to be native-USB Espressif, family unknown.
    assert _chip_from_usb(0x303A, 0x9999) == "esp32 (native-usb)"


def test_resolve_native_usb_p4_not_mislabeled_as_s3():
    # A P4 on native USB shares PID 0x1001 with the S3 — the descriptor must NOT guess "esp32-s3".
    # With a registry match (MAC), the P4's real chip (from firmware) wins over the generic guess.
    devs = [{"deviceName": "MM-P4", "mac": "80:F1:B2:D0:AC:F7", "ip": "192.168.1.133",
             "firmware": "esp32p4rev1-eth"}]
    usb = {"vid": 0x303A, "pid": 0x1001, "product": "USB JTAG/serial debug unit",
           "serial": "80:F1:B2:D0:AC:F7"}
    got = _resolve_port("/dev/cu.usbmodem5ABA0767221", usb, devs)
    assert got["board"] == "MM-P4"
    assert got["chip"] == "esp32-p4"      # firmware wins over the generic USB guess, NOT esp32-s3


def test_chip_from_usb_external_adapter_reveals_nothing():
    # CP2102N (SiLabs) and CH343 (WCH) are external bridges — the chip family
    # isn't in USB, so return "" and let the registry firmware fill level 2.
    assert _chip_from_usb(0x10C4, 0xEA60) == ""
    assert _chip_from_usb(0x1A86, 0x55D3) == ""


# --- level 2 fallback: chip from the registry firmware id ---------------------

def test_firmware_to_chip():
    assert _firmware_to_chip("esp32p4rev1-eth") == "esp32-p4"
    assert _firmware_to_chip("esp32s3-n8r8") == "esp32-s3"
    assert _firmware_to_chip("esp32s31") == "esp32-s3"        # S31 is an S3 variant
    assert _firmware_to_chip("esp32") == "esp32 (classic)"
    assert _firmware_to_chip("") == ""


# --- the stable per-adapter serial embedded in a macOS port name --------------

def test_port_serial_extracts_stable_key():
    assert _port_serial("/dev/cu.usbserial-20213240") == "20213240"
    assert _port_serial("/dev/cu.usbmodem5ABA0767291") == "5ABA0767291"
    assert _port_serial("/dev/cu.debug-console") == ""       # not a usb serial device


# --- level 3: full resolution against the registry ----------------------------

_DEVS = [
    {"deviceName": "MM-LC16", "mac": "10:B4:1D:E1:A5:8C", "ip": "192.168.1.107",
     "firmware": "esp32s3-n8r8", "usbSerial": "20213240"},
    {"deviceName": "MoonLight-testbench-S3", "mac": "CC:BA:97:0A:F3:F8",
     "ip": "192.168.1.159", "firmware": "esp32s3-n16r8"},   # native USB, no usbSerial
    {"deviceName": "MM-P4", "mac": "80:F1:B2:D0:AC:F7", "ip": "192.168.1.133",
     "firmware": "esp32p4rev1-eth", "usbSerial": "5ABA0767221"},
]


def test_resolve_native_usb_matches_by_mac_and_reveals_chip():
    # Native-USB Espressif: USB serial IS the MAC → level 3 with no seeding. The chip is the
    # registry firmware's (esp32s3-n16r8 → esp32-s3), which wins over the descriptor's generic
    # native-USB family (PID 0x1001 is shared across S3/P4, so it can't name the chip on its own).
    usb = {"vid": 0x303A, "pid": 0x1001, "product": "USB JTAG/serial debug unit",
           "serial": "CC:BA:97:0A:F3:F8"}
    got = _resolve_port("/dev/cu.usbmodem2021401", usb, _DEVS)
    assert got["board"] == "MoonLight-testbench-S3"
    assert got["chip"] == "esp32-s3"
    assert got["ip"] == "192.168.1.159"


def test_resolve_external_adapter_matches_by_port_serial_and_derives_chip():
    # CP2102N: chip absent from USB, board matched on the port-name serial, chip
    # then derived from that board's firmware (level-2 fallback).
    usb = {"vid": 0x10C4, "pid": 0xEA60, "product": "CP2102N USB to UART Bridge",
           "serial": "46fe21db"}   # adapter's own hex serial — NOT the key
    got = _resolve_port("/dev/cu.usbserial-20213240", usb, _DEVS)
    assert got["board"] == "MM-LC16"
    assert got["chip"] == "esp32-s3"      # from firmware esp32s3-n8r8


def test_resolve_ch343_p4_by_port_serial():
    usb = {"vid": 0x1A86, "pid": 0x55D3, "product": "USB Single Serial", "serial": "5ABA076722"}
    got = _resolve_port("/dev/cu.usbmodem5ABA0767221", usb, _DEVS)
    assert got["board"] == "MM-P4"
    assert got["chip"] == "esp32-p4"


def test_resolve_known_non_esp_is_labeled_and_not_probeable():
    # An LG monitor: a known USB vendor that isn't ESP-capable → labeled "not an
    # ESP32" (never blank) and marked non-probeable so Identify won't reset it.
    usb = {"vid": 0x043E, "pid": 0x9A39, "product": "LG Monitor Controls", "serial": "x"}
    got = _resolve_port("/dev/cu.usbmodem109NTRLN38302", usb, _DEVS)
    assert got["board"] == ""
    assert got["chip"] == ""
    assert got["note"] == "LG Monitor Controls"   # its actual USB product name
    assert got["probeable"] is False


def test_resolve_non_esp_without_product_falls_back():
    # A non-ESP vendor whose descriptor carries no product string still gets a note.
    usb = {"vid": 0x043E, "pid": 0x9A39, "product": "", "serial": "x"}
    assert _resolve_port("/dev/cu.usbmodemX", usb, _DEVS)["note"] == "not an ESP32"


def test_resolve_unknown_vendor_stays_probeable():
    # No USB descriptor (usb={}) or a UART bridge with no match yet: unknown chip,
    # unknown vendor → still worth probing (the Identify button targets these).
    got = _resolve_port("/dev/cu.usbserial-99999999", {}, _DEVS)
    assert got["chip"] == ""
    assert got["probeable"] is True
    assert got["note"] == ""
    # A CP210x bridge (ESP-capable vendor) with no registry match: probeable.
    usb = {"vid": 0x10C4, "pid": 0xEA60, "product": "CP2102N", "serial": "z"}
    assert _resolve_port("/dev/cu.usbserial-99999999", usb, _DEVS)["probeable"] is True


def test_already_labeled_esp_port_stays_probeable():
    # On a dev bench boards move between ports, so an already-resolved ESP32 board
    # is STILL probeable — a re-probe re-reads what's actually on the port and
    # corrects a label a swap may have staled. Only non-ESP vendors are excluded.
    usb = {"vid": 0x303A, "pid": 0x1001, "product": "USB JTAG/serial debug unit",
           "serial": "CC:BA:97:0A:F3:F8"}
    got = _resolve_port("/dev/cu.usbmodem2021401", usb, _DEVS)
    assert got["board"] == "MoonLight-testbench-S3"   # labeled
    assert got["probeable"] is True                   # ...and still re-probeable


def test_resolve_no_usb_descriptor_still_matches_by_port_serial():
    # ioreg unavailable (usb={}) → level 3 still reachable via the port-name serial.
    got = _resolve_port("/dev/cu.usbserial-20213240", {}, _DEVS)
    assert got["board"] == "MM-LC16"
    assert got["chip"] == "esp32-s3"


# --- level 2 by active probe: the esptool result overrides the firmware guess -

def test_resolve_prefers_probed_chip_over_firmware():
    # A board behind a UART bridge whose chip we learned by probing: the cached
    # probedChip wins over the (coarser) firmware derivation.
    devs = [{"deviceName": "MysteryBoard", "mac": "AA:BB:CC:DD:EE:FF",
             "ip": "192.168.1.5", "firmware": "esp32",   # would guess "classic"
             "usbSerial": "20213340", "probedChip": "esp32-s3"}]
    got = _resolve_port("/dev/cu.usbserial-20213340", {}, devs)
    assert got["board"] == "MysteryBoard"
    assert got["chip"] == "esp32-s3"      # probed, not the firmware "classic" guess


# --- esptool probe output parsing (the Identify button's chip read) -----------

def test_normalize_chip():
    assert _normalize_chip("ESP32-S3") == "esp32-s3"
    assert _normalize_chip("ESP32-S31") == "esp32-s3"     # S31 is an S3 variant
    assert _normalize_chip("ESP32-P4") == "esp32-p4"
    assert _normalize_chip("ESP32") == "esp32 (classic)"
    assert _normalize_chip("nonsense") == ""


def test_parse_esptool_probe():
    # Real esptool 5.x `read-mac` output captured from the S31 bench board.
    out = ("Detecting chip type... ESP32-S31\n"
           "Features:  Wi-Fi 6, BT 5.4 (LE), IEEE802.15.4\n"
           "MAC:                30:ed:a0:f3:d4:68\n"
           "MAC:                30:ed:a0:f3:d4:68\n")
    assert _parse_esptool_probe(out) == {"chip": "esp32-s3", "mac": "30:ed:a0:f3:d4:68"}


def test_apply_probe_caches_serial_and_chip():
    devs = [{"deviceName": "MM-P4", "mac": "80:F1:B2:D0:AC:F7", "firmware": "esp32p4rev1-eth"}]
    _apply_probe_results(devs, {"/dev/cu.usbmodem5ABA0767221":
                                {"chip": "esp32-p4", "mac": "80:f1:b2:d0:ac:f7"}})
    assert devs[0]["usbSerial"] == "5ABA0767221"
    assert devs[0]["probedChip"] == "esp32-p4"


def test_apply_probe_corrects_stale_serial_on_swap():
    # The adapter-reused-for-another-board case: board A's cache holds serial
    # "S", then board B is probed on the same adapter (same serial). The probe
    # is authoritative — S moves to B, and A no longer claims it.
    devs = [
        {"deviceName": "BoardA", "mac": "AA:AA:AA:AA:AA:AA", "usbSerial": "20213340"},
        {"deviceName": "BoardB", "mac": "BB:BB:BB:BB:BB:BB"},
    ]
    _apply_probe_results(devs, {"/dev/cu.usbserial-20213340":
                                {"chip": "esp32-s3", "mac": "bb:bb:bb:bb:bb:bb"}})
    assert "usbSerial" not in devs[0]              # stale claim stripped from A
    assert devs[1]["usbSerial"] == "20213340"      # moved to the board actually on the port


def test_apply_probe_ignores_unmatched_and_failed():
    devs = [{"deviceName": "X", "mac": "AA:AA:AA:AA:AA:AA"}]
    # A MAC no device reports, and a failed probe (no MAC) — both no-ops.
    _apply_probe_results(devs, {"/dev/cu.usbserial-1": {"chip": "esp32", "mac": "cc:cc:cc:cc:cc:cc"},
                                "/dev/cu.usbserial-2": {"chip": "", "mac": ""}})
    assert "usbSerial" not in devs[0]
    assert "probedChip" not in devs[0]


def test_parse_esptool_probe_alt_wording_and_failure():
    # "Chip is ESP32-P4" is the other phrasing esptool uses on some paths.
    assert _parse_esptool_probe("Chip is ESP32-P4 (revision v0.1)\nMAC: 80:f1:b2:d0:ac:f7") \
        == {"chip": "esp32-p4", "mac": "80:f1:b2:d0:ac:f7"}
    # A busy/failed port yields neither.
    assert _parse_esptool_probe("A fatal error occurred: Failed to connect") \
        == {"chip": "", "mac": ""}

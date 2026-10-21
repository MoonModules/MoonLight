"""flash_esp32.py writes last_port into moondeck.json directly (the CLI path).

`last_port` used to be set ONLY by MoonDeck's discover/refresh, which consumes the
.last_flash.json breadcrumb. So a board flashed purely from the CLI (the agent path)
never gained a last_port, and every later flash had to re-probe every serial port to
find it (the shiffy case). `_set_last_port_in_catalog` closes that gap: the flash
script now writes last_port against the flashed device's MAC directly.

Run: `uv run --with pytest pytest test/python -q`.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

import flash_esp32  # noqa: E402


def _catalog(tmp_path, devices):
    """Write a minimal moondeck.json under a fake ROOT and point flash_esp32 at it."""
    md = tmp_path / "moondeck"
    md.mkdir()
    (md / "moondeck.json").write_text(
        json.dumps({"networks": [{"name": "net", "devices": devices}]}, indent=2) + "\n",
        encoding="utf-8",
    )
    # flash_esp32 builds the path as ROOT / "moondeck" / "moondeck.json"
    flash_esp32.ROOT = tmp_path
    return md / "moondeck.json"


def _read(catalog):
    return json.loads(catalog.read_text(encoding="utf-8"))["networks"][0]["devices"]


def test_sets_last_port_on_matching_mac(tmp_path):
    catalog = _catalog(tmp_path, [{"mac": "24:58:7C:DE:79:28", "deviceName": "shiffy"}])
    flash_esp32._set_last_port_in_catalog("24:58:7C:DE:79:28", "/dev/cu.usbmodemAAA")
    assert _read(catalog)[0]["last_port"] == "/dev/cu.usbmodemAAA"


def test_mac_match_is_case_insensitive(tmp_path):
    catalog = _catalog(tmp_path, [{"mac": "aa:bb:cc:dd:ee:ff"}])
    flash_esp32._set_last_port_in_catalog("AA:BB:CC:DD:EE:FF", "/dev/port1")
    assert _read(catalog)[0]["last_port"] == "/dev/port1"


def test_strips_stale_port_from_swapped_out_board(tmp_path):
    # Two boards; the port previously linked to the OLD board. Flashing a NEW board on
    # that same physical port must move last_port, not leave two records sharing it.
    catalog = _catalog(tmp_path, [
        {"mac": "AA:AA:AA:AA:AA:AA", "last_port": "/dev/shared"},
        {"mac": "BB:BB:BB:BB:BB:BB"},
    ])
    flash_esp32._set_last_port_in_catalog("BB:BB:BB:BB:BB:BB", "/dev/shared")
    devices = _read(catalog)
    assert "last_port" not in devices[0]                 # stale link stripped
    assert devices[1]["last_port"] == "/dev/shared"       # moved to the flashed board


def test_s31_eui64_mac_alias_still_links(tmp_path):
    # The S31 case: esptool reports the raw EFUSE MAC, but the catalog holds the MAC the DEVICE
    # reports — the EUI-64-truncated form (FF:FE inserted after the OUI, cut to 6 bytes). The
    # catalog write must accept the alias via moondeck's _mac_matches, or a CLI-flashed S31
    # silently never gains a last_port (the exact regression _mac_matches exists for).
    catalog = _catalog(tmp_path, [{"mac": "30:ED:A0:FF:FE:F3", "deviceName": "MM-S31"}])
    flash_esp32._set_last_port_in_catalog("30:ED:A0:F3:D4:68", "/dev/cu.s31port")
    assert _read(catalog)[0]["last_port"] == "/dev/cu.s31port"


def test_no_mac_is_a_noop(tmp_path):
    # A flash whose MAC couldn't be parsed leaves the catalog untouched (the breadcrumb
    # path still covers the MoonDeck-GUI case).
    catalog = _catalog(tmp_path, [{"mac": "AA:BB:CC:DD:EE:FF"}])
    flash_esp32._set_last_port_in_catalog("", "/dev/port1")
    assert "last_port" not in _read(catalog)[0]


def test_unknown_mac_is_a_noop(tmp_path):
    catalog = _catalog(tmp_path, [{"mac": "AA:BB:CC:DD:EE:FF"}])
    flash_esp32._set_last_port_in_catalog("99:99:99:99:99:99", "/dev/port1")
    assert "last_port" not in _read(catalog)[0]

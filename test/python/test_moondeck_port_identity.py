"""The boards on this computer's USB ports, known by their MAC: a port is found when it is used, never stored (moondeck/_usb.py).

A native-USB Espressif board reports its MAC as its USB serial; a board behind a UART bridge is known by the adapter's serial, mapped to its
MAC by Identify in the per-computer cache; a bridge with no serial, or one two present adapters share, stays unknown. These pin the pure
resolvers; the ioreg read and esptool are the only I/O, and neither runs here. Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import _usb  # noqa: E402

NATIVE_S3 = {"vid": 0x303A, "pid": 0x1001, "product": "USB JTAG/serial debug unit", "serial": "CC:BA:97:0A:F3:F8"}
CP2102N = {"vid": 0x10C4, "pid": 0xEA60, "product": "CP2102N USB to UART Bridge", "serial": "46fe21db"}
LG_MONITOR = {"vid": 0x043E, "pid": 0x9A39, "product": "LG Monitor Controls", "serial": "x"}


# --- the chip from the USB descriptor, and from a firmware id ----------------------------------------------------------------------

def test_the_shared_native_usb_pid_names_no_chip_and_a_bridge_names_none_at_all():
    assert _usb.chip_from_usb(0x303A, 0x1001) == _usb.NATIVE_USB   # S3, C3, C6, H2 and P4 share it
    assert _usb.chip_from_usb(0x303A, 0x1002) == "esp32-s2"        # the S2's own PID
    assert _usb.chip_from_usb(0x10C4, 0xEA60) == "" and _usb.chip_from_usb(0x1A86, 0x55D3) == ""


def test_a_firmware_names_its_chip_family():
    assert _usb.firmware_to_chip("esp32p4rev1-eth") == "esp32-p4"
    assert _usb.firmware_to_chip("esp32s3-n8r8") == "esp32-s3"
    assert _usb.firmware_to_chip("esp32s31") == "esp32-s31"
    assert _usb.firmware_to_chip("esp32") == "esp32 (classic)"
    assert _usb.firmware_to_chip("") == ""


def test_a_port_name_carries_its_adapters_serial():
    assert _usb.port_serial("/dev/cu.usbserial-20213240") == "20213240"
    assert _usb.port_serial("/dev/cu.usbmodem5ABA0767291") == "5ABA0767291"
    assert _usb.port_serial("/dev/cu.debug-console") == ""


# --- the port for a MAC: native serial, cached adapter serial, unknown -------------------------------------------------------------

def test_a_native_usb_board_is_found_by_the_mac_its_usb_serial_reports():
    ports = {"/dev/cu.usbmodem2021401": NATIVE_S3}
    assert _usb.port_for_mac("cc:ba:97:0a:f3:f8", ports, {}) == "/dev/cu.usbmodem2021401"
    assert _usb.port_mac("/dev/tty.usbmodem2021401", ports, {}) == "cc:ba:97:0a:f3:f8"   # its tty name alike


def test_a_board_behind_a_bridge_is_found_by_its_adapters_cached_serial():
    cache = {"20213240": {"mac": "10:b4:1d:e1:a5:8c", "chip": "esp32-s3"}}   # macOS names the port after the adapter's serial, on any USB socket
    assert _usb.port_for_mac("10:B4:1D:E1:A5:8C", {"/dev/cu.usbserial-20213240": CP2102N}, cache) == "/dev/cu.usbserial-20213240"
    ident = _usb.port_identities({"/dev/cu.usbserial-20213240": CP2102N}, cache)[0]
    assert ident["chip"] == "esp32-s3" and ident["mac"] == "10:b4:1d:e1:a5:8c"


def test_a_bridge_without_a_cached_serial_is_unknown_until_identify():
    ports = {"/dev/cu.usbserial-99999999": CP2102N}
    assert _usb.port_for_mac("10:b4:1d:e1:a5:8c", ports, {}) == ""
    assert _usb.port_identities(ports, {})[0]["mac"] == ""


def test_a_serial_two_present_adapters_share_gives_neither_the_cached_mac():
    # Cheap CP2102 boards often all report serial 0001: which board sits behind which is then unknowable without a probe.
    ports = {"/dev/cu.usbserialA": {**CP2102N, "serial": "0001"}, "/dev/cu.usbserialB": {**CP2102N, "serial": "0001"}}
    assert _usb.port_for_mac("10:b4:1d:e1:a5:8c", ports, {"0001": {"mac": "10:b4:1d:e1:a5:8c"}}) == ""


def test_the_s31_is_found_by_the_mac_its_device_reports():
    ports = {"/dev/cu.usbmodem1": {**NATIVE_S3, "serial": "30:ED:A0:F3:D4:68"}}   # the efuse MAC
    assert _usb.port_for_mac("30:ed:a0:ff:fe:f3", ports, {}) == "/dev/cu.usbmodem1"   # its EUI-64 form


def test_a_known_vendor_that_fronts_no_esp32_is_labeled_and_never_probed_nor_a_board():
    ports = {"/dev/cu.usbmodem109NTRLN38302": LG_MONITOR, "/dev/cu.usbmodemX": {**LG_MONITOR, "product": ""}}
    idents = {i["path"]: i for i in _usb.port_identities(ports, {})}
    assert idents["/dev/cu.usbmodem109NTRLN38302"]["note"] == "LG Monitor Controls"
    assert idents["/dev/cu.usbmodemX"]["note"] == "not an ESP32"
    assert not any(i["probeable"] for i in idents.values())
    assert _usb.usb_boards(ports, {}) == []


def test_a_port_with_no_descriptor_stays_probeable_but_is_no_map_board():
    ports = {"/dev/cu.usbserial-99999999": {}}
    assert _usb.port_identities(ports, {})[0]["probeable"] is True
    assert _usb.usb_boards(ports, {}) == []


def test_the_maps_usb_rows_carry_port_chip_mac_and_the_macs_the_board_reports_itself_by():
    rows = _usb.usb_boards({"/dev/cu.usbmodem2021401": NATIVE_S3, "/dev/cu.usbserial-1": CP2102N}, {})
    assert {"name": "esp32 (native-usb) on usbmodem2021401", "usb": "/dev/cu.usbmodem2021401", "type": "USB",
            "chip": _usb.NATIVE_USB, "mac": "cc:ba:97:0a:f3:f8", "macs": ["cc:ba:97:0a:f3:f8", "cc:ba:97:ff:fe:0a"]} in rows
    assert {"name": "USB on usbserial-1", "usb": "/dev/cu.usbserial-1", "type": "USB"} in rows


def test_the_present_ports_read_ioreg_again_only_when_the_listed_ports_change_or_the_cache_expires(monkeypatch):
    listed, reads, clock = ["/dev/cu.usbserial-1"], [], [1000.0]
    monkeypatch.setattr(_usb, "_ioreg", {"listed": None, "at": 0.0, "usb": {}})
    monkeypatch.setattr(_usb.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(_usb, "list_serial_ports", lambda: list(listed))
    monkeypatch.setattr(_usb, "read_usb_ports", lambda: reads.append(1) or dict.fromkeys(listed, CP2102N))
    assert _usb.present_ports() == {"/dev/cu.usbserial-1": CP2102N}
    assert len(reads) == 1
    clock[0] += _usb.IOREG_MAX_AGE_S - 1
    _usb.present_ports()
    assert len(reads) == 1   # the map's next ask before expiry: the same ports, no dump
    clock[0] += 2
    _usb.present_ports()
    assert len(reads) == 2   # expired: read again
    listed.append("/dev/cu.usbserial-2")
    assert set(_usb.present_ports()) == {"/dev/cu.usbserial-1", "/dev/cu.usbserial-2"}
    assert len(reads) == 3   # a changed port list reads at once


def test_a_native_usb_board_reports_its_mac_and_an_adapters_serial_is_its_own():
    assert _usb.usb_mac("80:B5:4E:C6:3C:B4") == "80:b5:4e:c6:3c:b4"
    assert _usb.usb_mac("80B54EC63CB4") == "80:b5:4e:c6:3c:b4"
    assert _usb.usb_mac("5ABA0767291") == "" and _usb.usb_mac("") == ""


# --- Identify: esptool's answer, and the cache it writes ---------------------------------------------------------------------------

def test_esptool_names_the_chip_in_the_builds_family_style():
    assert _usb.normalize_chip("ESP32-S3") == "esp32-s3"
    assert _usb.normalize_chip("ESP32-S31") == "esp32-s31"
    assert _usb.normalize_chip("ESP32-P4") == "esp32-p4"
    assert _usb.normalize_chip("ESP32") == "esp32 (classic)"
    assert _usb.normalize_chip("nonsense") == ""


def test_esptools_read_mac_output_gives_chip_and_mac():
    out = ("Detecting chip type... ESP32-S31\n"   # real esptool 5.x output from the S31 bench board
           "Features:  Wi-Fi 6, BT 5.4 (LE), IEEE802.15.4\n"
           "MAC:                30:ed:a0:f3:d4:68\n")
    assert _usb.parse_esptool_probe(out) == {"chip": "esp32-s31", "mac": "30:ed:a0:f3:d4:68"}
    assert _usb.parse_esptool_probe("Chip is ESP32-P4 (revision v0.1)\nMAC: 80:f1:b2:d0:ac:f7") == {"chip": "esp32-p4", "mac": "80:f1:b2:d0:ac:f7"}
    assert _usb.parse_esptool_probe("A fatal error occurred: Failed to connect") == {"chip": "", "mac": ""}


def test_identify_maps_the_adapter_to_the_board_and_takes_the_mac_from_any_other_adapter():
    cache = {"OLD": {"mac": "bb:bb:bb:bb:bb:bb"}, "KEEP": {"mac": "cc:cc:cc:cc:cc:cc"}}
    assert _usb.remember_probe(cache, "20213340", {"chip": "esp32-s3", "mac": "bb:bb:bb:bb:bb:bb"})
    assert cache == {"20213340": {"mac": "bb:bb:bb:bb:bb:bb", "chip": "esp32-s3"}, "KEEP": {"mac": "cc:cc:cc:cc:cc:cc"}}


def test_a_failed_probe_or_a_port_without_a_serial_leaves_the_cache_alone():
    cache = {}
    assert not _usb.remember_probe(cache, "20213340", {"chip": "", "mac": ""})
    assert not _usb.remember_probe(cache, "", {"chip": "esp32", "mac": "aa:aa:aa:aa:aa:aa"})
    assert cache == {}


def test_a_board_identified_once_is_found_by_mac_on_the_next_read():
    cache, path = {}, "/dev/cu.usbserial-20213340"
    _usb.remember_probe(cache, _usb.usb_key(path, CP2102N), {"chip": "esp32-s3", "mac": "bb:bb:bb:bb:bb:bb"})
    assert _usb.port_for_mac("bb:bb:bb:bb:bb:bb", {path: CP2102N}, cache) == path


def test_an_exact_mac_wins_over_another_ports_eui64_alias():
    s31 = {"vid": 0x303A, "pid": 0x1001, "product": "", "serial": "30:ED:A0:F3:D4:68"}
    other = {"vid": 0x303A, "pid": 0x1001, "product": "", "serial": "30:ED:A0:FF:FE:F3"}
    ports = {"/dev/cu.usbmodem1": s31, "/dev/cu.usbmodem2": other}   # port 1's alias is port 2's exact MAC
    assert _usb.port_for_mac("30:ed:a0:ff:fe:f3", ports, {}) == "/dev/cu.usbmodem2"
    assert _usb.port_for_mac("30:ed:a0:f3:d4:68", ports, {}) == "/dev/cu.usbmodem1"


def test_a_bridge_without_a_serial_never_takes_the_hubs_serial_from_ioreg(monkeypatch):
    class R:
        stdout = b"""+-o hub  <class IOUSBHostDevice>
  | "idVendor" = 1507
  | "idProduct" = 1
  | "USB Serial Number" = "HUBSERIAL"
  +-o bridge  <class IOUSBHostDevice>
    | "idVendor" = 4292
    | "idProduct" = 60000
    | "USB Product Name" = "CP2102N"
    +-o AppleUSBHostLegacyClient
      +-o IOSerialBSDClient
        | "IOCalloutDevice" = "/dev/cu.usbserial-x"
"""
    monkeypatch.setattr(_usb.sys, "platform", "darwin")
    monkeypatch.setattr(_usb.subprocess, "run", lambda *a, **k: R())
    assert _usb.read_usb_ports() == {"/dev/cu.usbserial-x": {"vid": 4292, "pid": 60000, "product": "CP2102N", "serial": ""}}


def test_linux_usb_ports_are_read_from_sysfs_by_their_usb_device_not_a_parent(tmp_path):
    usb = tmp_path / "devices" / "usb1" / "1-1"
    (usb / "1-1:1.0" / "tty" / "ttyACM0").mkdir(parents=True)
    for name, value in (("idVendor", "303a"), ("idProduct", "1001"), ("product", "USB JTAG/serial debug unit"), ("serial", "CC:BA:97:0A:F3:F8")):
        (usb / name).write_text(value + "\n")
    (tmp_path / "devices" / "usb1" / "serial").write_text("HUBSERIAL\n")
    tty = tmp_path / "class" / "tty" / "ttyACM0"
    tty.mkdir(parents=True)
    (tty / "device").symlink_to(usb / "1-1:1.0")
    bare = tmp_path / "class" / "tty" / "ttyS0"   # a built-in UART: no USB parent
    bare.mkdir()
    (bare / "device").symlink_to(tmp_path / "devices" / "usb1")
    assert _usb.read_sysfs_usb_ports(str(tmp_path / "class" / "tty")) == {
        "/dev/ttyACM0": {"vid": 0x303A, "pid": 0x1001, "product": "USB JTAG/serial debug unit", "serial": "CC:BA:97:0A:F3:F8"}}


def test_concurrent_port_refreshes_run_one_at_a_time(monkeypatch):
    import threading
    monkeypatch.setattr(_usb, "_ioreg", {"listed": None, "at": 0.0, "usb": {}})
    inside, overlap = [], []

    def listing():
        inside.append(1)
        overlap.append(len(inside) > 1)
        time.sleep(0.05)
        inside.pop()
        return []
    monkeypatch.setattr(_usb, "list_serial_ports", listing)
    monkeypatch.setattr(_usb, "read_usb_ports", lambda: {})
    threads = [threading.Thread(target=_usb.present_ports) for _ in range(3)]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert not any(overlap)

"""The boards on this computer's USB ports, each known by its MAC: the port is found when it is used, never stored.

A port path is the computer's name for a connection: two computers name one board differently, and a replug renames it. The MAC is the
board's own. A native-USB Espressif board (S3, C3, C6, the P4's USB-JTAG) reports it as its USB serial number, read from the descriptor
the OS already enumerated, without opening the port, so without a reset. A board behind a UART bridge (CP210x, CH343) shows only the
adapter, known by the adapter's serial, which survives a replug; one esptool probe (Identify, which resets the board) maps that serial to
the board's MAC, cached per computer in moondeck.json's `usbMacs`. A bridge with no serial of its own, or one sharing its serial with
another present adapter, stays unknown until Identify. The flash keeps its guard: esptool reads the MAC before writing and stops on a
stranger, so a cache a board swap made stale costs a refused flash, never a wrong one.

stdlib only, so MoonDeck, the flash script and the run scripts import it without the server.
"""

import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from collections import Counter

# Espressif chips with native USB advertise VID 0x303A. PID 0x1001 is the USB-Serial-JTAG PID the S3, C3, C6, H2 and P4 share, so it names
# no chip and its product string is always the generic "USB JTAG/serial debug unit"; only 0x1002, the S2's own PID, names one.
ESPRESSIF_VID = 0x303A
_ESP_PID_CHIP = {0x1002: "esp32-s2"}
NATIVE_USB = "esp32 (native-usb)"

# The vendors that could front an ESP32: Espressif native USB and the bridges ESP32 boards ship. A port of any other known vendor (a monitor's
# control channel, a dongle) is no ESP32: it is labeled by its own product name and never probed.
ESP_CAPABLE_VIDS = frozenset({
    ESPRESSIF_VID,   # Espressif native USB
    0x10C4,          # Silicon Labs CP210x
    0x1A86,          # WCH CH340 / CH343
    0x0403,          # FTDI
    0x067B,          # Prolific PL2303
})


def list_serial_ports() -> list[str]:
    """The serial ports present: the conventional /dev device files on POSIX (macOS's call-out cu.* nodes, which flashing uses), and on
    Windows the SERIALCOMM table the OS keeps of present COM ports, read through winreg, so no dependency."""
    import glob
    ports = glob.glob("/dev/cu.usb*") + glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        _, port, _ = winreg.EnumValue(key, i)
                        ports.append(port)
                        i += 1
                    except OSError:
                        break
        except FileNotFoundError:
            pass   # no SERIALCOMM key: no ports present
    return sorted(ports)


def read_sysfs_usb_ports(root: str = "/sys/class/tty") -> dict:
    """Linux: sysfs parsed into {port_path: {vid, pid, product, serial}} for every USB tty, read from files, so no port opens. The tty's `device` link is
    its USB interface and that link's parent the USB device holding idVendor, idProduct, product and serial; a tty with no such parent
    (a built-in UART) is left out, and a device that reports no serial reads "" rather than a hub's. `root` is a parameter so a fake tree tests it."""
    def attr(node: Path, name: str) -> str:
        try:
            return (node / name).read_text(errors="replace").strip()
        except OSError:
            return ""
    result: dict = {}
    for tty in sorted(Path(root).glob("tty*")):
        node = tty / "device" / ".."
        vid, pid = attr(node, "idVendor"), attr(node, "idProduct")
        if vid and pid:
            result[f"/dev/{tty.name}"] = {"vid": int(vid, 16), "pid": int(pid, 16), "product": attr(node, "product"), "serial": attr(node, "serial")}
    return result


def read_usb_ports() -> dict:
    """{port_path: {vid, pid, product, serial}} for every USB serial device, read from the system rather than the port: macOS from `ioreg`, Linux from sysfs.
    {} elsewhere (Windows lists its COM ports by name alone) or when the source is unavailable, and callers degrade to the path alone."""
    if sys.platform.startswith("linux"):
        return read_sysfs_usb_ports()
    if sys.platform != "darwin":
        return {}
    try:
        # Bytes, then a lenient decode: `ioreg -l` dumps raw device data that is no text, and the parse reads ASCII keys only.
        out = subprocess.run(["ioreg", "-l", "-w0"], capture_output=True, timeout=5).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        return {}
    # The registry is a tree: a USB device node holds the descriptor, and a child IOSerialBSDClient node the /dev path. They sit in different
    # `+-o` blocks, so a stack of nodes by the indent of their marker pairs each call-out device with its nearest ancestors' fields.
    stack: list = []
    cur: dict | None = None
    result: dict = {}
    fields = (("idVendor", "vid", int), ("idProduct", "pid", int),
              ("USB Product Name", "product", str), ("USB Serial Number", "serial", str))
    for ln in out.splitlines():
        d = ln.find("+-o")
        if d >= 0:
            while stack and stack[-1][0] >= d:
                stack.pop()
            cur = {"vid": 0, "pid": 0, "product": "", "serial": ""}
            stack.append((d, cur))
        if cur is not None:
            for tag, key, cast in fields:
                m = re.search(rf'"{tag}" = (?:"([^"]*)"|(\d+))', ln)
                if m:
                    cur[key] = cast(m.group(1) if m.group(1) is not None else m.group(2))
        mc = re.search(r'"IOCalloutDevice" = "([^"]+)"', ln)
        if mc:
            # The USB device node, the nearest ancestor holding a vendor id, supplies every field: a hub above it has a serial of its own,
            # which is no key for this device.
            result[mc.group(1)] = dict(next((node for _, node in reversed(stack) if node["vid"]), {"vid": 0, "pid": 0, "product": "", "serial": ""}))
    return result


def chip_from_usb(vid: int, pid: int) -> str:
    """The ESP32 family a native-USB Espressif descriptor names, NATIVE_USB when its PID names none, "" for a bridge, whose descriptor is
    the adapter's. Pure."""
    if vid != ESPRESSIF_VID:
        return ""
    return _ESP_PID_CHIP.get(pid, NATIVE_USB)


def usb_mac(serial: str) -> str:
    """The MAC a native-USB Espressif board reports as its USB serial ("80:B5:4E:C6:3C:B4" or "80B54EC63CB4"), lower-case and
    colon-separated, or "" for an adapter's own serial. Pure."""
    hexes = re.sub(r"[^0-9a-fA-F]", "", serial or "")
    return ":".join(hexes[i:i + 2] for i in range(0, 12, 2)).lower() if len(hexes) == 12 and len(serial) in (12, 17) else ""


def port_serial(path: str) -> str:
    """The adapter's serial in a macOS port name: /dev/cu.usbserial-20213240 gives '20213240', /dev/cu.usbmodem5ABA0767291 gives
    '5ABA0767291'; "" when the name carries none. Pure."""
    m = re.search(r"usb(?:serial-|modem)(.+)$", path)
    return m.group(1) if m else ""


def usb_key(path: str, usb: dict) -> str:
    """What this computer knows a port's board by across replugs: the adapter's serial in the port name, else its USB serial; "" for none."""
    return port_serial(path) or (usb or {}).get("serial", "")


def normalize_chip(detected: str) -> str:
    """esptool's chip name ('ESP32-S3', 'ESP32-S31', 'ESP32') as the family the builds use ('esp32-s3', 'esp32-s31', 'esp32 (classic)'),
    the S31 its own family as build_esp32's TARGET_TO_FAMILY has it. Pure."""
    d = (detected or "").strip().lower()
    if not d.startswith("esp32"):
        return ""
    for fam in ("esp32-p4", "esp32-s31", "esp32-s3", "esp32-s2", "esp32-c6", "esp32-c5", "esp32-c3", "esp32-h2"):
        if d.startswith(fam):
            return fam
    return "esp32 (classic)"


def firmware_to_chip(firmware: str) -> str:
    """The ESP32 family of a firmware id: 'esp32s3-n8r8' is 'esp32-s3', 'esp32p4rev1-eth' 'esp32-p4', 'esp32' 'esp32 (classic)'. Pure."""
    key = re.sub(r"rev\d+$", "", (firmware or "").lower().split("-")[0])   # esp32p4 from esp32p4rev1-eth, as the picker's firmwareChip
    if not key.startswith("esp32"):
        return ""
    return normalize_chip("ESP32" + (f"-{key[5:].upper()}" if key[5:] else ""))


def device_macs(flash_mac: str) -> list[str]:
    """The MACs a board whose raw efuse MAC esptool or a USB serial reads as `flash_mac` may report itself by, lowercase; [] for none. Pure.

    Most chips report the efuse MAC itself. The ESP32-S31's esp_efuse_mac_get_default() returns the EUI-64 encoding of it, FF:FE inserted
    after the OUI, cut back to six bytes: 30:ED:A0:F3:D4:68 reads as 30:ED:A0:FF:FE:F3, and the device reports that everywhere. Both forms
    are listed, the second computed forward from the efuse MAC, since the reverse lost the dropped bytes.
    """
    b = (flash_mac or "").strip().lower().split(":")
    return [":".join(b), ":".join([b[0], b[1], b[2], "ff", "fe", b[3]])] if len(b) == 6 else []


def mac_matches(flash_mac: str, device_mac: str) -> bool:
    """Whether the MAC esptool or a USB serial reads (the raw efuse MAC) is the board a device reports as `device_mac` (device_macs)."""
    flash_mac, device_mac = (flash_mac or "").strip().lower(), (device_mac or "").strip().lower()
    return bool(device_mac) and (device_mac == flash_mac or device_mac in device_macs(flash_mac))


def parse_esptool_probe(text: str) -> dict:
    """{chip, mac} from esptool's `read-mac` output, the chip as normalize_chip names it; "" for what the output lacks. Pure."""
    chip = mac = ""
    m = re.search(r"Detecting chip type\.\.\.\s*(ESP32\S*)", text, re.I) or re.search(r"Chip is\s+(ESP32\S*)", text, re.I)
    if m:
        chip = normalize_chip(m.group(1))
    mm = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", text)
    if mm:
        mac = mm.group(1).lower()
    return {"chip": chip, "mac": mac}


def probe_port_chip(path: str) -> dict:
    """The chip and MAC of the board on `path`, read by esptool, which RESETS the board; "" fields for a busy port, no chip, or no esptool.
    esptool comes through `uv run --with esptool`, so it needs no IDF toolchain."""
    try:
        out = subprocess.run(["uv", "run", "--with", "esptool", "python", "-m", "esptool",
                              "--port", path, "--before", "default-reset", "read-mac"],
                             capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return {"chip": "", "mac": ""}
    return parse_esptool_probe(out.stdout + out.stderr)


def port_identities(ports: dict, cache: dict) -> list[dict]:
    """Each port of `ports` ({path: USB descriptor}) as {path, key, chip, mac, probeable, note}: the MAC its native USB serial reports or
    the cache maps its adapter's serial to, and the chip the cache's probe or the descriptor names. A serial two present ports share is
    ambiguous, so neither port takes a cached MAC from it. `probeable` is false for a known vendor that fronts no ESP32. Pure."""
    keys = {path: usb_key(path, usb) for path, usb in ports.items()}
    shared = Counter(k for k in keys.values() if k)
    out = []
    for path in sorted(ports):
        usb = ports[path] or {}
        vid = usb.get("vid", 0)
        key = keys[path]
        known = cache.get(key) if key and shared[key] == 1 and isinstance(cache.get(key), dict) else {}
        mac = (usb_mac(usb.get("serial", "")) if vid == ESPRESSIF_VID else "") or known.get("mac", "")
        known_not_esp = bool(vid) and vid not in ESP_CAPABLE_VIDS
        out.append({"path": path, "key": key, "chip": known.get("chip") or chip_from_usb(vid, usb.get("pid", 0)), "mac": mac,
                    "probeable": not known_not_esp,
                    "note": (usb.get("product", "").strip() or "not an ESP32") if known_not_esp else ""})
    return out


# The last ioreg read, kept while the listed ports stay the same and for at most IOREG_MAX_AGE_S: one dump costs about 0.3 s of a core, and
# the Live tab's map asks every two seconds.
IOREG_MAX_AGE_S = 30
_ioreg = {"listed": None, "at": 0.0, "usb": {}}
_ioreg_lock = threading.Lock()   # list and refresh as one step, so the newest read is the one kept, a slow older read included


def present_ports() -> dict:
    """Every serial port present, {path: its USB descriptor, {} where none is read}: the listed ports and the ones ioreg names."""
    with _ioreg_lock:
        listed = frozenset(list_serial_ports())
        if listed != _ioreg["listed"] or time.monotonic() - _ioreg["at"] > IOREG_MAX_AGE_S:
            _ioreg.update(listed=listed, at=time.monotonic(), usb=read_usb_ports())
        usb = _ioreg["usb"]
    return {path: usb.get(path, {}) for path in listed | set(usb)}


def usb_boards(ports: dict, cache: dict) -> list[dict]:
    """The boards on `ports` that could be an ESP32, as map rows: each its port, its chip where known, and its MAC and the MACs it may report
    itself by where known. Pure."""
    rows = []
    for ident in port_identities(ports, cache):
        if (ports[ident["path"]] or {}).get("vid", 0) not in ESP_CAPABLE_VIDS:   # no descriptor (Bluetooth, the debug console) or another vendor's
            continue
        row = {"name": f"{ident['chip'] or 'USB'} on {ident['path'].removeprefix('/dev/cu.')}", "usb": ident["path"], "type": "USB"}
        if ident["chip"]:
            row["chip"] = ident["chip"]
        if ident["mac"]:
            row["mac"] = ident["mac"]
            row["macs"] = device_macs(ident["mac"])   # what the board's network row may carry, which the map joins it by
        rows.append(row)
    return rows


def port_for_mac(mac: str, ports: dict, cache: dict) -> str:
    """The port the board with `mac` sits on right now, "" when no present port is known to hold it; a port whose MAC is exactly `mac` wins
    over one that only reports it in its EUI-64 form. Pure."""
    idents = port_identities(ports, cache)
    want = (mac or "").strip().lower()
    return next((i["path"] for i in idents if i["mac"].lower() == want), "") or next((i["path"] for i in idents if mac_matches(i["mac"], mac)), "")


def port_mac(port: str, ports: dict, cache: dict) -> str:
    """The MAC of the board on `port`, its tty or cu name alike; "" when unknown. Pure."""
    want = (port or "").replace("/dev/tty.", "/dev/cu.", 1)
    return next((i["mac"] for i in port_identities(ports, cache) if i["path"].replace("/dev/tty.", "/dev/cu.", 1) == want), "")


def remember_probe(cache: dict, key: str, probe: dict) -> bool:
    """Write an Identify probe into the cache, in place: the adapter `key` maps to the probed MAC and chip, and no other adapter keeps that
    MAC, since a board sits behind one adapter at a time. False, the cache untouched, for a probe without a MAC or a port without a key."""
    if not key or not probe.get("mac"):
        return False
    for other in [k for k, v in cache.items() if k != key and isinstance(v, dict) and mac_matches(probe["mac"], v.get("mac", ""))]:
        del cache[other]
    cache[key] = {"mac": probe["mac"], **({"chip": probe["chip"]} if probe.get("chip") else {})}
    return True

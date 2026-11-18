"""Shared moondeck.json helpers used across the check/ and run/ scripts.

Kept dependency-free (stdlib only) so a PEP-723 script can import it after adding
moondeck/ to sys.path, without threading extra `--with` deps. Mirrors the shared
`_net_probe.py` pattern in scenario/, one level up so both check/ and run/ reach it.
"""

import json
import threading
import urllib.request
from contextlib import contextmanager
from pathlib import Path

import _presence
import _usb

_ROOT = Path(__file__).resolve().parent.parent
_STATE = _ROOT / "moondeck" / "moondeck.json"

# logLevel option indices, matching SystemModule's addSelect order
# (None, Error, Warn, Info, Debug, Verbose).
LOG_NONE, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_VERBOSE = range(6)


def saved_state():
    """moondeck.json as the page shows it, an older file's shape carried over by `migrate_state`, {} when it is absent, unreadable or no
    object. A read leaves the file as it is."""
    try:
        state = json.loads(_STATE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return migrate_state(state) if isinstance(state, dict) else {}


# The keys of an older moondeck.json that load drops: the device registry a subnet scan filled, and what only that scan and its table read.
_DROPPED_KEYS = ("devices", "env", "onlineOnly", "_scanned_subnet", "_found_count")


def migrate_state(state: dict) -> dict:
    """An older moondeck.json in the shape MoonDeck reads, in place; a file in that shape passes through unchanged.

    Each network keeps its name, subnet, WiFi credentials and port. Its device list goes: a device ticked `selected` becomes a MAC in
    `liveTests`, and one whose adapter serial an Identify recorded (`usbSerial`, `probedChip`) an entry in `usbMacs`. A file from before
    networks, its devices and port at the top level, becomes one network named Home holding that port.
    """
    if "networks" not in state and ("devices" in state or "port" in state):
        state["networks"] = [{"name": "Home", "subnet": "", "wifi": {"ssid": "", "password": ""}, "port": state.pop("port", "")}]
        state["active_network"] = "Home"
    devices = (state.get("devices") or []) + [d for n in state.get("networks") or [] for d in n.pop("devices", None) or []]
    for d in devices:
        mac = (d.get("mac") or "").strip().lower()
        if mac and d.get("selected") and mac not in state.setdefault("liveTests", []):
            state["liveTests"].append(mac)
        if mac and d.get("usbSerial"):
            _usb.remember_probe(state.setdefault("usbMacs", {}), d["usbSerial"], {"mac": mac, "chip": d.get("probedChip", "")})
    for key in _DROPPED_KEYS:
        state.pop(key, None)
    return state


def selected_live_tests():
    """The MACs selected for the live tests in moondeck.json, in the order they were selected."""
    return saved_state().get("liveTests") or []


def device_ips_for_port(port):
    """The address of the board on `port`, by the MAC its USB serial or this computer's adapter cache gives it, among the devices a running
    MoonDeck hears and the Discovery lists they lead to, as a one-element list, so a tool watching one board changes only that board; []
    when the board or its address is unknown."""
    mac = _usb.port_mac(port, _usb.present_ports(), saved_state().get("usbMacs") or {})
    if not mac:
        return []
    found = _presence.resolve([mac], _presence.known_rows(_presence.heard_from_moondeck(), want=[mac]))
    return [found[mac]["host"]] if mac in found else []


def live_test_devices():
    """The devices selected for the live tests in MoonDeck's map popup, stored by MAC in moondeck.json's `liveTests`, each as {ip, deviceName,
    mac} at the address it answers on now; a selected MAC that no device MoonDeck hears or a Discovery list names is reported and left out."""
    macs = selected_live_tests()
    found = _presence.resolve(macs, _presence.known_rows(_presence.heard_from_moondeck(), want=macs)) if macs else {}
    for mac in macs:
        if mac not in found:
            print(f"  WARN  {mac} is selected for the live tests but no device answers with it: is MoonDeck running, the device on?")
    return [{"ip": d["host"], "deviceName": d["name"], "mac": d["mac"]} for d in found.values()]


def open_serial(port, baud=115200, timeout=1):
    """Open a serial port with DTR and RTS cleared first: on a native USB-Serial-JTAG port they drive the chip's reset and boot pins, and pyserial's default of asserting them holds the chip in reset."""
    import serial   # here, so this module stays importable without pyserial
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, baud, timeout
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def _get_log_level(ip):
    """Read one device's current System.logLevel index from /api/state, or None if unreachable
    or unparseable. Used to snapshot a device before a temporary change so it can be restored."""
    try:
        with urllib.request.urlopen(f"http://{ip}/api/state", timeout=3) as r:
            state = json.loads(r.read().decode("utf-8", errors="replace"))
    except Exception:
        return None

    def walk(node):
        if isinstance(node, dict):
            if node.get("type") == "SystemModule":
                for c in node.get("controls", []):
                    if c.get("name") == "logLevel":
                        v = c.get("value")
                        return v if isinstance(v, int) else None
            for v in node.values():
                found = walk(v)
                if found is not None:
                    return found
        elif isinstance(node, list):
            for v in node:
                found = walk(v)
                if found is not None:
                    return found
        return None

    return walk(state)


def set_log_level(ips, index):
    """POST System.logLevel=<index> to each device IP. The value is a numeric JSON index
    (a quoted value parses to 0). Best-effort per device: a stale/absent entry is skipped,
    never raised, so it can't fail the caller's real work (a KPI capture, a monitor session)."""
    body = json.dumps({"module": "System", "control": "logLevel", "value": index}).encode("utf-8")
    for ip in ips:
        try:
            # Request() is inside the try too: a malformed IP makes its construction raise, and that
            # must skip only this device — not abort the loop and strand the rest (which, under
            # raised_log_level, would also skip the restore of every device after it).
            req = urllib.request.Request(f"http://{ip}/api/control", data=body,
                                         headers={"Content-Type": "application/json"}, method="POST")
            urllib.request.urlopen(req, timeout=3).read()
        except Exception:
            pass


@contextmanager
def raised_log_level(ips, index=LOG_INFO, background=False):
    """Temporarily raise each device (`ips`: addresses, or a function returning them) to `index` (default Info, so the serial tick line prints) and restore each device's ORIGINAL level on exit, so a device set to Debug or Error keeps its choice.

    A device whose level can't be read is still raised, and restored to Warn, the resting default. `background` raises from a thread, so a caller reading a port starts at once rather than after the HTTP calls, 3 s each for a device that is off. Best-effort throughout; never raises.
    """
    saved = {}
    stop = threading.Event()

    def raise_all():
        # In the background the board may be rebooting, since opening its native USB port resets it, so the level is retried until it answers or the caller is done.
        for ip in ips() if callable(ips) else ips:   # a callable resolves the addresses here, off the caller's thread when `background`
            prev = _get_log_level(ip)
            for _ in range(15 if background else 0):
                if prev is not None or stop.wait(2):
                    break
                prev = _get_log_level(ip)
            if stop.is_set():
                return
            saved[ip] = prev
            set_log_level([ip], index)

    raiser = threading.Thread(target=raise_all, daemon=True)
    if background:
        raiser.start()
    else:
        raise_all()
    try:
        yield
    finally:
        if background:
            stop.set()   # a board that never answered must not hold the port for the retries left
            raiser.join()
        for ip, prev in saved.items():   # only what was raised goes back
            set_log_level([ip], prev if prev is not None else LOG_WARN)

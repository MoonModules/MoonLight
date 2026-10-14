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

_ROOT = Path(__file__).resolve().parent.parent
_STATE = _ROOT / "moondeck" / "moondeck.json"

# logLevel option indices, matching SystemModule's addSelect order
# (None, Error, Warn, Info, Debug, Verbose).
LOG_NONE, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_VERBOSE = range(6)


def device_ips_for_port(port):
    """The IP of the device moondeck.json last saw on `port`, as a one-element list, so a tool watching one board changes only that board; [] when none matches. A tty and a cu name of one port match."""
    if not port or not _STATE.exists():
        return []
    key = Path(port).name.replace("tty.", "cu.", 1)
    try:
        state = json.loads(_STATE.read_text(encoding="utf-8"))
        active = next((n for n in (state.get("networks") or [])
                       if n.get("name") == state.get("active_network")), None)
        return [d["ip"] for d in (active or {}).get("devices", [])
                if d.get("ip") and ":" not in d["ip"]
                and Path(d.get("last_port") or "").name.replace("tty.", "cu.", 1) == key][:1]
    except Exception:
        return []


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
    """Temporarily raise each device to `index` (default Info, so the serial tick line prints) and restore each device's ORIGINAL level on exit, so a device set to Debug or Error keeps its choice.

    A device whose level can't be read is still raised, and restored to Warn, the resting default. `background` raises from a thread, so a caller reading a port starts at once rather than after the HTTP calls, 3 s each for a device that is off. Best-effort throughout; never raises.
    """
    saved = {}
    stop = threading.Event()

    def raise_all():
        # In the background the board may be rebooting, since opening its native USB port resets it, so the level is retried until it answers or the caller is done.
        for ip in ips:
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

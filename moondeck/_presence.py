"""MoonDeck's own discovery: the presence packets MoonLight boards and desktops announce on UDP 65506, heard without sending anything.

Every MoonLight board and desktop sends its presence packet every ten seconds, to the broadcast address and the multicast group
239.255.77.77: WLED's 44-byte discovery header, then MoonLight's web port and the board's base MAC (src/core/system/WledPacket.h, the
layout's one home). A WLED sends the header alone. This listens to both copies on a shared bind (SO_REUSEPORT on POSIX), as the desktop
app's Discovery binds its own (UdpSocket::bindShared), so each runs whether the other does or not.

The Live tab's map reads a MoonLight's Discovery list; this list is how MoonDeck finds one to read when the map's address does not answer,
and what it draws when no MoonLight is heard at all. The same rows, with the Discovery lists they lead to, turn a MAC into the address a
device answers on right now (`resolve`), which is how the live tests reach the devices selected by MAC.

stdlib only and cheap to import, as `_host.py` is.
"""

import json
import socket
import threading
import time
import urllib.request

from _usb import mac_matches

MOONDECK_PORT = 8420          # MoonDeck's own web port, where a script outside it asks for what it heard
DESKTOP = "localhost:8080"    # the desktop app MoonDeck's Host tab runs, whose Discovery list the map reads first
PORT = 65506                  # WledPacket::kPort
GROUP = "239.255.77.77"       # WledPacket::kDiscoveryGroup
HEADER = 44                   # WledPacket::kSize: a WLED's packet
WITH_MAC = 52                 # WledPacket::kSizeWithMac: a MoonLight's or MoonBase's, with the port and the MAC
MARKERS = {b"MM\x01": "MoonLight", b"MB\x01": "MoonBase"}   # the sentinel's top three bytes at offset 41 (WledPacket::kMmMarker, kMbMarker)
FORGET_S = 60                 # a device unheard for six of its announcements leaves the list
RETRY_S = 10                  # a bind refused is tried again after one announcement


def parse(data: bytes, sender: str) -> dict | None:
    """One presence packet as a Discovery row: name, address, type, and for a MoonLight or MoonBase its web port and MAC; None for any
    other datagram on the port."""
    if len(data) not in (HEADER, WITH_MAC) or data[0] != 255 or data[1] != 1:
        return None
    ip = ".".join(str(b) for b in data[2:6])
    if ip == "0.0.0.0":
        ip = sender
    name = data[6:38].split(b"\0", 1)[0].decode("utf-8", "replace")
    row = {"name": name or ip, "ip": ip, "type": MARKERS.get(bytes(data[41:44]), "WLED")}
    if len(data) == WITH_MAC:
        port = data[44] | (data[45] << 8)
        if port and port != 80:
            row["port"] = port
        row["mac"] = ":".join(f"{b:02x}" for b in data[46:52])
    return row


class Listener:
    """Hears presence on a background thread for as long as MoonDeck runs; `rows()` is what it heard lately, each with its age."""

    def __init__(self, port: int = PORT):
        self.port = port
        self._heard: dict[str, tuple[dict, float]] = {}   # by MAC, or by address for a WLED, the row and when it was last heard
        self._lock = threading.Lock()
        self._thread = None
        self.listening = False

    def start(self) -> None:
        if self._thread is None:
            self._thread = threading.Thread(target=self._run, name="presence", daemon=True)
            self._thread.start()

    def rows(self) -> list[dict]:
        now = time.monotonic()
        with self._lock:
            for key in [k for k, (_, at) in self._heard.items() if now - at > FORGET_S]:
                del self._heard[key]
            return sorted(({**row, "ageSec": int(now - at)} for row, at in self._heard.values()), key=lambda r: r["name"].lower())

    def heard(self, data: bytes, sender: str) -> None:
        row = parse(data, sender)
        if row:
            with self._lock:
                self._heard[row.get("mac") or row["ip"]] = (row, time.monotonic())

    def _open(self) -> socket.socket | None:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):   # Windows shares through the address option alone
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            s.bind(("", self.port))
        except OSError:
            s.close()
            return None
        try:   # best effort: a network without multicast still carries the broadcast copy
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(GROUP) + socket.inet_aton("0.0.0.0"))
        except OSError:
            pass
        s.settimeout(1.0)
        return s

    def _run(self) -> None:
        while True:
            s = self._open()
            self.listening = s is not None
            if s is None:
                time.sleep(RETRY_S)
                continue
            with s:
                while True:
                    try:
                        data, (sender, _) = s.recvfrom(2048)
                    except socket.timeout:
                        continue
                    except OSError:
                        break   # the socket went away: open it again
                    self.heard(data, sender)


def address(row: dict) -> str:
    """Where a row's device answers, `ip`, or `ip:port` for a web port other than 80."""
    port = row.get("port")
    return f"{row['ip']}:{port}" if port and int(port) != 80 else row["ip"]


def discovery_rows(entry: str, timeout: float = 1.5) -> list[dict]:
    """The Discovery list the MoonLight at `entry` ("host" or "host:port") serves, each row its value merged with its detail, as the map's
    fetchPeerGraph reads it; [] when it does not answer."""
    try:
        with urllib.request.urlopen(f"http://{entry}/api/modules/Discovery", timeout=timeout) as resp:
            module = json.loads(resp.read())
    except Exception:
        return []
    controls = module.get("controls") if isinstance(module, dict) else None
    for c in controls if isinstance(controls, list) else []:
        if isinstance(c, dict) and c.get("name") == "devices":
            values, detail = c.get("value") or [], c.get("detail") or []
            return [{**v, **(detail[i] if i < len(detail) and isinstance(detail[i], dict) else {})}
                    for i, v in enumerate(values) if isinstance(v, dict)]
    return []


def heard_from_moondeck(timeout: float = 1.0) -> list[dict]:
    """What a running MoonDeck heard, for a script outside its process; [] when MoonDeck does not run."""
    try:
        with urllib.request.urlopen(f"http://localhost:{MOONDECK_PORT}/api/presence", timeout=timeout) as resp:
            return json.loads(resp.read()).get("rows", [])
    except Exception:
        return []


def known_rows(heard: list[dict], lists=discovery_rows, want: list[str] = (), budget_s: float = 6.0) -> list[dict]:
    """The devices known right now: the `heard` presence rows, then the Discovery lists of the desktop app and of each heard MoonLight, which
    bring their peers; each MAC once, the first source's row kept. `lists` reads one device's list. The sources are asked in turn until the
    MACs of `want` all resolve, or, with none wanted, until one list is not empty, and no new source is asked once `budget_s` has passed."""
    rows = list(heard)
    seen = {r["mac"].lower() for r in rows if r.get("mac")}
    sources = [DESKTOP] + [address(r) for r in heard if r.get("type") == "MoonLight" and r.get("ip") and r["ip"] != "127.0.0.1"]
    deadline = time.monotonic() + budget_s
    for entry in sources:
        if (want and len(resolve(list(want), rows)) == len(want)) or time.monotonic() > deadline:
            break
        peers = lists(entry)
        for r in peers:
            if r.get("mac") and r["mac"].lower() not in seen:
                seen.add(r["mac"].lower())
                rows.append(r)
        if peers and not want:
            break
    return rows


def resolve(macs: list[str], rows: list[dict]) -> dict[str, dict]:
    """Each MAC of `macs` that a MoonLight row of `rows` carries, mapped to {mac, name, host}, `host` the address it answers on. A row whose
    MAC is exactly the MAC wins, and a row reporting the MAC's EUI-64 form (a USB board's raw MAC, usb.device_macs) matches next."""
    found = {}
    movers = [r for r in rows if r.get("type") == "MoonLight" and r.get("ip")]
    for mac in macs:
        row = (next((r for r in movers if (r.get("mac") or "").lower() == (mac or "").strip().lower()), None)
               or next((r for r in movers if mac_matches(mac, r.get("mac", ""))), None))
        if row:
            found[mac] = {"mac": mac, "name": row.get("name") or row["ip"], "host": address(row)}
    return found

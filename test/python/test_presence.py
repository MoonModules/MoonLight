"""MoonDeck's own discovery: the presence packets MoonLight devices announce, read as Discovery rows, and a MAC turned into the address its
device answers on now, through what MoonDeck heard and the Discovery lists it leads to."""

import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import _presence  # noqa: E402


def packet(name: bytes, ip=(192, 168, 1, 101), marker=b"\x00MM\x01", port=None, mac=None) -> bytes:
    """A presence packet in WledPacket.h's layout: token, id, address, name, board kind, instance, the marker, then port and MAC."""
    head = bytes([255, 1, *ip]) + name.ljust(32, b"\0") + bytes([0x80 | 32, ip[3]]) + marker
    assert len(head) == 44
    return head if port is None else head + bytes([port & 0xFF, port >> 8]) + bytes(mac)


def test_a_moonlight_packet_reads_as_a_row_with_its_port_and_mac():
    row = _presence.parse(packet(b"MM-MIDI-rtp", port=80, mac=[0x80, 0xB5, 0x4E, 0xC6, 0x3C, 0xB4]), "192.168.1.101")
    assert row == {"name": "MM-MIDI-rtp", "ip": "192.168.1.101", "type": "MoonLight", "mac": "80:b5:4e:c6:3c:b4"}   # port 80 is the default
    desktop = _presence.parse(packet(b"MM-Desktop", port=8080, mac=[2, 0, 0, 0, 0, 1]), "192.168.1.107")
    assert desktop["port"] == 8080


def test_moonbase_and_a_plain_wled_read_as_their_own_kinds_and_anything_else_is_ignored():
    assert _presence.parse(packet(b"MB-1", marker=b"\x00MB\x01", port=80, mac=[1] * 6), "x")["type"] == "MoonBase"
    wled = _presence.parse(packet(b"WLED-Troy", marker=b"\x10\x2a\x34\x01"), "192.168.1.60")
    assert wled == {"name": "WLED-Troy", "ip": "192.168.1.101", "type": "WLED"}
    assert _presence.parse(b"\xff\x01short", "x") is None and _presence.parse(bytes(44), "x") is None


def test_the_listener_hears_a_packet_and_forgets_a_device_gone_quiet(monkeypatch):
    listener = _presence.Listener()
    listener.heard(packet(b"MM-A", port=80, mac=[1, 2, 3, 4, 5, 6]), "192.168.1.5")
    listener.heard(packet(b"MM-A", port=80, mac=[1, 2, 3, 4, 5, 6]), "192.168.1.5")   # its next announcement: still one row
    assert [r["name"] for r in listener.rows()] == ["MM-A"]
    now = time.monotonic()
    monkeypatch.setattr(_presence.time, "monotonic", lambda: now + _presence.FORGET_S + 1)
    assert listener.rows() == []


def test_the_listener_shares_its_port_with_another_listener():
    port = 65432   # a port of the test's own, so a desktop beside the suite keeps 65506
    other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    other.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        other.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)   # as UdpSocket::bindShared binds the desktop's
    other.bind(("", port))
    try:
        s = _presence.Listener(port)._open()
        assert s is not None
        s.close()
    finally:
        other.close()


HEARD_A = {"name": "MM-A", "ip": "192.168.1.5", "type": "MoonLight", "mac": "aa:aa:aa:aa:aa:aa"}
PEER_B = {"name": "MM-B", "ip": "192.168.1.6", "type": "MoonLight", "mac": "bb:bb:bb:bb:bb:bb"}
DESKTOP = {"name": "desk", "ip": "192.168.1.2", "port": 8080, "type": "MoonLight", "mac": "dd:dd:dd:dd:dd:dd"}


def test_a_selected_mac_resolves_to_the_address_its_moonlight_answers_on():
    rows = [HEARD_A, DESKTOP, {"name": "WLED", "ip": "192.168.1.9", "type": "WLED", "mac": "ee:ee:ee:ee:ee:ee"}]
    found = _presence.resolve(["aa:aa:aa:aa:aa:aa", "dd:dd:dd:dd:dd:dd", "ee:ee:ee:ee:ee:ee", "ff:ff:ff:ff:ff:ff"], rows)
    assert found == {"aa:aa:aa:aa:aa:aa": {"mac": "aa:aa:aa:aa:aa:aa", "name": "MM-A", "host": "192.168.1.5"},
                     "dd:dd:dd:dd:dd:dd": {"mac": "dd:dd:dd:dd:dd:dd", "name": "desk", "host": "192.168.1.2:8080"}}   # no WLED, no stranger


def test_the_known_devices_are_the_heard_ones_then_the_desktops_list_or_else_the_first_heard_moonlights():
    lists = {"localhost:8080": [DESKTOP, {**HEARD_A, "ip": "10.0.0.1"}], "192.168.1.5": [HEARD_A, PEER_B]}
    asked = []
    read = lambda entry: asked.append(entry) or lists.get(entry, [])   # noqa: E731
    assert _presence.known_rows([HEARD_A], read) == [HEARD_A, DESKTOP]   # the heard row wins its MAC: it is the freshest
    assert asked == ["localhost:8080"]                                   # a desktop list answered: no board asked
    lists.pop("localhost:8080")
    assert _presence.known_rows([HEARD_A], read) == [HEARD_A, PEER_B]    # no desktop app: the heard board's list brings its peers

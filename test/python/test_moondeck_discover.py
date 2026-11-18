"""MoonDeck's Discover: the known devices' own lists first, kept to the subnet asked for, the known devices of that subnet always asked.

The network is mocked: `_discovery_rows` stands for a device's Devices list and `_probe_device` for its /api/system answer.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import moondeck  # noqa: E402


def _discover(monkeypatch, rows_by_entry, known):
    probed = []
    monkeypatch.setattr(moondeck, "_discovery_rows", lambda entry: rows_by_entry.get(entry, []))
    monkeypatch.setattr(moondeck, "_probe_device", lambda ip, port=8080, timeout=0.4, name="": probed.append((ip, port)) or None)
    moondeck.discover_devices("192.168.1", known)
    return probed


def test_rows_of_another_subnet_are_left_to_that_subnets_scan(monkeypatch):
    rows = {"192.168.1.5": [{"type": "MoonLight", "ip": "10.20.30.7"}]}
    probed = _discover(monkeypatch, rows, [{"ip": "192.168.1.5"}])
    assert ("10.20.30.7", 80) not in probed
    assert ("192.168.1.100", 80) in probed            # no row of this subnet: the sweep


def test_a_known_device_its_peer_has_not_heard_yet_is_still_asked(monkeypatch):
    rows = {"192.168.1.5": [{"type": "MoonLight", "ip": "192.168.1.5"}]}   # A restarted a moment ago and lists only itself
    probed = _discover(monkeypatch, rows, [{"ip": "192.168.1.5"}, {"ip": "192.168.1.6"}])
    assert ("192.168.1.6", 80) in probed and ("192.168.1.5", 80) in probed
    assert ("192.168.1.100", 80) not in probed        # a list answered: no sweep

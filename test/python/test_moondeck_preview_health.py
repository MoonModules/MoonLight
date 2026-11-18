"""The preview-health probe and the live-matrix run scripts (moondeck/diag/preview_health.py, moondeck/scenario/run_network_live.py): the
probe pulls the binary preview from /wsp, fails when a selected device was not probed, and the matrix fails when its filter selects no
sender. No device is contacted. Run: `uv run --with pytest pytest test/python -q`."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

import preview_health
import run_network_live


def test_the_probe_opens_the_binary_preview_channel_and_asks_for_frames_on_every_connection(monkeypatch):
    opened, sent = [], []

    class Sock:
        def sendall(self, data):
            sent.append(data)
    monkeypatch.setattr(preview_health, "handshake", lambda host, path, timeout: opened.append(path) or (Sock(), b""))
    preview_health._Ws("10.0.0.5")
    preview_health._Ws("10.0.0.5")   # a reconnect
    assert opened == ["/wsp", "/wsp"] and len(sent) == 2
    frame = sent[0]
    mask = frame[2:6]
    assert frame[0] == 0x82 and bytes(b ^ mask[i % 4] for i, b in enumerate(frame[6:])) == bytes([0x51, 1, 25])   # a masked binary [0x51][stride][fps]


def test_the_probe_fails_when_a_selected_device_was_not_found_though_the_found_one_is_smooth(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["preview_health"])
    monkeypatch.setattr(preview_health, "live_test_devices", lambda: [{"ip": "10.0.0.5"}])
    monkeypatch.setattr(preview_health, "selected_live_tests", lambda: ["aa:aa:aa:aa:aa:aa", "bb:bb:bb:bb:bb:bb"])
    monkeypatch.setattr(preview_health, "measure", lambda host, seconds, grid: "SMOOTH")
    assert preview_health.main() == 1
    assert "1 selected device(s) were not probed" in capsys.readouterr().out
    monkeypatch.setattr(preview_health, "selected_live_tests", lambda: ["aa:aa:aa:aa:aa:aa"])
    assert preview_health.main() == 0
    monkeypatch.setattr(sys, "argv", ["preview_health", "10.9.9.9"])   # an explicit host is unaffected by the selection
    assert preview_health.main() == 0


def test_the_matrix_fails_when_its_host_filter_selects_no_sender(monkeypatch, capsys):
    monkeypatch.setattr(sys, "argv", ["run_network_live", "--host", "10.0.0.99"])
    monkeypatch.setattr(run_network_live, "load_selected_devices", lambda: [{"ip": "10.0.0.5", "deviceName": "MM-A"}])
    monkeypatch.setattr(run_network_live, "Board", lambda d: (_ for _ in ()).throw(AssertionError("no device is contacted")))
    assert run_network_live.main() == 2
    assert "no round would run" in capsys.readouterr().out

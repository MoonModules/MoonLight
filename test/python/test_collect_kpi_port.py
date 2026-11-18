"""collect_kpi.py: the serial port of standalone ESP32 capture, read from a moondeck.json of any age."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
import _moondeck_config
import collect_kpi


def test_a_moondeck_json_from_before_networks_still_names_its_port_for_live_capture(tmp_path, monkeypatch, capsys):
    state = tmp_path / "moondeck.json"
    state.write_text(json.dumps({"port": "/dev/cu.none-here"}), encoding="utf-8")   # the top-level port of an unmigrated file
    monkeypatch.setattr(_moondeck_config, "_STATE", state)
    monkeypatch.setattr(collect_kpi, "_LIVE_CAPTURE_ENABLED", True)
    assert collect_kpi._live_capture(tmp_path / "monitor.log") is False
    assert "configured port /dev/cu.none-here not present" in capsys.readouterr().out   # the port was read, so the skip names it

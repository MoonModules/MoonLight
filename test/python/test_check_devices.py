"""check_devices.py guards the device catalog the web installer and MoonDeck read: its shape against deviceModels.schema.json, then what only the firmware knows.

Each rule is pinned on a catalog written to break it, since a gate that stopped reading would otherwise pass as clean."""

import json
import sys
from pathlib import Path

import jsonschema

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import check_devices  # noqa: E402

SCHEMA = json.loads((ROOT / "mooninstaller" / "deviceModels.schema.json").read_text(encoding="utf-8"))
ENTRY = {"name": "Board", "chip": "ESP32", "firmwares": ["esp32"],
         "state": {"System": {"deviceModel": "Board"}, "Drivers": {"$patch": "replace", "Led": {"type": "RmtLedDriver", "pins": "18"}}}}


def problems(entry: dict) -> list:
    return [e.message for e in jsonschema.Draft202012Validator(SCHEMA).iter_errors([entry])]


def run(tmp_path, monkeypatch, catalog) -> int:
    path = tmp_path / "deviceModels.json"
    path.write_text(json.dumps(catalog), encoding="utf-8")
    monkeypatch.setattr(check_devices, "CATALOG", path)
    try:
        check_devices.main()
    except SystemExit as e:
        return e.code or 0
    return 0


def test_the_schema_is_valid_json_schema_and_the_repo_catalog_meets_it():
    jsonschema.Draft202012Validator.check_schema(SCHEMA)
    assert jsonschema.Draft202012Validator(SCHEMA).is_valid(json.loads(check_devices.CATALOG.read_text(encoding="utf-8")))


def test_a_well_formed_entry_meets_the_schema():
    assert problems(ENTRY) == []


def test_an_entry_missing_its_state_is_refused():
    assert problems({k: v for k, v in ENTRY.items() if k != "state"}) == ["'state' is a required property"]


def test_a_flash_baud_outside_the_standard_rates_is_refused():
    assert problems({**ENTRY, "flashBaud": 12345}) == ["12345 is not one of [115200, 230400, 460800, 921600]"]


def test_a_supported_capability_no_module_drives_is_refused():
    assert problems({**ENTRY, "supported": ["Lasers"]})


def test_a_name_longer_than_the_device_keeps_is_refused():
    assert problems({**ENTRY, "name": "x" * 32})


def test_a_child_module_without_a_type_is_refused():
    state = {"Drivers": {"Led": {"pins": "18"}}}
    assert problems({**ENTRY, "state": state}) == ["'type' is a required property"]


def test_a_patch_other_than_replace_is_refused():
    assert problems({**ENTRY, "state": {"Drivers": {"$patch": "merge"}}}) == ["'replace' was expected"]


def test_a_field_the_catalog_does_not_know_is_refused():
    assert problems({**ENTRY, "colour": "red"})


def test_the_gate_fails_on_a_schema_violation(tmp_path, monkeypatch):
    assert run(tmp_path, monkeypatch, [{**ENTRY, "flashBaud": 12345}]) == 1


def test_the_gate_fails_on_a_firmware_the_registry_lacks_which_no_schema_can_know(tmp_path, monkeypatch):
    assert run(tmp_path, monkeypatch, [{**ENTRY, "firmwares": ["esp99"]}]) == 1


def test_the_gate_passes_a_well_formed_entry(tmp_path, monkeypatch):
    assert run(tmp_path, monkeypatch, [ENTRY]) == 0


def test_a_name_that_is_no_string_is_reported_rather_than_crashing_the_gate(tmp_path, monkeypatch):
    assert run(tmp_path, monkeypatch, [{**ENTRY, "name": ["Board"]}]) == 1


def test_the_gate_refuses_a_baud_written_as_a_float_which_esptool_would_misread(tmp_path, monkeypatch):
    assert run(tmp_path, monkeypatch, [{**ENTRY, "flashBaud": 921600.0}]) == 1

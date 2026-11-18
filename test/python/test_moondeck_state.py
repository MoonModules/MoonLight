"""MoonDeck's moondeck.json and what reads it: an older file carried over, the server's own keys kept from a page's save, the live tests
selected by MAC, Identify's cache, the local builds, and a board flashed by its MAC on the port it sits on now, with the flash's guard.

The present ports and the esptool probe are stand-ins, so the tests open no serial port. Run: `uv run --with pytest pytest test/python -q`.
"""

import json

import flash_esp32
import moondeck

CP2102N = {"vid": 0x10C4, "pid": 0xEA60, "product": "CP2102N USB to UART Bridge", "serial": "46fe21db"}

OLD_FILE = {
    "networks": [
        {"name": "Home", "subnet": "192.168.1.0/24", "wifi": {"ssid": "home", "password": "pw"}, "port": "/dev/cu.usbserial-1",
         "devices": [
             {"mac": "AA:AA:AA:AA:AA:AA", "ip": "192.168.1.5", "deviceName": "MM-A", "selected": True, "online": True,
              "last_port": "/dev/cu.usbserial-20213240", "usbSerial": "20213240", "probedChip": "esp32-s3", "deviceModel": "LOLIN D32"},
             {"mac": "bb:bb:bb:bb:bb:bb", "ip": "192.168.1.6", "selected": False},
             {"ip": "192.168.1.7", "selected": True},   # a MAC-less row: dropped, since the selection is kept by MAC
         ]},
        {"name": "Lab", "subnet": "10.0.0.0/24", "wifi": {"ssid": "", "password": ""}, "port": "", "devices": []},
    ],
    "active_network": "Home", "tab": "live", "firmware": "esp32", "onlineOnly": True, "_scanned_subnet": "192.168.1", "_found_count": 3,
}


def test_an_older_file_keeps_its_networks_and_preferences_and_carries_its_devices_over_by_mac():
    state = moondeck._migrate_state(json.loads(json.dumps(OLD_FILE)))
    assert state["networks"] == [
        {"name": "Home", "subnet": "192.168.1.0/24", "wifi": {"ssid": "home", "password": "pw"}, "port": "/dev/cu.usbserial-1"},
        {"name": "Lab", "subnet": "10.0.0.0/24", "wifi": {"ssid": "", "password": ""}, "port": ""}]
    assert state["liveTests"] == ["aa:aa:aa:aa:aa:aa"]                                      # the ticked device, by MAC
    assert state["usbMacs"] == {"20213240": {"mac": "aa:aa:aa:aa:aa:aa", "chip": "esp32-s3"}}   # the identified adapter
    assert state["tab"] == "live" and state["firmware"] == "esp32" and state["active_network"] == "Home"
    assert not {"onlineOnly", "_scanned_subnet", "_found_count", "devices"} & set(state)
    assert moondeck._migrate_state(json.loads(json.dumps(state))) == state                 # a migrated file passes through


def test_a_file_from_before_networks_becomes_one_network_holding_its_port():
    state = moondeck._migrate_state({"port": "/dev/cu.usbmodem1", "devices": [{"mac": "cc:cc:cc:cc:cc:cc", "selected": True}], "env": "x"})
    assert state["networks"] == [{"name": "Home", "subnet": "", "wifi": {"ssid": "", "password": ""}, "port": "/dev/cu.usbmodem1"}]
    assert state["active_network"] == "Home" and state["liveTests"] == ["cc:cc:cc:cc:cc:cc"]
    assert not {"port", "devices", "env"} & set(state)


def test_moondeck_starts_on_an_older_file_and_saves_it_migrated(tmp_path, monkeypatch):
    monkeypatch.setattr(moondeck, "STATE_FILE", tmp_path / "moondeck.json")
    moondeck.STATE_FILE.write_text(json.dumps(OLD_FILE))
    moondeck.mutate_state(lambda s: None)
    saved = json.loads(moondeck.STATE_FILE.read_text())
    assert "devices" not in saved["networks"][0] and saved["liveTests"] == ["aa:aa:aa:aa:aa:aa"]


def test_a_pages_save_keeps_the_servers_keys_and_gives_a_new_network_this_computers_subnet():
    state = {"networks": [{"name": "Home", "subnet": "192.168.1.0/24"}], "usbMacs": {"S": {"mac": "aa:aa:aa:aa:aa:aa"}}, "liveTests": ["x"]}
    page = {"networks": [{"name": "Home", "subnet": "192.168.1.0/24"}, {"name": "New", "subnet": ""}], "usbMacs": {}, "liveTests": [],
            "firmware": "esp32s3-n8r8"}
    moondeck.merge_page_state(state, page, "10.1.2.0/24")
    assert state["usbMacs"] == {"S": {"mac": "aa:aa:aa:aa:aa:aa"}} and state["liveTests"] == ["x"]   # the page read them before
    assert state["firmware"] == "esp32s3-n8r8" and state["networks"][1]["subnet"] == "10.1.2.0/24"


def test_the_live_tests_are_selected_by_mac_in_the_order_ticked():
    state = {}
    moondeck.select_live_test(state, "aa:aa:aa:aa:aa:aa", True)
    moondeck.select_live_test(state, "bb:bb:bb:bb:bb:bb", True)
    moondeck.select_live_test(state, "aa:aa:aa:aa:aa:aa", True)   # ticked twice: once
    assert state["liveTests"] == ["bb:bb:bb:bb:bb:bb", "aa:aa:aa:aa:aa:aa"]
    moondeck.select_live_test(state, "bb:bb:bb:bb:bb:bb", False)
    assert state["liveTests"] == ["aa:aa:aa:aa:aa:aa"]


def test_the_live_tests_find_each_selected_mac_at_its_address_now_and_report_the_rest(monkeypatch):
    rows = [{"name": "MM-A", "ip": "192.168.1.50", "type": "MoonLight", "mac": "aa:aa:aa:aa:aa:aa"}]   # moved to a new address
    monkeypatch.setattr(moondeck._presence, "known_rows", lambda heard: rows)
    got = moondeck.live_tests({"liveTests": ["aa:aa:aa:aa:aa:aa", "bb:bb:bb:bb:bb:bb"]})
    assert got == {"devices": [{"mac": "aa:aa:aa:aa:aa:aa", "name": "MM-A", "host": "192.168.1.50"}], "missing": ["bb:bb:bb:bb:bb:bb"]}


def test_the_scripts_read_the_same_selection(monkeypatch, tmp_path):
    import _moondeck_config
    monkeypatch.setattr(_moondeck_config, "_STATE", tmp_path / "moondeck.json")
    _moondeck_config._STATE.write_text(json.dumps({"liveTests": ["aa:aa:aa:aa:aa:aa"]}))
    monkeypatch.setattr(_moondeck_config._presence, "heard_from_moondeck", lambda: [])
    monkeypatch.setattr(_moondeck_config._presence, "known_rows",
                        lambda heard: [{"name": "MM-A", "ip": "192.168.1.5", "port": 8080, "type": "MoonLight", "mac": "aa:aa:aa:aa:aa:aa"}])
    assert _moondeck_config.live_test_devices() == [{"ip": "192.168.1.5:8080", "deviceName": "MM-A", "mac": "aa:aa:aa:aa:aa:aa"}]


def test_identify_probes_only_present_ports_and_remembers_the_mac_by_the_adapter(monkeypatch, tmp_path):
    monkeypatch.setattr(moondeck, "STATE_FILE", tmp_path / "moondeck.json")
    path = "/dev/cu.usbserial-20213340"
    probed = []
    monkeypatch.setattr(moondeck._usb, "present_ports", lambda: {path: CP2102N})
    monkeypatch.setattr(moondeck._usb, "probe_port_chip", lambda p: probed.append(p) or {"chip": "esp32-s3", "mac": "bb:bb:bb:bb:bb:bb"})
    result = moondeck.identify_ports([path, "/dev/not-present"])
    assert probed == [path] and result == {path: {"chip": "esp32-s3", "mac": "bb:bb:bb:bb:bb:bb"}}
    assert moondeck.load_state()["usbMacs"] == {"20213340": {"mac": "bb:bb:bb:bb:bb:bb", "chip": "esp32-s3"}}


def test_the_local_builds_are_the_esp32_images_on_disk_with_their_chip(monkeypatch, tmp_path):
    (tmp_path / "esp32-esp32s3-n8r8").mkdir()
    (tmp_path / "esp32-esp32s3-n8r8" / moondeck.APP_BIN).write_bytes(b"x" * 10)
    monkeypatch.setattr(moondeck, "build_dir_for", lambda fw: tmp_path / f"esp32-{fw}")
    builds = moondeck.local_builds()
    assert [(b["firmware"], b["chip"], b["size"]) for b in builds] == [("esp32s3-n8r8", "esp32-s3", 10)]


def _run(monkeypatch, tmp_path, params, ports):
    """POST /api/run/flash_esp32 with `params`: what the handler answered, and the command it would start."""
    monkeypatch.setattr(moondeck, "STATE_FILE", tmp_path / "moondeck.json")
    moondeck.STATE_FILE.write_text(json.dumps({"usbMacs": {"20213340": {"mac": "bb:bb:bb:bb:bb:bb"}}}))
    monkeypatch.setattr(moondeck._usb, "present_ports", lambda: ports)
    started = []

    def popen(cmd, **kwargs):
        started.append(cmd)
        raise OSError("not started in a test")
    monkeypatch.setattr(moondeck.subprocess, "Popen", popen)
    monkeypatch.setattr(moondeck, "kill_script", lambda *a, **k: None)   # no process of this computer is touched
    handler = moondeck.MoonDeckHandler.__new__(moondeck.MoonDeckHandler)
    answers = []
    handler._send_json = lambda data, status=200: answers.append((status, data))
    handler._handle_run("flash_esp32", params)
    return answers, started


def test_a_board_named_by_mac_is_flashed_on_the_port_it_sits_on_now_with_the_mac_guard(monkeypatch, tmp_path):
    answers, started = _run(monkeypatch, tmp_path, {"mac": "bb:bb:bb:bb:bb:bb", "firmware": "esp32"}, {"/dev/cu.usbserial-20213340": CP2102N})
    cmd = started[0]
    assert cmd[cmd.index("--port") + 1] == "/dev/cu.usbserial-20213340" and cmd[cmd.index("--mac") + 1] == "bb:bb:bb:bb:bb:bb"


def test_a_board_on_no_port_of_this_computer_is_refused_before_anything_runs(monkeypatch, tmp_path):
    answers, started = _run(monkeypatch, tmp_path, {"mac": "cc:cc:cc:cc:cc:cc", "firmware": "esp32"}, {"/dev/cu.usbserial-20213340": CP2102N})
    assert started == [] and answers[0][0] == 400 and "cc:cc:cc:cc:cc:cc" in answers[0][1]["error"]
    answers, started = _run(monkeypatch, tmp_path, {"mac": "--erase-all", "firmware": "esp32"}, {})
    assert started == [] and answers[0][0] == 400


def test_the_flash_stops_on_another_board_or_none_and_goes_on_for_the_one_meant():
    meant = "30:ed:a0:ff:fe:f3"   # the S31's reported MAC: its efuse MAC is 30:ed:a0:f3:d4:68
    assert flash_esp32._stranger("/dev/p", meant, lambda port: {"mac": "30:ed:a0:f3:d4:68"}) == ""
    assert "11:22:33:44:55:66" in flash_esp32._stranger("/dev/p", meant, lambda port: {"mac": "11:22:33:44:55:66"})
    assert "no board answers" in flash_esp32._stranger("/dev/p", meant, lambda port: {"chip": "", "mac": ""})


def test_a_local_build_is_served_as_a_file_for_the_popups_install_and_an_unknown_firmware_is_refused(tmp_path, monkeypatch):
    import io
    monkeypatch.setattr(moondeck, "build_dir_for", lambda fw: tmp_path / f"esp32-{fw}")
    sent = {}

    class Probe(moondeck.MoonDeckHandler):
        def __init__(self, path):   # a handler for one GET, its socket left out
            self.path, self.wfile = path, io.BytesIO()
        def _send_file(self, path, content_type, data=None):
            sent.update(path=path, type=content_type)
        def send_error(self, code, message=None):
            sent.update(code=code)

    fw = next(iter(sorted(moondeck.ESP32_BUILDS)))
    image = tmp_path / f"esp32-{fw}" / moondeck.APP_BIN
    image.parent.mkdir()
    image.write_bytes(b"\xe9app")
    Probe(f"/api/local-build/{fw}").do_GET()
    assert sent == {"path": image, "type": "application/octet-stream"}
    sent.clear()
    Probe("/api/local-build/not-a-firmware").do_GET()
    assert sent == {"code": 404}

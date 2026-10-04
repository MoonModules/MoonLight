"""What the live scenario runner proves, and what it leaves behind.

It names the device it measured, proves a restart from two uptime readings, and puts back every control and module a scenario changed.

The runner reads the uptime, asks for a restart, and reads it again.
Its HTTP client waits out a short outage, so a board that restarts quickly is never seen to go away, and the uptime is then the only evidence there is.

The restart case this pins: two scenarios restart the same board back to back.
The second one reads its "before" seconds after the first restart, and by the time the board answers again its new uptime has already reached that figure.
Comparing the two readings alone reads that as "did not restart", which failed `scenario_Services_audio_drives_the_effects` on an S3 that had restarted correctly.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
import run_live_scenario as runner  # noqa: E402


def test_a_restart_long_after_boot_is_seen():
    assert runner._restarted(before=400, now=8, elapsed=12)


def test_a_restart_seconds_after_the_previous_one_is_seen():
    # Read at 12 s of uptime; fourteen seconds later the restarted board already reports 12 s again.
    assert runner._restarted(before=12, now=12, elapsed=14)
    assert runner._restarted(before=12, now=20, elapsed=25)


def test_a_device_that_kept_running_is_not_mistaken_for_a_restart():
    assert not runner._restarted(before=12, now=26, elapsed=14)
    assert not runner._restarted(before=400, now=460, elapsed=60)


def test_whole_second_rounding_does_not_invent_a_restart():
    # Both readings are whole seconds, so a running device can appear a second short of the wall.
    assert not runner._restarted(before=12, now=25, elapsed=14.9)


def _state(firmware, model):
    return {"modules": [{"type": "SystemModule", "controls": [{"name": "deviceModel", "value": model}]},
                        {"type": "FirmwareUpdateModule", "controls": [{"name": "firmware", "value": firmware}]}]}


def test_a_board_is_named_after_its_firmware_wherever_the_runner_is():
    assert runner._detect_target(_state("esp32s3-n16r8", "MM testbench S3"), local=False) == "esp32s3-n16r8"


def test_a_desktop_build_on_another_machine_is_named_after_its_own_platform():
    # Measured from a Mac, a container on a NanoPi was recorded as desktop-macos, into the Mac's figures.
    assert runner._detect_target(_state("unknown", "docker-arm64"), local=False) == "desktop-docker-arm64"


def test_a_desktop_build_on_this_machine_is_named_after_this_host():
    assert runner._detect_target(_state("unknown", "docker-arm64"), local=True) == runner.desktop_target()


def test_only_a_host_on_this_machine_counts_as_local():
    assert runner.Client("localhost:8080").local
    assert runner.Client("127.0.0.1:8080").local
    assert runner.Client("[::1]:8080").local
    assert runner.Client("[::1]").local
    assert runner.Client("::1").local
    assert not runner.Client("192.168.1.156:8080").local
    assert not runner.Client("192.168.1.158").local


class _FakeDevice:
    """Answers the two calls the control restore makes, and records what it was told to set."""

    def __init__(self, controls):
        self.controls = dict(controls)
        self.posts = []

    def get(self, path):
        return {"controls": [{"name": k, "value": v} for k, v in self.controls.items()]}

    def post(self, path, data):
        if data["module"] == "Gone":
            raise OSError("module not found")
        self.posts.append((data["module"], data["control"], data["value"]))
        self.controls[data["control"]] = data["value"]
        return {"ok": True}


def test_a_control_a_scenario_wrote_is_put_back_to_what_it_held():
    device = _FakeDevice({"deviceName": "MM-testbench-S3"})
    prior = {}
    runner._remember_control(prior, device, "System", "deviceName")
    device.controls["deviceName"] = "MM-ScenarioProof"          # the scenario's own write
    assert runner._restore_controls(device, prior) == 1
    assert device.controls["deviceName"] == "MM-testbench-S3"


def test_only_the_value_before_the_first_write_is_remembered():
    device = _FakeDevice({"gain": 128})
    prior = {}
    runner._remember_control(prior, device, "Audio", "gain")
    device.controls["gain"] = 200
    runner._remember_control(prior, device, "Audio", "gain")   # a second write must not overwrite it
    assert prior == {("Audio", "gain"): 128}


def test_a_control_whose_module_is_gone_does_not_stop_the_rest():
    device = _FakeDevice({"deviceName": "MM-testbench-S3"})
    prior = {("System", "deviceName"): "MM-testbench-S3", ("Gone", "speed"): 5}
    assert runner._restore_controls(device, prior) == 1
    assert device.posts == [("System", "deviceName", "MM-testbench-S3")]


class _FakeTree:
    """One Layer's children as {name: type}, answering the module read and the replace endpoint."""

    def __init__(self, children):
        self.children = dict(children)

    def get(self, path):
        return {"type": self.children[path.rsplit("/", 1)[1]]}

    def post(self, path, data):
        name = path.split("/")[3]
        del self.children[name]
        self.children[data["name"]] = data["type"]
        return {"ok": True}


def test_a_replaced_slot_is_swapped_back_to_the_type_it_held_under_its_own_name():
    layer = _FakeTree({"Pulse": "PulseEffect"})
    replaced = {}
    runner._remember_type(replaced, layer, "Pulse")
    layer.post("/api/modules/Pulse/replace", {"type": "RainbowEffect", "name": "Pulse"})   # the scenario's swap
    runner._remember_type(replaced, layer, "Pulse")   # a second replace must not overwrite the original
    assert runner._restore_types(layer, replaced) == 1
    assert layer.children == {"Pulse": "PulseEffect"}


class _FakeSwitch:
    """One module whose `enabled` is a field of its own, the way the device publishes it."""

    def __init__(self, enabled):
        self.enabled = enabled

    def get(self, path):
        return {"enabled": self.enabled, "controls": []}

    def post(self, path, data):
        self.enabled = data["value"]
        return {"ok": True}


def test_a_module_a_scenario_switched_off_is_switched_back_on():
    audio = _FakeSwitch(enabled=True)
    prior = {}
    runner._remember_control(prior, audio, "Audio", "enabled")
    audio.enabled = False                                         # the scenario's own write
    assert runner._restore_controls(audio, prior) == 1
    assert audio.enabled is True


def test_a_restore_the_device_declines_is_not_counted():
    class _Declines(_FakeDevice):
        def post(self, path, data):
            return {"ok": False}
    device = _Declines({"gain": 128})
    assert runner._restore_controls(device, {("Audio", "gain"): 128}) == 0


def test_a_password_is_not_remembered_because_the_device_returns_it_obfuscated():
    class _Secret:
        def get(self, path):
            return {"controls": [{"name": "password", "type": "Password", "value": "b2JmdXNjYXRlZA=="}]}
    prior = {}
    runner._remember_control(prior, _Secret(), "Network", "password")
    assert prior == {}


def test_a_restore_that_fails_unexpectedly_is_reported_and_a_vanished_module_is_not(capsys):
    import urllib.error

    class _Mixed:
        def post(self, path, data):
            if data["module"] == "Created":
                raise urllib.error.HTTPError(path, 404, "module not found", {}, None)
            raise urllib.error.HTTPError(path, 500, "server error", {}, None)
    runner._restore_controls(_Mixed(), {("Created", "speed"): 5, ("Audio", "gain"): 128})
    out = capsys.readouterr().out
    assert "Audio.gain" in out
    assert "Created" not in out


def test_an_http_step_on_a_path_goes_to_the_device_under_test():
    """A path names the device the run targets, so one scenario serves every board; an absolute URL stays as written, as a captive check needs."""
    r = runner._http_request("/moonbase", "http://192.168.1.50/")
    assert r.full_url == "http://192.168.1.50/moonbase" and r.get_method() == "GET"
    assert runner._http_request("http://captive.apple.com/", "http://192.168.1.50").full_url == "http://captive.apple.com/"


def test_an_http_post_step_sends_an_empty_body_as_a_button_does():
    r = runner._http_request("/api/firmware/boot-app", "http://192.168.1.50", "POST")
    assert r.get_method() == "POST" and r.data == b""

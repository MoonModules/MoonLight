"""A live scenario names the device it measured, and proves a restart from two uptime readings.

The runner reads the uptime, asks for a restart, and reads it again. Its HTTP client waits out a
short outage, so a board that restarts quickly is never seen to go away, and the uptime is then the
only evidence there is.

The case this pins: two scenarios restart the same board back to back. The second one reads its
"before" seconds after the first restart, and by the time the board answers again its new uptime has
already reached that figure. Comparing the two readings alone reads that as "did not restart", which
failed `scenario_Services_audio_drives_the_effects` on an S3 that had restarted correctly.
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

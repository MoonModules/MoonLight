"""The smoke test's URL reading: a provisioned board's URL comes from either step, so the network check runs whenever the board reports one."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / "build"))

import improv_smoke_test  # noqa: E402


def test_the_provision_step_reports_the_url():
    out = "==> using host SSID: home\n==> provisioned: http://192.168.1.20/\n"
    assert improv_smoke_test._device_url(out) == "http://192.168.1.20/"


def test_the_probe_reports_a_provisioned_boards_url():
    """A board already provisioned keeps its credentials, so the probe's current state is where its URL is."""
    out = "      → GET_CURRENT_STATE\n        state: provisioned\n        url: http://192.168.1.127/\n"
    assert improv_smoke_test._device_url(out) == "http://192.168.1.127/"


def test_output_without_a_url_reports_none():
    assert improv_smoke_test._device_url("==> device reports PROVISIONED already\n") is None

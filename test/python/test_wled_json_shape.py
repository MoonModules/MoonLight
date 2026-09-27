# /// script
# dependencies = ["pytest", "wled"]
# ///
"""HA WLED integration `/json` contract test.

Pins the shape HttpServerModule::serveWledDeviceJson emits, by parsing a golden
capture through frenck/python-wled's real dataclass models — the exact library
Home Assistant's WLED integration uses. If HA's WLED library changes what it
requires (a new mandatory field, a stricter validator), or if serveWledDeviceJson
drops a field the library needs, this test fails locally BEFORE HA fails on the
live device — the parse either raises or leaves a required attribute missing.

The golden vector below is a byte-for-byte capture of `curl /json` from the
desktop build, so a regression in the C++ writer is caught as a raw-string diff
on this constant; the `Device.from_dict` assertion then re-confirms python-wled
still accepts the shape. Refresh the golden by running the desktop and pasting
the new /json body here — the point of the vector is that a change is a
deliberate acknowledgement, not a silent drift.
"""

import json

from wled.models import Device


# Captured from `curl http://127.0.0.1:8080/json` against a Windows desktop
# build (2026-07-07). MAC/IP are the desktop stub's synthetic values, but every
# WLED-shim field python-wled reads is present.
GOLDEN_JSON = (
    '{"state":{"on":true,"bri":20,"transition":7,"ps":-1,"pl":-1,"nl":{},"udpn":{},"lor":0,"mainseg":0,'
    '"seg":[{"id":0,"on":true,"bri":255,"col":[[255,255,255]]}]},'
    # ver is the sentinel "99.0.0", NOT the MoonLight semver. Reason: HA's WLED integration
    # parses WLED tags as CalVer (`16.0.1` is year-16, not `0.16.1`), so a MoonLight semver
    # like `2.1.0-dev` compares LOWER (2 < 16) and HA nagged "update to WLED 16.0.1" on the
    # bench P4 even after the flash. `99.0.0` beats every WLED tag in the CalVer regime; the
    # real MoonLight version lives on the MQTT update entity's `installed_version`.
    '"info":{"ver":"99.0.0","vid":2410150,"name":"MM-CAFE","mac":"deadbeefcafe","ip":"192.168.1.246",'
    '"arch":"esp32","brand":"WLED","product":"MoonModules","release":"MoonModules",'
    '"leds":{"count":1,"fps":60,"rgbw":false,"wv":false,"cct":false,"maxpwr":0,"maxseg":1,"pwr":0,"lc":1,"seglc":[1]},'
    '"wifi":{"bssid":"00:00:00:00:00:00","rssi":-50,"channel":0,"signal":100},'
    '"fs":{"t":256,"u":32,"pmt":1},"freeheap":100000,"uptime":42,"udpport":21324,"live":false,'
    '"lm":"","lip":"","ws":-1,"fxcount":1,"palcount":1,"cpalcount":0,"umpalcount":0,"str":false},'
    '"effects":["Solid"],"palettes":["Default"]}'
)


def test_golden_parses_as_json():
    d = json.loads(GOLDEN_JSON)
    assert set(d.keys()) == {"state", "info", "effects", "palettes"}


def test_python_wled_accepts_device():
    """The canonical assertion: python-wled's Device.from_dict must not raise.

    This runs the same __pre_deserialize__ (version gate, effects/palettes
    normalisation) and post-deserialize hooks (arch lowercasing, websocket
    None-fill) HA's WLED integration runs on every state refresh.
    """
    d = json.loads(GOLDEN_JSON)
    dev = Device.from_dict(d)

    # Info — the required `fs` object populates; `ver` clears the version gate;
    # brand/product/mac/arch reach HA's device card.
    assert dev.info.brand == "WLED"
    assert dev.info.product == "MoonModules"
    assert dev.info.architecture == "esp32"     # post_deserialize lowercases
    assert dev.info.mac_address == "deadbeefcafe"
    assert dev.info.filesystem.total == 256
    assert dev.info.filesystem.used == 32

    # State — nl / udpn / lor are the three non-defaulted State fields that,
    # missing, would raise. Empty {} for nl/udpn is intentional: dataclass
    # defaults are correct for a device that doesn't implement nightlight or
    # WLED-native UDP sync.
    assert dev.state.on is True
    assert dev.state.brightness == 20
    assert dev.state.nightlight.on is False     # default
    assert dev.state.sync.receive is False      # default (udpn: {})
    assert int(dev.state.live_data_override) == 0  # LiveDataOverride.OFF

    # Effects + palettes — one each is enough for HA to render pickers.
    assert dev.effects and len(dev.effects) >= 1
    assert dev.palettes and len(dev.palettes) >= 1


def test_all_python_wled_required_fields_present():
    """The compile-time check python-wled gives us: every field WITHOUT a
    dataclass default MUST be present in the JSON, or the parse raises. Guard
    against a future python-wled release adding a new required field going
    unnoticed until an HA user's config flow crashes."""
    d = json.loads(GOLDEN_JSON)
    # Info.filesystem is the ONLY Info field without a default; State.nl / udpn
    # / lor are the three required State fields. This test doubles as an audit:
    # if the assertion set below stops matching the model at some point, the
    # writer needs the new field too.
    assert "fs" in d["info"], "python-wled Info.filesystem has no default"
    assert "nl" in d["state"], "python-wled State.nightlight has no default"
    assert "udpn" in d["state"], "python-wled State.sync has no default"
    assert "lor" in d["state"], "python-wled State.live_data_override has no default"

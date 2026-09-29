"""Every scenario runs on at least one tier.

A scenario declares `mode` to say what shape it expects the world to be in, and the two runners read it differently: in-process, a `mutate` scenario replays its own fixture, while live the device IS the fixture and a `construct` scenario is skipped because main.cpp owns the top-level tree. A file with no `mode` defaults to `construct`, which the live runner then skips.

Combined with `live_only`, which the in-process runner skips, a file can declare both and run NOWHERE. That is what happened: `scenario_Network_hardware_reconfigures_live` carried `live_only` and no `mode`, so it was skipped in-process for being live-only and skipped live for being construct. Nothing failed, no output said so, and its observation block stayed empty across every target while four archived scenarios were deleted on the strength of it covering them.
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCENARIOS = sorted((ROOT / "test" / "scenarios").rglob("scenario_*.json"))


def test_there_are_scenarios_to_check():
    # A glob that matched nothing would make every assertion below vacuously true.
    assert SCENARIOS, "no scenario files found"


def test_no_scenario_is_skipped_by_both_runners():
    stranded = []
    for path in SCENARIOS:
        doc = json.loads(path.read_text(encoding="utf-8"))
        # `construct` is the default, and it is the value the live runner skips.
        mode = doc.get("mode", "construct")
        if doc.get("live_only") and mode != "mutate":
            stranded.append(f"{path.name}: live_only with mode={mode}")
    assert not stranded, "these run on neither tier: " + "; ".join(stranded)


def test_a_scenario_with_a_fixture_declares_mutate():
    # A fixture is the in-process stand-in for a wired device, which is what mutate means.
    # Without the field the file defaults to construct and the live runner skips it, so the
    # fixture is built for a tier the scenario then never reaches.
    wrong = []
    for path in SCENARIOS:
        doc = json.loads(path.read_text(encoding="utf-8"))
        if doc.get("fixture") and doc.get("mode", "construct") != "mutate":
            wrong.append(f"{path.name}: fixture with mode={doc.get('mode', '(absent)')}")
    assert not wrong, "these carry a fixture the live tier never honours: " + "; ".join(wrong)

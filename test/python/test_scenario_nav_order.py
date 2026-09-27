"""Scenarios run in the order the interface lists its cards, read from the interface itself.

A suite that ran alphabetically told its story out of order, and the clip following it jumped
around the interface for no reason a viewer could see. The order has one home, `NAV_GROUPS` in
app.js, and these pin that it is read rather than restated: a second copy would drift the first
time someone reorders the nav and does not think to look in a Python file.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import _test_metadata as meta  # noqa: E402


def test_the_order_comes_from_the_interface():
    """Every card app.js names is read back, in the order it names them."""
    js = (ROOT / "src" / "ui" / "app.js").read_text()
    assert "const NAV_GROUPS = [" in js, "the nav order moved; this reader needs updating with it"
    assert meta.NAV_ORDER, "nothing was read from app.js"
    assert meta.NAV_ORDER[0] == "Control", "Control opens the nav, so it opens the suite"
    # The groups are flattened in order, so the light pipeline stays in pipeline order.
    assert meta.NAV_ORDER.index("Layouts") < meta.NAV_ORDER.index("Effects")
    assert meta.NAV_ORDER.index("Effects") < meta.NAV_ORDER.index("Drivers")


def test_scenarios_are_ordered_by_the_card_they_prove():
    """A scenario sorts by its card's nav position, whatever its filename would sort as."""
    names = [s["path"].name for s in meta.collect_scenario_files()]
    if not names:
        return                      # no scenarios present is a valid tree, not a failure
    cards = [c for c in meta.NAV_ORDER
             if any(n.lower().startswith(f"scenario_{c.replace(' ', '').lower()}") for n in names)]
    seen = []
    for name in names:
        for card in cards:
            if name.lower().startswith(f"scenario_{card.replace(' ', '').lower()}"):
                if card not in seen:
                    seen.append(card)
                break
    assert seen == cards, f"scenarios ran out of nav order: {seen} against {cards}"


def test_a_card_the_interface_does_not_name_sorts_last():
    """An unlisted card is visible at the end rather than silently anywhere."""
    ranked = meta._nav_rank({"path": Path("scenario_SomethingNew_does_a_thing.json")})
    assert ranked[0] == len(meta.NAV_ORDER)

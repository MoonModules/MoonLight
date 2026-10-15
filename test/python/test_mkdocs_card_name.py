"""A catalog card's name is what links it to its own `## <Name>, details` section.

The renderer looked the name up by splitting the title on a hard-coded list of emoji.
Every card whose emoji was missing from that list kept the emoji in its name, found no
matching section, and rendered "Details: none yet" beside a section that existed: HUB75
(🟦), NDI and HLS (🖥️) all did, silently, and check_docgen used its own regex so it
reported the page clean. These pin the one shared `card_name`, with the emoji that broke
it and with one outside every list.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

from mkdocs_hooks import _H3_RE, card_name  # noqa: E402


def test_card_name_stops_at_the_emoji_whatever_it_is():
    """The name is the title up to its first emoji, for any emoji at all."""
    assert card_name("HUB75 🟦 · panels on your own pins") == "HUB75"
    assert card_name("NDI 🖥️ · video out") == "NDI"
    assert card_name("Hue 💫 · bridge") == "Hue"
    assert card_name("NetworkReceive 🖼️🌙") == "NetworkReceive"
    # One nothing has used yet: a new emoji must not need a code change.
    assert card_name("Sunrise 🌅 · dawn") == "Sunrise"


def test_card_name_keeps_names_that_carry_their_own_punctuation():
    """A name with no emoji survives whole, brackets included."""
    assert card_name("Shared") == "Shared"
    assert card_name("LED driver 💫 · wire") == "LED driver"
    assert card_name("Fixture (DMX) 💫 · channels") == "Fixture (DMX)"


def test_every_details_section_in_the_catalogs_is_reachable():
    """Each `## <Name>, details` matches a card, so no row renders 'none yet' wrongly."""
    for page in sorted((ROOT / "docs" / "moonmodules").rglob("*.md")):
        lines = page.read_text(encoding="utf-8").split("\n")
        names = {card_name(m.group("title"))
                 for m in (_H3_RE.match(l) for l in lines) if m}
        for line in lines:
            if line.startswith("## ") and line.rstrip().endswith(", details"):
                section = line[3:].rstrip()[: -len(", details")]
                assert section in names, (
                    f"{page.relative_to(ROOT)}: '{section}, details' matches no card; "
                    f"the row would render 'Details: none yet'")

"""help.json is the module's header read as help, or its catalog card while the card exists.

The app shows these strings per module and per control, so each rule here is something a user reads: which sentence is the one line, where a control's help comes from, and that every link in it works from a device's own page."""

import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import gen_help  # noqa: E402

CLASS_COMMENT = """/// A wave surface where every beat drops a stone.
/// @card BeatRipplesEffect.gif
///
/// Ripples pass through each other and reflect off the walls.
///
/// The loudest band picks where the stone lands, see @xref{rain|the rain}.
///
/// @attribution Gomez 2000,
/// the demoscene's water.
///
/// @guide
///
/// ## Silence
///
/// Without music the rain keeps the surface moving.
///
/// ## rain
///
/// How often an idle drop falls.
///
/// @moreinfo
///
/// ## The wave equation
///
/// Each cell's next height is its neighbors averaged."""


def test_the_first_paragraph_is_the_line_and_the_rest_of_the_lead_the_summary():
    h = gen_help.class_help(CLASS_COMMENT)
    assert h["line"] == "A wave surface where every beat drops a stone."
    assert h["summary"] == ("Ripples pass through each other and reflect off the walls.\n\n"
                            "The loudest band picks where the stone lands, see the rain.")


def test_a_card_tag_names_the_picture_then_its_alt_text(demo):
    comment = "/// Water.\n/// @card PictureEffect.gif A wave on a grid\n///\n/// More."
    h = gen_help.header_help(SimpleNamespace(raw_comment=comment, get_children=demo.get_children))
    assert h["image"].endswith("/PictureEffect.gif") and h["alt"] == "A wave on a grid"


def test_a_block_tag_is_the_help_field_of_its_name_and_runs_to_the_blank_line():
    h = gen_help.class_help(CLASS_COMMENT)
    assert h["attribution"] == "Gomez 2000, the demoscene's water."
    assert "Gomez" not in h["summary"]


def test_card_names_the_image_and_stays_out_of_the_text():
    h = gen_help.class_help(CLASS_COMMENT)
    assert h["card"] == "BeatRipplesEffect.gif"
    assert "@card" not in h["line"] + h["summary"]


def test_each_guide_section_is_help_with_its_heading_as_title():
    guide = gen_help.class_help(CLASS_COMMENT)["guide"]
    assert [g["title"] for g in guide] == ["Silence", "rain"]
    assert guide[1]["text"] == "How often an idle drop falls."


def test_moreinfo_is_for_developers_and_stays_out_of_the_help():
    h = gen_help.class_help(CLASS_COMMENT)
    assert "wave equation" not in str(h)


def test_a_class_without_a_comment_has_no_line():
    assert gen_help.class_help(None)["line"] == ""


HEADER = """
struct Controls {
    void addControl(const char*, int&, int, int) {}
    void addButton(const char*) {}
    void addList(const char*) {}
    void addStatus(const char*) {}
};
class DemoEffect {
    /// How long the water keeps ringing.
    int damping = 0;
    int bare = 0;
    Controls controls_;
    void defineControls() {
        controls_.addControl("damping", damping, 0, 255);
        /// Start the show again.
        controls_.addButton("restart");
        controls_.addList("presets");   ///< The saved looks, newest first.
        controls_.addControl("bare", bare, 0, 1);
        controls_.addStatus("fps");
    }
};
"""


@pytest.fixture(scope="module")
def demo(tmp_path_factory):
    ci = pytest.importorskip("clang.cindex")
    path = tmp_path_factory.mktemp("help") / "DemoEffect.h"
    path.write_text(HEADER, encoding="utf-8")
    try:
        tu = ci.Index.create().parse(str(path), args=["-x", "c++", "-std=c++20"])
    except ci.LibclangError as e:   # no libclang on this host: nothing here can run
        pytest.skip(str(e))
    cls = next(c for c in tu.cursor.walk_preorder()
               if c.kind == ci.CursorKind.CLASS_DECL and c.spelling == "DemoEffect")
    return cls


def test_a_controls_line_is_the_comment_on_the_member_it_publishes(demo):
    assert gen_help.controls_of(demo)["damping"] == "How long the water keeps ringing."


def test_a_control_without_a_member_takes_the_comment_above_its_add_line(demo):
    assert gen_help.controls_of(demo)["restart"] == "Start the show again."


def test_a_control_without_a_member_takes_the_comment_behind_its_add_line(demo):
    assert gen_help.controls_of(demo)["presets"] == "The saved looks, newest first."


def test_a_control_with_no_comment_anywhere_has_an_empty_line(demo):
    controls = gen_help.controls_of(demo)
    assert list(controls) == ["damping", "restart", "presets", "bare", "fps"]
    assert controls["bare"] == controls["fps"] == ""


def test_a_guide_section_titled_with_a_controls_name_is_that_controls_more(demo):
    comment = ("/// Water.\n///\n/// @guide\n///\n/// ## damping\n///\n/// Low rings long."
               "\n///\n/// ## Tips\n///\n/// Use rain.")
    h = gen_help.header_help(SimpleNamespace(raw_comment=comment, get_children=demo.get_children))
    assert h["controls"]["damping"] == {"line": "How long the water keeps ringing.", "more": "Low rings long."}
    assert h["controls"]["restart"] == {"line": "Start the show again."}
    assert [g["title"] for g in h["guide"]] == ["Tips"]


CARD_PAGE = """# Effects

<a id="beatripples"></a>

### BeatRipples 💫🎶🖌️ · 2D

<img src="../../assets/light/effects/BeatRipplesEffect.gif" width="300" alt="preview">

Every beat is a stone dropped in water. It lands where the [Picture service](../core/services.md#picture) says, or [here](#pulse).

- `damping`: how long the water keeps ringing.
- `width` / `height`: the size per axis.

Origin: MoonLight original

Detail: [technical](moxygen/BeatRipplesEffect.md)

[Tests](../../reference/tests/unit-tests.md#beatripples)

### Pulse 💫 · 3D

A pulse.

## BeatRipples, details

How the stone falls.
"""


@pytest.fixture
def card(tmp_path, monkeypatch):
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    page.parent.mkdir(parents=True)
    page.write_text(CARD_PAGE, encoding="utf-8")
    monkeypatch.setattr(gen_help, "ROOT", tmp_path)
    return gen_help.card_help(page, "beatripples")


def test_a_cards_first_sentence_is_the_line_and_its_other_prose_the_summary(card):
    assert card["line"] == "Every beat is a stone dropped in water."
    assert card["summary"].startswith("It lands where the [Picture service](")


def test_a_cards_links_and_image_are_absolute_so_they_work_from_a_device(card):
    assert "(https://moonmodules.org/MoonLight/moonmodules/core/services.html#picture)" in card["summary"]
    assert "(https://moonmodules.org/MoonLight/moonmodules/light/effects.html#pulse)" in card["summary"]
    assert card["image"] == "https://moonmodules.org/MoonLight/assets/light/effects/BeatRipplesEffect.gif"


def test_a_cards_controls_read_shared_lines(card):
    assert card["controls"] == {"damping": {"line": "How long the water keeps ringing."},
                                "width": {"line": "The size per axis."},
                                "height": {"line": "The size per axis."}}


def test_a_cards_origin_is_its_attribution(card):
    assert card["attribution"] == "MoonLight original"


def test_a_cards_details_section_is_its_guide(card):
    assert card["guide"] == [{"title": "BeatRipples, details", "text": "How the stone falls."}]


def test_a_card_cut_to_its_heading_hands_the_module_to_its_header(tmp_path, monkeypatch, card):
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    page.write_text(CARD_PAGE.replace("A pulse.", ""), encoding="utf-8")
    assert gen_help.card_help(page, "pulse") is None


def test_a_registry_link_to_no_card_reads_nothing(tmp_path, monkeypatch, card):
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    assert gen_help.card_help(page, "picture") is None
    assert gen_help.card_help(page.with_name("absent.md"), "beatripples") is None


def test_a_link_outside_the_docs_points_at_the_file_on_github(tmp_path, monkeypatch):
    monkeypatch.setattr(gen_help, "ROOT", tmp_path)
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    assert gen_help.site_url("../../../src/light/X.h", page) == \
        "https://github.com/MoonModules/MoonLight/blob/main/src/light/X.h"


HELP = {"line": "A picture on the layer.",
        "summary": "Read [the service](https://moonmodules.org/MoonLight/moonmodules/core/services.html#picture).",
        "image": "https://moonmodules.org/MoonLight/assets/light/effects/PictureEffect.gif",
        "alt": "A test card",
        "attribution": "Gomez 2000",
        "controls": {"fit": {"line": "Keep the aspect.", "more": "Long."}, "file": {"line": "FSEQ only."}},
        "doc": "light/effects.md#picture",
        "source": "https://github.com/MoonModules/MoonLight/blob/main/src/light/effects/PictureEffect.h",
        "tests": "https://github.com/MoonModules/MoonLight/blob/main/test/unit/light/unit_PictureEffect.cpp"}


@pytest.fixture
def hooks(monkeypatch):
    import mkdocs_hooks
    monkeypatch.setitem(mkdocs_hooks._help_cache, "modules", {HELP["doc"]: HELP})
    return mkdocs_hooks


def test_a_card_cut_to_its_heading_is_drawn_from_the_help(hooks):
    page = '<a id="picture"></a>\n\n### Picture 🖼️ · 2D\n\n<a id="sine"></a>\n\n### Sine\n\nA sine.\n'
    out = hooks.fill_cards_from_help(page, "moonmodules/light/effects.md")
    assert '<img src="../../assets/light/effects/PictureEffect.gif" width="300" alt="A test card">' in out
    assert "A picture on the layer. Read [the service](../core/services.md#picture)." in out
    assert "- `fit`: keep the aspect." in out          # a card writes `name: what it does`
    assert "- `file`: FSEQ only." in out               # an acronym keeps its capitals
    assert "Origin: Gomez 2000" in out
    assert "Detail: [technical](moxygen/PictureEffect.md)" in out
    assert "[Tests](../../reference/tests/unit-tests.md#pictureeffect)" in out   # the site's own page, by @module
    assert out.endswith('<a id="sine"></a>\n\n### Sine\n\nA sine.\n')   # the next card keeps its anchor


def test_a_card_with_a_body_is_left_as_written(hooks):
    page = '<a id="picture"></a>\n\n### Picture 🖼️ · 2D\n\nWritten by hand.\n'
    assert hooks.fill_cards_from_help(page, "moonmodules/light/effects.md") == page


def test_a_link_to_the_same_page_becomes_its_anchor(hooks):
    md = "[here](https://moonmodules.org/MoonLight/moonmodules/light/effects.html#sine)"
    assert hooks._site_relative(md, "moonmodules/light/effects.md") == "[here](#sine)"


def test_an_absolute_link_or_picture_stays_as_written(tmp_path, monkeypatch):
    monkeypatch.setattr(gen_help, "ROOT", tmp_path)
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    url = "https://raw.githubusercontent.com/scottrbailey/WLED-Utils/main/gifs/FX_008.gif"
    assert gen_help.site_url(url, page) == url


def test_card_markdown_becomes_inline_text_the_help_can_show():
    """Help shows inline text, so a card's code block, table and video are left out rather than shown as raw markup, and a heading or a bullet keeps its words."""
    out = gen_help.plain(["#### Wiring", "<video src=x controls></video>", "```sh", "ffplay x", "```",
                          "| a | b |", "|---|---|", "- read-only: the level", "- `gain`: how loud", "Plain <span class=\"x\">words</span>."])
    assert out == ["**Wiring**", "read-only: the level", "- `gain`: how loud", "Plain words."]

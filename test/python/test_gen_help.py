"""help.json is the module's header read as help, or its catalog card while the card exists.

The app shows these strings per module and per control, so each rule here is something a user reads: which sentence is the one line, where a control's help comes from, and that every link in it works from a device's own page."""

import re
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


def test_a_ref_anywhere_in_the_help_becomes_the_link_its_class_name_resolves_to():
    data = {"line": "The sibling of @ref CircleModifier.", "guide": [{"text": "See @ref Layer, then @ref Layer."}]}
    out = gen_help.resolve_refs(data, lambda name: f"[{name}](#{name.lower()})")
    assert out == {"line": "The sibling of [CircleModifier](#circlemodifier).",
                   "guide": [{"text": "See [Layer](#layer), then [Layer](#layer)."}]}


def test_an_attribution_names_catalog_entries_and_keeps_an_unlisted_name_as_written(tmp_path):
    page = tmp_path / "attributions.md"
    page.write_text("| Name | Known for |\n|---|---|\n| [Brandon502 / WildCats08](https://github.com/Brandon502) | Rotate |\n| Sören | The installer. |\n")
    catalog = gen_help.attribution_catalog(page)
    assert catalog == {"brandon502": "https://github.com/Brandon502", "wildcats08": "https://github.com/Brandon502", "sören": None}
    text, unlisted = gen_help.resolve_attribution("WildCats08, Sören, @Troy", catalog)
    assert text == "[WildCats08](https://github.com/Brandon502), Sören, @Troy"   # an entry with no page is listed, and shows as written
    assert unlisted == ["@Troy"]


def test_every_attribution_catalog_row_is_read():
    text = gen_help.ATTRIBUTIONS.read_text()
    rows = list(gen_help.catalog_rows(text))
    assert len(rows) == sum(1 for ln in text.splitlines() if ln.startswith("| ")) - 1   # all but the header
    assert all(names and all(names) for _, names, _, _ in rows)


def test_a_ref_to_a_module_links_its_card_and_to_a_class_its_technical_page_under_its_own_name():
    layer = SimpleNamespace(location=SimpleNamespace(file=SimpleNamespace(name=str(gen_help.ROOT / "src/light/layers/Layer.h"))))
    registered = {"CircleModifier": ("CircleModifier", "light/modifiers.md#circle")}
    definition = {"FooDriver": layer}.get
    assert gen_help.ref_link("CircleModifier", registered, definition) == "[Circle](https://moonmodules.org/MoonLight/moonmodules/light/modifiers.html#circle)"
    # A class that is no module keeps its role noun: only a module's display name drops it.
    assert gen_help.ref_link("FooDriver", registered, definition) == "[FooDriver](https://moonmodules.org/MoonLight/moonmodules/light/moxygen/Layer.html)"
    with pytest.raises(LookupError, match="@ref Nosuch"):
        gen_help.ref_link("Nosuch", registered, definition)


def test_the_role_nouns_and_the_technical_page_path_match_the_app_and_the_firmware():
    """Three homes derive a display name and two a technical page; this pins them to one another, so a noun added in one place cannot rename a module in only some of them."""
    js = (gen_help.ROOT / "src/ui/help.js").read_text()
    cpp = (gen_help.ROOT / "src/core/util/ModuleFactory.h").read_text()
    js_nouns = re.search(r"const ROLE_NOUNS = \[([^\]]+)\]", js).group(1)
    assert set(re.findall(r'"(\w+)"', js_nouns)) == set(gen_help._ROLE_NOUNS)
    assert set(re.findall(r'suffix = "(\w+)"', cpp)) == set(gen_help._ROLE_NOUNS)
    template = re.search(r"`(https://moonmodules\.org/MoonLight/moonmodules/\$\{m\[1\]\}/moxygen/\$\{m\[2\]\}\.html)`", js).group(1)
    layer = SimpleNamespace(location=SimpleNamespace(file=SimpleNamespace(name=str(gen_help.ROOT / "src/light/layers/Layer.h"))))
    assert gen_help.technical_url(layer) == template.replace("${m[1]}", "light").replace("${m[2]}", "Layer")


def test_a_link_reads_the_type_as_the_app_names_it_without_its_role_noun():
    assert gen_help.display_name("CircleModifier") == "Circle"
    assert gen_help.display_name("Layer") == "Layer"
    assert gen_help.display_name("Effect") == "Effect"


def test_a_class_that_is_no_module_links_to_its_headers_technical_page():
    cls = SimpleNamespace(location=SimpleNamespace(file=SimpleNamespace(name=str(gen_help.ROOT / "src/light/layers/Layer.h"))))
    assert gen_help.technical_url(cls) == "https://moonmodules.org/MoonLight/moonmodules/light/moxygen/Layer.html"


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


KINDS = """
enum class ModuleRole { Modifier };
struct ModifierBase {
    virtual ModuleRole role() const { return ModuleRole::Modifier; }
    virtual bool hasModifyLive() const { return false; }
    virtual void tick() {}
    virtual bool consumeNeedsRebuild() { return false; }
};
struct Baked : ModifierBase {};
struct Spinning : ModifierBase { bool hasModifyLive() const override { return true; } void tick() override {} };
struct Shuffling : ModifierBase { void tick() override {} bool consumeNeedsRebuild() override { return true; } };
struct Recompiling : ModifierBase { bool consumeNeedsRebuild() override { return true; } };
"""


def test_a_modifiers_kind_is_dynamic_animated_or_static_as_its_overrides_say(tmp_path):
    """Dynamic remaps per frame; animated rebuilds on its own clock, a tick of its own asking for the rebuild; a rebuild asked for without a clock, as a recompile does, stays static."""
    ci = pytest.importorskip("clang.cindex")
    path = tmp_path / "Kinds.h"
    path.write_text(KINDS, encoding="utf-8")
    try:
        tu = ci.Index.create().parse(str(path), args=["-x", "c++", "-std=c++20"])
    except ci.LibclangError as e:
        pytest.skip(str(e))
    kind = {c.spelling: gen_help.identity(c).get("kind") for c in tu.cursor.walk_preorder()
            if c.kind == ci.CursorKind.STRUCT_DECL and c.is_definition() and c.spelling != "ModifierBase"}
    assert kind == {"Baked": "static", "Spinning": "dynamic", "Shuffling": "animated", "Recompiling": "static"}


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


def test_the_attribution_catalog_shows_avatars_and_the_modules_that_name_each_entry(monkeypatch):
    import mkdocs_hooks
    modules = {"RotateModifier": {"doc": "light/modifiers.md#rotate", "attribution": "[Brandon502](https://github.com/Brandon502), [Rotation matrix](https://en.wikipedia.org/wiki/Rotation_matrix)"},
               "CheckerboardModifier": {"doc": "light/modifiers.md#checkerboard", "attribution": "[WildCats08](https://github.com/Brandon502)"},
               "InstallerModule": {"doc": "core/system.md#installer", "attribution": "Sören"}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", {"modules": modules})
    page = "| Name | Known for |\n|------|-----------|\n| [Brandon502 / WildCats08](https://github.com/Brandon502) | Modifiers. |\n| Sören | The installer. |\n| [NDI](https://en.wikipedia.org/wiki/NDI) | Video. |"
    assert mkdocs_hooks.attributed_by(page).split("\n") == [
        "| Name | Known for | Attributed by |",
        "|------|-----------|---|",
        '| [Brandon502 / WildCats08](https://github.com/Brandon502)<br><img src="https://github.com/Brandon502.png?size=96" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover"> | Modifiers. | [Checkerboard](../moonmodules/light/modifiers.md#checkerboard), [Rotate](../moonmodules/light/modifiers.md#rotate) |',
        "| Sören | The installer. | [Installer](../moonmodules/core/system.md#installer) |",
        "| [NDI](https://en.wikipedia.org/wiki/NDI) | Video. |  |"]


def test_a_catalog_row_naming_a_commons_file_shows_it_linked_to_its_file_page(monkeypatch):
    import mkdocs_hooks
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", {"modules": {}})
    page = "| Name | Known for |\n|---|---|\n| [Bresenham](https://en.wikipedia.org/wiki/Bresenham) File:Bresenham.svg | Lines. |"
    assert mkdocs_hooks.attributed_by(page).split("\n")[2] == (
        '| [Bresenham](https://en.wikipedia.org/wiki/Bresenham)<br><a href="https://commons.wikimedia.org/wiki/File:Bresenham.svg">'
        '<img src="https://commons.wikimedia.org/wiki/Special:FilePath/Bresenham.svg?width=96" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover"></a> | Lines. |  |')
    assert list(gen_help.catalog_rows(page))[0][1:] == (["Bresenham"], "https://en.wikipedia.org/wiki/Bresenham", "Bresenham.svg")


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


def test_a_catalog_whose_page_is_gone_is_generated_from_the_help(monkeypatch):
    """The page keeps its name and every anchor, so the registry's links and the site's hold: its intro, a heading per module in name order with its kind, and each guide as a details section."""
    import mkdocs_hooks
    help_file = {"catalogs": {"light/demo.md": {"title": "Demos", "line": "What a demo is.", "summary": "One.\n\nTwo."}},
                 "modules": {"ZetaModifier": {"doc": "light/demo.md#zeta", "tags": "💫", "kind": "dynamic", "line": "Z."},
                             "AlphaModifier": {"doc": "light/demo.md#alpha", "tags": "💫", "kind": "static", "line": "A.",
                                               "guide": [{"title": "Tips", "text": "Use it."}]}}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", help_file)
    md = mkdocs_hooks.generated_catalog("moonmodules/light/demo.md")
    assert md.startswith("# Demos\n\nWhat a demo is.\n\nOne.\n\nTwo.\n\n<noscript>")   # the table shows only without JavaScript
    assert '<div class="mm-deck" data-page="light/demo.md" markdown="1">' in md
    assert md.index("### Zeta") < md.index("</div>") < md.index("## Alpha, details")   # the cards in the deck, the details after it
    assert md.index('<a id="alpha"></a>') < md.index('<a id="zeta"></a>')
    assert "### Alpha 💫 · static" in md and "### Zeta 💫 · dynamic" in md
    assert "## Alpha, details\n\n#### Tips\n\nUse it." in md
    assert mkdocs_hooks.generated_catalog("moonmodules/light/other.md") is None


def test_a_catalog_intro_shows_its_picture_above_the_line_and_its_clip_below_the_summary(monkeypatch):
    import mkdocs_hooks
    help_file = {"catalogs": {"light/demo.md": {"title": "Demos", "line": "What a demo is.", "summary": "One.",
                                                "image": "https://moonmodules.org/MoonLight/assets/core/Demos.png", "alt": "The Demos card",
                                                "video": "https://moonmodules.org/MoonLight/assets/moontube/demo.webm", "caption": "Demos at work."}},
                 "modules": {}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", help_file)
    md = mkdocs_hooks.generated_catalog("moonmodules/light/demo.md")
    assert md.startswith('# Demos\n\n<img src="../../assets/core/Demos.png" width="300" alt="The Demos card">\n\nWhat a demo is.\n\nOne.\n\n'
                         '<video src="../../assets/moontube/demo.webm" controls playsinline width="360" title="Demos at work."></video>')


def test_a_closing_div_ends_the_last_card_so_it_is_still_drawn(hooks):
    """A deck holds its cards in a container, and the last card ended at the next heading, past the container's end, so a card read as having a body and was left blank."""
    page = '<div class="mm-deck" markdown="1">\n\n<a id="picture"></a>\n\n### Picture 🖼️ · 2D\n\n</div>\n\n## After\n'
    out = hooks.fill_cards_from_help(page, "moonmodules/light/effects.md")
    assert out.index("- `fit`: keep the aspect.") < out.index("</div>")


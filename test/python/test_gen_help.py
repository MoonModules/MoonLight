"""help.json is the module's header read as help, or its catalog card while the card exists.

The app shows these strings per module and per control, so each rule here is something a user reads: which sentence is the one line, where a control's help comes from, and that every link in it works from a device's own page."""

import re
from pathlib import Path
from types import SimpleNamespace

import pytest

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
/// @technical
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
    h = gen_help.header_help(SimpleNamespace(raw_comment=comment, get_children=demo.get_children, hash=demo.hash, spelling=demo.spelling))
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


def test_technical_is_for_developers_and_stays_out_of_the_help():
    h = gen_help.class_help(CLASS_COMMENT)
    assert "wave equation" not in str(h)


def test_a_class_without_a_comment_has_no_line():
    assert gen_help.class_help(None)["line"] == ""


def test_a_ref_anywhere_in_the_help_becomes_the_link_its_class_name_resolves_to():
    data = {"line": "The sibling of @ref CircleModifier.", "guide": [{"text": "See @ref Layer, then @ref Layer \"the layer\"'s picture."}]}
    out = gen_help.resolve_refs(data, lambda name, text: f"[{text or name}](#{name.lower()})")
    assert out == {"line": "The sibling of [CircleModifier](#circlemodifier).",
                   "guide": [{"text": "See [Layer](#layer), then [the layer](#layer)'s picture."}]}   # Doxygen's `@ref Name "text"` reads as its text


def test_an_attribution_names_catalog_entries_and_keeps_an_unlisted_name_as_written(tmp_path):
    page = tmp_path / "attributions.md"
    page.write_text("| Name | Known for |\n|---|---|\n| [Brandon502 / WildCats08](https://github.com/Brandon502) | Rotate |\n| Sören | The installer. |\n")
    catalog = gen_help.attribution_catalog(page)
    assert catalog == {"brandon502": "brandon502", "wildcats08": "brandon502", "sören": "soren"}   # every name of an entry to its card
    text, unlisted = gen_help.resolve_attribution("WildCats08, Sören, @Troy", catalog)
    cards = gen_help.ATTRIBUTIONS_PAGE
    assert text == f"[WildCats08]({cards}#brandon502), [Sören]({cards}#soren), @Troy"   # our card, where the entry's own link waits, even for an entry with no page
    assert unlisted == ["@Troy"]


def test_every_attribution_catalog_entry_has_a_card_anchor_of_its_own():
    anchors = [gen_help.attribution_anchor(names) for _, names, _, _ in gen_help.catalog_rows(gen_help.ATTRIBUTIONS.read_text())]
    assert all(anchors) and len(set(anchors)) == len(anchors)   # a shared anchor sends a module's attribution to another entry's card


def test_every_attribution_catalog_row_is_read():
    text = gen_help.ATTRIBUTIONS.read_text()
    rows = list(gen_help.catalog_rows(text))
    assert len(rows) == sum(1 for ln in text.splitlines() if ln.startswith("| ")) - 1   # all but the header
    assert all(names and all(names) for _, names, _, _ in rows)


def test_every_attribution_entry_has_a_kind_the_deck_filters_by():
    """The deck's chips are the six kinds, so an entry with none, or with another emoji, has no chip to find it by."""
    text = gen_help.ATTRIBUTIONS.read_text()
    kinds = set("".join(re.findall(r"(\S+) an? ", text.partition("The deck filters by kind:")[2].partition("\n")[0])))
    assert len(kinds) == 6
    lines = text.splitlines()
    for i, _, _, _ in gen_help.catalog_rows(text):
        kind = lines[i].split("|")[2].strip()
        assert kind and set(kind) <= kinds, lines[i][:60]


def test_a_ref_to_a_module_links_its_card_and_to_a_class_its_header_under_its_own_name():
    layer = SimpleNamespace(location=SimpleNamespace(file=SimpleNamespace(name=str(gen_help.ROOT / "src/light/layers/Layer.h"))))
    registered = {"CircleModifier": ("CircleModifier", "light/modifiers.md#circle")}
    definition = {"FooDriver": layer}.get
    assert gen_help.ref_link("CircleModifier", registered, definition) == "[Circle](https://moonmodules.org/MoonLight/moonmodules/light/modifiers.html#circle)"
    # A class that is no module keeps its role noun: only a module's display name drops it.
    assert gen_help.ref_link("FooDriver", registered, definition) == "[FooDriver](https://github.com/MoonModules/MoonLight/blob/main/src/light/layers/Layer.h)"
    assert gen_help.ref_link("CircleModifier", registered, definition, text="a circle").startswith("[a circle](")
    with pytest.raises(LookupError, match="@ref Nosuch"):
        gen_help.ref_link("Nosuch", registered, definition)


def test_the_role_nouns_match_the_app_and_the_firmware():
    """Three homes derive a display name; this pins them to one another, so a noun added in one place cannot rename a module in only some of them."""
    js = (gen_help.ROOT / "src/ui/help.js").read_text()
    cpp = (gen_help.ROOT / "src/core/util/ModuleFactory.h").read_text()
    js_nouns = re.search(r"const ROLE_NOUNS = \[([^\]]+)\]", js).group(1)
    assert set(re.findall(r'"(\w+)"', js_nouns)) == set(gen_help._ROLE_NOUNS)
    assert set(re.findall(r'suffix = "(\w+)"', cpp)) == set(gen_help._ROLE_NOUNS)


def test_a_picture_is_named_by_its_file_and_found_under_the_assets():
    assert gen_help.asset_url("RotateModifier.gif").endswith("/assets/light/modifiers/RotateModifier.gif")
    assert gen_help.asset_url("nosuch.gif") is None


def test_a_link_reads_the_type_as_the_app_names_it_without_its_role_noun():
    assert gen_help.display_name("CircleModifier") == "Circle"
    assert gen_help.display_name("Layer") == "Layer"
    assert gen_help.display_name("Effect") == "Effect"


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


def parsed(path: Path, source: str):
    """The translation unit clang reads from `source` written to `path`, with the bindings; the test skips on a host without libclang."""
    ci = pytest.importorskip("clang.cindex")
    path.write_text(source, encoding="utf-8")
    try:
        return ci, ci.Index.create().parse(str(path), args=["-x", "c++", "-std=c++20"])
    except ci.LibclangError as e:   # no libclang on this host: nothing here can run
        pytest.skip(str(e))


@pytest.fixture(scope="module")
def demo(tmp_path_factory):
    ci, tu = parsed(tmp_path_factory.mktemp("help") / "DemoEffect.h", HEADER)
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
    ci, tu = parsed(tmp_path / "Kinds.h", KINDS)
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


DRIVERS = """
struct Controls { void addControl(const char*, int&) {} void addButton(const char*) {} };
struct DriverBase {
    Controls controls_;
    /// This driver's dim.
    int localBrightness = 0;
    /// The first light it reads.
    /// @guide Two drivers can share one buffer.
    int start = 0;
    void defineControls() {
        if (hasCorrectionControls()) defineCorrectionControls();
        if (hasWindowControls()) addWindowControls();
        defineDriverControls();
    }
    virtual bool hasCorrectionControls() const { return true; }
    virtual bool hasWindowControls() const { return false; }
    void defineCorrectionControls() { controls_.addControl("localBrightness", localBrightness); }
    void addWindowControls() { controls_.addControl("start", start); }
    virtual void defineDriverControls() {}
};
struct WireDriver : DriverBase {
    /// The data pins.
    int pins = 0;
    bool hasWindowControls() const override { return true; }
    void defineDriverControls() override { controls_.addControl("pins", pins); }
};
struct BridgeDriver : DriverBase {
    /// The bridge's address.
    int bridge = 0;
    bool hasCorrectionControls() const override { return false; }
    bool bare() const { return false; }
    void defineDriverControls() override {
        controls_.addControl("bridge", bridge);
        /// Claim a key.
        /// @guide Press the bridge's own button first.
        controls_.addButton("pair");
        if (!bare()) addWindowControls();
    }
};
"""


def test_a_base_classs_template_method_and_its_helpers_publish_the_controls_a_driver_shows(tmp_path):
    """A driver adds its own controls in the hook its base's `defineControls` calls, between the base's own blocks, and a block the driver opts out of is not its control."""
    ci, tu = parsed(tmp_path / "Drivers.h", DRIVERS)
    cls = {c.spelling: c for c in tu.cursor.walk_preorder() if c.kind == ci.CursorKind.STRUCT_DECL and c.is_definition()}
    assert list(gen_help.controls_of(cls["WireDriver"])) == ["localBrightness", "start", "pins"]
    assert gen_help.controls_of(cls["WireDriver"]) == {"localBrightness": "This driver's dim.", "pins": "The data pins.",
                                                      "start": "The first light it reads.\n@guide Two drivers can share one buffer."}
    # A negated branch on a literal `false` runs, and a run of `///` above an `add…` line is that control's comment.
    assert gen_help.controls_of(cls["BridgeDriver"]) == {"bridge": "The bridge's address.",
                                                        "pair": "Claim a key.\n@guide Press the bridge's own button first.",
                                                        "start": "The first light it reads.\n@guide Two drivers can share one buffer."}
    # The base's member carries the control's longer help, so every driver shows it.
    assert gen_help.header_help(cls["WireDriver"])["controls"]["start"] == {"line": "The first light it reads.",
                                                                           "more": "Two drivers can share one buffer."}


STRATEGY = """
struct ControlList { void addControl(const char*, int&) {} };
struct Driver;
struct Peripheral {
    Driver* owner_ = nullptr;
    virtual void addBusControls(ControlList&) {}
    virtual int speed() const { return 0; }
};
struct Driver {
    ControlList controls_;
    Peripheral* peripheral_ = nullptr;
    /// The data pins.
    int pins = 0;
    /// The write strobe, the board's wiring.
    int clockPin = 10;
    /// Freeze each frame before it is sent.
    int snapshot = 1;
    int& snapshotRef() { return snapshot; }
    void defineControls() {
        controls_.addControl("pins", pins);
        if (peripheral_) peripheral_->addBusControls(controls_);
        if (peripheral_) peripheral_->speed();
    }
};
struct FastBus : Peripheral {
    /// Buffers the DMA circulates.
    int bufs = 12;
    void addBusControls(ControlList& c) override {
        c.addControl("clockPin", owner_->clockPin);
        c.addControl("bufs", bufs);
        c.addControl("snapshot", owner_->snapshotRef());
    }
};
struct PlainBus : Peripheral {
    /// The data/command select.
    int dcPin = 11;
    void addBusControls(ControlList& c) override { c.addControl("clockPin", owner_->clockPin); c.addControl("dcPin", dcPin); }
};
"""


def test_a_strategy_handed_the_control_list_publishes_its_controls_with_their_help(tmp_path):
    """A driver's peripheral declares controls on the driver's card through a virtual call handed the control list; each implementation can run, so each one's controls take the help on their members, an accessor's member included."""
    ci, tu = parsed(tmp_path / "Strategy.h", STRATEGY)
    cls = {c.spelling: c for c in tu.cursor.walk_preorder() if c.kind == ci.CursorKind.STRUCT_DECL and c.is_definition()}
    assert gen_help.controls_of(cls["Driver"]) == {"pins": "The data pins.", "clockPin": "The write strobe, the board's wiring.",
                                                  "bufs": "Buffers the DMA circulates.", "snapshot": "Freeze each frame before it is sent.",
                                                  "dcPin": "The data/command select."}


def test_controls_defined_in_the_cpp_beside_the_header_and_through_a_local_add_lambda_count(tmp_path):
    """A class that only declares `defineControls` is read from its `.cpp`, and a lambda named `add…` publishes as a method does."""
    header = tmp_path / "Slot.h"
    header.write_text("""struct Controls { void addControl(const char*, int&) {} };
struct Slot {
    Controls controls_;
    /// The clock pin.
    int clk = 0;
    void defineControls();
};
""", encoding="utf-8")
    ci, tu = parsed(tmp_path / "Slot.cpp", """#include "Slot.h"
void Slot::defineControls() {
    const auto addPinRow = [&](const char* name, int& pin) { controls_.addControl(name, pin); };
    addPinRow("clk", clk);
}
""")
    gen_help._CLANG[:] = [ci, ["-I", str(tmp_path)]]
    hdr = ci.Index.create().parse(str(header), args=["-x", "c++", "-std=c++20"])
    cls = next(c for c in hdr.cursor.walk_preorder() if c.kind == ci.CursorKind.STRUCT_DECL and c.spelling == "Slot")
    assert gen_help.controls_of(cls) == {"clk": "The clock pin."}


def test_a_lists_field_lines_are_its_row_fields_help_and_fields_takes_a_types_own(tmp_path):
    """A list's rows have fields of their own, each documented on one `@field` line beside the list; `@fields` takes the lines a shared type writes once, so every list using it shows the same help."""
    ci, tu = parsed(tmp_path / "Rows.h", """struct Controls { void addList(const char*, int&) {} };
/// What a row does.
/// @field target The bank it drives.
struct Action {};
struct Svc {
    Controls controls_;
    int rows = 0;
    void defineControls() {
        /// The rows.
        /// @field pin The GPIO; -1 leaves the row unused.
        /// @fields Action
        /// @guide One row per button.
        controls_.addList("rows", rows);
    }
};
""")
    cls = next(c for c in tu.cursor.walk_preorder() if c.kind == ci.CursorKind.STRUCT_DECL and c.spelling == "Svc")
    assert gen_help.control_help(gen_help.controls_of(cls)["rows"]) == {
        "line": "The rows.", "more": "One row per button.",
        "fields": {"pin": {"line": "The GPIO; -1 leaves the row unused."}, "target": {"line": "The bank it drives."}}}


def test_fields_naming_a_type_without_field_lines_stops_the_generator(tmp_path):
    """A typo in `@fields` would leave every row field without help and say nothing, so it fails as an unknown `@ref` does."""
    ci, tu = parsed(tmp_path / "Typo.h", """struct Controls { void addList(const char*, int&) {} };
/// No fields here.
struct Action {};
struct Svc {
    Controls controls_;
    int rows = 0;
    void defineControls() {
        /// The rows.
        /// @fields Actoin
        controls_.addList("rows", rows);
    }
};
""")
    cls = next(c for c in tu.cursor.walk_preorder() if c.kind == ci.CursorKind.STRUCT_DECL and c.spelling == "Svc")
    with pytest.raises(LookupError, match="@fields Actoin"):
        gen_help.controls_of(cls)


def test_a_lead_may_name_several_clips_each_with_its_title():
    _, fields = gen_help.block_tags("Lead.\n@video a.webm On a desktop.\n@video b.webm On a\ndevice.")
    assert fields["video"] == ["a.webm On a desktop.", "b.webm On a device."]


def test_a_controls_comment_is_its_line_and_its_guide_line_its_more():
    """Doxygen's brief and detailed description: the first line under the control, the `@guide` line behind its arrow."""
    assert gen_help.control_help("How long the water keeps ringing.\n@guide Low rings long.") == \
        {"line": "How long the water keeps ringing.", "more": "Low rings long."}
    assert gen_help.control_help("Start the show again.") == {"line": "Start the show again."}
    # The word inside a sentence is text, not the marker.
    assert gen_help.control_help("Use the @guide link.") == {"line": "Use the @guide link."}


def test_the_module_guide_stays_general_even_when_a_section_names_a_control(demo):
    comment = "/// Water.\n///\n/// @guide\n///\n/// ## damping\n///\n/// Low rings long.\n///\n/// ## Tips\n///\n/// Use rain."
    h = gen_help.header_help(SimpleNamespace(raw_comment=comment, get_children=demo.get_children, hash=demo.hash, spelling=demo.spelling))
    assert h["controls"]["damping"] == {"line": "How long the water keeps ringing."}
    assert [g["title"] for g in h["guide"]] == ["damping", "Tips"]   # check_docgen flags the first


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
        "tests": "https://moonmodules.org/MoonLight/reference/tests.html#pictureeffect"}


def test_the_attribution_catalog_shows_avatars_and_the_modules_that_name_each_entry(monkeypatch):
    import mkdocs_hooks
    cards = "https://moonmodules.org/MoonLight/reference/attributions.html"
    modules = {"RotateModifier": {"doc": "light/modifiers.md#rotate", "attribution": f"[Brandon502]({cards}#brandon502), [Rotation matrix]({cards}#rotation-matrix)"},
               "CheckerboardModifier": {"doc": "light/modifiers.md#checkerboard", "attribution": f"[WildCats08]({cards}#brandon502)"},
               "InstallerModule": {"doc": "core/system.md#installer", "attribution": "Sören"}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", {"modules": modules})
    monkeypatch.setattr(mkdocs_hooks, "_source_attributions", lambda modules: [("[draw.h](https://x/draw.h)", "NDI")])
    page = "| Name | Known for |\n|------|-----------|\n| [Brandon502 / WildCats08](https://github.com/Brandon502) | Modifiers. |\n| Sören | The installer. |\n| [NDI](https://en.wikipedia.org/wiki/NDI) | Video. |"
    assert mkdocs_hooks.attributed_by(page).split("\n") == [
        "| Name | Known for | Attributed by |",
        "|------|-----------|---|",
        '| [Brandon502 / WildCats08](https://github.com/Brandon502)<span id="brandon502" data-anchor></span><br><img src="https://github.com/Brandon502.png?size=96" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover"> | Modifiers. | [Checkerboard](../moonmodules/light/modifiers.md#checkerboard), [Rotate](../moonmodules/light/modifiers.md#rotate) |',
        '| Sören<span id="soren" data-anchor></span> | The installer. | [Installer](../moonmodules/core/system.md#installer) |',
        '| [NDI](https://en.wikipedia.org/wiki/NDI)<span id="ndi" data-anchor></span> | Video. | [draw.h](https://x/draw.h) |']   # code that is no module credits by its own `@attribution`


def test_a_catalog_row_naming_a_commons_file_shows_it_linked_to_its_file_page(monkeypatch):
    import mkdocs_hooks
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", {"modules": {}})
    monkeypatch.setattr(mkdocs_hooks, "_source_attributions", lambda modules: [])
    page = "| Name | Known for |\n|---|---|\n| [Bresenham](https://en.wikipedia.org/wiki/Bresenham) File:Bresenham.svg | Lines. |"
    assert mkdocs_hooks.attributed_by(page).split("\n")[2] == (
        '| [Bresenham](https://en.wikipedia.org/wiki/Bresenham)<span id="bresenham" data-anchor></span><br><a href="https://commons.wikimedia.org/wiki/File:Bresenham.svg">'
        '<img src="https://commons.wikimedia.org/wiki/Special:FilePath/Bresenham.svg?width=96" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover"></a> | Lines. |  |')
    assert list(gen_help.catalog_rows(page))[0][1:] == (["Bresenham"], "https://en.wikipedia.org/wiki/Bresenham", "Bresenham.svg")


def test_a_module_reads_as_plain_markdown_under_its_deck():
    """What search indexes and a reader without JavaScript sees: the heading, picture, line and summary, controls, attribution and links."""
    import mkdocs_hooks
    out = "\n".join(mkdocs_hooks._module_markdown("PictureEffect", HELP, "moonmodules/light/effects.md"))
    assert out.startswith('<a id="picture"></a>\n\n**Picture**')
    assert '<img src="../../assets/light/effects/PictureEffect.gif" width="300" alt="A test card">' in out
    assert "A picture on the layer. Read [the service](../core/services.md#picture)." in out
    assert "- `fit`: Keep the aspect." in out and "- `file`: FSEQ only." in out
    assert "Attribution: Gomez 2000" in out
    assert "[Source](https://github.com/MoonModules/MoonLight/blob/main/src/light/effects/PictureEffect.h)" in out   # the header on GitHub
    assert "[Tests](../../reference/tests.md#pictureeffect)" in out   # the site's own page, by @module


def test_a_modules_tests_link_opens_its_section_in_the_test_inventory():
    assert gen_help.tests_url("PictureEffect", "main") == "https://moonmodules.org/MoonLight/reference/tests.html#pictureeffect"
    assert gen_help.tests_url("NoSuchThing", "main") is None


def test_the_attribution_table_is_wrapped_in_a_deck_the_page_never_writes():
    import mkdocs_hooks
    out = mkdocs_hooks.deck_around_table("# T\n\nIntro.\n\n| Name |\n|---|\n| A |\n\nAfter.")
    assert out == '# T\n\nIntro.\n\n<div class="mm-deck" markdown="1">\n\n| Name |\n|---|\n| A |\n\n</div>\n\nAfter.'


def test_a_source_file_outside_the_modules_credits_by_its_attribution_line():
    """A header comment, a `.cpp` comment and a script's docstring each carry `@attribution`, and a module header is left to help.json."""
    import mkdocs_hooks
    found = mkdocs_hooks._source_attributions({"Fx": {"source": "https://github.com/MoonModules/MoonLight/blob/main/src/light/effects/FireEffect.h"}})
    names = {link.split("]")[0][1:]: attr for link, attr in found}
    assert names.get("draw.h") == "Bresenham, Xiaolin Wu"
    assert names.get("moonbase_main.cpp") == "MycilaSafeBoot, Tasmota"
    assert names.get("mtvoiceover.py") == "Piper, rhasspy"
    assert "FireEffect.h" not in names


def test_a_link_to_the_same_page_becomes_its_anchor():
    import mkdocs_hooks
    md = "[here](https://moonmodules.org/MoonLight/moonmodules/light/effects.html#sine)"
    assert mkdocs_hooks._site_relative(md, "moonmodules/light/effects.md") == "[here](#sine)"


def test_an_absolute_link_or_picture_stays_as_written(tmp_path, monkeypatch):
    monkeypatch.setattr(gen_help, "ROOT", tmp_path)
    page = tmp_path / "docs" / "moonmodules" / "light" / "effects.md"
    url = "https://raw.githubusercontent.com/scottrbailey/WLED-Utils/main/gifs/FX_008.gif"
    assert gen_help.site_url(url, page) == url


def test_a_catalog_whose_page_is_gone_is_generated_from_the_help(monkeypatch):
    """The page keeps its name and every anchor, so the registry's links and the site's hold: its intro, each module in name order with its kind, and each guide as a details section."""
    import mkdocs_hooks
    help_file = {"catalogs": {"light/demo.md": {"title": "Demos", "line": "What a demo is.", "summary": "One.\n\nTwo."}},
                 "modules": {"ZetaModifier": {"doc": "light/demo.md#zeta", "tags": "💫", "kind": "dynamic", "line": "Z."},
                             "AlphaModifier": {"doc": "light/demo.md#alpha", "tags": "💫", "kind": "static", "line": "A.",
                                               "guide": [{"title": "Tips", "text": "Use it."}]}}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", help_file)
    md = mkdocs_hooks.generated_catalog("moonmodules/light/demo.md")
    assert md.startswith('# Demos\n\nWhat a demo is.\n\nOne.\n\nTwo.\n\n<div class="mm-deck"')
    assert '<div class="mm-deck" data-page="light/demo.md" markdown="1">' in md
    assert md.index("**Zeta**") < md.index("</div>") < md.index("## Alpha, details")   # the cards in the deck, the details after it
    assert md.index('<a id="alpha"></a>') < md.index('<a id="zeta"></a>')
    assert "**Alpha** 💫 · static" in md and "**Zeta** 💫 · dynamic" in md
    assert "## Alpha, details\n\n#### Tips\n\nUse it." in md
    assert mkdocs_hooks.generated_catalog("moonmodules/light/other.md") is None


def test_a_written_catalog_page_is_the_intro_its_deck_follows_and_its_intro_class_is_no_card(monkeypatch):
    """A catalog no class introduces keeps its own markdown as the intro; a container that introduces its page is not drawn again as a card."""
    import mkdocs_hooks
    help_file = {"catalogs": {"core/demo.md": {"title": "Demos", "base": "Demos"}},
                 "modules": {"Demos": {"doc": "core/demo.md#demos", "line": "The container."},
                             "AlphaService": {"doc": "core/demo.md#alpha", "line": "A."}}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", help_file)
    md = mkdocs_hooks.generated_catalog("moonmodules/core/demo.md", "# Demos\n\nWritten intro.\n")
    assert md.startswith('# Demos\n\nWritten intro.\n\n<div class="mm-deck"')
    assert '<a id="alpha"></a>' in md and '<a id="demos"></a>' not in md


def test_a_catalog_intro_shows_its_picture_above_the_line_and_its_clips_side_by_side_below_the_summary(monkeypatch):
    import mkdocs_hooks
    help_file = {"catalogs": {"light/demo.md": {"title": "Demos", "line": "What a demo is.", "summary": "One.",
                                                "image": "https://moonmodules.org/MoonLight/assets/core/Demos.png", "alt": "The Demos card",
                                                "videos": [{"video": "https://moonmodules.org/MoonLight/assets/moontube/demo.webm", "caption": "Demos at work."},
                                                           {"video": "https://moonmodules.org/MoonLight/assets/moontube/demo2.webm", "caption": "On a device."}]}},
                 "modules": {}}
    monkeypatch.setitem(mkdocs_hooks._help_cache, "file", help_file)
    md = mkdocs_hooks.generated_catalog("moonmodules/light/demo.md")
    assert md.startswith('# Demos\n\n<img src="../../assets/core/Demos.png" width="300" alt="The Demos card">\n\nWhat a demo is.\n\nOne.\n\n'
                         '<video src="../../assets/moontube/demo.webm" controls playsinline title="Demos at work."></video> '
                         '<video src="../../assets/moontube/demo2.webm" controls playsinline title="On a device."></video>')



def test_the_docs_strip_the_same_role_nouns_the_app_does():
    """A module's name on the site and in the app is its type without the role noun; `gen_help` and `help.js` each keep the list, so they must agree."""
    import re as _re
    js = (gen_help.ROOT / "src" / "ui" / "help.js").read_text()
    nouns = _re.search(r"const ROLE_NOUNS = \[([^\]]*)\]", js).group(1)
    assert tuple(_re.findall(r'"(\w+)"', nouns)) == gen_help._ROLE_NOUNS


def test_a_cursor_kind_newer_than_the_bindings_reads_as_a_placeholder(tmp_path):
    """The bindings lag the library they load, so a kind they lack, here the `flag_enum` attribute, reads as a placeholder rather than raising mid-walk."""
    ci, tu = parsed(tmp_path / "Flags.h", "enum __attribute__((flag_enum)) Flags { A = 1, B = 2 };\n")
    gen_help.know_every_cursor_kind(ci)
    assert "ENUM_DECL" in [n.kind.name for n in tu.cursor.walk_preorder()]

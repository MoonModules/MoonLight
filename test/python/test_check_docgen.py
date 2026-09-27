"""check_docgen.py guards the generated documentation, so this guards it back.

The rules it enforces (documentation-standards.md § The card) are invisible when they
break: a regex that stops matching makes the check print a clean run, which is
indistinguishable from a tree with nothing wrong in it. Four `## HLS, details` headings
lived in the tree unlinked for exactly that reason, found by adding the rule rather than
by the rule working.

So each rule is pinned twice: it FIRES on a page written to break it, and it stays SILENT
on a page that obeys. A test that only asserted the silence would pass against a check
that had stopped reading anything at all.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

from check_docgen import (ANIMATED_PAGES, MAX_CONTROL, MAX_CONTROL_VISUAL,  # noqa: E402
                           MAX_DESC, MAX_DESC_VISUAL, _card_rules, _cards,
                           _header_rules, _structure)


def _card(desc: str = "Short.", controls=("- `a` — one.",)) -> str:
    return "### Thing 💫 · kind\n\n" + desc + "\n\n" + "\n".join(controls) + "\n"


# ---- sizes ----

def test_description_over_the_limit_is_measured():
    card = list(_cards(_card(desc="x" * (MAX_DESC + 50))))[0]
    assert card["desc"] > MAX_DESC


def test_a_card_within_the_limits_measures_under_them():
    card = list(_cards(_card()))[0]
    assert card["desc"] <= MAX_DESC
    assert card["widest"] <= MAX_CONTROL


def test_a_wrapped_sentence_in_a_class_comment_is_a_finding():
    # The one-line budget cannot reach here: a class comment is ALLOWED ten lines, so a
    # sentence carried onto the next line passes every other rule.
    header = ("#pragma once\nnamespace mm {\n"
              "/// A sentence that runs long and wraps onto\n"
              "/// a second line, which is what this catches.\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    wraps = [w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]
    assert len(wraps) == 1


def test_a_continuation_that_does_not_start_lowercase_is_still_a_wrap():
    # The test is the PREVIOUS line's unfinished sentence, not the next line's case. A
    # continuation beginning with a digit or an identifier is most of a technical comment,
    # and a lowercase-only test missed every one of them.
    digit = ("#pragma once\nnamespace mm {\n"
             "/// A sentence that does not end here and gives\n"
             "/// 8-16 lanes for the wall time of one.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    ident = ("#pragma once\nnamespace mm {\n"
             "/// The wire format is\n"
             "/// ParallelSlots.h in the same directory.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    for header in (digit, ident):
        assert len([w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]) == 1


def test_an_indented_block_keeps_its_layout():
    # A wire table, a listing or a diagram is a BLOCK: joining its lines destroys the layout
    # that carries the meaning. Collapsing one produced a 43-word "sentence" of protocol bytes
    # in PreviewDriver.h, which is the worst of both readings.
    table = ("#pragma once\nnamespace mm {\n"
             "/// Lead.\n///\n/// @moreinfo\n///\n/// ## S\n///\n"
             "///     POST /api            pair, once pressed\n"
             "///     GET  /api/lights     the lights by id\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    assert not [w for _, w in _header_rules("probe.h", table) if "hard wrap" in w]


def test_a_colon_or_semicolon_does_not_end_a_sentence():
    # A colon INTRODUCES what follows and a semicolon joins two clauses, so neither ends a
    # thought. Unicode's sentence-boundary rules, Vale's own detector and the standards line
    # agree ("joined by a comma or a colon that a full stop should have been"). Treating them
    # as terminators passed every `Prior art: ...` block that wrapped mid-sentence.
    colon = ("#pragma once\nnamespace mm {\n"
             "/// Prior art: one discovery, one lineage, one driver:\n"
             "/// architecture studied, never copied.\n"
             "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    semi = ("#pragma once\nnamespace mm {\n"
            "/// The difference is underneath;\n"
            "/// it buys two things the other cannot give.\n"
            "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    for header in (colon, semi):
        assert len([w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]) == 1


def test_structure_is_not_read_as_a_wrapped_sentence():
    # A list item, a heading and a table row continue across lines as STRUCTURE. Reading
    # one as wrapped prose would fire on every appendix that carries a table.
    header = ("#pragma once\nnamespace mm {\n"
              "/// A finished sentence.\n"
              "///\n"
              "/// @moreinfo\n"
              "///\n"
              "/// ## A section\n"
              "///\n"
              "/// - an item that continues\n"
              "///   onto the next line\n"
              "///\n"
              "/// | a | b |\n"
              "/// |---|---|\n"
              "class Probe {\npublic:\n    /// Fine.\n    int a = 0;\n};\n}\n")
    assert not [w for _, w in _header_rules("probe.h", header) if "hard wrap" in w]


def test_many_short_controls_are_not_a_finding():
    # A module with twelve honest controls is not worse documented than one with three.
    # Only a control that runs to a paragraph makes a card unreadable, so the cap is per
    # LINE: ten short ones stay clean however tall the column gets.
    many = [f"- `p{i}` — short." for i in range(10)]
    card = list(_cards(_card(controls=many)))[0]
    assert card["widest"] < MAX_CONTROL


def test_the_visual_catalogs_are_held_tighter():
    # An effect's gif says what it looks like, so its prose says only what the eye cannot
    # see. The SAME card passes off a still-image page and fails on a gif one, which is the
    # whole rule: asserting the constants alone would pass against a _card_rules that never
    # read them.
    VISUAL, PLAIN = "moonmodules/light/effects.md", "moonmodules/light/drivers.md"
    assert VISUAL in ANIMATED_PAGES and PLAIN not in ANIMATED_PAGES

    def findings(rel, desc_len, control_len):
        text = _card(desc="x" * desc_len, controls=("- `a` — " + "z" * control_len + ".",))
        card = list(_cards(text))[0]
        card["img"], card["img_src"] = True, ("a.gif" if rel in ANIMATED_PAGES else "a.png")
        return [w for _, w in _card_rules(rel, card)]

    # Just under the tighter caps: clean on both pages.
    assert findings(VISUAL, MAX_DESC_VISUAL - 50, MAX_CONTROL_VISUAL - 30) == []
    assert findings(PLAIN, MAX_DESC_VISUAL - 50, MAX_CONTROL_VISUAL - 30) == []

    # Between the two caps: the visual page reports, the plain page stays silent.
    over = findings(VISUAL, MAX_DESC_VISUAL + 50, MAX_CONTROL_VISUAL + 10)
    assert any("description" in w for w in over)
    assert any("one control" in w for w in over)
    assert findings(PLAIN, MAX_DESC_VISUAL + 50, MAX_CONTROL_VISUAL + 10) == []

    # Past the loose caps: both pages report.
    assert findings(PLAIN, MAX_DESC + 50, MAX_CONTROL + 10)


def test_the_widest_control_is_reported_with_its_text():
    card = list(_cards(_card(controls=("- `a` — ok.", "- `b` — " + "z" * 200)))) [0]
    assert card["widest"] > MAX_CONTROL
    assert "zzz" in card["widest_text"]


def test_links_and_image_lines_are_not_counted_as_description():
    # A card's Detail:/Tests:/Origin: lines and its <img> render in other cells, so
    # counting them as description would fail a card for text the column never holds.
    page = ('### Thing 💫 · kind\n\nShort.\n\n'
            '<img src="../../assets/x.png" alt="x">\n'
            '- `a` — one.\n'
            'Origin: Someone · source [x.h](../../src/x.h)\n'
            '[Tests](../../reference/tests/unit-tests.md#x)\n'
            'Detail: [technical](moxygen/X.md)\n')
    card = list(_cards(page))[0]
    assert card["desc"] == len("Short.")


def test_a_details_section_is_not_counted_into_the_card():
    # `## ` closes the block: the details prose belongs to the section, not the row.
    page = _card() + "\n## Thing — details\n\n" + "w" * 5000 + "\n"
    card = list(_cards(page))[0]
    assert card["desc"] <= MAX_DESC


# ---- structure ----

def test_details_above_a_card_is_flagged():
    page = _card() + "\n## Thing, details\n\nWhy.\n\n### Other 💫 · kind\n\nShort.\n"
    issues = _structure(page, "p.md")
    assert issues and "above the last card" in issues[0][1]


def test_details_below_every_card_is_accepted():
    page = _card() + "\n### Other 💫 · kind\n\nShort.\n\n## Thing, details\n\nWhy.\n"
    assert _structure(page, "p.md") == []


def test_a_details_heading_without_a_comma_is_flagged():
    # The build matches `, details` exactly to link a row to its section, so any other
    # punctuation renders a section nothing points at. The em-dash form is in this list
    # because it is banned repo-wide, not merely unmatched.
    for bad in ("## Thing — details", "## Thing: details", "## Thing details"):
        page = _card() + "\n" + bad + "\n\nWhy.\n"
        issues = _structure(page, "p.md")
        assert issues and "comma" in issues[0][1], bad


def test_details_naming_no_card_is_flagged():
    page = _card() + "\n## Ghost, details\n\nWhy.\n"
    issues = _structure(page, "p.md")
    assert issues and "names no card" in issues[0][1]


def test_two_details_sections_with_one_name_are_flagged():
    """Both slug to the same anchor, so the row's More: link reaches one and the other
    is unreachable. A sweep that adds a section where one already exists does this
    silently, which is how drivers.md ended up with two LED driver sections."""
    page = (_card() + "\n## Thing, details\n\nOne.\n\n## Thing, details\n\nTwo.\n")
    issues = _structure(page, "p.md")
    assert issues and "second details section" in issues[0][1]


def test_one_details_section_per_name_is_accepted():
    page = (_card() + "\n### Other 💫 · kind\n\nShort.\n\n"
            "## Thing, details\n\nOne.\n\n## Other, details\n\nTwo.\n")
    assert _structure(page, "p.md") == []


def test_a_section_heading_that_is_not_details_is_left_alone():
    # `## LED drivers` is a group header, not a details section: it must not be judged
    # by the details rules, or every catalog page would fail on its own structure.
    page = "## LED drivers\n\n" + _card() + "\n## Network drivers\n\n" + _card()
    assert _structure(page, "p.md") == []


# ---- the rendered row: two columns, labeled links ----

def _row(md: str) -> str:
    """One rendered table row, through the build's own renderer."""
    import mkdocs_hooks
    return mkdocs_hooks._render_catalog_table(md)


def test_a_row_renders_as_two_columns():
    """Two, not three or four. Every added column divides the page again, which is what
    turned a long description into a ribbon and made a three-link card taller than its
    own controls."""
    out = _row(_card())
    row = [l for l in out.split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    assert row.count(" |") == 2, row


def test_the_header_names_the_two_columns():
    out = _row(_card())
    assert "| Module | Details |" in out


def test_links_are_labelled_not_only_iconed():
    """An icon alone made the reader decode a glyph, and the tests and API rows both
    render as a list of near-identical module names. The WORD carries the distinction."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            '[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    out = _row(page)
    assert "**Tests:**" in out and "**API:**" in out


def test_attribution_is_not_linked_in_the_second_column():
    """Attribution travels with the code it credits, in the header's `///` where the
    generated page shows it. A `Source:` row under the controls duplicated it and made the
    row length vary per card."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            '[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n'
            'Origin: MoonLight, by somebody\n')
    out = _row(page)
    assert "**Source:**" not in out
    assert "by somebody" not in out


def test_all_three_links_show_even_when_a_target_is_missing():
    """A card with no tests is a gap worth seeing. A row that silently drops the label
    hides it, and the three rows stop being in the same place on every card."""
    out = _row(_card())
    for label in ("**Tests:**", "**API:**", "**Details:**"):
        assert label in out, label
    assert out.count("none yet") == 3


def test_the_three_labels_keep_their_order():
    """Same three, same sequence, so position carries meaning across cards."""
    import re as _re
    out = _row(_card())
    assert _re.findall(r"\*\*(Tests|API|Details):\*\*", out) == ["Tests", "API", "Details"]


def test_a_gif_on_a_non_animated_page_is_flagged():
    """The format rule runs both ways: a png on effects was caught, a gif on drivers was
    not, so half the convention went unenforced."""
    import check_docgen
    gif = ('### Thing 💫 · kind\n\n<img src="../../assets/x.gif" alt="x">\n\n'
           'Short.\n\n- `a` — one.\n')
    card = list(_cards(gif))[0]
    assert card["img_src"].endswith(".gif")
    assert "moonmodules/light/drivers.md" not in check_docgen.ANIMATED_PAGES


def test_a_card_with_no_image_is_reported():
    """Every card leads with a picture, and the rule must FIRE on one that does not: the
    assertions beside this one read _cards() only, which is the data rather than the rule."""
    import check_docgen
    findings = check_docgen._card_rules("moonmodules/light/drivers.md",
                                        list(_cards(_card()))[0])
    assert any("no image" in why for _, why in findings)


def test_a_summary_page_row_needs_no_image():
    """The image rule is a CATALOG rule: a reader picks an effect by looking at it, where a
    summary row names a base class with no card in the UI to capture. The exemption is scoped
    to that one rule, so an over-long description on the same page is still reported."""
    import check_docgen
    page = sorted(check_docgen.PREVIEWLESS_PAGES)[0]
    assert page not in check_docgen.ANIMATED_PAGES
    # Silent on the missing image...
    plain = list(_cards(_card()))[0]
    assert not any("no image" in why for _, why in check_docgen._card_rules(page, plain))
    # ...and still loud on everything else the card rules measure.
    fat = list(_cards(_card(desc="x" * (MAX_DESC + 50))))[0]
    assert any("description" in why for _, why in check_docgen._card_rules(page, fat))


def test_the_image_format_rule_fires_both_ways():
    """A png on an animated page and a gif on a static one are each half the convention,
    and a rule that only caught one left the other unenforced for four pages."""
    import check_docgen
    animated = sorted(check_docgen.ANIMATED_PAGES)[0]
    png = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\n'
           'Short.\n\n- `a`: one.\n')
    gif = ('### Thing 💫 · kind\n\n<img src="../../assets/x.gif" alt="x">\n\n'
           'Short.\n\n- `a`: one.\n')
    on_animated = check_docgen._card_rules(animated, list(_cards(png))[0])
    assert any("image is png" in why for _, why in on_animated)
    on_static = check_docgen._card_rules("moonmodules/light/drivers.md",
                                         list(_cards(gif))[0])
    assert any("image is gif" in why for _, why in on_static)
    # And silent the right way round: a gif on the animated page, a png on the static one.
    assert not any("image is" in why for _, why
                   in check_docgen._card_rules(animated, list(_cards(gif))[0]))
    assert not any("image is" in why for _, why
                   in check_docgen._card_rules("moonmodules/light/drivers.md",
                                               list(_cards(png))[0]))


def test_an_effect_card_needs_a_gif_not_a_png():
    """Effects, modifiers and layouts show MOTION: a still frame of a moving effect says
    almost nothing about it. Everything else is cards and controls, where a png is
    sharper and smaller."""
    import check_docgen
    png = ('### Thing 💫 · 2D\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n- `a` — one.\n')
    issues = check_docgen._structure(png, "moonmodules/light/effects.md")
    # _structure covers placement; the format rule lives with the per-card checks, so
    # exercise it the way _violations does.
    card = list(_cards(png))[0]
    assert card["img_src"].endswith(".png")
    assert "moonmodules/light/effects.md" in check_docgen.ANIMATED_PAGES
    assert "moonmodules/light/drivers.md" not in check_docgen.ANIMATED_PAGES


def test_the_image_source_is_captured_for_the_format_rule():
    gif = ('### Thing 💫 · 2D\n\n<img src="../../assets/x.gif" alt="x">\n\nShort.\n\n- `a` — one.\n')
    assert list(_cards(gif))[0]["img_src"].endswith(".gif")


def test_the_second_column_carries_only_tests_api_and_details():
    """Three doors, always the same three, in the same place. They are followed often
    enough to earn a standing position. A how-to, an explanation or a sibling module is a
    link the DESCRIPTION makes in a sentence, where the reader meets it in context instead
    of as a bare label under the controls."""
    import check_docgen
    assert check_docgen.SECOND_COLUMN_LINKS == ("Tests:", "API:", "Details:")
    page = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n'
            '- `a` — one.\n[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    assert check_docgen._rendered_links("p.md", page) == []


def test_a_third_link_in_the_second_column_is_flagged():
    """The rule has to FIRE, or it is indistinguishable from a check that reads nothing.
    Renders a row, then asserts an extra label in cell 2 is caught."""
    import check_docgen, mkdocs_hooks
    real = mkdocs_hooks._emit_row

    def patched(b, names):
        row = real(b, names)
        cells = row.strip("| ").split(" | ")
        cells[1] += '<span class="mm-links">**More:** [x](#x)</span>'
        return "| " + " | ".join(cells) + " |"

    mkdocs_hooks._emit_row = patched
    try:
        page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n')
        issues = check_docgen._rendered_links("p.md", page)
    finally:
        mkdocs_hooks._emit_row = real
    assert issues and "More:" in issues[0][1]


# ---- details sections ----

def test_a_wide_details_table_is_flagged():
    """A details section continues the card, so it is for the same reader. Past four
    columns a table stops being scannable and starts being a spec sheet."""
    import check_docgen
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b | c | d | e |\n|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 |\n")
    issues = check_docgen._details_tables("p.md", page)
    assert issues and "5 columns" in issues[0][1]


def test_a_details_table_cell_carrying_an_essay_is_flagged():
    """The widest cell is where implementation prose hides: one Notes cell reached 991
    characters of GDMA chains and ISR refills, which belongs on the API page."""
    import check_docgen
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b |\n|---|---|\n| 1 | " + "z" * 400 + " |\n")
    issues = check_docgen._details_tables("p.md", page)
    assert issues and "prose in a grid" in issues[0][1]


def test_a_narrow_details_table_is_accepted():
    import check_docgen
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b |\n|---|---|\n| 1 | short enough |\n")
    assert check_docgen._details_tables("p.md", page) == []


def test_a_table_outside_a_details_section_is_not_judged():
    """The rule is about details sections. A table in the page intro or in a card is a
    different thing with its own reasons."""
    import check_docgen
    page = "| a | b | c | d | e |\n|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 |\n" + _card()
    assert check_docgen._details_tables("p.md", page) == []


def test_a_card_without_an_image_is_flagged():
    """Every card leads with its picture: a row that starts with a name against blank
    space reads as a gap rather than as a module that happens to be invisible."""
    import check_docgen
    cards = list(_cards(_card()))
    assert cards[0]["img"] is False
    page_with = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\n'
                 'Short.\n\n- `a` — one.\n')
    assert list(_cards(page_with))[0]["img"] is True


def test_links_share_the_controls_cell():
    """Not their own column: stacked under the controls they cost no page width."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    row = [l for l in _row(page).split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    cells = row.strip("| ").split(" | ")
    assert "mm-param" in cells[1] and "mm-links" in cells[1]


def test_the_card_name_leads_the_first_cell():
    """The name first, the preview under it: a reader scanning the table is looking for a
    name, and a picture above it pushes that down the row. The image stays in this column
    rather than its own, which is what kept a long description from rendering as a ribbon."""
    page = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n- `a`: one.\n')
    row = [l for l in _row(page).split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    cells = row.strip("| ").split(" | ")
    assert cells[0].index("mm-name") < cells[0].index("mm-preview")


def test_the_catalog_pages_actually_yield_cards():
    """A check that reads NOTHING reports a clean run. Emptying split_blocks makes every
    rule pass on zero cards, which is indistinguishable from a tree with nothing wrong in
    it, so the count itself is pinned: these pages have cards, and a change that stops
    finding them fails here rather than going quiet."""
    import check_docgen
    from pathlib import Path as _P
    total = 0
    for rel in check_docgen._pages():
        path = ROOT / "docs" / rel
        if path.exists():
            total += len(list(_cards(path.read_text())))
    assert total > 100, f"only {total} cards found across the catalog pages"


def test_split_blocks_is_the_shared_boundary_rule():
    """check_specs and check_docgen both ask the build where a card starts and ends. One
    rule, so a page cannot be split two ways by two readers."""
    import mkdocs_hooks
    page = ("### One 💫 · kind\n\nA.\n\n- `a` — one.\n\n"
            "### Two 💫 · kind\n\nB.\n\n## One, details\n\nprose\n")
    blocks = list(mkdocs_hooks.split_blocks(page))
    assert [t.split()[0] for t, _, _ in blocks] == ["One", "Two"]
    # The details section closes the second block rather than joining it.
    _t, start, end = blocks[1]
    assert "prose" not in "\n".join(page.split("\n")[start:end])


# ---- the header `///` budget ----

def _hdr(text: str):
    import check_docgen
    return check_docgen._header_rules("h.h", text)


def test_a_class_comment_past_ten_lines_is_flagged():
    """The class `///` is the module's summary, not its manual: a deep dive goes after
    `@moreinfo`, where a post-process moves it below the member lists."""
    doc = "\n".join(f"/// line {i}" for i in range(12))
    issues = _hdr(doc + "\nclass Foo : public Bar {")
    assert issues and "class comment 12 lines" in issues[0][1]


def test_a_ten_line_class_comment_is_accepted():
    doc = "\n".join(f"/// line {i}" for i in range(10))
    assert not [i for i in _hdr(doc + "\nclass Foo {") if "class comment" in i[1]]


def test_a_member_comment_past_one_line_is_flagged():
    """One line beside the thing it describes. The generated page shows the first
    sentence as the summary, so a second line is the author still talking."""
    issues = _hdr("#include <x.h>\n\nvoid g();\n\n/// one\n/// two\nvoid doThing();")
    assert issues and "member comment 2 lines" in issues[0][1]


def test_a_one_line_member_comment_is_accepted():
    assert not [i for i in _hdr("#include <x.h>\n\nvoid g();\n\n/// one\nvoid doThing();") if "member comment" in i[1]]


def test_a_header_leads_with_a_defgroup_block():
    """A header of free functions opens with `@defgroup`, so its page says what the file is for."""
    src = "#pragma once\n\n/// @defgroup g Thing\n/// What it is.\nvoid doThing();"
    assert not [i for i in _hdr(src) if "file" in i[1]]


def test_a_single_class_header_leads_with_its_class_comment():
    """The class comment IS the file's documentation where the header holds one class."""
    src = "#pragma once\nnamespace mm {\n/// What it is.\nclass Foo {};\n}"
    assert not [i for i in _hdr(src) if "file" in i[1]]


def test_a_header_opening_with_a_slash_slash_block_is_flagged():
    """Doxygen reads `//` as a note to the next reader of the source, so it generates nothing.

    Four platform headers opened this way and their pages were a bare member list."""
    src = "#pragma once\n\n// What it is.\nvoid doThing();"
    issues = [i for i in _hdr(src) if "file opens with //" in i[1]]
    assert issues


def test_a_header_with_no_lead_comment_is_flagged():
    src = "#pragma once\n#include <cstdint>\nvoid doThing();"
    assert [i for i in _hdr(src) if "file lead missing" in i[1]]


def test_an_xref_to_a_real_heading_resolves():
    """`@xref{anchor}` names a heading on the same generated page, by its MkDocs slug."""
    src = ("#pragma once\n/// Lead.\n/// See @xref{terminology|Terminology}.\n///\n"
           "/// @moreinfo\n///\n/// ## Terminology\n///\n/// Words.\nclass Foo {};")
    assert not [i for i in _hdr(src) if "@xref" in i[1]]


def test_an_xref_naming_no_heading_is_flagged():
    """A renamed heading leaves the link dead, and the generator strips it without a word.

    That is the whole reason to check it here: the cross-reference vanishes from the page and
    nothing else reports the loss."""
    src = ("#pragma once\n/// Lead.\n/// See @xref{glossary|Glossary}.\n///\n"
           "/// @moreinfo\n///\n/// ## Terminology\n///\n/// Words.\nclass Foo {};")
    assert [i for i in _hdr(src) if "names no heading" in i[1]]


def test_a_moreinfo_section_past_ten_lines_is_flagged():
    """The cap is PER SECTION, not per appendix.

    A whole-appendix budget punishes a file for having several distinct topics, and the cheapest
    way to satisfy it is to delete a section rather than tighten the prose. Each `## ` section is
    asked to be disciplined instead, so a file may carry as many as it genuinely has."""
    body = "".join(f"/// line {i}\n" for i in range(11))
    src = ("#pragma once\n/// @defgroup g G\n/// Lead.\n///\n/// @moreinfo\n///\n"
           "/// ## Topic\n///\n" + body + "class Foo {};")
    assert [i for i in _hdr(src) if "appendix section 11 lines" in i[1]]


def test_a_fenced_block_does_not_count_toward_the_section_cap():
    """A fenced block is structure rather than prose, the exemption the no-wrap rule already makes.

    A protocol listing is as long as the thing it describes, so counting its lines would ask the
    author to delete wire format to fit a prose budget."""
    fence = "///\n".join(f"/// [0x{i:02x}][count][stride]\n" for i in range(12))
    src = ("#pragma once\n/// @defgroup g G\n/// Lead.\n///\n/// @moreinfo\n///\n"
           "/// ## Wire format\n///\n/// ```text\n" + fence + "/// ```\nclass Foo {};")
    assert not [i for i in _hdr(src) if "appendix section" in i[1]]


def test_two_short_moreinfo_sections_are_accepted():
    """Sixteen lines across two sections is fine; one section of eleven is not."""
    a = "".join(f"/// a{i}\n" for i in range(8))
    b = "".join(f"/// b{i}\n" for i in range(8))
    src = ("#pragma once\n/// @defgroup g G\n/// Lead.\n///\n/// @moreinfo\n///\n"
           "/// ## A\n///\n" + a + "///\n/// ## B\n///\n" + b + "class Foo {};")
    assert not [i for i in _hdr(src) if "appendix section" in i[1]]


def test_a_moreinfo_on_a_member_is_flagged():
    """`@moreinfo` is an appendix, and only a file or class lead carries one.

    Relaxing this put a four-line block on every function in a swept header, which cost 230 lines
    to undo. A member gets one line, because the page shows that line as its summary."""
    src = ("#pragma once\n/// @defgroup g G\n/// Lead.\n\n"
           "/// Doc.\n///\n/// @moreinfo detail\nvoid doThing();")
    assert [i for i in _hdr(src) if "@moreinfo on a member" in i[1]]


def test_a_moreinfo_on_a_struct_is_accepted():
    """A struct comment is a lead, so it carries an appendix like a class does."""
    src = "#pragma once\n/// Doc.\n///\n/// @moreinfo detail\nstruct S { int a; };"
    assert not [i for i in _hdr(src) if "@moreinfo on a member" in i[1]]


def test_a_moreinfo_does_not_exempt_a_member_comment():
    """`@moreinfo` is the CLASS comment's appendix. A member gets one line, whatever it holds.

    Relaxing this let a four-line block stand on every function in a swept header. The budget is
    one line because the generated page shows that line as the summary; depth belongs on the
    class comment or the module's page."""
    src = ("#include <x.h>\n\nvoid g();\n\n"
               "/// one\n///\n/// @moreinfo a detail\n/// and another\nvoid doThing();")
    issues = [i for i in _hdr(src) if "member comment" in i[1]]
    # TWO, not four: the blank `///` and the `@moreinfo` marker are structure, and the cap counts
    # the prose. What the test pins is that `@moreinfo` buys a member no exemption at all.
    assert issues and "member comment 2 lines" in issues[0][1]


def test_a_code_comment_run_past_one_line_is_flagged():
    """`//` carries the same one-line budget as `///`. Without that the `///` cap moves text
    rather than removing it: a fifty-line member comment re-spelled as `//` passes every other
    rule and the file is exactly as long, which is what a first sweep of these headers did."""
    run = "\n".join(f"// line {i}" for i in range(2))
    issues = _hdr("class Foo {\npublic:\n" + run + "\n/// does a thing\nvoid doThing();\n};")
    assert any("code comment 2 lines" in why for _, why in issues)


def test_a_long_code_comment_line_is_flagged():
    """A `//` line carries the same word budget as a `///` one: without it the line cap is
    satisfied by one very long line, which is a paragraph that happens to lack line breaks."""
    long = "// " + " ".join(["word"] * 35)
    issues = _hdr("class Foo {\npublic:\n" + long + "\n/// does a thing\nvoid doThing();\n};")
    assert any("comment line 35 words" in why for _, why in issues)


def test_a_one_line_code_comment_is_accepted():
    """One line is room to say WHY. The rule cuts essays, not reasons."""
    issues = _hdr("class Foo {\npublic:\n// why\n/// does a thing\nvoid doThing();\n};")
    assert not [i for i in issues if "code comment" in i[1]]


def test_a_file_level_code_comment_is_exempt():
    """The `//` block above the first class explains the compilation unit: it is the
    non-Doxygen sibling of the class comment and has no member to sit beside."""
    run = "\n".join(f"// line {i}" for i in range(12))
    issues = _hdr(run + "\nclass Foo {\npublic:\n/// does a thing\nvoid doThing();\n};")
    assert not [i for i in issues if "code comment" in i[1]]


def test_a_long_doc_sentence_is_flagged():
    long = "/// " + " ".join(["word"] * 35)
    issues = _hdr(long + "\nvoid doThing();")
    assert any("doc sentence 35 words" in why for _, why in issues)


def test_several_short_sentences_on_one_line_are_not_flagged():
    # The no-wrap rule makes a line a paragraph, so the cap counts SENTENCES. Counting the
    # line would fail correct prose and pass nothing the standards ask for.
    line = "/// " + " ".join("A short thought here." for _ in range(4))
    issues = _hdr(line + "\nvoid doThing();")
    assert not [i for i in issues if "doc sentence" in i[1]]


def test_an_undivided_moreinfo_appendix_is_flagged():
    """An appendix with no `## ` heading is one section, so the per-section cap catches it whole.

    This was a 20-line whole-appendix cap. Counting per section instead lets a file carry as many
    topics as it genuinely has, while still refusing a single undivided wall of prose."""
    body = "\n".join(f"/// deep {i}" for i in range(25))
    issues = _hdr("/// @moreinfo\n" + body + "\nclass Foo {")
    assert any("appendix section 25 lines" in why for _, why in issues)


def test_an_undocumented_public_member_is_flagged():
    """A member with no `///` renders as a bare signature, which tells a reader nothing
    the declaration did not."""
    src = "class Foo {\npublic:\n  uint8_t count = 0;\n  void doThing();\n};"
    issues = _hdr(src)
    whys = [why for _, why in issues]
    assert "public variable has no ///" in whys
    assert "public function has no ///" in whys


def test_a_documented_public_member_is_accepted():
    src = ("class Foo {\npublic:\n  /// how many\n  uint8_t count = 0;\n"
           "  /// does the thing\n  void doThing();\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_private_member_needs_no_doc():
    """The budget is about the GENERATED page, and a private member never reaches it."""
    src = "class Foo {\nprivate:\n  uint8_t hidden_ = 0;\n};"
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_statement_inside_a_function_body_is_not_a_member():
    """`return true;` and a local variable parse as declarations, and an inline body keeps
    `public:` in scope, so 251 statements were reported as undocumented public members
    before the brace depth was tracked."""
    src = ("class Foo {\npublic:\n"
           "  /// does the thing\n"
           "  bool doThing() {\n"
           "    Preset& p = presets_[count_];\n"
           "    return true;\n"
           "  }\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_real_member_beside_an_inline_body_is_still_seen():
    """The guard must not silence the rule: a genuine undocumented member declared after
    a function with an inline body is still reported."""
    src = ("class Foo {\npublic:\n"
           "  /// does the thing\n"
           "  bool doThing() { return true; }\n"
           "  uint8_t undocumented = 0;\n};")
    assert [i for i in _hdr(src) if "public variable has no ///" in i[1]]


def test_a_public_member_after_a_nested_type_is_still_seen():
    """A nested type opens its own body, and the enclosing class resumes when it closes.
    Tracking one depth instead of a stack meant the nested struct overwrote the class that
    held it, so closing the struct read as closing the class and every public member after
    one escaped this check: a class with a nested type reported nothing at all."""
    nested = ("class Foo {\npublic:\n"
              "  /// documented\n"
              "  void before() {}\n"
              "  /// a nested type\n"
              "  struct Inner { uint8_t a; };\n"
              "  void after();\n"
              "  uint8_t alsoAfter = 0;\n};")
    whys = [why for _, why in _hdr(nested) if "has no ///" in why]
    assert "public function has no ///" in whys
    assert "public variable has no ///" in whys


def test_a_private_nested_type_stays_exempt():
    """The stack must not undo what it replaced: a type declared after `private:` is private
    however it is spelled, and its fields never reach the generated page."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "private:\n"
           "  struct Hidden { uint8_t undocumentedField; };\n"
           "  uint8_t alsoPrivate_ = 0;\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_friend_declaration_is_not_a_member():
    """`friend class X;` grants access rather than declaring anything: it reaches no generated
    page, so asking it for a `///` would document a line no reader ever sees."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "  friend class ScratchBufferBase;\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_function_local_type_is_exempt():
    """A struct declared inside a METHOD body cannot be named outside it and never reaches the
    generated page, so demanding a `///` on its fields asks the file to document what no reader
    sees. The same reason `friend` and a private nested type are already exempt."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  bool go() {\n"
           "    struct Local { uint8_t* out; void emit(int x) {} };\n"
           "    return true;\n"
           "  }\n};")
    assert not [i for i in _hdr(src) if "has no ///" in i[1]]


def test_a_class_body_nested_type_still_reports():
    """The exemption must not swallow the case the stack was built for: a type declared
    directly in the class body IS on the generated page, one level below it rather than two."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  void ok() {}\n"
           "  /// a nested type\n"
           "  struct Inner {\n"
           "    uint8_t undocumentedField;\n"
           "  };\n};")
    whys = [why for _, why in _hdr(src) if "has no ///" in why]
    assert "public variable has no ///" in whys


def test_a_member_after_a_function_local_type_is_still_seen():
    """The local frame must end with the method that held it: a public member declared after
    one is an ordinary member, and skipping it would hide the whole rest of the class."""
    src = ("class Foo {\npublic:\n"
           "  /// documented\n"
           "  bool go() {\n"
           "    struct Local { uint8_t hidden; };\n"
           "    return true;\n"
           "  }\n"
           "  uint8_t afterTheMethod = 0;\n};")
    whys = [why for _, why in _hdr(src) if "has no ///" in why]
    assert "public variable has no ///" in whys


def test_the_headers_are_actually_scanned():
    """A rule that reads no files reports a clean run, which looks exactly like a clean
    tree. The count is pinned so an empty scan fails here instead of going quiet."""
    import check_docgen
    assert len(list(check_docgen._headers())) > 10


def test_the_scan_covers_cpp_as_well_as_headers():
    """The same `///` and `//` comments live on both sides of a header/source split, so a
    scan that stopped at `.h` left every `.cpp` silently exempt. Both suffixes are pinned
    here because the count test above passes just as happily with one of them missing."""
    import check_docgen
    scanned = {p.suffix for p in check_docgen._headers()}
    assert scanned == {".h", ".cpp"}, scanned


def test_a_cpp_file_is_held_to_the_comment_rules():
    """A `.cpp` gets no generated page, so only the comment-shape rules reach it. This pins
    that they DO reach it: the rule fires on a source file, not just on a header."""
    import check_docgen
    src = ("class X {\n"
           "public:\n"
           "    /// Does the thing.\n"
           "    void f() {\n"
           + "".join(f"        // line {i}\n" for i in range(9)) +
           "    }\n"
           "};")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    assert any("code comment 9 lines > 1" in w for w in whys), whys


def test_a_cpp_is_asked_for_no_member_docs():
    """`public` is read from a declaration's SHAPE, which cannot see `namespace {` or a function
    body. A header's declarations mostly ARE the public API and each becomes a page row, so the
    approximation holds there. In a `.cpp` it matched fields of file-local structs and plain
    locals, asking a variable named `got` for a doc comment, so the rule stops at the header."""
    import check_docgen
    src = ("struct State {\n"
           "    bool busy = false;   // a frame is on the wire\n"
           "};")
    cpp = [w for _, w in check_docgen._header_rules("src/core/x.cpp", src) if "public variable" in w]
    assert not cpp, cpp
    hdr = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "public variable" in w]
    assert hdr, hdr


def test_one_rule_set_applies_to_both_kinds_of_file():
    """A `.cpp` follows the same rules as a `.h`; only the SEVERITY differs. Three rules were
    scoped away from implementation files on reasons that did not hold: a `.cpp` carries a file
    lead with its own `@moreinfo` appendix, declares classes, and is read as source like any
    other file. Scoping them back out is how the two columns drift apart again."""
    import check_docgen
    src = ("#include <x.h>\n"
           "\n"
           "void g();\n"
           "\n"
           "/// A two-line note\n"
           "/// about the thing below.\n"
           "void f();\n")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        whys = [w for _, w in check_docgen._header_rules(rel, src) if "member comment" in w]
        assert whys, (rel, "the member budget applies to both")


def test_a_list_is_structure_rather_than_an_over_long_run():
    """A numbered item is a bullet that starts with a digit, so it is layout exactly as `-` is.
    Counted as prose, an enumerated spec can satisfy the one-line cap only by being fused onto
    one line, which destroys the layout that carries its meaning: a five-item wire-format spec
    became one 299-character line that way. The prose lines in a run still count, so an essay
    with a list in it is measured on the essay."""
    import check_docgen
    lead = "#include <x.h>\n\nvoid g();\n\n"
    lst = ("// Pins three things:\n"
           "//   1. the first\n"
           "//   2. the second\n"
           "//   3. the third\n"
           "void f();\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.cpp", lead + lst)]
    assert not [w for w in whys if "code comment" in w], whys
    assert not [w for w in whys if "hard wrap" in w], whys

    essay = lead + "".join(f"// prose line {i}.\n" for i in range(5)) + "void f();\n"
    runs = [w for _, w in check_docgen._header_rules("src/core/x.cpp", essay) if "code comment" in w]
    assert runs == ["code comment 5 lines > 1"], runs


def test_a_lead_is_measured_as_a_block_so_chopping_does_not_satisfy_it():
    """The line cap alone is satisfiable by splitting one long line into two shorter ones, which
    leaves the text identical and every rule passing. A run is what a reader takes in before the
    next declaration, so it carries the same budget over the whole block: line count times line
    length. Pinned because the chopping move is the cheapest way to make this check green, and a
    check that rewards it measures the wrong thing."""
    import check_docgen
    # Twelve lines of 240 characters: every LINE is inside the cap, the run is not.
    lead = "\n".join("/// " + "x" * 236 for _ in range(12))
    src = "/// @defgroup g G\n" + lead + "\n/// @{\nvoid f();\n/// @}\n"
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src)]
    assert any("lead" in w and "chars" in w for w in whys), whys
    assert not any("doc line" in w for w in whys), \
        "no single line is over the cap, which is the point of measuring the block"


def test_the_ratchet_refuses_a_rule_that_rose_against_the_committed_report(monkeypatch):
    """A warning is staged work, and staged work that grows is not a sweep. The committed report
    is the number to beat, per RULE and on the total: per rule alone misses a rule still under
    its own baseline while the total rises, and the total alone hides one rule paying for
    another, which is what splitting a long line does to the width and block counts.

    Drives `_ratchet` itself. An earlier version of this test asserted against a local copy of the
    comparison and passed while the real function compared internal keys against display labels,
    so it matched nothing and the per-rule half never fired."""
    import check_docgen
    base = {"(total)": 10, "multi-line comment blocks": 4, "hard wraps": 2}
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: base)

    # A `.cpp` key, because the ratchet guards the WARNING column and a header's finding blocks.
    cpp = "src/core/x.cpp"
    block = "code comment 3 lines > 1"

    assert check_docgen._ratchet([(cpp, block)] * 3) == [], "under every baseline: silent"

    rose = check_docgen._ratchet([(cpp, block)] * 5)
    assert rose == [("multi-line comment blocks", 4, 5)], \
        f"one rule paying for another is caught, and the total (5) stays under 10: {rose}"

    # The labels the report writes ARE the keys the comparison uses, in both directions.
    assert check_docgen._rule_label(check_docgen._rule_name(block)) in base

    # And the real parser finds the tracked report's numbers, or the comparison reads nothing.
    monkeypatch.undo()
    counts = check_docgen._committed_counts()
    assert counts is None or "(total)" in counts, counts
    # Only the `## By rule` table feeds the baseline: an area row would be a rule that never appears.
    assert counts is None or not any(k.startswith("`") for k in counts), counts


def test_the_ratchet_catches_a_rule_the_baseline_never_had(monkeypatch):
    """A rule at zero is ABSENT from the report, so the baseline has no row for it. Iterating the
    baseline alone let such a rule rise silently while the total fell, which is the same hole the
    per-rule half exists to close. The comparison is over the union, a missing baseline reading
    as zero."""
    import check_docgen
    base = {"(total)": 10, "multi-line comment blocks": 4}
    monkeypatch.setattr(check_docgen, "_committed_counts", lambda: base)

    cpp = "src/core/x.cpp"
    # A warning-side rule with no baseline row. (A hard wrap cannot serve here: it blocks
    # everywhere by design, so it is an error and never enters the warning column.)
    fresh = "file opens with // rather than ///"
    assert check_docgen._rule_label(check_docgen._rule_name(fresh)) not in base

    rose = check_docgen._ratchet([(cpp, fresh)] * 5)
    assert rose == [("files opening with //", 0, 5)], \
        f"a rule absent from the baseline must still only shrink: {rose}"


def test_a_hard_wrap_blocks_in_an_implementation_file_too():
    """The one rule staged the OTHER way. Every other `.cpp` finding warns while the sweep runs,
    but the tree is at ZERO hard wraps, so there is nothing left to stage and the exemption only
    protects a regression. The reason holds everywhere too: a split sentence reflows every line
    it spans on the next word change, which costs a reviewer the same in either file."""
    import check_docgen
    src = ("#include <x.h>\n\nvoid g();\n\n"
           "// A sentence that carries on\n"
           "// onto the following line.\n"
           "void f();\n")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        hits = [(k, w) for k, w in check_docgen._header_rules(rel, src) if "hard wrap" in w]
        assert hits, rel
        assert all(check_docgen._blocks(k, w) for k, w in hits), (rel, "must BLOCK")
    # And the rules still being swept keep warning in a `.cpp`, or the staging is gone entirely.
    runs = [(k, w) for k, w in check_docgen._header_rules("src/core/x.cpp", src)
            if "code comment" in w]
    assert runs and not any(check_docgen._blocks(k, w) for k, w in runs), runs


def test_the_line_length_cap_warns_even_in_a_header():
    """The one staged exception. Every other rule describes how the tree is already written, so
    a finding is a defect against a standard the file was written under. This cap is new, so its
    findings are lines nobody wrote wrongly, and erroring on them would stop commits over a
    target rather than a defect. It becomes an error once the tree meets it."""
    import check_docgen
    long_line = "/// " + "word " * 60
    assert len(long_line) > check_docgen.MAX_COMMENT_LINE_CHARS
    src = "#include <x.h>\n\nvoid g();\n\n" + long_line + "\nvoid f();\n"
    hits = [(k, w) for k, w in check_docgen._header_rules("src/core/x.h", src) if "chars >" in w]
    assert hits, "a header's over-long line is still REPORTED"
    assert not any(check_docgen._blocks(k, w) for k, w in hits), \
        "but it warns rather than blocking, unlike every other header finding"
    # A header finding of any OTHER kind still blocks, or the exception has swallowed the rule.
    other = [(k, w) for k, w in check_docgen._header_rules("src/core/x.h", src) if "chars >" not in w]
    assert any(check_docgen._blocks(k, w) for k, w in other), other


def test_a_local_struct_in_an_allman_function_is_not_a_member():
    """A free function's body is tracked by the brace that opens it, wherever that brace sits.
    With the brace on its own line the signature and the body open on different lines, so a
    local struct declared inside could be mistaken for a documented type's member and its
    fields asked for a `///` they do not owe. Both brace styles are pinned, and a genuine
    member after the function still reports, so the exclusion cannot swallow a real finding."""
    import check_docgen
    body = ("/// @defgroup g G\n/// @{\n/// doc\nint f()%s\n"
            "    struct Local { int x; };\n    return sizeof(Local);\n}\n"
            "struct S {\n    int shouldBeFlagged;\n};\n/// @}\n")
    for brace in ("\n{", " {"):
        whys = [w for k, w in check_docgen._header_rules("src/core/x.h", body % brace)]
        assert whys == ["public variable has no ///"], (brace, whys)


def test_a_header_finding_blocks_and_an_implementation_one_warns():
    """Severity follows the same split `_generates_a_page` draws. A header's comments ARE the
    published page, so a finding there is a defect in what ships and fails the gate. An
    implementation file publishes nothing, so an over-long note to the next reader is worth
    fixing without holding a commit. A catalog page publishes too, so it blocks like a header.
    What the split must NOT do is hide the warnings: they stay in the report and in the count,
    because the split stages the sweep rather than ranking the two kinds of comment, and it goes
    once the warning side reaches zero."""
    import check_docgen
    assert check_docgen._blocks("src/core/x.h::Thing")
    assert check_docgen._blocks("docs/moonmodules/light/effects.md::SomeCard")
    assert not check_docgen._blocks("src/core/x.cpp::line 12")


def test_a_provenance_marker_above_the_lead_is_not_the_lead():
    """A file's lead is the first thing that DOCUMENTS it. An SPDX tag is machine-read and an
    `Author:` line is a credit, so both sit above the real lead and neither is one: counted as the
    lead they reported every licensed or credited header as opening with `//` while a proper `///`
    lead stood two lines below. A forward declaration names a type defined elsewhere and documents
    nothing either. What none of them may do is stand in for a lead that is missing."""
    import check_docgen
    def whys(src):
        return [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "lead" in w]

    for marker in ("// SPDX-License-Identifier: GPL-3.0-or-later",
                   "// Author: MoonLight original"):
        src = f"{marker}\n#pragma once\n/// One thing.\nclass Thing {{\npublic:\n    /// Go.\n    void go();\n}};\n"
        assert not whys(src), marker

    fwd = "#pragma once\nclass Other;\n/// One thing.\nclass Thing {\npublic:\n    /// Go.\n    void go();\n};\n"
    assert not whys(fwd), "a forward declaration is not a lead"

    bare = "// Author: MoonLight original\n#pragma once\nclass Thing {\npublic:\n    void go();\n};\n"
    assert whys(bare), "a marker must not stand in for a missing lead"


def test_a_defgroup_is_asked_for_only_where_a_lone_class_already_leads():
    """Two lead shapes generate a page, and which is right follows from what the header holds. A
    lone class IS its page, so a group around it is a second lead saying the same thing twice.

    Counting what is TOP-LEVEL is the whole rule, and several shapes defeated a naive count, so
    each is pinned: an Allman brace belongs to the declaration above it; a brace inside a string
    literal is text and must not raise the depth; and one line may open two namespaces.

    A TEMPLATE class is exempt outright. Its `template <...>` line sits between the comment and
    the class, so the comment cannot attach to the class at all and a group is the only lead
    shape that reaches the page. Reported, the rule would push such a header back into the shape
    that loses its documentation. A header of free declarations keeps its group, and a nested type never makes a
    header a multi-type one."""
    import check_docgen
    def whys(src):
        return [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "@defgroup on" in w]
    G = "/// @defgroup g G\n/// @{\n/// A lead.\n"
    MEM = "public:\n    /// Go.\n    void go();\n};\n/// @}\n"

    assert whys(G + "class Thing {\n" + MEM), "a lone class needs no group"
    assert not whys(G + "template <class T>\nclass Thing {\n" + MEM), \
        "a template class keeps its group: the template line stops a class comment attaching"
    assert whys("namespace mm\n{\n" + G + "class Thing\n{\n" + MEM + "}\n"), \
        "an Allman brace belongs to the declaration above it"
    assert whys("namespace mm { namespace detail {\n" + G + "class Thing {\n" + MEM + "}}\n"), \
        "one line may open two namespaces"
    assert whys(G + 'class Thing {\npublic:\n    /// Go.\n'
                '    void go() { const char* s = "{"; }\n};\n/// @}\n'), \
        "a brace in a string literal is text, not scope"
    assert whys(G + "class Thing {\npublic:\n    /// Go.\n    void go();\n"
                "private:\n    struct H { int n; };\n};\n/// @}\n"), \
        "a nested struct is not a second page-level type"

    quiet = ("/// The class.\nclass Thing {\n" + MEM)
    for free in ("inline void f();", "template <class T>\ninline void f(T t);",
                 "int f()\n{\n    return 0;\n}", "int f() {\n    return 0;\n}"):
        assert not whys(G + free + "\n" + quiet), (free, "a free declaration justifies the group")

    lone = G + "class Thing {\n" + MEM
    assert [w for _, w in check_docgen._header_rules("src/core/x.cpp", lone) if "@defgroup on" in w], \
        "an implementation file leads the same way: a group around its lone class is still a " \
        "second lead saying the same thing twice"


def test_two_headers_claiming_one_group_id_are_reported(tmp_path, monkeypatch):
    """Doxygen MERGES two groups sharing an id, so one header's page absorbs the other's and the
    loser vanishes from the site. It fails silently both ways: the surviving page looks complete
    and the missing one is simply absent. A core parser and a light-domain one both named their
    group `PinList` that way, and only a dangling link in the strict build caught it. An
    implementation file counts too — Doxygen reads it all the same — and is reported against
    rather than the header, since the header owns the page."""
    import check_docgen
    from pathlib import Path
    (tmp_path / "a.h").write_text("/// @defgroup Shared A\n/// @{\n/// A.\n/// @}\n")
    (tmp_path / "b.h").write_text("/// @defgroup Shared B\n/// @{\n/// B.\n/// @}\n")
    (tmp_path / "c.h").write_text("/// @defgroup Own C\n/// @{\n/// C.\n/// @}\n")
    (tmp_path / "c.cpp").write_text("/// @defgroup Own C\n/// @{\n/// C.\n/// @}\n")
    monkeypatch.setattr(check_docgen, "ROOT", tmp_path)
    monkeypatch.setattr(check_docgen, "_headers",
                        lambda: [Path(n) for n in ("a.h", "b.h", "c.h", "c.cpp")])
    found = {key: why for key, why in check_docgen._duplicate_group_ids()}
    assert any(k.startswith("b.h") for k in found), "the second header to claim an id is reported"
    assert not any(k.startswith("a.h") for k in found), "the first keeps the id"
    assert any(k.startswith("c.cpp") for k in found), "a .cpp re-declaring its header's id merges"
    assert not any(k.startswith("c.h") for k in found), "the header owns the page"


def test_a_group_that_documents_nothing_is_reported():
    """A group is a label, not an entity, so `gen_api` infers its page from where its MEMBERS
    sit. A `@}` that closes before the first declaration wraps only the comment: the group holds
    nothing, resolves to no header, and the whole page is skipped — lead, appendix and members —
    with nothing reported, because the header still looks documented and the unreachable-page
    check only fires on a page that exists. Two headers shipped that way, their appendices
    unreachable. A group closing after what it documents is the working shape."""
    import check_docgen
    def whys(src, rel="src/core/x.h"):
        return [w for _, w in check_docgen._header_rules(rel, src) if "documents anything" in w]

    LEAD = "/// @defgroup g G\n/// @{\n/// A lead.\n"
    assert whys(LEAD + "/// @}\n\nnamespace mm {\n/// A thing.\ninline void f();\n}\n"), \
        "a group closing before the namespace holds nothing"
    assert not whys("namespace mm {\n" + LEAD + "/// A thing.\ninline void f();\n/// @}\n}\n"), \
        "a group closing after its declarations is the working shape"
    assert not whys(LEAD + "/// @}\n"), "a header with no declaration at all is not this fault"
    assert whys(LEAD + "/// @}\n\nnamespace mm {\ninline void f();\n}\n",
                rel="src/core/x.cpp"), \
        "an implementation file carries groups too, so a group closing before its first " \
        "declaration loses the same appendix there"


def test_an_xref_that_cannot_render_is_reported():
    """`@xref{anchor|label}` renders to a same-page link, and the generator resolves `[a-z0-9-]+`
    only. An underscore therefore never becomes a link: the marker survives Doxygen untouched and
    the reader sees raw `@xref{...}` mid-sentence. Matching that same narrow class HERE made the
    rule silent on exactly the names that cannot work, so it reported a clean run over 21 of them
    — a check that read nothing is indistinguishable from a check that found nothing. So anything
    `@xref{...}`-shaped is matched loosely and then judged: a name that is not a slug is
    unrenderable, and a slug naming no heading in the file is dead."""
    import check_docgen
    def whys(src):
        return [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "xref" in w]

    HEAD = ("/// @defgroup g G\n/// @{\n/// A lead.\n///\n/// @moreinfo\n///\n"
            "/// ## A real heading\n///\n")
    TAIL = "inline void f();\n/// @}\n"
    assert any("renders as raw text" in w
               for w in whys(HEAD + "/// See @xref{some_anchor|this}.\n" + TAIL)), \
        "an underscore cannot render, so it must be reported"
    assert any("names no heading" in w
               for w in whys(HEAD + "/// See @xref{no-such-heading|this}.\n" + TAIL)), \
        "a slug naming no heading is dead"
    assert not whys(HEAD + "/// See @xref{a-real-heading|this}.\n" + TAIL)
    # The label separator may be written escaped, which the generator accepts, so the backslash
    # lands in the captured anchor and must not be read as part of the slug.
    assert not whys(HEAD + "/// See @xref{a-real-heading\\|this}.\n" + TAIL), \
        "an escaped pipe is a valid reference"


def test_an_unclosed_group_scope_is_reported():
    """`@defgroup` opens a scope with `@{` and Doxygen runs it to `@}`. Left open, every
    declaration after the lead is absorbed into the group and the generated page is silently
    reshaped: three headers shipped that way in one sweep because nothing counted the pair. An
    orphan `@}` is the same bug from the other side, which is what a conversion away from a group
    leaves behind."""
    import check_docgen
    def whys(src, rel="src/core/x.h"):
        return [w for _, w in check_docgen._header_rules(rel, src) if "group scope" in w]

    assert whys("/// @defgroup g G\n/// @{\n/// A lead.\ninline void f();\n")
    assert whys("/// A lead.\ninline void f();\n/// @}\n")
    assert not whys("/// @defgroup g G\n/// @{\n/// A lead.\ninline void f();\n/// @}\n")
    assert whys("/// @defgroup g G\n/// @{\n/// A lead.\ninline void f();\n",
                rel="src/core/x.cpp"), "an unbalanced scope is a defect in either kind of file"


def test_a_cpp_may_open_with_a_plain_comment_but_must_open_with_one():
    """The SPELLING of a file lead is a header's business, because a header's lead is its
    generated page's opening paragraph and `//` generates nothing. An implementation file has no
    page, so there `//` is a note to the next reader and reads correctly. What both owe is a lead
    at all: a file that opens on code says nothing about itself to anybody."""
    import check_docgen
    plain, none_ = "// what this file is for\nint f() { return 1; }\n", "#include <x>\nint f() { return 1; }\n"
    for rel in ("src/core/x.cpp", "src/core/x.h"):
        assert [w for _, w in check_docgen._header_rules(rel, plain) if "opens with //" in w], rel
    for rel in ("src/core/x.cpp", "src/core/x.h"):
        assert [w for _, w in check_docgen._header_rules(rel, none_) if "file lead missing" in w], rel


def test_a_cpp_is_asked_for_no_member_docs_of_either_kind():
    """Functions go the same way as variables, for the same reason: what the rule matched in a
    `.cpp` was the constructor or destructor of a file-local RAII struct — `~WinsockInit`,
    `Lock`, `ParkGuard` — none of them public API, each already explained by the struct's own
    comment. A header keeps both halves, where a declaration really is the public surface."""
    import check_docgen
    src = ("struct State {\n"
           "    void start();\n"
           "    bool busy = false;\n"
           "};")
    cpp = [w for _, w in check_docgen._header_rules("src/core/x.cpp", src)
           if "has no ///" in w]
    assert not cpp, cpp
    hdr = [w for _, w in check_docgen._header_rules("src/core/x.h", src)
           if "has no ///" in w]
    assert any("public function has no ///" in w for w in hdr), hdr
    assert any("public variable has no ///" in w for w in hdr), hdr


def test_a_fenced_block_is_not_read_as_wrapped_prose():
    """Skipping the ``` delimiters alone was not enough. A fence's whole point is that what sits
    BETWEEN them is verbatim, so a wire table was flagged on every row: each line ends without a
    full stop, which is exactly what the no-wrap rule looks for. The same exemption the appendix
    section cap already makes, and the one the standards state."""
    import check_docgen
    src = ("/// @defgroup g A group\n"
           "/// A lead line.\n"
           "///\n"
           "/// ```\n"
           "/// div 3  26.67 MHz  zero 300 ns   strict spec, but strands scramble\n"
           "/// div 4  20.00 MHz  zero 400 ns   the reliability point\n"
           "/// ```\n"
           "/// @{\n"
           "void f();\n"
           "/// @}\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "hard wrap" in w]
    assert not whys, whys


def test_prose_after_a_closed_fence_is_still_checked():
    """The exemption tracks the fence STATE rather than switching off at the first one, so a
    wrapped sentence below a closed block is caught as it always was."""
    import check_docgen
    src = ("/// @defgroup g A group\n"
           "/// A lead line.\n"
           "///\n"
           "/// ```\n"
           "/// a table row\n"
           "/// ```\n"
           "///\n"
           "/// This sentence is carried\n"
           "/// onto the next line.\n"
           "/// @{\n"
           "void f();\n"
           "/// @}\n")
    whys = [w for _, w in check_docgen._header_rules("src/core/x.h", src) if "hard wrap" in w]
    assert whys, whys


def test_one_comment_cap_holds_in_both_kinds_of_file():
    """A `//` run gets one line wherever it sits. Depth is not forbidden but HOMED: a header
    carries it in an `@moreinfo` appendix and an implementation file carries it in the same
    appendix on its own file lead, so the escape hatch exists in both. A larger cap in a `.cpp`
    was an attempt to home depth inline, which leaves the reasoning beside one call rather than
    where a reader goes looking for it."""
    import check_docgen
    body = ("class X {\n"
            "public:\n"
            "    /// Does the thing.\n"
            "    void f() {\n"
            + "".join(f"        // line {i}\n" for i in range(4)) +
            "    }\n"
            "};")
    for rel in ("src/core/x.h", "src/core/x.cpp"):
        whys = [w for _, w in check_docgen._header_rules(rel, body) if "code comment" in w]
        assert any("code comment 4 lines > 1" in w for w in whys), (rel, whys)


def test_the_scan_reaches_beyond_src():
    """The entry points and the ISA generator sit outside `src` by build-system convention
    alone, and were exempt for that reason rather than by decision. Pinned by root so a
    dropped one fails here instead of quietly shrinking the measured surface."""
    import check_docgen
    scanned = {str(p) for p in check_docgen._headers()}
    for expected in ("esp32/main/main.cpp",
                     "moonbase/main/moonbase_main.cpp",
                     "moondeck/moonlive/emit_isa.cpp"):
        assert expected in scanned, expected


def test_the_file_comment_is_exempt_but_nothing_below_it_is():
    """The file comment is the one `//` block at the top, before anything is declared. The rule
    used to exempt everything above the first `class`, which in a header of free functions is
    most of the file: 1002 multi-line runs across 149 files sat unmeasured on the declarations
    they documented. Both halves are pinned, since exempting nothing is the opposite mistake."""
    import check_docgen
    src = ("// The file comment.\n"
           "// Its second line, which is allowed.\n"
           "\n"
           "#include <cstdint>\n"
           "\n"
           "// A comment on a declaration,\n"
           "// spanning two lines, which is not.\n"
           "using Fn = void (*)(int);\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "code comment 2 lines" in w], whys
    assert len([w for w in whys if "code comment" in w]) == 1, whys


def test_a_file_lead_written_in_paragraphs_stays_exempt():
    """A lead is written in paragraphs, and a blank line between two of them does not end it.
    Stopping at the first blank exempted only the opening paragraph and reported the rest as
    essays sitting on the next declaration, when they are the file's documentation in the only
    place a `.cpp` has for it. The `@module`/`@also` lead of unit_Effects_golden.cpp is the
    case: 33 lines that no appendix could hold, because they ARE the lead."""
    import check_docgen
    src = ("// The first paragraph of the lead.\n"
           "//\n"
           "// The second, after a blank line inside the comment.\n"
           "\n"
           "// A third, after a blank line BETWEEN blocks, still the lead:\n"
           "// nothing has been declared yet.\n"
           "\n"
           "#include <cstdint>\n"
           "\n"
           "void f();\n"
           "\n"
           + "".join(f"// essay line {i}\n" for i in range(9)) +
           "void g();\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    runs = [w for w in whys if "code comment" in w]
    assert runs == ["code comment 9 lines > 1"], runs


def test_a_file_with_no_leading_comment_exempts_nothing():
    """`_file_comment_end` answers 0 once a declaration has been seen, so a file that opens with
    code buys no exemption for a comment further down. Otherwise a file could dodge the rule by
    omitting its own header comment, which is the wrong incentive.

    The file comment may sit BELOW the includes, which is where this repo puts it, so the
    fixture has to declare something first to prove the other half."""
    import check_docgen
    src = ("#include <cstdint>\n"
           "using Fn = void (*)(int);\n"
           "// Two lines,\n"
           "// on a later declaration.\n"
           "using G = int;\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "code comment 2 lines" in w], whys


def test_a_templated_member_is_not_asked_for_a_second_doc():
    """Doxygen attaches a comment to the declaration, and a `template <...>` header sits
    between the two. Reading only the line above asked a documented templated member for a
    `///` it already had, and the answer was a second one wedged under the template header,
    which doxygen drops and a reader sees twice."""
    import check_docgen
    src = ("class X {\n"
           "public:\n"
           "    /// Walks the thing.\n"
           "    template <typename F>\n"
           "    void walk(F&& f) const { (void)f; }\n"
           "};")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_a_multiline_struct_inside_a_free_function_is_exempt():
    """The one-line form was already exempt, but a struct whose fields sit on their own lines
    reached the member scan and reported `public variable has no ///`. A free function has no
    enclosing class, so the frame stack is empty and the depth test had nothing to compare
    against: locality there is the function body's own depth."""
    import check_docgen
    src = ("namespace mm {\n"
           "\n"
           "void f() {\n"
           "    struct Guard {\n"
           "        int fd;\n"
           "    };\n"
           "    Guard g{0};\n"
           "    (void)g;\n"
           "}\n"
           "\n"
           "}\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_a_namespace_level_struct_is_still_public():
    """The free-function exemption must not swallow an ordinary type: one declared at namespace
    scope reaches the generated page and still owes a `///`."""
    import check_docgen
    src = ("namespace mm {\n"
           "\n"
           "struct Public {\n"
           "    int field;\n"
           "};\n"
           "\n"
           "}\n")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.h", src)]
    assert [w for w in whys if "has no ///" in w], whys


def test_a_struct_inside_a_free_function_is_exempt():
    """A type declared in a free function's body reaches no reader: it has no enclosing class,
    so the frame stack is empty and the old rule read it as a top-level public member. With
    `.cpp` files in scope that shape is ordinary (a scoped RAII helper), so it is pinned."""
    import check_docgen
    src = ("void f() {\n"
           "    struct Guard { int fd; };\n"
           "    Guard g{0};\n"
           "    (void)g;\n"
           "}")
    whys = [why for _, why in check_docgen._header_rules("src/core/x.cpp", src)]
    assert not [w for w in whys if "has no ///" in w], whys


def test_every_source_folder_maps_to_a_summary_page():
    """The report groups findings by the page a sweep runs against, so a folder that maps to
    nothing would silently vanish from that view. Pinned per domain folder: the mapping is the
    machine-readable half of the hierarchy zoom in documentation-standards.md."""
    import check_docgen, pathlib
    root = pathlib.Path(check_docgen.ROOT)
    for domain in ("core", "light"):
        for folder in sorted((root / "src" / domain).iterdir()):
            if not folder.is_dir():
                continue
            rel = f"src/{domain}/{folder.name}/X.h"
            area = check_docgen._doc_area(rel)
            assert area.endswith(".md"), f"{rel} -> {area}"


def test_a_path_outside_the_card_surface_is_bucketed_not_lost():
    """The tests carry no card by design, so they get a named bucket rather than a summary
    page. Anything else is `(unassigned)`, which is the finding: a header no page owns
    reaches no reader. The platform layer USED to sit in a bucket here and now has its own
    page, which is what a bucket is for: naming the gap until it is filled."""
    import check_docgen
    assert check_docgen._doc_area("src/platform/platform.h") == "platform/index.md"
    assert check_docgen._doc_area("test/unit/core/x.cpp") == "(tests, no card)"
    # A path under no DOC_AREAS prefix at all. `moonbase/main` stood here until it was
    # assigned to core/system.md, which is the bucket working as intended rather than a
    # regression: an example that gets a page is replaced, not restored.
    assert check_docgen._doc_area("tools/nowhere/x.cpp") == "(unassigned)"


def test_the_scan_includes_tests():
    """A test is C++ we own, and these rules are about how code READS rather than about what
    gets published, so `test/` is in scope like any other source. Pinned because excluding it
    is the easy mistake: nothing generates a page from a test, which looks like a reason."""
    import check_docgen
    assert [p for p in check_docgen._headers() if str(p).startswith("test/")]


def test_the_real_pages_obey_the_structure_rules():
    """The tree itself, as the control: the synthetic cases above prove the rules fire,
    and this proves they are satisfied where it counts. Structure must hold everywhere,
    with no tolerated list to fall back on."""
    import check_docgen
    for rel in check_docgen._pages():
        path = ROOT / "docs" / rel
        if path.exists():
            assert _structure(path.read_text(), rel) == [], rel

#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["markdown"]
# ///
# `markdown` because this check renders a row through the BUILD's own renderer rather than
# reimplementing it, and that renderer slugs a heading with Python-Markdown's own function.
# MkDocs supplies it at build time; a bare `uv run` of this script does not.
"""The documentation the build GENERATES, held to the standards that describe it.

Two surfaces, one check, because they are one pipeline: the hand-written catalog pages
that render as card tables, and the `///` comments in the sources that become the
technical pages beside them. A rule about the first is a rule about a cell width; a rule
about the second is a rule doxygen enforces by silently dropping what it cannot read.
Prose is NOT here: em-dashes, spelling and weasel words are Vale's, and a second prose
checker is the thing the documentation sweep exists to delete.

A card is one table row, and a row is read across. Text that outgrows its cell turns the
column into a ribbon: at the worst, 3,416 characters of description in a cell 44% of the
page wide. The rule and its limits are in documentation-standards.md § The card; this
script owns the measuring.

Over a limit, cut first. What survives the cut, and only that, has exactly two homes:

  * Behavior of the module itself -> the `///` comments in its .h, which reach the reader
    through the generated technical page the card already links. Free, and it cannot drift
    from the code it sits beside.
  * Rationale spanning modules, or guidance a user needs before choosing -> a
    `## <Name>, details` section under the same page, which the build links as
    `⌄ details`. documentation-standards.md forbids a per-module detail page, and this
    section is what it offers instead.

Parsing is the BUILD's parser (mkdocs_hooks._render_catalog_table's block loop), imported
rather than reimplemented: a second parser of the same format is the drift this check exists
to prevent.

Pinned by test/python/test_check_docgen.py. A check like this fails silently when it
breaks: a regex that stops matching prints a clean run, which reads exactly like a tree
with nothing wrong in it. So every rule here is tested firing on a page built to break it,
not only staying quiet on one that obeys.

    uv run moondeck/check/check_docgen.py
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

# The limits. Description and controls share one number deliberately: two cells side by side
# with different caps makes one column reliably longer than the other, which is the shape
# this check exists to remove.
# A card is read in a narrow column beside its image, so the limits are per LINE rather than
# per card: a module with twelve honest controls is not worse documented than one with three,
# and capping their sum only punished the richer module for being rich. What makes a card
# unreadable is one control that runs to a paragraph, which the per-control cap catches.
#
# The visual catalogs are tighter because the GIF carries the description: a moving effect
# shows what it looks like far better than prose can, so the words are there to say what the
# reader cannot see. Elsewhere a still PNG shows a card, and the prose does more of the work.
MAX_DESC = 600            # characters of prose under the name
MAX_DESC_VISUAL = 400     # ... on effects, modifiers and layouts, where the gif describes it
MAX_CONTROL = 100         # characters of any ONE control line
MAX_CONTROL_VISUAL = 80   # ... on the visual catalogs

# A details section continues the CARD, so it is written for the same reader: someone
# choosing and using the module, not someone reading its implementation. Two limits keep
# it that way. A table wider than this stops being scannable and starts being a spec
# sheet, and its widest cell is where implementation prose hides (one Notes cell reached
# 991 characters of GDMA chains and ISR refills, which is API-page material).
MAX_DETAILS_TABLE_COLS = 4
MAX_DETAILS_CELL = 300
# Every card carries an image, no threshold: it either leads with one or it does not.
# Effects, modifiers and layouts show MOTION, so theirs is a .gif: a still frame of a
# moving effect says almost nothing about it. The rest are cards and controls, where a
# .png is sharper and smaller. The tree already follows this (66 gifs on effects.md, png
# throughout drivers and system); the rule writes down what it does.
# From the hook, the one home: the build renders these pages and screenshot_modules
# captures for them, so a third copy here is a third thing to forget.
import mkdocs_hooks as _hooks  # noqa: E402
ANIMATED_PAGES = _hooks.ANIMATED_PAGES
PREVIEWLESS_PAGES = _hooks.PREVIEWLESS_PAGES

# ---------------------------------------------------------------------------
# The OTHER generated surface: the `///` comments that become the technical pages.
#
# A card is read across a row; a member comment is read beside the thing it describes,
# and the generated page shows its first sentence as the summary. So the budget is one
# line: cut first, and only a fact neither the code nor the spec says goes after `@moreinfo`.
# The word cap is 30: a sentence is one thought and past twenty it is
# usually two, but that is writing advice, and a gate stops a commit only on a real
# ramble. Measured over the tree, 20 fired on the median sentence and 30 leaves the
# outliers alone. Vale's SentenceLength carries the same 30 as a suggestion.
# EVERY C++ source in the repository, not a list of directories inside one of them. A list is a
# tolerance wearing different clothes: each directory it omits is silently exempt, and nobody
# notices a new one appearing. `src` is the bulk; the rest are the entry points, the ISA generator
# and the tests, which sat outside it only by build-system convention.
#
# `test` is in scope even though nothing generates a page from it: these rules are about how the
# code READS, which is why a test's comments are held to them like any other source we own. Only
# the page rules key off a generated page, and they run from `_orphan_pages` over src/{core,light}.
HEADER_ROOTS = ("src", "test", "esp32/main", "moonbase/main", "moondeck/moonlive")
# `.cpp` too: a rule that stopped at the header was a tolerance the file extension decided, and
# the same `///` and `//` comments live on both sides of a split. A `.cpp` gets no generated page,
# so only the comment-shape rules reach it; the page rules key off the header that names it.
HEADER_SUFFIXES = ("*.h", "*.cpp")
# Upstream code, vendored rather than written here, spelled exactly as check_prose.py spells it.
# `test/doctest.h` is the single-header doctest release: vendored the same way, just not under a
# directory named for it, and holding a hundred findings nobody here will ever fix.
HEADER_EXEMPT = ("src/platform/desktop/vendor/", "src/ui/vendor/", "test/doctest.h")
MAX_CLASS_DOC = 10      # lines of `///` directly above `class X`
MAX_MOREINFO_SECTION = 10  # lines of ONE `## ` section inside the `@moreinfo` appendix
MAX_MEMBER_DOC = 1      # lines of a `///` run that is NOT a class comment
MAX_DOC_WORDS = 30      # words in one sentence of a comment, `///` or `//`
# Characters of one `//` line, whatever it holds. The word budget governs a sentence and the
# no-wrap rule keeps a sentence whole, so neither bounds a line carrying several of them: without
# this a comment stays legal while running off the side of the screen. Related sentences share a
# line up to here, which is what keeps a dense comment compact instead of fanning out one line per
# sentence.
#
# Every file, because the limit is about reading SOURCE and a header is read as source far more
# often than as a rendered page. It was briefly implementation-only, on the argument that the
# browser wraps a generated paragraph anyway, which ignores the audience doing the work.
# 250 for now, on the way down. One number, so tightening it is this line and nothing else.
# 120 was tried first and punished the compactness the other rules ask for: several short
# sentences sharing a line is good writing and runs past 120 easily, so the cap fired on 4727
# lines whose sentences were each already inside the word budget. At 250 it catches the runaway
# a reader actually struggles with, the 320-to-1057-character paragraph living on one line.
MAX_COMMENT_LINE_CHARS = 250
# The same budget over a whole `///` RUN, so the line cap cannot be satisfied by chopping. A run
# is measured in lines and each line in characters, and neither catches the move that splits one
# over-long line into two half-long ones: the text is identical and every rule passes. Multiplying
# the line cap by the run's line budget states the real limit, which is how much a reader is asked
# to take in before the next declaration.
MAX_RUN_CHARS = MAX_CLASS_DOC * MAX_COMMENT_LINE_CHARS
# A LEAD's line, which is wider. A lead is the summary that opens a header or a class, so one of
# its lines carries what the whole file is for: what the thing is, which hardware it runs on, where
# the shared body lives, what the wire format is. Held to the member cap, the driver headers each
# lost about a third of that, and what went was content rather than padding. The run budget above
# still bounds the lead as a whole, so the wider line buys density rather than sprawl.
MAX_LEAD_LINE_CHARS = 400
# The prefixes a LINE-length finding carries, so `_blocks` can recognize the one rule that warns
# even in a header. Matching on the reason keeps the exception in ONE place rather than making
# every rule pass its own severity. Prefix AND unit: "chars >" alone also matched the run rule
# (`lead N chars > M`), and the prefix alone also matched the word budget (`comment line N words`),
# each a differently motivated rule silently demoted.
_LINE_LENGTH_RULES = ("doc line ", "comment line ")
# The one rule staged the OTHER way: an implementation file's hard wrap blocks too. The tree is at
# zero, so nothing is held hostage, and a split sentence reflows every line it spans on the next
# word change, which costs a reviewer the same in a `.cpp` as in a header.
_HARD_WRAP_RULE = "hard wrap"
# One line, in every file. Depth is not forbidden, it is HOMED: a header carries it in an
# `@moreinfo` appendix (212 do) and an implementation file carries it in the same appendix on its
# own file lead. A cap of 8 in a `.cpp` was an attempt to give depth a home inline, which leaves
# the reasoning beside one call rather than where a reader looks for it.
MAX_CODE_COMMENT = 1


def _generates_a_page(rel: str) -> bool:
    """Whether this file becomes a generated documentation page.

    A header does; an implementation file does not. THE REASON, not the extension, is what the
    three rules below actually test: a page is what a member doc becomes a row on, and what a
    line budget keeps readable. Spelling it `rel.endswith(".cpp")` at each site stated the
    syntactic fact three times and left the semantic one unwritten, so a future `.hpp` or an
    excluded vendor header would be judged by a rule whose reason does not apply to it.
    """
    return not rel.endswith(".cpp")

def _blocks(key: str, why: str = "") -> bool:
    """Whether a finding fails the gate or annotates it.

    The same split `_generates_a_page` draws, carried through to severity. A header's comments
    ARE the published page, so a finding there is a documentation defect and blocks. An
    implementation file publishes nothing: its comments are a note to the next reader, and an
    over-long one is worth fixing without being worth stopping a commit for.

    Keeping both in one report is the point. A warning that vanishes is a warning nobody fixes,
    so the count stays visible per area and the sweep still closes it; what changes is that the
    remaining implementation-file work cannot hold a header's commit hostage.

    TEMPORARY, and it names its own removal: this is a staging device for the sweep, not a claim
    that an implementation comment matters less. When the `.cpp` side reaches zero this function
    goes and every finding blocks, the same way `.vale.ini` promotes a page to error once the
    sweep has finished it and loses its per-page section when the last page lands.

    ONE rule is staged in each direction. The line-length cap warns in a header too: every other
    rule describes how this tree is already written, so a finding is a defect against a standard
    the file was written under, while the cap is new and erroring on 820 lines nobody wrote
    wrongly would stop commits over a target. And NO-HARD-WRAP blocks in both kinds of file: the
    tree meets it, at zero findings, so the staging has nothing left to stage. The reason holds
    everywhere too, being the diff rather than the page, and a rule met everywhere is a rule that
    should hold everywhere.
    """
    if why.startswith(_HARD_WRAP_RULE):
        return True
    if why.startswith(_LINE_LENGTH_RULES) and "chars >" in why:
        return False
    return _generates_a_page(key.partition("::")[0])


# No hard wrap: a sentence continued on the next `///` line. The one-line budget above already
# forbids this on a header's member or code comment, so it bites where a block is ALLOWED to be
# multi-line: the class comment, the `@moreinfo` appendix, and any multi-line run in a `.cpp`.
# Same reason markdown forbids it: let the editor soft-wrap, so a one-word edit is a one-word
# diff rather than a reflowed paragraph.
_WRAP_SKIP = ("##", "@", "-", "*", "|", "```", ">")
# A NUMBERED item is a bullet that happens to start with a digit, so it is structure exactly as
# `-` is. Left out, an enumerated spec read as prose being wrapped and the joiner fused a five-item
# list onto one 299-character line, which is the layout carrying the meaning destroyed to satisfy
# a rule about sentences. Matched rather than prefixed because the marker is `1.`, not one string.
_LIST_ITEM_RE = re.compile(r"^\d+[.)]\s")
# An INDENTED line inside a comment is a block, not prose: a wire table, a listing, a diagram.
# Joining one destroys the layout that carries its meaning, and a reference table read as a
# sentence is the worst of both. Four spaces after the marker is the markdown convention.
_WRAP_INDENT = 4



def _is_structural(body: str) -> bool:
    """Whether a comment line is layout rather than prose: a list item, a table row, a fence.

    Structure is measured by what it shows, so its line count is not a budget to meet. The same
    set the no-wrap rule skips, plus an indented line, which is a block by the markdown rule the
    standards already name.
    """
    b = body.strip()
    if not b:
        return True                     # a blank `//` line separates paragraphs
    return (b.startswith(_WRAP_SKIP) or bool(_LIST_ITEM_RE.match(b))
            or body.startswith(" " * _WRAP_INDENT))




def _pages():
    """The pages the build renders as card tables: the hook's own list, so a page added
    there is checked here without a second edit."""
    import mkdocs_hooks
    return sorted(mkdocs_hooks._CATALOG_PAGES)


def _cards(text: str):
    """Every `### ` block on a page, measured the way the build splits it: a line starting
    `- ` is a control, an <img> is the preview, Detail:/Tests:/Origin: are links, and the
    rest is description. A `## ` closes the block, so a details section is never counted.

    Takes the page TEXT rather than a path, so the rules can be exercised against a page
    written to break them (test/python/test_check_docgen.py) rather than only against the
    docs tree, where a rule that silently stopped matching would look like a clean run."""
    import mkdocs_hooks as h
    lines = text.split("\n")
    # WHERE a block begins and ends is the build's rule, shared with check_specs. What the
    # lines inside it MEAN is this check's own business, and stays here.
    for title, start, end in h.split_blocks(text):
        cur = {"title": title, "desc": 0, "controls": 0, "widest": 0,
               "widest_text": "", "img": False, "img_src": ""}
        for ln in lines[start + 1:end]:
            if h._IMG_RE.match(ln):
                cur["img"] = True
                src = re.search(r'src="([^"]+)"', ln)
                cur["img_src"] = src.group(1) if src else ""
            elif (h._ORIGIN_RE.match(ln) or h._TESTS_RE.match(ln)
                  or h._TESTS_MULTI_RE.match(ln) or h._DETAIL_RE.match(ln)
                  or h._ANCHOR_RE.match(ln)):
                continue                      # renders in another cell, or not at all
            elif h._PARAM_RE.match(ln):
                control = re.sub(r"^-\s+", "", ln.strip())
                cur["controls"] += len(control)
                if len(control) > cur["widest"]:
                    cur["widest"], cur["widest_text"] = len(control), control
            elif ln.strip():
                cur["desc"] += len(ln.strip())
        yield cur


def _structure(text: str, rel: str):
    """The two rules that keep a page readable as a whole rather than card by card.

    A `## <Name>, details` section is a CONTINUATION of a card, so it belongs after every
    card on the page. Placed between two cards it splits the table in half, and a reader
    scanning the rows meets a wall of prose where the next row should be.

    And the heading form is load-bearing: the build links a card to its details by matching
    `, details` exactly, so `Infrared: details` renders a section that nothing links to. A
    COMMA rather than an em-dash, because em-dashes are banned repo-wide and a form the
    build requires must not be the one character the prose rules forbid.
    """
    import mkdocs_hooks as h
    out = []
    lines = text.split("\n")

    last_card = max((i for i, l in enumerate(lines) if h._H3_RE.match(l)), default=-1)
    card_names = {h.card_name(h._H3_RE.match(l).group("title"))
                  for l in lines if h._H3_RE.match(l)}

    # One section per card. Two headings with the same name both slug to the same anchor,
    # so the row's `More:` link resolves to whichever MkDocs emits first and the other is
    # unreachable. A sweep that adds a section where one already exists is exactly how
    # that happens, silently.
    seen = set()

    for i, ln in enumerate(lines):
        if not ln.startswith("## ") or ln.startswith("###"):
            continue
        # A details section by intent: any `## ` heading whose last word is "details".
        if not re.search(r"\bdetails\s*$", ln, re.I):
            continue
        m = h._DETAILS_RE.match(ln)
        if not m:
            out.append((f"{rel}::{ln[3:].strip()}",
                        "details heading must read `## <Name>, details` "
                        "(a comma, never an em-dash), or the build links nothing to it"))
            continue
        if i < last_card:
            out.append((f"{rel}::{m.group('name')}",
                        f"details section sits at line {i + 1}, above the last card "
                        f"(line {last_card + 1}): move it below every card"))
        if m.group("name") not in card_names:
            out.append((f"{rel}::{m.group('name')}",
                        "details heading names no card on the page, so no row links to it"))
        if m.group("name") in seen:
            out.append((f"{rel}::{m.group('name')}",
                        "a second details section with this name: both slug to one anchor, "
                        "so one of them is unreachable"))
        seen.add(m.group("name"))
    return out


# The second column links to exactly three places: the test inventory, the generated API
# page, and the card's details section. Three because all three are followed often enough
# to earn a standing position, so they sit identically on every card and the eye learns
# them once. Everything else a card points at (a how-to, an explanation, a sibling module)
# is a link the DESCRIPTION makes in a sentence, where the reader meets it in context
# rather than as a bare label under the controls.
SECOND_COLUMN_LINKS = ("Tests:", "API:", "Details:")
_LINK_LABEL_RE = re.compile(r"\*\*([A-Z][A-Za-z ]{1,12}):\*\*")


def _rendered_links(rel: str, text: str):
    """Which labeled links the build puts in the second column, per card."""
    import mkdocs_hooks as h
    out = []
    for row in h._render_catalog_table(text).split("\n"):
        if not row.startswith("| ") or row.startswith("|--") or "| Module |" in row:
            continue
        cells = row.strip("| ").split(" | ")
        if len(cells) < 2:
            continue
        for label in _LINK_LABEL_RE.findall(cells[1]):
            if f"{label}:" not in SECOND_COLUMN_LINKS:
                name = re.search(r'class="mm-name">([^<]*)', cells[0])
                out.append((f"{rel}::{name.group(1) if name else '?'}",
                            f"second column links to `{label}:`, which is not one of "
                            f"{', '.join(sorted(SECOND_COLUMN_LINKS))}: put it in the description"))
    return out


def _details_tables(rel: str, text: str):
    """Tables inside a details section: narrow enough to read, and no cell carrying an
    essay. A wide table with one huge column is the shape prose takes when it is put in a
    grid to look organized."""
    out = []
    lines = text.split("\n")
    sec = None
    for i, ln in enumerate(lines):
        m = re.match(r"^## (.+?), details$", ln)
        if m:
            sec = m.group(1)
            continue
        if ln.startswith("## "):
            sec = None
            continue
        if sec is None or not ln.startswith("|"):
            continue
        if set(ln.replace("|", "").strip()) > set("-: "):
            continue                       # not the separator row
        cols = lines[i - 1].count("|") - 1
        widest = 0
        for body in lines[i + 1:]:
            if not body.startswith("|"):
                break
            for cell in body.strip("|").split("|"):
                widest = max(widest, len(cell.strip()))
        if cols > MAX_DETAILS_TABLE_COLS:
            out.append((f"{rel}::{sec}",
                        f"details table has {cols} columns (max {MAX_DETAILS_TABLE_COLS})"))
        if widest > MAX_DETAILS_CELL:
            out.append((f"{rel}::{sec}",
                        f"details table cell is {widest} characters "
                        f"(max {MAX_DETAILS_CELL}): prose in a grid"))
    # One entry per table+rule, however many rows it has: a table reported once per body
    # row turned a single wide table into five identical lines of output.
    return list(dict.fromkeys(out))


# Every rule this check emits, longest first so "details table cell" wins over
# "details table". The FULL name, not a first word: "no image" and "one control" each
# truncate to a word that names nothing, and the two details-table rules share theirs.
_RULE_NAMES = (
    "details table cell", "details table has", "details section", "details heading",
    "public function has no", "public variable has no", "second column",
    "file opens with //", "file lead missing", "@moreinfo on a member", "@xref",
    "code comment", "comment line", "doc line", "lead",
    "appendix section",
    "class comment", "member comment", "no image", "image is", "one control",
    "description", "controls", "doc sentence", "hard wrap",
)


# A rule's key is a prefix of the message, which is stable but reads as a fragment in prose
# ("31 public function has no"). The report says what a reader would say out loud. Keys absent
# here fall through to themselves, so a new rule needs no edit until its wording is awkward.
_RULE_LABELS = {
    "code comment": "multi-line comment blocks",
    "hard wrap": "hard wraps",
    "public function has no": "undocumented functions",
    "public variable has no": "undocumented variables",
    "member comment": "member deep dives",
    "file opens with //": "files opening with //",
    "file lead missing": "missing file leads",
    "class comment": "over-long class comments",
    "doc sentence": "over-long sentences",
    "comment line": "over-wide comment lines",
    "doc line": "over-wide doc lines",
    "lead": "over-long leads",
    "appendix section": "over-long appendix sections",
    "@moreinfo on a member": "@moreinfo on a member",
    "@xref": "unresolved @xref",
    "no image": "cards with no image",
    "image is": "wrong image format",
    "one control": "cards listing one control",
    "description": "over-long descriptions",
    "second column": "wrong second column",
    "details section": "misplaced details sections",
    "details heading": "wrong details heading",
    "details table has": "over-wide details tables",
    "details table cell": "prose in a details table",
    "controls": "over-long control lists",
}


def _rule_label(key: str) -> str:
    """The human name for a rule key, for prose in the report."""
    return _RULE_LABELS.get(key, key)


def _rule_name(reason: str) -> str:
    """Which rule a finding reports, as a stable key for the baseline and the report."""
    head = reason.split(":")[0]
    for name in _RULE_NAMES:
        if head.startswith(name):
            return name
    return head.split(" ")[0]


def _headers():
    """Every source file the rules cover, repo-relative.

    RECURSIVE: the old per-directory glob was flat, so a header one level down was never
    read and its findings never counted. Widening the root without this would have reported
    a clean run over an empty set, which is the failure this check exists to prevent.
    """
    seen = set()
    for root in HEADER_ROOTS:
        for suffix in HEADER_SUFFIXES:
            for p in sorted((ROOT / root).rglob(suffix)):
                rel = p.relative_to(ROOT)
                if rel.as_posix().startswith(HEADER_EXEMPT) or rel in seen:
                    continue
                seen.add(rel)
                yield rel


def _appendix_lines(text: str) -> int:
    """How many `///` lines follow an `@moreinfo` in one file: the depth its comments hold outside the code."""
    n, inside = 0, False
    for line in text.split("\n"):
        s = line.strip()
        if not s.startswith("///"):
            inside = False
        elif inside:
            n += 1
        elif re.match(r"///\s*@moreinfo\b", s):
            inside = True
    return n


def _appendix_total() -> int:
    """The tree's appendix size, ratcheted so a cleanup cannot pass by moving comments rather than cutting them."""
    return sum(_appendix_lines((ROOT / rel).read_text(encoding="utf-8")) for rel in _headers())


def _appendix_growth() -> list:
    """The changed files whose appendix grew against HEAD, so a rise names where it came from."""
    import subprocess
    r = subprocess.run(["git", "diff", "--name-only", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    covered = {rel.as_posix() for rel in _headers()}
    out = []
    for rel in r.stdout.split():
        if rel not in covered:
            continue
        old = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, text=True)
        was = _appendix_lines(old.stdout) if old.returncode == 0 else 0
        now = _appendix_lines((ROOT / rel).read_text(encoding="utf-8"))
        if now > was:
            out.append((rel, was, now))
    return out


def _sentences(text: str):
    """The sentences in one comment line, for the word cap.

    A full stop, question mark or exclamation followed by a space ends one. An abbreviation
    ("e.g.", "i.e.") and a decimal ("0.1 bpp") carry a full stop that ends nothing, so a split
    there would report two short sentences where there is one long one, which is the opposite
    of what the cap is for. Both are rare in a comment and neither can lengthen a sentence,
    so the split is deliberately simple: it can merge, never wrongly divide.
    """
    parts = re.split(r"(?<=[.!?])\s+(?=[A-Z`\"'(\[])", text)
    return [p for p in (x.strip() for x in parts) if p]


def _wrap_rule(rel: str, lines: list, k: int, end: int, start: int = 0):
    """A sentence carried onto the next `///` line, which the editor should have soft-wrapped.

    A function rather than a loop body, because two callers need it: a member or class run,
    and a `@defgroup` file lead. The `@defgroup` branch returns early on its own budget, and
    when this test lived inline there it simply never ran on a file-level block.

    Every file, header or implementation. The reason is the DIFF, not the page: a sentence
    split across lines reflows every one of them when a word changes, so a one-word edit reads
    as a rewritten paragraph in review. That cost is the same in a `.cpp`, and one sentence per
    line is as available there as anywhere: the line simply runs long, which is what the editor
    soft-wraps.
    """
    # A list item, a heading, a table row or a fence is structure rather than prose being
    # wrapped, so each is skipped rather than read as a continuation.
    if k + 1 >= end:
        return []
    # INSIDE a fenced block nothing is prose, so nothing is a wrapped sentence. Skipping the
    # ``` lines alone was not enough: a fence's whole point is that the lines BETWEEN the
    # delimiters are verbatim, and a wire table read as prose is flagged on every row. The
    # same exemption the section cap already makes, and the one the standards state.
    fenced = False
    for j in range(start, k):
        if re.sub(r"^\s*//+\s*", "", lines[j]).strip().startswith("```"):
            fenced = not fenced
    if fenced:
        return []
    # `//+` rather than `///`: the rule binds on both markers, and stripping only the Doxygen one
    # left every `//` line with its slashes attached, so no continuation ever matched.
    body = re.sub(r"^\s*//+\s*", "", lines[k]).strip()
    nbody = re.sub(r"^\s*//+\s*", "", lines[k + 1]).strip()
    if not body or not nbody:
        return []
    if (body.startswith(_WRAP_SKIP) or nbody.startswith(_WRAP_SKIP)
            or _LIST_ITEM_RE.match(body) or _LIST_ITEM_RE.match(nbody)):
        return []
    # Either side indented means a block: leave its layout alone.
    raw = re.sub(r"^\s*//+", "", lines[k])
    nraw = re.sub(r"^\s*//+", "", lines[k + 1])
    if (len(raw) - len(raw.lstrip()) >= _WRAP_INDENT
            or len(nraw) - len(nraw.lstrip()) >= _WRAP_INDENT):
        return []
    # The test is the PREVIOUS line: a sentence that has not ended is carried over, whatever
    # the continuation happens to start with. Requiring a lowercase opener missed every
    # continuation beginning with a digit, a backtick or an identifier ("8-16 lanes...",
    # "`ParallelSlots.h`..."), which is most of a technical comment.
    # `.`, `!`, `?` only. A colon INTRODUCES what follows and a semicolon joins two clauses,
    # so neither ends a thought: Unicode's sentence-boundary rules, Vale's own detector and
    # the standards line all agree ("joined by a comma or a colon that a full stop should
    # have been"). Treating them as terminators passed every `Prior art: ...` block that
    # wrapped mid-sentence.
    if body[-1] not in ".!?":
        return [(f"{rel}::line {k + 1}",
                 "hard wrap: one line per sentence, let the editor soft-wrap")]
    return []


def _doc_runs(lines):
    """Each run of consecutive `///` lines, as (start, end, next_code_line)."""
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("///"):
            j = i
            while j < len(lines) and lines[j].lstrip().startswith("///"):
                j += 1
            nxt = lines[j].lstrip() if j < len(lines) else ""
            yield i, j, nxt
            i = j
        else:
            i += 1


def _declared_key(decl: str, start: int) -> str:
    """The baseline key for a declaration: its name, plus its arity where it takes one.

    Two overloads share a name, so keying on the name alone put both under one entry and
    the baseline then tolerated whichever the check happened to measure first (the tree
    has `PreviewDriver::emit` twice today). The parameter COUNT separates them without
    depending on how a signature is spelled, which a normalized type list would.
    """
    name = _declared_name(decl, start)
    m = re.search(r"\(([^)]*)\)", decl)
    if not m or "(" not in decl:
        return name
    args = m.group(1).strip()
    n = 0 if not args or args == "void" else args.count(",") + 1
    return f"{name}/{n}"


# A keyword reads as an identifier before `(`, so `if (ok) {` keys as `if` and a literal
# initializer keys as `true`. Neither locates the comment a finding is about, and `if/1`
# alone accounted for 56 of them; the line number is what a reader can act on.
_NOT_A_NAME = frozenset((
    "if", "for", "while", "switch", "catch", "return", "else", "do",
    "true", "false", "nullptr", "break", "continue", "sizeof", "case", "default",
))


def _declared_name(decl: str, start: int) -> str:
    """The NAME a declaration introduces, for a stable baseline key.

    Splitting on "(" and taking the last token yields `true;` from `bool ok = true;` and
    `Slot>` from a template line, so several members collapse onto one key: the baseline
    then records one member's size and reads every other as having grown. The identifier
    before `(`, `=` or `;` is the name, and a line number is the fallback where no
    declaration follows (a file-level comment block).
    """
    # The name is the identifier that PRECEDES the initializer, so `bool ok = true;` is
    # `ok` rather than `true`. Searching for the first identifier before any of `( = ;`
    # matched the initializer's value and collapsed nine members onto `::true`.
    m = re.match(r"^(?:class|struct)\s+(\w+)", decl)
    if m:
        return m.group(1)
    # The identifier immediately BEFORE the initializer, parameter list or terminator.
    # A greedy prefix swallows it and captures the initializer instead, so anchor on the
    # last identifier that precedes one of `( = ; {`.
    m = re.match(r"^[\w:<>,\s\*&\[\]]*?\b(\w+)\s*(?:\(|=|;|\{)", decl)
    if m and m.group(1) not in _NOT_A_NAME:
        return m.group(1)
    return f"line {start + 1}"


_CLASS_RE = re.compile(r"^(class|struct)\s+\w+")
# A forward declaration names a type defined elsewhere, so it documents nothing and is not a lead.
_FWD_DECL_RE = re.compile(r"^(class|struct)\s+\w+\s*;")
# A quoted string's braces are text, not scope: `const char* s = "{";` must not raise the depth.
_STRING_LIT_RE = re.compile(r'"(?:[^"\\]|\\.)*"' + r"|'(?:[^'\\]|\\.)*'")
# A free constant or function at file scope: what a `@defgroup` exists to gather under one lead.
# The trailing form accepts a signature that ENDS at the parameter list, because an Allman brace
# sits on the next line: without that a header written in that style reads as having no free
# functions at all, and the single-class rule below would flag its legitimate group.
_TOP_DECL_RE = re.compile(r"^(inline|constexpr|extern|template|using|typedef)\b|"
                          r"^[\w:<>,\s\*&]+\s+\w+\s*\([^)]*\)\s*(const)?\s*[;{]?\s*$")
_FUNC_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*\([^)]*\)\s*(const)?\s*(override)?\s*[;{]")
# A constructor and a destructor have no return type, and a pure virtual ends `= 0;`, so the
# form above matches none of them: three public declarations the "needs a ///" rule never saw.
_SPECIAL_FUNC_RE = re.compile(
    r"^(?:explicit\s+|virtual\s+)*~?(\w+)\s*\([^)]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:=\s*(?:0|default|delete)\s*)?[;{:]")
_VAR_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*(=[^;]+)?;")


def _file_comment_end(lines) -> int:
    """Where the file comment stops, as a line index.

    The file comment is the `//` block at the top, before any code. Include lines and pragmas may
    precede it and blank lines separate its paragraphs; the first line of actual content ends it,
    an `#include` included. A file with no leading comment answers 0, so nothing is exempt.

    A BLANK LINE does not end it either, which is the whole reason this is a scan rather than a
    single run: a lead is written in paragraphs, and stopping at the first blank exempted only
    the opening one. The rest then read as prose sitting on the next declaration and were
    reported as essays in the wrong place, when they are the file's documentation in the only
    place a `.cpp` has for it.
    """
    started = False
    end = 0
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st.startswith("//") and not st.startswith("///"):
            started = True
            end = i + 1            # the block reaches at least this far
            continue
        if not st:
            continue               # a blank line separates the lead's paragraphs
        if not started and st.startswith(("#", "/*", "*")):
            continue               # includes and pragmas may still precede it
        break                      # anything else, an include included, ends the lead
    return end


def _slugify_heading(text: str) -> str:
    """A heading's MkDocs anchor: lowercased, punctuation dropped, spaces to hyphens."""
    text = re.sub(r"[`*]", "", text).strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"[\s_]+", "-", text)


def _xref_resolves_rule(rel: str, lines: list[str], out: list) -> None:
    """Every `@xref{anchor}` names a heading that exists in this file.

    The marker renders to a same-page `[label](#anchor)`. A renamed heading leaves the anchor
    pointing at nothing, and the generator's final pass strips the dead link rather than failing,
    so the cross-reference disappears with no warning. This is what makes that visible.

    The name is matched LOOSELY and checked strictly, which is the whole point. The generator
    resolves `[a-z0-9-]+`, so an underscore in a name is not a link at all: it survives Doxygen
    untouched and the reader sees a raw `@xref{...}` mid-sentence. Matching the same narrow class
    here made the rule silent on exactly the names that cannot work, and it reported a clean run
    over 21 of them. So anything `@xref{...}`-shaped is a claim to check.
    """
    headings = {_slugify_heading(m.group(1))
                for ln in lines
                for m in [re.match(r"\s*///\s*#{1,6}\s+(.+)$", ln)] if m}
    for i, ln in enumerate(lines):
        for m in re.finditer(r"@xref\{([^}|]+)(?:\\?\|[^}]*)?\}", ln):
            # The label separator may be escaped as `\|`, which the generator accepts, so the
            # backslash lands in the anchor here and would fail the slug test on its own.
            anchor = m.group(1).strip().rstrip("\\")
            if not re.fullmatch(r"[a-z0-9-]+", anchor):
                out.append((f"{rel}::line {i + 1}",
                            f"@xref{{{anchor}}} is not a heading slug, so it renders as raw text"))
            elif anchor not in headings:
                out.append((f"{rel}::line {i + 1}",
                            f"@xref{{{anchor}}} names no heading in this file"))


def _file_lead_rule(rel: str, lines: list[str], out: list) -> None:
    """A header opens with `///`, so its generated page says what the file is for.

    Two shapes qualify, because two kinds of header exist. A header of free functions or constants
    leads with `@defgroup`. A single-class header leads with the class comment, which IS the file's
    documentation and already generates the page's summary. Anything else, including the `//` block
    four platform headers used to open with, generates nothing: Doxygen reads `//` as a note to the
    next reader of the source, not as documentation.
    """
    for i, ln in enumerate(lines):
        st = ln.strip()
        if not st or st.startswith(("#", "/*", "*")):
            continue
        # A provenance marker is not the file's documentation: an SPDX tag is machine-read, and
        # an `Author:` line credits where the code came from. Both sit above the real lead, so
        # they are skipped rather than counted as one, and the `///` below them generates the page.
        if st.startswith(("// SPDX-", "// Author:")):
            continue
        # The first comment in the file, whatever it is.
        if st.startswith("///"):
            return                      # a `///` lead: @defgroup or a class comment, both fine
        if st.startswith("//"):
            # Every file leads with `///`, header or not. A lead is the one comment written for
            # someone who has not read the file yet, and `///` is what carries it onto a page and
            # what lets an `@moreinfo` appendix hang off it: spelled `//` the same words reach
            # nobody but the next person to scroll past them. 216 implementation files still lead
            # with `//`, which is the sweep this states rather than an exemption.
            out.append((f"{rel}::line {i + 1}",
                        "file opens with // : a file leads with /// so it can carry an appendix"))
            return
        # Code before any comment: no lead at all. `namespace`, `using` and a forward declaration
        # are not a file's documentation, so keep looking until something documented turns up.
        if st.startswith(("namespace", "using", "extern")) or _FWD_DECL_RE.match(st):
            continue
        out.append((f"{rel}::line {i + 1}",
                    "file lead missing: a header opens with /// saying what the file is for"))
        return


def _group_scope_rule(rel: str, lines: list[str], out: list) -> None:
    """A `@defgroup` opens a scope with `@{`, and an unclosed one swallows the rest of the file.

    Doxygen's member-group scope runs from `@{` to `@}`. Left open, every declaration after the
    lead is absorbed into the group, so the generated page is silently reshaped: three headers
    shipped that way in one sweep because nothing counted the pair. The close sits INSIDE the
    namespace, just before it ends, which is where the majority of the tree already puts it and
    what keeps group membership the same in every file.
    """
    opens = [i for i, l in enumerate(lines) if l.strip() == "/// @{"]
    closes = [i for i, l in enumerate(lines) if l.strip() == "/// @}"]
    if len(opens) == len(closes):
        return
    line = (opens[len(closes)] if len(opens) > len(closes) else closes[len(opens)]) + 1
    which = "@{ with no @}" if len(opens) > len(closes) else "@} with no @{"
    out.append((f"{rel}::line {line}",
                f"unbalanced group scope: {which}, so the group runs past what it documents"))


def _group_appendix_publishes_rule(rel: str, lines: list[str], out: list) -> None:
    """A `@defgroup` scope actually contains the declarations it documents.

    A group is a label rather than an entity, so it has no location of its own and `gen_api`
    infers one from where its MEMBERS sit. A group holding no member resolves to no header, and
    the whole page is skipped: the lead, the appendix and every heading go nowhere, with nothing
    reported, because the header still LOOKS documented and the check that would notice only
    fires on a page that exists.

    The shape that loses it is a `@}` closing before the first declaration, so the brackets wrap
    only the comment and the group holds nothing at all. Two headers shipped that way. The fix is
    to close the group AFTER what it documents, which is what every publishing group already does.

    The symptom is an appendix a reader can never reach, which is why this is checked from the
    source shape rather than from a page that was never written.
    """
    opens = [i for i, l in enumerate(lines) if l.strip() == "/// @{"]
    closes = [i for i, l in enumerate(lines) if l.strip() == "/// @}"]
    if not opens or not closes:
        return                          # the scope rule above reports an unbalanced pair
    first_decl = next((i for i, l in enumerate(lines)
                       if not l.strip().startswith(("///", "//", "#", "*", "/*"))
                       and (_CLASS_RE.match(l.strip()) or l.strip().startswith("enum class")
                            or _TOP_DECL_RE.match(l.strip()))), None)
    if first_decl is None:
        return                          # nothing to document either way
    if closes[-1] < first_decl:
        out.append((f"{rel}::line {closes[-1] + 1}",
                    "group closes before it documents anything, so the page is never generated"))



def _redundant_defgroup_rule(rel: str, lines: list[str], out: list) -> None:
    """A single-class header leads with the class comment, not with a `@defgroup`.

    Two lead shapes generate a page, and which one is right follows from what the header holds.
    A header of free functions, constants or several peer types needs a `@defgroup`, because no
    one declaration speaks for the file. A header declaring ONE type has that type's comment as
    its page already, so a group around it is a second lead saying the same thing in a second
    place: the generated page then carries the group's summary and the class's, and an editor has
    two homes to keep in step.

    Counting what is top-level is the whole rule, and four shapes each defeated a naive count:

    - `template <class T>` above a class is PART of that class, not a free declaration. Read as
      one, every single-class template header was permanently exempt, which is the rule's own
      headline case.
    - A brace on its own line, Allman style, belongs to the declaration above it. Counted where
      it sits, a class after `namespace mm` + `{` read as nested and vanished.
    - A brace inside a string literal is text. `const char* s = "{";` raised the depth forever,
      so every later declaration read as nested.
    - `namespace a { namespace b {` is one line opening two scopes, so skipping the whole line
      let its contents read as top-level.

    Nested types do not count either, being implementation detail rather than anything the page
    documents.
    """

    grp = next((i for i, l in enumerate(lines) if l.strip().startswith("/// @defgroup")), None)
    if grp is None:
        return
    depth = 0
    top_level_types = 0
    others = 0
    pending_template = False
    templated_type = False              # the single top-level type is a template
    ns_debt = 0                         # namespace braces still to arrive, Allman style
    for ln in lines:
        st = ln.strip()
        if st.startswith(("//", "/*", "*", "#")):
            continue
        # A string literal's braces are text, so they are removed before any brace is counted.
        bare = _STRING_LIT_RE.sub("", ln)
        if depth == 0:
            if _CLASS_RE.match(st):
                top_level_types += 1
                # Whether THIS class carried a template line, which is what the exemption
                # below turns on. A template elsewhere in the file says nothing about it.
                if pending_template:
                    templated_type = True
                pending_template = False
            elif st.startswith("template"):
                # Whether it introduces a type or a free function is decided by the NEXT
                # declaration, so it is held rather than counted here.
                pending_template = True
            elif st.startswith("enum class"):
                others += 1
            elif _TOP_DECL_RE.match(st):
                others += 1             # a free function or constant, template or not
                pending_template = False
        # A namespace is not what nests a type, so its own braces do not count. A line may open
        # two (`namespace a { namespace b {`), and one may open on the NEXT line, so the count
        # is by keyword rather than by line: each `namespace` seen owes one brace to skip.
        opens = bare.count("{")
        closes = bare.count("}")
        ns_open = len(re.findall(r"\bnamespace\b", bare))
        if ns_open:
            skipped = min(ns_open, opens)
            opens -= skipped
            ns_debt += ns_open - skipped
        elif ns_debt and opens:
            take = min(ns_debt, opens)   # an Allman namespace brace, landing a line later
            opens -= take
            ns_debt -= take
        depth += opens - closes
        depth = max(depth, 0)
    # A TEMPLATE class is the exception: its `template <...>` line sits between the comment and
    # the class, so the comment cannot attach to the class at all and a group is the only lead
    # shape left. Reported here, the rule would push a header back into the shape that loses it.
    # The test is on THAT class, not on the file: a single-class header with an unrelated
    # template helper further down still has a class comment the page can lead with.
    if top_level_types == 1 and others == 0 and not templated_type:
        out.append((f"{rel}::line {grp + 1}",
                    "@defgroup on a single-class header: the class comment is already the page's lead"))


def _moreinfo_placement_rule(rel: str, lines: list[str], out: list) -> None:
    """`@moreinfo` is an appendix, and only a lead carries one.

    The file lead and each class or struct comment may have one; a member may not, because a member
    gets a single line and the generated page shows that line as its summary. An `@moreinfo` on a
    member is depth in the wrong place, and relaxing the member budget to allow it put a four-line
    block on every function in a swept header.
    """
    for start, end, nxt in _doc_runs(lines):
        if not any("@moreinfo" in lines[k] for k in range(start, end)):
            continue
        # A bare `///` run at the TOP of the file is a lead too. The `@defgroup` spelling is how a
        # header gathers free declarations under one page, and a file that needs no group still
        # opens with prose saying what it is for: read as a member comment that block gets one
        # line, so every such lead reported its own length as a finding. What makes a lead is its
        # POSITION, not the marker it happens to carry.
        is_lead = any("@defgroup" in lines[k] for k in range(start, end)) or (
            start == _first_content_line(lines))
        if is_lead:
            continue                    # the file lead
        if _CLASS_RE.match(nxt):
            continue                    # a class or struct comment
        out.append((f"{rel}::{_declared_key(nxt, start)}",
                    "@moreinfo on a member: the appendix belongs on the file or class lead"))


def _first_content_line(lines: list[str]) -> int:
    """The index of the first line that is neither blank nor a preprocessor or block-comment line.

    What a file lead sits on. Include lines and pragmas may precede a lead, so a `///` run opening
    at this index is the file's own comment rather than one documenting a declaration.
    """
    for i, ln in enumerate(lines):
        st = ln.strip()
        if not st or st.startswith(("#", "/*", "*")):
            continue
        return i
    return 0


def _header_rules(rel: str, text: str):
    """The comment budget, on one header.

    Counted rather than judged, so the constants at the top of this file ARE the rules and
    this docstring names no number of its own. A class comment and each `@moreinfo` `## `
    section have a line budget; any other `///` run has one line; a sentence has a word
    budget; a `//` run has one line in a header and an essay's worth in a `.cpp`; and every
    public member of a header carries a `///`. The budgets cut and the last one adds, pulling
    against each other so the result is a short line on everything rather than an essay on a
    few things.

    A file also opens with `///`, `@moreinfo` sits only on a lead, and every `@xref` resolves
    to a heading in its own file.

    `//` carries the same budget as `///` IN A HEADER, because without that the `///` cap moves
    text rather than removing it: a fifty-line member comment re-spelled as `//` satisfies
    every other rule and leaves the file exactly as long. An implementation file generates no
    page, so there the line count catches an essay rather than policing a page's shape.
    """
    out = []
    lines = text.split("\n")

    for start, end, nxt in _doc_runs(lines):
        n = end - start
        # A run that opens a @defgroup documents the FILE, not a member: it precedes an include,
        # a constant or nothing at all, so the one-line member budget measured the whole block and
        # reported a 45-line finding on every such header. It is a lead comment, so it is held to
        # the class budget and splits at @moreinfo the same way.
        # A bare `///` run at the TOP of the file is a lead too, and gets the lead budget. The
        # `@defgroup` spelling is how a header gathers free declarations under one page; a file
        # needing no group still opens with prose saying what it is for. Read as a member comment
        # that block gets ONE line, so each such lead reported its own length as a finding: what
        # makes a lead is its POSITION, not the marker it carries.
        if (any("@defgroup" in lines[k] for k in range(start, end))
                or start == _first_content_line(lines)):
            head = next((k for k in range(start, end) if "@moreinfo" in lines[k]), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::line {start + 1}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
            # And the same budget in CHARACTERS, so the line cap cannot be met by chopping one
            # long line into two shorter ones: the run is what a reader takes in, and splitting
            # it leaves that unchanged.
            chars = sum(len(re.sub(r"^\s*///\s?", "", lines[k])) for k in range(start, head))
            if chars > MAX_RUN_CHARS:
                out.append((f"{rel}::line {start + 1}",
                            f"lead {chars} chars > {MAX_RUN_CHARS}"))
            # The word cap applies here too: a file-level block is prose a reader sees on the
            # generated page, and skipping the check exempted every `@defgroup` header from it.
            for k in range(start, end):
                for sentence in _sentences(re.sub(r"^\s*///\s*", "", lines[k]).strip()):
                    if len(sentence.split()) > MAX_DOC_WORDS:
                        out.append((f"{rel}::line {k + 1}",
                                    f"doc sentence {len(sentence.split())} words > {MAX_DOC_WORDS}"))
                width = len(re.sub(r"^\s*///\s?", "", lines[k]))
                if width > MAX_LEAD_LINE_CHARS:
                    out.append((f"{rel}::line {k + 1}",
                                f"doc line {width} chars > {MAX_LEAD_LINE_CHARS}"))
                # The no-wrap rule binds here too: the file lead is the longest block in the
                # tree, so exempting it exempted the prose most likely to be wrapped.
                out += _wrap_rule(rel, lines, k, end, start)
            continue
        if _CLASS_RE.match(nxt):
            # The LEAD only: the run ends at @moreinfo, whose own lines are the appendix and
            # are measured per section below. Counting both here would put the documented
            # budget (10 lead, then 10 per appendix section) out of reach of any header.
            head = next((k for k in range(start, end) if "@moreinfo" in lines[k]), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::{nxt.split()[1].rstrip('{:')}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
            # The run cap reaches HERE too. Applied only to the `@defgroup` branch it missed the
            # block it was written for: nine lines of 359 characters is 3231, over the cap, with
            # every line inside the lead cap and every sentence inside the word budget.
            chars = sum(len(re.sub(r"^\s*///\s?", "", lines[k])) for k in range(start, head))
            if chars > MAX_RUN_CHARS:
                out.append((f"{rel}::line {start + 1}",
                            f"lead {chars} chars > {MAX_RUN_CHARS}"))
        else:
            # Both kinds of file: a `.cpp` carries a file lead with its own `@moreinfo` appendix exactly as a header does.
            #
            # Structure discounts here exactly as it does on the `//` cap. Without that, one
            # marker exempted a bulleted list and the other did not, so the same six lines were
            # clean as `//` and a finding as `///`, which is two rule sets wearing one name.
            prose = n - sum(1 for k in range(start, start + n)
                            if _is_structural(re.sub(r"^\s*///\s?", "", lines[k])))
            if prose > MAX_MEMBER_DOC:
                out.append((f"{rel}::{_declared_key(nxt, start)}",
                            f"member comment {prose} lines > {MAX_MEMBER_DOC}: "
                            f"cut it to one line"))
        for k in range(start, end):
            # PER SENTENCE, not per line. The no-wrap rule makes a line a paragraph, so a line
            # holding three short sentences is correct and a single rambling one is not: counting
            # the line would fail the first and pass nothing the standards care about. The rule
            # reads "a sentence is one thought; past twenty words it is usually two", which is a
            # statement about sentences.
            text_ = re.sub(r"^\s*///\s*", "", lines[k]).strip()
            for sentence in _sentences(text_):
                words = len(sentence.split())
                if words > MAX_DOC_WORDS:
                    out.append((f"{rel}::line {k + 1}",
                                f"doc sentence {words} words > {MAX_DOC_WORDS}"))
            # A LEAD's line is wider than a member's: it carries what the whole type is for,
            # and held to the member cap the driver headers each lost about a third of that.
            cap = MAX_LEAD_LINE_CHARS if _CLASS_RE.match(nxt) else MAX_COMMENT_LINE_CHARS
            # The PROSE, not the marker and the indent. Measuring the raw line held a member
            # nested in a class to a stricter budget than the same words at file scope, which
            # is a rule nobody wrote, and it disagreed with the run cap below that strips both.
            width = len(re.sub(r"^\s*///\s?", "", lines[k]))
            if width > cap:
                out.append((f"{rel}::line {k + 1}",
                            f"doc line {width} chars > {cap}"))
            out += _wrap_rule(rel, lines, k, end, start)

    # A file OPENS with `///`, so the generated page says what the file is for. The lead is a
    # `@defgroup` block in a header of free functions, or the class comment in a single-class
    # header, where the class IS the file. Without one the page is a bare member list: 222 headers
    # were in that state, and four of the six platform headers opened with `//`, which Doxygen
    # drops entirely.
    _file_lead_rule(rel, lines, out)

    # An unclosed `@{` reshapes the page silently, so the pair is counted.
    _group_scope_rule(rel, lines, out)

    # A group that resolves to no single header generates no page at all, appendix included.
    _group_appendix_publishes_rule(rel, lines, out)

    # Which lead shape is right follows from what the header holds: a `@defgroup` gathers free
    # declarations or peer types, where a lone class already IS the page and needs no second lead.
    _redundant_defgroup_rule(rel, lines, out)

    # `@moreinfo` is an APPENDIX, and a page has one per documented entity: the file lead and each
    # class or struct comment. A member gets one line (MAX_MEMBER_DOC), so an `@moreinfo` on one is
    # depth in the wrong place: it belongs in the file lead's appendix or on the module's page.
    _moreinfo_placement_rule(rel, lines, out)

    # An `@xref{anchor}` names a heading on the SAME generated page. When the heading is renamed
    # the link resolves to nothing, and `_strip_unresolved_anchor_links` drops it silently: the
    # reader loses the cross-reference and no build says so. Checking it here is what makes the
    # rename visible. The reverse (every section must be referenced) is NOT a rule: a `@moreinfo`
    # section is an appendix a reader scrolls to, and 101 headers carry one that nothing links.
    _xref_resolves_rule(rel, lines, out)

    # PER SECTION, not per appendix. A whole-appendix cap punishes a file for having several
    # distinct topics, and the cheapest way to satisfy it is to delete a section rather than
    # tighten the prose. Capping each `## ` section instead asks every one of them to be
    # disciplined and lets a file carry as many as it genuinely has: the platform implementations
    # each hold a handful of separately-diagnosed findings (a clock-divisor window, a latch-versus-
    # data fault, a ring's competing axes) that one shared budget could only force out of the tree.
    for i, ln in enumerate(lines):
        if "@moreinfo" not in ln:
            continue
        section, count, fenced = "(lead)", 0, False
        for m in lines[i + 1:]:
            if not m.lstrip().startswith("///"):
                break
            text = re.sub(r"^\s*///\s?", "", m).strip()
            # A fenced block is structure rather than prose, the same exemption the no-wrap rule
            # makes: a protocol listing or a table is as long as the thing it describes, and
            # counting its lines asks the author to delete wire format to fit a prose budget.
            if text.startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            if text.startswith("## "):
                if count > MAX_MOREINFO_SECTION:
                    out.append((f"{rel}::@moreinfo {section}",
                                f"appendix section {count} lines > {MAX_MOREINFO_SECTION}"))
                section, count = text[3:].strip(), 0
            elif text:
                count += 1
        if count > MAX_MOREINFO_SECTION:
            out.append((f"{rel}::@moreinfo {section}",
                        f"appendix section {count} lines > {MAX_MOREINFO_SECTION}"))

    # `//` carries the same budget as `///` in a HEADER, or the `///` cap only MOVES text:
    # re-spelling a fifty-line member comment as `//` satisfies every other rule and leaves the
    # file the same length, which is what a first pass through these headers produced.
    # An implementation file has no page to protect, so it gets the essay limit instead.
    #
    # The file comment is exempt: the non-Doxygen sibling of the class comment, with no member to
    # sit beside. That is the ONE block at the top, before anything is declared, and it used to be
    # spelled "above the first class". In a header of free functions the first class can be 250
    # lines down (MoonLiveBuiltins_light.h) or absent, which exempted every comment in the file:
    # 1002 runs across 149 files, each sitting on the declaration it documents.
    file_comment_end = _file_comment_end(lines)
    i = 0
    while i < len(lines):
        st = lines[i].strip()
        if st.startswith("//") and not st.startswith("///"):
            j = i
            while j < len(lines) and lines[j].strip().startswith("//") \
                    and not lines[j].strip().startswith("///"):
                j += 1
            cap = MAX_CODE_COMMENT
            # A list, a table or a fenced block is STRUCTURE, and its lines are its layout rather
            # than an over-long run of prose. Counted, an enumerated spec could only satisfy the
            # cap by being fused onto one line, which is the layout destroyed to pass a rule about
            # sentences. The lines that are prose still count, so an essay with a list in it is
            # measured on the essay.
            structural = sum(1 for k in range(i, j)
                             if _is_structural(re.sub(r"^\s*//+\s?", "", lines[k])))
            if j - i - structural > cap and i >= file_comment_end:
                nxt = next((lines[k].strip() for k in range(j, len(lines)) if lines[k].strip()), "")
                out.append((f"{rel}::{_declared_key(nxt, i) if nxt else f'line {i + 1}'}",
                            f"code comment {j - i - structural} lines > {cap}"))
            # The same word budget as a `///` line, and measured the same way: PER SENTENCE.
            # Counting the whole line instead made this rule fight the no-wrap rule above, which
            # asks for one sentence per line: a dense three-sentence comment then had to fan out
            # over three lines to pass, which is longer to read rather than shorter. A line
            # carrying several short sentences is good writing, and one rambling sentence is not.
            for k in range(i, j):
                text_ = re.sub(r"^\s*//+\s*", "", lines[k]).strip()
                for sentence in _sentences(text_):
                    words = len(sentence.split())
                    if words > MAX_DOC_WORDS:
                        out.append((f"{rel}::line {k + 1}",
                                    f"comment line {words} words > {MAX_DOC_WORDS}"))
                width = len(re.sub(r"^\s*//+\s?", "", lines[k]))
                if width > MAX_COMMENT_LINE_CHARS:
                    out.append((f"{rel}::line {k + 1}",
                                f"comment line {width} chars > {MAX_COMMENT_LINE_CHARS}"))
                # And the same no-wrap rule. It used to run on `///` only, which left every `//`
                # block in the tree unmeasured: the budget above says a line is a sentence, and
                # a sentence carried onto the next line breaks that whichever marker spells it.
                out += _wrap_rule(rel, lines, k, j, i)
            i = j
        else:
            i += 1

    # Every public member carries one. A generated page shows a member with no `///` as a
    # bare signature, which tells a reader nothing the declaration did not.
    # Only declarations in the CLASS BODY itself. A `{` opens a function body, and
    # `return true;` or `Preset& p = presets_[count_];` inside one parses as a
    # declaration, so 251 statements were reported as undocumented public members.
    #
    # The depth of a class body is NOT a constant: every header opens `namespace mm`, but
    # a nested struct adds another level (Hub75Slots puts its class at 3 where the others
    # are at 2). So remember WHICH depth the class opened at, rather than counting to a
    # number that is right for most files and wrong for the rest.
    # A STACK, not one depth: a nested type opens its own body and the enclosing class
    # resumes when it closes. Holding a single depth meant a nested struct overwrote the
    # class that contained it, and closing the struct then read as closing the class, so
    # every public member after one escaped this check entirely (a class with a nested
    # type reported nothing at all).
    # LOCAL types are exempt for the reason the private ones are: a struct declared inside a
    # METHOD body is unnameable outside it and reaches no generated page, so asking it for a
    # `///` documents a line no reader sees. The depth says which is which: a type declared in
    # a class body opens one level below it, one declared in a method opens two or more,
    # because the method's own `{` sits between them.
    frames = []                                  # (body_depth, public, local) per open class/struct
    public = False
    depth = 0
    class_depth = None
    in_local = False
    # The depth a FREE function's body opens at, or None outside one. `frames` is empty there,
    # so the depth test below has nothing to compare against and a struct inside `void f() { … }`
    # read as a public member, though it is as unnameable as one inside a method.
    fn_depth = None
    for i, ln in enumerate(lines):
        st = ln.strip()
        opens, closes = ln.count("{"), ln.count("}")
        before = depth
        depth += opens - closes
        if fn_depth is not None and depth < fn_depth:
            fn_depth = None                      # the function body closed
        if (not frames and fn_depth is None and opens and not _CLASS_RE.match(st)
                and not st.startswith(("namespace", "extern", "//", "/*", "*", "#"))
                and "(" in st):
            fn_depth = before + 1                # a free function's body opens here
        if _CLASS_RE.match(st) and opens:
            # A NESTED struct inherits the enclosing access: one declared after `private:` is
            # private however it is spelled, and demanding a `///` on its fields asked a file to
            # document what no reader of the generated page can see. Only a top-level struct
            # (nothing open above it) starts public.
            nested = bool(frames)
            # Below the enclosing class body rather than directly in it: a method's body
            # intervenes, so this type is local to that method and reaches no reader.
            local = (bool(frames) and (before + 1 > frames[-1][0] + 1 or frames[-1][2])) \
                or (fn_depth is not None and before >= fn_depth)
            frames.append((before + 1, public, local))
            class_depth = before + 1
            public = st.startswith("struct") and not (nested and not public)
            in_local = local
            continue
        while frames and depth < frames[-1][0]:
            _, public, _ = frames.pop()          # this body closed; the enclosing one resumes
            class_depth = frames[-1][0] if frames else None
            in_local = frames[-1][2] if frames else False
        if st.startswith("public:"):
            public = True
            continue
        if st.startswith(("private:", "protected:")):
            public = False
            continue
        if not public or not st or st.startswith(("//", "/*", "*", "#")):
            continue
        # `before` is the depth the line STARTS at: a member sits exactly in the body.
        # `friend` grants access to another type; it declares no member and reaches no
        # generated page, so asking it for a `///` documents a line no reader sees.
        if (before != class_depth or in_local
                or st.startswith(("return", "if", "for", "while", "}", "friend "))):
            continue
        # Past a `template <...>` line: doxygen attaches a comment to the declaration, and the
        # template header sits between the two. Reading only the line above asked a templated
        # member for a `///` it already had, and the answer was a second one wedged below the
        # template header, which doxygen drops and a reader sees twice.
        j = i - 1
        while j >= 0 and lines[j].lstrip().startswith("template"):
            j -= 1
        prev = lines[j].lstrip() if j >= 0 else ""
        if prev.startswith("///") or "///" in ln:
            continue
        # Header-only, and this is the ONE place that is a detector limit rather than a rule
        # difference. Everything else applies to both kinds of file; "public" here is read from a
        # declaration's SHAPE, which cannot see `namespace {` or a function body, and a `.cpp` is
        # mostly bodies. Turned on there it names `true`, `false`, `nullptr` and `break` as public
        # variables wanting a doc comment: 5599 findings, none of them real.
        if (_FUNC_RE.match(st) or _SPECIAL_FUNC_RE.match(st)) and _generates_a_page(rel):
            # In a `.cpp` this matched the constructor or destructor of a file-local RAII struct:
            # `~WinsockInit`, `Lock`, `ParkGuard`. None is public API, and each is already explained
            # by the struct's own comment above it.
            out.append((f"{rel}::{_declared_key(st, i)}", "public function has no ///"))
        elif "(" not in st and _VAR_RE.match(st) and _generates_a_page(rel):
            # "Public" here is the declaration's SHAPE, which cannot see `namespace {` or a
            # function body. In a `.cpp` that matched fields of file-local structs and plain
            # locals, naming `got`, `ok` and `cur` and asking a local variable for a doc comment.
            out.append((f"{rel}::{_declared_name(st, i)}", "public variable has no ///"))
    return out


def _card_rules(rel: str, c: dict):
    """Every rule that reads ONE card: its sizes and its image.

    A function rather than a loop body inside _violations, so a test can hand it a card
    and assert the rule fires. Read through the page's rendered form, the rules stayed
    untested against anything but the real tree, which is where a rule that had stopped
    matching would look like a clean run.
    """
    out = []
    key = f"{rel}::{c['title']}"
    visual = rel in ANIMATED_PAGES
    max_desc = MAX_DESC_VISUAL if visual else MAX_DESC
    max_control = MAX_CONTROL_VISUAL if visual else MAX_CONTROL
    if c["desc"] > max_desc:
        out.append((key, f"description {c['desc']} > {max_desc}"))
    if c["widest"] > max_control:
        out.append((key, f"one control {c['widest']} > {max_control}: "
                         f"{c['widest_text'][:60]}..."))
    # A card leads with its picture. The image is the first thing the eye reaches on a
    # row, and a card without one starts with a name against blank space, which reads as
    # a gap rather than as a module that happens to be invisible. The summary pages are
    # the exception: their rows describe machinery with no card in the UI to capture.
    if not c["img"]:
        if rel not in PREVIEWLESS_PAGES:
            out.append((key, "no image: every card leads with one"))
    else:
        want = ".gif" if rel in ANIMATED_PAGES else ".png"
        if not c["img_src"].lower().endswith(want):
            why = ("an effect, modifier or layout shows motion" if want == ".gif"
                   else "a card or control is sharper and smaller as a png")
            out.append((key, f"image is {c['img_src'].rsplit('.', 1)[-1]}, "
                             f"not a {want.lstrip('.')}: {why}"))
    return out


# The domains that carry a generated moxygen tree. IMPORTED rather than restated: hard-coding
# ("core", "light") here once left every page of a third domain reporting as unreachable, and a
# copy kept in step by hand is only correct until someone moves. One tuple owns the answer.
from gen_api import DOMAINS as _DOC_DOMAINS  # noqa: E402


def _orphan_pages():
    """Generated pages nothing links to: a technical page a reader cannot reach.

    Every `.h` under a domain directory gets a page, so a header nobody references from a
    catalog card, another page, or another header's `///` is documentation that exists and
    is unreachable. The link may come from anywhere: a card's Detail line, a prose page, or
    a sibling header naming the file (the hook retargets a `.h` mention at its page).
    """
    # Keyed by (DOMAIN, stem): core/ and light/ each carry their own moxygen tree, so a page
    # sharing a name across the two would mask the other and one domain's link would silently
    # mark the other domain's page as reachable.
    pages = {(p.parts[-3], p.stem)
             for p in (ROOT / "docs" / "moonmodules").rglob("moxygen/*.md")}
    linked = set()
    for md in (ROOT / "docs").rglob("*.md"):
        if "moxygen" in md.parts:
            continue
        for m in re.finditer(r"(?:(\w+)/)?moxygen/(\w+)\.md", md.read_text(errors="ignore")):
            # A relative link inside a domain omits it, so fall back to the linking page's own.
            domain = m.group(1) or (md.parts[-2] if md.parts[-2] in _DOC_DOMAINS else None)
            for d in ((domain,) if domain else _DOC_DOMAINS):
                linked.add((d, m.group(2)))
    for d in _DOC_DOMAINS:
        for h in (ROOT / "src" / d).rglob("*.h"):
            for m in re.finditer(r"\b(\w+)\.h\b", h.read_text(errors="ignore")):
                if m.group(1) != h.stem:
                    linked.add((d, m.group(1)))
    return [(f"moonmodules/{domain}::{name}", "generated page nothing links to: unreachable")
            for domain, name in sorted(pages - linked)]


def _duplicate_group_ids():
    """Each `@defgroup` id is claimed by one header.

    Doxygen MERGES two groups sharing an id, so one header's page absorbs the other's and the
    loser vanishes from the site. It fails silently in both directions: the surviving page looks
    complete and the missing one is simply absent, so only a reader looking for the second page
    ever notices. A core parser and a light-domain one both named their group `PinList` and took
    `drivers_PinList.md` off the site that way; the strict docs build caught it by the dangling
    link alone, which is luck rather than a guarantee.

    Cross-file by nature, so it sits here rather than in the per-header rules. An implementation
    file counts too, even though it generates no page of its own: Doxygen reads it all the same,
    so a `.cpp` re-declaring its header's group id merges into that page and is a conflict like
    any other. A header and its own `.cpp` sharing an id is the common shape, and it is reported
    against the `.cpp`, since the header is the one that owns the page.
    """
    seen: dict[str, list[str]] = {}
    for rel in _headers():
        s = rel.as_posix()
        for ln in (ROOT / rel).read_text(encoding="utf-8").splitlines():
            st = ln.strip()
            if st.startswith("/// @defgroup"):
                parts = st.split(None, 2)
                if len(parts) >= 3:
                    seen.setdefault(parts[2].split()[0], []).append(s)
    out = []
    for gid, files in sorted(seen.items()):
        if len(files) < 2:
            continue
        # The header owns the page, so it is the one kept and the others are reported.
        owner = min(files, key=lambda f: (not _generates_a_page(f), f))
        for f in sorted(files):
            if f == owner:
                continue
            out.append((f"{f}::@defgroup {gid}",
                        f"@defgroup id `{gid}` is also claimed by {owner}, and Doxygen "
                        "merges them, so one page absorbs the other"))
    return out


# Markdown prose is hard-wrapped when a paragraph line continues onto the next. The standards
# forbid it for the same reason the header rule does: the editor soft-wraps, so a one-word edit
# is a one-word diff rather than a reflowed paragraph. Checked over AUTHORED pages only, since a
# generated one carries its source's shape and is fixed at the generator.
# Indentation is tested on the RAW line above, since these are matched after stripping.
_MD_SKIP_PREFIX = ("#", "|", "-", "*", ">", "<", "```", "!", "=", ":")
_MD_NUMBERED = re.compile(r"^\d+[.)]\s")


def _md_generated(text: str) -> bool:
    """A page a tool rewrites, which every generator marks with the same warning.

    Keyed on "do not edit by hand" rather than on "generated by": a hand-written page may well
    say what produced the files it lists, and exempting on that wording let an authored index
    out of the check. A tool that owns a file says so in the imperative.
    """
    head = "\n".join(text.split("\n")[:8]).lower()
    return "do not edit by hand" in head


def _md_hard_wraps(rel: str, text: str):
    """Each paragraph line that continues onto the next one."""
    if _md_generated(text):
        return []
    out = []
    lines = text.split("\n")
    fenced = False
    for i, line in enumerate(lines[:-1]):
        stripped = line.strip()
        if stripped.startswith("```"):
            fenced = not fenced
            continue
        if fenced or not stripped:
            continue
        # INDENTATION is read before stripping: an indented block is a listing or a fenceless code
        # sample, not prose, and the stripped line can never carry the leading whitespace that says so.
        if line.startswith(("    ", "\t")) or stripped.startswith(_MD_SKIP_PREFIX) or _MD_NUMBERED.match(stripped):
            continue
        nxt = lines[i + 1].strip()
        if (not nxt or lines[i + 1].startswith(("    ", "\t"))
                or nxt.startswith(_MD_SKIP_PREFIX) or _MD_NUMBERED.match(nxt)):
            continue
        # A line ending mid-sentence is the wrap; one ending a sentence is a deliberate break.
        if stripped[-1] not in ".!?:":
            out.append((f"{rel}::line {i + 1}",
                        "hard wrap: one line per paragraph, let the editor soft-wrap"))
    return out


def _violations():
    out = []
    for rel in _pages():
        path = ROOT / "docs" / rel
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        out.extend(_structure(text, rel))
        out.extend(_rendered_links(rel, text))
        out.extend(_details_tables(rel, text))
        for card in _cards(text):
            out.extend(_card_rules(rel, card))

    for rel in sorted((ROOT / "docs").rglob("*.md")):
        parts = rel.relative_to(ROOT / "docs").parts
        # moxygen writes generated pages; docs/work/ is the exemption the standards name.
        if "moxygen" in parts or parts[0] == "work":
            continue
        # DOCS-relative, the key catalog findings use: a repo-relative one listed the same
        # page twice in the report, once per spelling.
        out.extend(_md_hard_wraps(rel.relative_to(ROOT / "docs").as_posix(), rel.read_text(encoding="utf-8")))

    for rel in _headers():
        out.extend(_header_rules(rel.as_posix(), (ROOT / rel).read_text(encoding="utf-8")))
    out.extend(_duplicate_group_ids())
    out.extend(_orphan_pages())
    return out


REPORT = ROOT / "docs" / "reference" / "metrics" / "docgen.md"


# Which summary page owns a source folder, mirroring the zoom diagram in
# documentation-standards.md. A sweep runs page by page, so the report groups by the same unit:
# the question "how far is the sweep" is answered per area, not per file.
#
# Longest prefix wins, so `light/moonlive` beats `light`. A path matching nothing is "unassigned",
# which is itself the finding: a header no summary page owns reaches no reader.
DOC_AREAS = (
    ("src/core/system",           "core/system.md"),
    ("src/core/services",         "core/services.md"),
    ("src/core/module",           "core/system.md"),
    ("src/core/util",             "core/supporting.md"),
    # The numeric vocabulary is the LIGHT domain's, whatever directory it sits in: effects and
    # scripts write against it and the control surface never touches it. A directory prefix put
    # five headers and 240 findings on the control page, where the sweep would have documented
    # them under a heading no reader of that page is looking for. Longest prefix wins, so these
    # file-level entries override the `src/core/util` line above.
    ("src/core/util/math16.h",      "light/power-functions.md"),
    ("src/core/util/math8.h",       "light/power-functions.md"),
    ("src/core/util/noise.h",       "light/power-functions.md"),
    ("src/core/util/oscillators.h", "light/power-functions.md"),
    ("src/core/util/color.h",       "light/power-functions.md"),
    ("src/core/moonlive",         "light/moonlive.md"),
    ("src/light/moonlive",        "light/moonlive.md"),
    ("src/light/effects",         "light/effects.md"),
    ("src/light/layouts",         "light/layouts.md"),
    ("src/light/modifiers",       "light/modifiers.md"),
    ("src/light/drivers",         "light/drivers.md"),
    ("src/light/powerfunctions",  "light/power-functions.md"),
    ("src/light/layers",          "light/supporting.md"),
    ("src/light/util",            "light/supporting.md"),
    ("src/ui",                    "core/ui.md"),
    ("src/platform",              "platform/index.md"),
    # The composition roots and the second boot image. `main.cpp` wires the module tree and
    # registers every docPath on the system page, the ESP32 entry point calls into it, and
    # MoonBase is the firmware-update surface that page documents.
    ("src/main.cpp",              "core/system.md"),
    ("esp32/main",                "core/system.md"),
    ("moonbase/main",             "core/system.md"),
    # A MoonLive backend tool: it compiles a script through one real assembler and prints
    # the bytes, which is what `check_encodings.py` drives.
    ("moondeck/moonlive",         "light/moonlive.md"),
)


def _doc_area(path: str) -> str:
    """The summary page that owns `path`, or a bucket for what no page covers."""
    best = ""
    area = ""
    for prefix, page in DOC_AREAS:
        if path.startswith(prefix) and len(prefix) > len(best):
            best, area = prefix, page
    if area:
        return area
    if path.startswith("test/"):
        return "(tests, no card)"
    return "(unassigned)"


def _write_report(found, appendix=None) -> None:
    """The current state as a tracked page, so its git history is the trend.

    The baseline records WHICH cards are tolerated. This records HOW MUCH is left, per page
    and per rule, so `git log -p docs/reference/metrics/docgen.md` answers whether the
    documentation is getting better. Same shape as repo-health.md, for the same reason.
    """
    from collections import Counter, defaultdict
    by_page = defaultdict(list)
    for key, why in found:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    errs = [f for f in found if _blocks(f[0], f[1])]
    warns = [f for f in found if not _blocks(f[0], f[1])]
    rules = Counter(_rule_name(why) for _, why in found)
    rules_e = Counter(_rule_name(why) for _, why in errs)
    rules_w = Counter(_rule_name(why) for _, why in warns)

    out = ["# Docgen", "",
           "Generated by [`moondeck/check/check_docgen.py`](../../../moondeck/check/check_docgen.py) "
           "on every run. **Do not edit by hand.**", "",
           "Every place the generated documentation breaks the shape [the standards]"
           "(../../contributing/documentation-standards.md#the-card) define. Current state only: "
           "the trend is this file's git history. The list only shrinks.", "",
           f"**{len(errs)} error(s)** and **{len(warns)} warning(s)** "
           f"across {len(by_page)} page(s).", "",
           *([f"**{appendix} appendix line(s)** after `@moreinfo`. This only falls as well: a cleanup "
              "cuts first, and moves only what neither the code, the test name nor the spec says.", ""]
             if appendix is not None else []),
           "An error is in a file that generates a documentation page, a header or a catalog page, "
           "so the finding is a defect in what gets published and it fails the gate. A warning is "
           "in an implementation file, which publishes nothing: its comments are a note to the "
           "next reader, worth fixing without being worth stopping a commit for. Both are counted "
           "here, because a warning nobody sees is a warning nobody fixes.", "",
           "The split is temporary. It stages the sweep rather than ranking the two kinds of "
           "comment, so when the warning column reaches zero it goes and every finding blocks.", "",
           "## By rule", "", "| Rule | Errors | Warnings |", "|---|---:|---:|"]
    # Errors first, since that is the number a gate turns on: a rule with more warnings than
    # errors would otherwise outrank the one actually blocking the commit.
    for rule in sorted(rules, key=lambda r: (-rules_e.get(r, 0), -rules_w.get(r, 0), r)):
        out.append(f"| {_rule_label(rule)} | {rules_e.get(rule, 0)} | {rules_w.get(rule, 0)} |")
    # By the unit a sweep actually runs in: one summary page and the headers it owns.
    areas = defaultdict(int)
    areas_e = defaultdict(int)
    areas_w = defaultdict(int)
    for page, items in by_page.items():
        if page.endswith(".md"):
            continue
        area = _doc_area(page)
        areas[area] += len(items)
        # Per FINDING, through _blocks: a file is not wholly one severity, because a header's
        # line-length finding warns while its neighbors block.
        for _, why in items:
            (areas_e if _blocks(page, why) else areas_w)[area] += 1
    if areas:
        out += ["", "## By documentation area", "",
                "The unit a sweep runs in: one summary page and the headers it owns, as the "
                "[hierarchy zoom](../../contributing/documentation-standards.md"
                "#zooming-in-on-the-green-boxes) lays them out. A page is swept when its errors "
                "reach zero; its warnings say how much implementation-file cleanup is left "
                "behind that.", "",
                "| Summary page | Errors | Warnings |", "|---|---:|---:|"]
        for area in sorted(areas, key=lambda a: (-areas_e[a], -areas[a], a)):
            out.append(f"| `{area}` | {areas_e[area]} | {areas_w[area]} |")
    cards = {k: v for k, v in by_page.items() if k.endswith(".md")}
    headers = {k: v for k, v in by_page.items() if not k.endswith(".md")}
    if cards:
        out += ["", "## Catalog pages", "", "| File | Findings |", "|---|---:|"]
        for page in sorted(cards, key=lambda k: (-len(cards[k]), k)):
            out.append(f"| `{page}` | {len(cards[page])} |")

    # PER AREA, because the area is the unit a sweep runs in and the question a reader has is
    # "what would I open first". A flat list of 300 files answered that only by being read whole,
    # and the per-finding dump under it was 90% of a 4,200-line page: a log rather than a report.
    # Each area gets its own files ranked, a tail line for the long thin end, and its rule mix.
    # What a single finding says is what the check PRINTS; this page is where the work is planned.
    out += ["", "## Where the work is", "",
            "Per area, since that is the unit a sweep runs in: the files ranked, then the rule mix. "
            "A file's own findings are in the check's output, which names every one.", ""]
    per_area = defaultdict(dict)
    for page, items in headers.items():
        per_area[_doc_area(page)][page] = items
    for area in sorted(per_area, key=lambda a: (-sum(len(v) for v in per_area[a].values()), a)):
        files = per_area[area]
        total = sum(len(v) for v in files.values())
        # Ranked WITHIN each kind, and each kind gets its own head, so the biggest warning file
        # cannot be buried in the tail behind ten smaller errors. An 88-finding implementation
        # file is worth seeing even though it does not block.
        # Counted through _blocks, which reads the REASON as well as the file: a header's
        # line-length finding warns, so a file holding only those is a warning file. Grouping
        # on the file alone labeled 74 such files "error" under a report saying 0 errors.
        def _blocking(page, items):
            return [f for f in items if _blocks(page, f[1])]
        errs_a = sum(len(_blocking(k, v)) for k, v in files.items())
        out += [f"### {area}", "",
                f"**{errs_a} error(s)** and **{total - errs_a} warning(s)** "
                f"across {len(files)} file(s).", "",
                "| Findings | File | |", "|---:|---|---|"]
        HEAD = 10
        for kind, blocking in (("error", True), ("warning", False)):
            counts = {k: len([f for f in v if bool(_blocks(k, f[1])) == blocking])
                      for k, v in files.items()}
            group = sorted((k for k in files if counts[k]), key=lambda k: (-counts[k], k))
            for page in group[:HEAD]:
                out.append(f"| {counts[page]} | `{page}` | {kind} |")
            tail = group[HEAD:]
            if tail:
                n = sum(counts[p] for p in tail)
                lo, hi = min(counts[p] for p in tail), max(counts[p] for p in tail)
                span = f"{lo}" if lo == hi else f"{lo}-{hi}"
                out.append(f"| {span} each | *{len(tail)} more {kind} files, {n} findings* | |")
        # Every ERRORING file's findings in full, right where the work is planned. An error
        # blocks a commit, so the one question its reader has is what to fix, and a summary
        # sends them back to a stdout that has scrolled away. Warnings stay counted rather
        # than listed: 3135 of them is the log this page was rewritten to stop being.
        for page in sorted(files):
            errs_f = [f for f in files[page] if _blocks(page, f[1])]
            if not errs_f:
                continue
            out += [f"#### `{page}`", ""]
            for key, why in sorted(errs_f, key=lambda f: f[1]):
                where = key.rpartition("::")[2] if "::" in key else ""
                out.append(f"- {where + ': ' if where else ''}{why}")
            out += [""]

        mix = Counter(_rule_name(why) for k, v in files.items()
                      for _, why in v if _blocks(k, why))
        mix_w = Counter(_rule_name(why) for k, v in files.items()
                        for _, why in v if not _blocks(k, why))
        out += [""]
        if mix:
            out += ["By rule, errors: "
                    + ", ".join(f"{n} {_rule_label(r)}" for r, n in mix.most_common()) + ".", ""]
        if mix_w:
            out += ["By rule, warnings: "
                    + ", ".join(f"{n} {_rule_label(r)}" for r, n in mix_w.most_common()) + ".", ""]
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Docgen report: {len(found)} finding(s) written to {REPORT.relative_to(ROOT)}")


def _report(entries, heading: str) -> None:
    """Every entry, grouped by page, with a per-page count.

    A check that prints only a total tells a reader nothing they can act on, and this one
    runs as a MoonDeck card whose whole output is its log. So the detail is the report.
    """
    from collections import defaultdict
    by_page = defaultdict(list)
    for key, why in entries:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    print(heading)
    for page in sorted(by_page):
        rows = by_page[page]
        print(f"\n  {page}  ({len(rows)})")
        for title, why in sorted(rows):
            print(f"    {title}: {why}")


def _committed_counts():
    """Every rule's warning count in the COMMITTED report, or None when there is no baseline.

    The report is tracked, so the last commit's copy is the number to beat. Read from git rather
    than from the working tree, because the run rewrites the file before the comparison happens.
    """
    import subprocess
    rel = REPORT.relative_to(ROOT).as_posix()
    r = subprocess.run(["git", "show", f"HEAD:{rel}"],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    counts = {}
    # Only the `## By rule` table: the area and file tables share its column shape, and a row from
    # either would enter the baseline as a rule that can never appear in `now`.
    in_rules = False
    for line in r.stdout.split("\n"):
        if line.startswith("## "):
            in_rules = line.strip() == "## By rule"
        m = re.match(r"^\| (.+?) \| (\d+) \| (\d+) \|$", line.strip())
        if in_rules and m and m.group(1) != "Rule":
            counts[m.group(1)] = int(m.group(3))
        # The TOTAL as its own entry, because a per-rule comparison alone misses a rule that is
        # below the baseline and still rising: one under budget can absorb a new finding silently.
        m = re.match(r"^\*\*\d+ error\(s\)\*\* and \*\*(\d+) warning\(s\)\*\* across", line.strip())
        if m and "(total)" not in counts:
            counts["(total)"] = int(m.group(1))
        m = re.match(r"^\*\*(\d+) appendix line\(s\)\*\*", line.strip())
        if m:
            counts["(appendix lines)"] = int(m.group(1))
    return counts or None


def _ratchet(found, appendix=None) -> list:
    """Which rules rose against the committed report. The list only shrinks.

    A warning is staged work, and staged work that grows is not a sweep. Per RULE rather than on
    the total alone, because a total hides one rule paying for another: splitting a long line
    lowers the width count and raises the block count, which is the move the run cap exists to
    refuse.
    """
    from collections import Counter
    base = _committed_counts()
    if base is None:
        return []
    # LABELLED, matching how the report writes them: `_rule_name` returns the internal key
    # ("code comment") while the table carries the display label ("multi-line comment blocks"),
    # and comparing the two key spaces silently compares nothing.
    now = Counter(_rule_label(_rule_name(why)) for key, why in found if not _blocks(key, why))
    now["(total)"] = sum(1 for key, why in found if not _blocks(key, why))
    # The UNION, not the baseline alone: the report lists only rules the run found, so a rule at
    # zero is absent from `base` and iterating it let that rule rise silently while the total fell.
    # A rule the baseline never saw starts at zero, which is what "only shrinks" means for it too.
    risen = [(rule, base.get(rule, 0), now.get(rule, 0))
             for rule in sorted((set(base) | set(now)) - {"(appendix lines)"}) if now.get(rule, 0) > base.get(rule, 0)]
    # Its own line, because warnings that fall while the appendix grows were moved, not cut.
    if appendix is not None and "(appendix lines)" in base and appendix > base["(appendix lines)"]:
        risen.append(("(appendix lines)", base["(appendix lines)"], appendix))
    return risen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()

    found = _violations()

    # The report is the artifact: stdout scrolls away and is truncated, the file is what the
    # reviewer reads, and its git history is the only record of the sweep's trend. Written on
    # every run with no way to suppress it: the run still prints and still returns its exit
    # code, so writing it costs a caller nothing and a skipped write loses the number to beat.
    appendix = _appendix_total()
    _write_report(found, appendix)

    errors = [f for f in found if _blocks(f[0], f[1])]
    warnings = [f for f in found if not _blocks(f[0], f[1])]

    risen = _ratchet(found, appendix)
    if not found and not risen:
        print(f"Docgen check: clean. Limits: description {MAX_DESC} "
              f"({MAX_DESC_VISUAL} visual), one control {MAX_CONTROL} "
              f"({MAX_CONTROL_VISUAL} visual), one comment line, "
              f"{MAX_DOC_WORDS} words.")
        return 0

    print(f"Docgen check: {len(errors)} error(s), {len(warnings)} warning(s).\n")
    if risen:
        print("ROSE against the committed report. These only shrink:")
        for rule, was, now in risen:
            print(f"  {rule}: {was} -> {now}")
            if rule == "(appendix lines)":
                for rel, a, b in _appendix_growth():
                    print(f"    {rel}: {a} -> {b}")
        print("  Fix them, or say in the commit why the rule itself changed.\n")
    if errors:
        _report(errors, "ERRORS, in files that generate a page. These fail the gate.")
    if warnings:
        if errors:
            print()
        _report(warnings, "WARNINGS, in files that generate no page. Worth fixing, "
                          "not worth blocking a commit.")
    print("\nCut first. Move only what neither the code, the test name nor the spec says,"
          "\nand prefer an existing home over a new appendix section."
          "\nRules: docs/contributing/documentation-standards.md § The card.")
    return 1 if (errors or risen) else 0


if __name__ == "__main__":
    sys.exit(main())

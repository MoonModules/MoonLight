"""MkDocs build hooks. Wired via `hooks:` in mkdocs.yml. Three jobs:

1. on_files — synthesise `reference/tests/unit-tests.md` + `reference/tests/scenario-tests.md` into
   MkDocs' virtual file tree from the test files (via `_test_metadata.py`, the
   same parser the CLI generator + MoonDeck use). So the inventory pages are NOT
   committed to the repo and can't drift — rebuilt from source every build. The
   per-effect `[Tests]` links in the catalog pages point at these pages; kept as
   plain one-line links so the effect cards stay compact (a link, not a case dump).

2. on_page_markdown — repoint links that escape docs/ into repo files
   (`../src/*.h`, `../CLAUDE.md`, `../moondeck/*`) to absolute GitHub blob URLs,
   since the site can't host `src/` (pre-Doxide) and a relative link would 404.

3. on_post_build (serve-only) — stage the web installer into the built site under
   /install/ so the local preview mirrors production's single-origin layout.
"""

import json
import logging
import os
import posixpath
import re
from pathlib import Path

# moondeck/docs/ is on sys.path when mkdocs runs from repo root; import the shared parser.
import sys
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from _test_metadata import (  # noqa: E402
    ROOT,
    collect_unit_files,
    parse_unit_file,
    collect_scenario_files,
)
from generate_test_docs import render_unit_tests, render_scenarios  # noqa: E402
import gen_api  # noqa: E402  (same dir, on sys.path via _HERE above)
from gen_help import ATTRIBUTIONS, catalog_rows, display_name  # noqa: E402


# ---- source-link rewrite: docs link OUT to repo files the site can't host ----

# The blob base for links that escape docs/ into the repo (src/, moondeck/, test/,
# CLAUDE.md, README.md, mooninstaller/). The site can't serve these — src/ isn't
# published — so a relative link 404s on the deployed site. Rewrite to an absolute
# GitHub blob URL so "source [Foo.h]" resolves everywhere (locally-served preview +
# moonmodules.org). A `.h` link for a module that HAS a generated technical page is
# repointed at that in-site page instead (see _API_MODULES / on_page_markdown); only
# files with no generated page fall through to the GitHub blob URL.
_BLOB_BASE = gen_api._BLOB_BASE   # one definition — branch-pinned, see gen_api._blob_base()

# {module stem: domain} for every module that got a generated technical page this
# build, e.g. {"Control": "core", "FireEffect": "light"} — populated by on_files,
# read by the link retargeter to build the domain-nested target path. Empty when the
# doxygen/moxygen toolchain is absent (so links fall back to GitHub, unchanged).
_API_MODULES: dict[str, str] = {}

# Repo top-level dirs/files a doc may link into but the site doesn't host.
_OUT_OF_DOCS = ("src/", "moondeck/", "test/", "esp32/", "mooninstaller/",
                ".github/", "CLAUDE.md", "README.md", "library.json", "CMakeLists.txt",
                ".clang-tidy", ".clangd")

# A markdown link into a repo file the site doesn't host. Two authored shapes, both
# common in the transient docs/work/ notes:
#   ../../src/foo   — climbs out of docs/ with `../`
#   src/foo         — repo-ROOT-relative, no `../` (resolves to a nonexistent docs/src/foo)
# The pattern accepts an optional `../` run, then the href; _sub sorts out which case.
_OUT_LINK_RE = re.compile(r'\]\((?P<href>(?:\.\./)*(?P<rest>[^)#]+)(?P<frag>#[^)]*)?)\)')

# The repo-root-relative prefixes a bare (no-`../`) link must start with to be treated
# as an out-of-docs source link. `docs/` is included because a plan that links
# `docs/plan.md` / `docs/architecture-light.md` (files that never existed or were
# renamed) still resolves outside the current page and 404s — send it to GitHub.
_ROOT_REL_PREFIXES = _OUT_OF_DOCS + ("docs/",)


def _rewrite_out_of_docs_links(markdown: str, src_uri: str) -> str:
    """Rewrite links that climb out of docs/ into repo files → absolute GitHub blob
    URLs, resolved against the page's own location so any `../` depth is handled.
    src_uri is the page's path under docs/ (e.g. 'moonmodules/light/effects.md')."""
    # The page's directory within docs/ — the anchor `../` hops resolve against.
    page_dir = Path("docs") / src_uri
    page_dir = page_dir.parent

    # Depth from this page up to docs/ (so a `.h` link can be repointed at the
    # in-site generated API page with the right relative `../` count).
    up = "../" * len(Path(src_uri).parent.parts) if str(Path(src_uri).parent) != "." else ""

    def _sub(m: re.Match) -> str:
        href = m.group("href")
        frag = m.group("frag") or ""
        rel = href.split("#", 1)[0]
        # A `.h` whose module has a generated technical page → point at the in-site
        # page, not GitHub. The generated reference IS the source view we want. The
        # target nests by domain (core/light), so use the recorded domain. EXCEPT a
        # `#L<n>` line-number fragment: that only resolves on GitHub's blob view (the
        # generated page has no line anchors), so such links fall through to the blob
        # rewrite below.
        stem = Path(rel).stem
        is_line_anchor = re.match(r'#L\d', frag)
        # EXCEPT on a generated moxygen page: that page IS the `.h`'s technical view, so
        # repointing its own `Source: Foo.h` banner at the in-site page links it to
        # ITSELF and the reader can never reach the header. A generated page's `.h`
        # links stay GitHub blob URLs.
        on_generated_page = "/moxygen/" in src_uri
        if rel.endswith(".h") and stem in _API_MODULES and not is_line_anchor and not on_generated_page:
            domain = _API_MODULES[stem]
            return f"]({up}moonmodules/{domain}/moxygen/{stem}.md{frag})"
        # Resolve against the page dir to a repo-relative path (the correct case).
        target = os.path.normpath(str(page_dir / rel)).replace(os.sep, "/")
        if target.startswith(_OUT_OF_DOCS) and not target.startswith("docs/"):
            return f"]({_BLOB_BASE}/{target}{frag})"
        # Fallback for links authored relative to the REPO ROOT (common in the
        # transient docs/work/ notes, written before their file sat
        # this deep under docs/): strip leading `../` and see if the remainder is
        # itself an out-of-docs repo path. Catches `../../moondeck/x.py` from a
        # docs/work/past/plans/ page, which resolves to a nonexistent docs/moondeck/x.py,
        # AND a bare `src/x.js` / `docs/plan.md` with no `../` at all (same intent,
        # written root-relative) — both 404 in-site, so send them to the GitHub blob.
        stripped = re.sub(r'^(?:\.\./)+', '', rel)
        if stripped.startswith(_ROOT_REL_PREFIXES):
            return f"]({_BLOB_BASE}/{stripped}{frag})"
        return m.group(0)  # in-docs link (incl. ../ into another docs page) — leave it

    return _OUT_LINK_RE.sub(_sub, markdown)


# ---- MkDocs hooks ----

# ---- catalog-page table transform: render prose ### blocks as a MoonLight-style table ----

# The consolidated catalog pages whose ### effect/modifier/layout blocks are rendered
# as a table (source stays authored as readable prose blocks; the table is build-time).
# The ONE list of pages rendered as card tables, and the one home for it: this is the code
# that renders them, so a page is a catalog page exactly when the build treats it as one.
# check_specs and check_docgen import this rather than keeping their own (check_specs kept a
# four-entry copy whose paths stopped existing at the docs restructure, so its catalog branch
# quietly never ran).
_CATALOG_PAGES = {
    "moonmodules/light/effects.md",
    "moonmodules/light/modifiers.md",
    "moonmodules/light/layouts.md",
    "moonmodules/light/drivers.md",
    # Summary pages for the non-catalog module groups use the same ### -block → table
    # transform, one common authoring process for every summary page (supporting rows
    # just leave the preview/controls columns blank).
    "moonmodules/core/supporting.md",
    "moonmodules/core/system.md",
    "moonmodules/core/services.md",
    "moonmodules/light/supporting.md",
}

# The module types whose preview is MOTION, so their image is a .gif rather than a .png:
# a still frame of a moving effect says almost nothing about it. One home, because three
# places act on it: check_docgen enforces the format, screenshot_modules captures it, and
# the catalog pages carry the result.
ANIMATED_TYPES = ("effects", "modifiers", "layouts")
ANIMATED_PAGES = frozenset(f"moonmodules/light/{t}.md" for t in ANIMATED_TYPES)

# The summary pages, whose rows carry no preview. A catalog card leads with its image
# because a reader picks an effect by looking at it, where these rows describe a base
# class, a binding type or an init-order sequencer: there is no card in the UI to capture.
# The other card rules still apply, since a summary row is read across like any other.
PREVIEWLESS_PAGES = frozenset({
    "moonmodules/core/supporting.md",
    "moonmodules/light/supporting.md",
})

_H3_RE = re.compile(r'^###\s+(?P<title>.+?)\s*$')
# A card title is `Name <emoji> · qualifier`: the name runs until the first word that does
# not start with a letter, digit or "(". Written as a class rather than a list of the emoji
# in use, because every new emoji silently broke the previous form (a name kept its emoji,
# so its `## <Name>, details` section stopped resolving) and nothing failed loudly.
_CARD_NAME_RE = re.compile(r"\s+[^\w(]")


def card_name(title: str) -> str:
    """The card's name: its title up to the first emoji or separator."""
    return _CARD_NAME_RE.split(title)[0].strip()
_H2_RE = re.compile(r'^##\s+')                         # any level-2 heading = section boundary
_ANCHOR_RE = re.compile(r'^<a id="(?P<id>[^"]+)"></a>\s*$')
_IMG_RE = re.compile(r'^<img\b.*?>\s*$')
_PARAM_RE = re.compile(r'^-\s+')
_ORIGIN_RE = re.compile(r'^Origin:\s*(?P<body>.+?)\s*$')
# `[Tests](href)` — the single-link form every card used before a card could cover several
# modules. `Tests: [RMT](..) · [Moon](..)` is the multi-link form (mirrors the `Detail:` line),
# for a merged card whose modules each have their own test section.
_TESTS_RE = re.compile(r'^\[Tests\]\((?P<href>[^)]+)\)\s*$')
_TESTS_MULTI_RE = re.compile(r'^Tests:\s*(?P<body>.+?)\s*$')
_DETAIL_RE = re.compile(r'^Detail:\s*(?P<body>.+?)\s*$')   # `Detail: [Foo.md](..) · …` → Links column
# `## <Name>, details`. A COMMA, not an em-dash: em-dashes are banned repo-wide
# (documentation-standards.md), and a heading form the build requires must not be the
# one character the prose rules forbid.
_DETAILS_RE = re.compile(r'^##\s+(?P<name>.+?),\s+details\s*$')


def _split_control(text: str):
    """A control line as (name, separator, description), written `- `name`: what it does`."""
    return text.partition(": ")


def split_blocks(markdown: str):
    """Every `### ` block on a catalog page, as (title, start, end) line indices.

    Where a card begins and ends is ONE rule, and three readers need it: this renderer,
    check_specs looking for the block that documents a module, and check_docgen measuring
    a card against the standards. Each had its own copy, and check_specs' drifted far
    enough that its block-finding accepted four different link shapes to compensate.

    A `### ` opens a block; the next `### ` or `## ` closes it, which is what keeps a
    `## <Name>, details` section out of the card it follows, and so does the `</div>` closing a deck that holds the cards.
    """
    lines = markdown.split("\n")
    starts = [i for i, ln in enumerate(lines) if _H3_RE.match(ln)]
    for n, i in enumerate(starts):
        end = next((j for j in range(i + 1, len(lines))
                    if _H3_RE.match(lines[j]) or _H2_RE.match(lines[j]) or lines[j].strip() == "</div>"), len(lines))
        yield _H3_RE.match(lines[i]).group("title"), i, end


_SITE_RE = re.compile(r"https://moonmodules\.org/MoonLight/([^)\"\s#]*)(#[^)\"\s]*)?")


def _site_relative(md: str, src_uri: str) -> str:
    """The help's absolute site links made relative to the page, so the build checks them and a local preview shows its own files: `moonmodules/core/services.html` is `../core/services.md`."""
    def repl(m: re.Match) -> str:
        path, anchor = m.group(1), m.group(2) or ""
        if path.endswith(".html") or not path:
            path = (path[:-5] or "index") + ".md"
        if path == src_uri:
            return anchor or "#"
        return posixpath.relpath(path, posixpath.dirname(src_uri)) + anchor
    return _SITE_RE.sub(repl, md)


_help_cache = {}


def _help_file() -> dict:
    """docs/help/help.json, read once per build."""
    if "file" not in _help_cache:
        path = Path(__file__).resolve().parent.parent.parent / "docs" / "help" / "help.json"
        _help_cache["file"] = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    return _help_cache["file"]


def generated_catalog(src_uri: str) -> str | None:
    """A catalog page drawn whole from help.json, for a page whose markdown is gone. The intro comes from the base class its modules share, the deck holds a heading per module for fill_cards_from_help to draw, and each module's guide is its details section."""
    page = src_uri.removeprefix("moonmodules/")
    cat = _help_file().get("catalogs", {}).get(page)
    if not cat:
        return None
    mods = sorted(((t, m) for t, m in _help_file().get("modules", {}).items()
                   if m.get("doc") == page or m.get("doc", "").startswith(page + "#")),
                  key=lambda tm: display_name(tm[0]).lower())
    out = [f"# {cat['title']}", ""]
    if cat.get("image"):
        out += [f'<img src="{_site_relative(cat["image"], src_uri)}" width="300" alt="{cat.get("alt", "")}">', ""]
    out += [_site_relative(cat.get("line", ""), src_uri), ""]
    for para in (p for p in cat.get("summary", "").split("\n\n") if p):
        out += [_site_relative(para, src_uri), ""]
    if cat.get("video"):
        out += [f'<video src="{_site_relative(cat["video"], src_uri)}" controls playsinline width="360" title="{cat.get("caption", "")}"></video>', ""]
    # The deck is the page: its cards render as the catalog table until the script replaces them, which is what search indexes and a reader without JavaScript sees.
    out += ["<noscript><style>.mm-deck .mm-catalog-wrap { display: block; }</style></noscript>", "",
            f'<div class="mm-deck" data-page="{page}" markdown="1">', ""]
    for t, m in mods:
        anchor = m["doc"].partition("#")[2] or _slug(display_name(t))
        suffix = m.get("kind") or (f"{m['dim']}D" if m.get("dim") else "")
        out += [f'<a id="{anchor}"></a>', "", f"### {display_name(t)} {m.get('tags', '')}".rstrip() + (f" · {suffix}" if suffix else ""), ""]
    out += ["</div>", ""]
    for t, m in mods:
        if m.get("guide"):
            out += [f"## {display_name(t)}, details", ""]
            for g in m["guide"]:
                out += [f"#### {g['title']}", "", _site_relative(g["text"], src_uri), ""]
    return "\n".join(out)


def catalog_markdown(src_uri: str) -> str | None:
    """A catalog page's markdown: the file while it exists, else the page generated from help.json."""
    path = Path(__file__).resolve().parent.parent.parent / "docs" / src_uri
    return path.read_text(encoding="utf-8") if path.exists() else generated_catalog(src_uri)


def _help_modules() -> dict:
    """docs/help/help.json's modules, keyed by the registry link that names each one's card."""
    if "modules" not in _help_cache:
        modules = _help_file().get("modules", {})
        _help_cache["modules"] = {m["doc"]: m for m in modules.values() if m.get("doc")}
    return _help_cache["modules"]


def _tests_link(url: str, src_uri: str) -> str:
    """The test inventory's section for a test file, which reads its cases as sentences, where the file names its `@module`; else the file on GitHub, the link the help itself carries."""
    rel = url.split("/blob/", 1)[-1].partition("/")[2]
    path = ROOT / rel
    module = parse_unit_file(path).get("module") if rel and path.exists() else None
    if not module:
        return url
    page = posixpath.relpath("reference/tests/unit-tests.md", posixpath.dirname(src_uri))
    return f"{page}#{_slug(module)}"


def _card_control(line: str) -> str:
    """A control's line as a card writes it, `name: what it does`, lowercase after the colon. The help keeps the sentence's capital; an acronym such as `FSEQ` keeps its own."""
    return re.sub(r"^([A-Z])(?=[a-z])", lambda m: m.group(1).lower(), line)


def fill_cards_from_help(markdown: str, src_uri: str) -> str:
    """Each card cut down to its heading, drawn from the module's help: the header's text, so the card shows what the app shows and nothing describes the module twice. A card with a body is left as written, which is the strangler-fig bridge while the headers take over."""
    lines = markdown.split("\n")
    page = src_uri.removeprefix("moonmodules/")
    for title, start, end in reversed(list(split_blocks(markdown))):
        if any(ln.strip() and not _ANCHOR_RE.match(ln) for ln in lines[start + 1:end]):
            continue
        anchors = [m.group("id") for ln in lines[max(0, start - 3):start] if (m := _ANCHOR_RE.match(ln))]
        entry = next((_help_modules().get(f"{page}#{a}") for a in anchors
                      if f"{page}#{a}" in _help_modules()), None)
        if entry is None:
            continue
        name = card_name(title)
        body = [""]
        if entry.get("image"):
            alt = entry.get("alt") or f"{name} preview"
            body += [f'<img src="{_site_relative(entry["image"], src_uri)}" width="300" alt="{alt}">', ""]
        body += [_site_relative(" ".join([entry["line"]] + entry.get("summary", "").split("\n\n")).strip(),
                                src_uri), ""]
        body += [f"- `{c}`: {_card_control(_site_relative(h['line'], src_uri))}"
                 for c, h in entry.get("controls", {}).items()]
        if entry.get("attribution"):
            body += ["", f"Origin: {_site_relative(entry['attribution'], src_uri)}"]
        stem = posixpath.basename(entry["source"]).removesuffix(".h")
        body += ["", f"Detail: [technical](moxygen/{stem}.md)"]
        if entry.get("tests"):
            body += ["", f"[Tests]({_tests_link(entry['tests'], src_uri)})"]
        # The next card's `<a id>` sits inside this block, right above that card's heading: keep it.
        anchors_after = [ln for ln in lines[start + 1:end] if _ANCHOR_RE.match(ln)]
        lines[start + 1:end] = body + [""] + [x for a in anchors_after for x in (a, "")]
    return "\n".join(lines)


def _cell(s: str) -> str:
    """A markdown-table cell: collapse newlines to <br> so multi-line content
    survives inside a single pipe cell."""
    return s.replace("\n", "<br>")


def _slug(text: str) -> str:
    """The heading anchor id MkDocs generates for a `## Heading`, so a hand-built
    `#anchor` link resolves. Delegates to Python-Markdown's OWN toc slugify (the exact
    function MkDocs' toc uses) rather than re-implementing it — a hand-rolled mimic
    drifts on unicode (accent-stripping), consecutive separators, and decoration.
    e.g. 'LED output — details' -> 'led-output-details'. `markdown` is always present
    in the build env (it's a MkDocs dependency)."""
    from markdown.extensions.toc import slugify
    return slugify(text, "-")


def _row_links(b: dict, details_names: set) -> list:
    """A card's three links, Tests, API and Details, always all three."""
    # THREE links, and always all three: Tests, API and Details. A card offers the same
    # doors on every row, so a reader learns their position once instead of re-reading a
    # list whose length changes per card. Everything else was prose wearing a link's clothes: the
    # details section is already named in the card's own description, so a `More:` row restated a
    # sentence the reader had read. The attribution reads under the description instead.
    #
    # A missing target still renders its label, greyed, rather than vanishing: a card with
    # no tests is a gap worth seeing, and a row that silently loses a link hides it.
    tests = (b["testsMulti"] if b.get("testsMulti")
             else f'[{b["testsName"]}]({b["tests"]})' if b["tests"]
             else '<span class="mm-missing">none yet</span>')
    detail = (re.sub(r'^\[technical\]', '[reference]', b["detail"]) if b["detail"]
              else '<span class="mm-missing">none yet</span>')
    links = [f":material-test-tube: **Tests:** {tests}",
             f":material-api: **API:** {detail}"]
    # Details is a fixed row, not a per-card extra: most cards have one and a reader
    # follows it often, so it earns a standing position. Any other link a card wants is
    # made in the description, in the sentence that needs it.
    name = card_name(b["title"])
    details = (f"[{name}, details](#{_slug(name + ', details')})" if name in details_names
               else '<span class="mm-missing">none yet</span>')
    links.append(f":material-text-long: **Details:** {details}")
    return links


def _emit_row(b: dict, details_names: set) -> str:
    """One 2-column table row for a parsed ### block: module (preview, name,
    description) and details (controls, then the labeled links)."""
    # Col 1: anchored name (so a help #anchor lands on the row) + description. A
    # merged card (e.g. the LED-output drivers) carries several ids so each old
    # per-driver anchor still resolves onto the one row.
    anchor_spans = "".join(f'<span id="{a}"></span>' for a in b.get("anchors", []))
    # The NAME leads the cell and the preview sits under it: a reader scanning the
    # table is looking for a name, and a picture above it pushes the thing they are
    # scanning for down the row. The image stays in this column rather than its own,
    # which is what kept a 900-character description from rendering as a ribbon.
    img_html = ""
    if b["img"]:
        img = re.sub(r'\s+(width|height)="[^"]*"', "", b["img"])
        img_html = "<br>" + img.replace("<img ", '<img class="mm-preview" ', 1)
    # Name in a distinct styled span, description below in a muted span, so the two
    # read as title + subtitle rather than one run-on line (styled in extra.css).
    name = f'{anchor_spans}<span class="mm-name">{b["title"]}</span>'
    desc = " ".join(b["desc"])
    # The attribution under the description, muted: writing our own rather than depending removes the credit a dependency carries automatically, so the card carries it instead.
    if b.get("origin"):
        desc += f'<br><span class="mm-attribution">Attribution: {b["origin"]}</span>'
    col1 = _cell(f'{name}{img_html}<br><span class="mm-desc">{desc}</span>'
                 if desc else f'{name}{img_html}')

    # Col 2: controls, one per line, split into the name part (keeps its `code` chips,
    # styled accent) and the description (greyed via .mm-pdesc).
    #
    # The separator is a colon, never an em-dash, which the prose rules ban repo-wide. A
    # control without one renders whole in the name part.
    col2_parts = []
    for p in b["params"]:
        p = re.sub(r'^-\s+', '', p)
        head, sep, tail = _split_control(p)
        if sep:
            col2_parts.append(f'<span class="mm-param">{head}'
                              f' — <span class="mm-pdesc">{tail}</span></span>')
        else:
            col2_parts.append(f'<span class="mm-param">{p}</span>')
    col2 = "".join(col2_parts) if col2_parts else "—"

    links = _row_links(b, details_names)
    # Controls and links share ONE cell: the links column was 18% of the page, which made
    # a three-link card taller than its own controls. Stacked under the controls they read
    # as what the card offers and where it continues.
    col2_links = (f'<br><span class="mm-links">{_cell("<br>".join(links))}</span>'
                  if links else "")

    return f"| {col1} | {col2}{col2_links} |"


def _render_catalog_table(markdown: str) -> str:
    """Render each run of consecutive `### ` blocks as a 2-column table, section by
    section. Everything else — the page H1/lead, `## Group` headers, the trailing
    `## Source` list, `## <Name>, details` sections, stray prose between blocks,
    passes through verbatim, so a table only ever contains genuine module blocks
    (a `##` heading closes the current table). Source .md stays authored as prose."""
    lines = markdown.split("\n")
    details_names = {_DETAILS_RE.match(ln).group("name") for ln in lines if _DETAILS_RE.match(ln)}

    out = []
    rows = []                # accumulated table rows for the current run
    cur = None               # block being parsed
    pending_anchors = []     # <a id> lines seen before the next ### (a merged card can carry several)

    def flush_block():
        nonlocal cur
        if cur is not None:
            rows.append(_emit_row(cur, details_names))
            cur = None

    def flush_table():
        """Emit the accumulated rows as one wrapped table, then reset."""
        if rows:
            out.append('<div class="mm-catalog-wrap" markdown="1">')
            out.append("")
            out.append("| Module | Details |")
            out.append("|--------|---------|")
            out.extend(rows)
            out.append("")
            out.append("</div>")
            out.append("")
            rows.clear()

    for ln in lines:
        if _ANCHOR_RE.match(ln):
            pending_anchors.append(_ANCHOR_RE.match(ln).group("id"))
            continue
        if _H3_RE.match(ln):                       # start a new block (same table run)
            flush_block()
            cur = {"anchors": pending_anchors, "title": _H3_RE.match(ln).group("title"),
                   "desc": [], "img": None, "params": [], "origin": None,
                   "tests": None, "testsMulti": None, "detail": None,
                   "testsName": card_name(_H3_RE.match(ln).group("title"))}
            pending_anchors = []
            continue
        if ln.strip() == "</div>":                 # the end of a container holding cards, such as a deck
            flush_block()
            flush_table()
            out.append(ln)
            continue
        if _H2_RE.match(ln):                       # section boundary: close block + table
            flush_block()
            flush_table()
            for a in pending_anchors:              # stray anchors before a ## — keep them
                out.append(f'<a id="{a}"></a>')
            pending_anchors = []
            out.append(ln)
            continue
        if cur is not None:                        # inside a block: classify the line
            if _IMG_RE.match(ln):
                cur["img"] = ln.strip()
            elif _PARAM_RE.match(ln):
                cur["params"].append(ln.strip())
            elif _ORIGIN_RE.match(ln):
                cur["origin"] = _ORIGIN_RE.match(ln).group("body")
            elif _TESTS_RE.match(ln):
                cur["tests"] = _TESTS_RE.match(ln).group("href")
            elif _TESTS_MULTI_RE.match(ln):
                cur["testsMulti"] = _TESTS_MULTI_RE.match(ln).group("body")
            elif _DETAIL_RE.match(ln):
                cur["detail"] = _DETAIL_RE.match(ln).group("body")
            elif ln.strip():
                cur["desc"].append(ln.strip())
            # blank line inside a block is a separator — ignore
            continue
        # outside any block and not a heading: passthrough (intro, prose, Source items)
        out.append(ln)

    flush_block()
    flush_table()
    for a in pending_anchors:
        out.append(f'<a id="{a}"></a>')
    return "\n".join(out)


def _deck_page(title: str, catalog: str, page: str) -> str:
    """A catalog page as a card deck: a container help-deck.js fills from help.json."""
    return (f"# {title}\n\n"
            f"Every module of the [{catalog} catalog]({posixpath.basename(page)}) as a card, drawn "
            f"from the help the app shows. Tap a card to turn it, filter by its emoji or search.\n\n"
            f'<div class="mm-deck" data-page="{page}"></div>\n')


def on_pre_build(config):
    """Forget the last build's help.json, so a live server draws the file as it is now."""
    _help_cache.clear()


def on_files(files, config):
    """Inject build-time-generated pages into the virtual tree: the test inventories,
    and (Phase 4b) a per-module API reference for each infrastructure module,
    generated from its `///` source comments by gen_api (Doxygen → moxygen)."""
    from mkdocs.structure.files import File

    def _add(src_uri: str, content: str):
        f = File.generated(config, src_uri, content=content)
        existing = files.get_file_from_path(src_uri)   # replace a same-URI file if any
        if existing:
            files.remove(existing)
        files.append(f)

    _add("reference/tests/unit-tests.md", render_unit_tests(collect_unit_files()))
    # The card deck renders help with the app's own help.js, served beside assets/help-deck.js, so a card on the site reads as the module's help does on a device. One deck per catalog page.
    _add("assets/help.js", (ROOT / "src" / "ui" / "help.js").read_text(encoding="utf-8"))
    # A catalog page whose markdown is gone is generated from help.json, under the same name, so every link and anchor into it holds.
    for page in _help_file().get("catalogs", {}):
        if not (ROOT / "docs" / "moonmodules" / page).exists():
            _add(f"moonmodules/{page}", generated_catalog(f"moonmodules/{page}"))
    # A generated catalog is a deck itself, so only a written catalog gets a deck page beside it.
    for page, title in (("light/effects", "Effects"), ("light/layouts", "Layouts"), ("light/modifiers", "Modifiers"),
                        ("light/drivers", "Drivers"), ("core/system", "System"), ("core/services", "Services")):
        if (ROOT / "docs" / "moonmodules" / f"{page}.md").exists():
            _add(f"moonmodules/{page}-deck.md", _deck_page(f"{title} deck", title.lower(), f"{page}.md"))
    _add("reference/tests/scenario-tests.md", render_scenarios(collect_scenario_files()))

    # Source-generated technical pages. gen_api.generate() writes each to disk under
    # docs/moonmodules/<domain>/moxygen/<Module>.md (gitignored, so a human can preview
    # the .md directly) and returns {uri: markdown}. We still inject via _add so THIS
    # build sees them (MkDocs' file scan ran before this hook, so the fresh writes
    # aren't in `files` yet; _add de-dups a same-URI entry a later scan would find).
    # Empty dict when doxygen/npx are absent (local without the toolchain) — the site
    # still builds; the pages appear in CI. But if the tools ARE present and fail (npx
    # fetch error, doxygen crash, near-empty output), generate() raises GenApiError,
    # which propagates and fails the build — so a transient failure can't silently ship
    # a docs site with zero API pages. Record {stem: domain} so on_page_markdown can
    # retarget a `.h` link to the domain-nested page.
    global _API_MODULES
    api_pages = gen_api.generate()
    # uri: 'moonmodules/<domain>/moxygen/<Stem>.md' → {'<Stem>': '<domain>'}
    _API_MODULES = {
        Path(uri).stem: Path(uri).parts[1] for uri in api_pages
    }
    for uri, md in api_pages.items():
        _add(uri, md)
    return files


# Pages that embed a file living at the REPO ROOT via a pymdownx snippet. Their
# borrowed text spells doc links `docs/explanation/architecture/index.md` — correct from the root (that
# is where CLAUDE.md is read by agents and on GitHub), but one level too deep once the
# text is rendered from inside docs/, where it resolves to docs/docs/… and 404s.
_EMBEDS_REPO_ROOT_FILE = {"contributing/principles-and-process.md"}

# `href="docs/<rest>"` → `href="<rest>"`, and the same for the `.md` → `.html` form MkDocs
# has already applied by this stage. Only the `docs/` prefix is dropped; anchor and path
# ride along untouched.
_DOCS_PREFIXED_HREF_RE = re.compile(r'href="docs/([^"#]+?)(\.md)?(#[^"]*)?"')

# The same borrowed text also links to files OUTSIDE docs/ (`moondeck/MoonDeck.md`), which
# have no page on the site at all. `_rewrite_out_of_docs_links` already sends those to the
# GitHub blob, but it runs at markdown stage where this page is still an unexpanded
# `--8<--` directive — so the embedded ones need the same treatment here.
_OUT_OF_DOCS_HREF_RE = re.compile(
    rf'href="({"|".join(re.escape(p) for p in _OUT_OF_DOCS)})([^"#]*)(#[^"]*)?"')

# The page's first `<h1 ...>text</h1>`, split so the tag and its attributes (the id the
# table of contents anchors on) survive and only the text is replaced.
_FIRST_H1_RE = re.compile(r"(<h1[^>]*>)(.*?)(</h1>)", re.S)


def _rebase_repo_root_doc_links(html):
    """Drop the leading `docs/` from links in content embedded from the repo root.

    Runs on the rendered HTML, not the source markdown: pymdownx expands a `--8<--`
    snippet during markdown *conversion*, so at on_page_markdown time this page is still
    just the one-line directive and there is nothing to rewrite yet.

    The alternative — rewriting CLAUDE.md's own links to be docs-relative — would break
    them everywhere they are actually used (an agent reading the file, GitHub's own
    rendering) to satisfy one embedded copy. Rebasing at build time keeps the source
    correct at the root and correct on the site.

    `.md` becomes `.html` here too. MkDocs normally does that itself, but only for links
    whose target it resolved during validation — and these did not resolve, precisely
    because of the `docs/` prefix this function strips. So they arrive still pointing at
    the source file and would 404 on the built site.
    """
    def _fix(m):
        path, _, anchor = m.group(1), m.group(2), m.group(3) or ""
        return f'href="{path}.html{anchor}"' if _ else f'href="{path}{anchor}"'
    html = _DOCS_PREFIXED_HREF_RE.sub(_fix, html)
    return _OUT_OF_DOCS_HREF_RE.sub(
        lambda m: f'href="{_BLOB_BASE}/{m.group(1)}{m.group(2)}{m.group(3) or ""}"', html)


class _MuteRebasedLinkWarnings(logging.Filter):
    """Drop the link warnings for the page whose links this hook rebases after validation.

    `principles-and-process.md` embeds CLAUDE.md, whose links (`docs/explanation/architecture/index.md`,
    `moondeck/MoonDeck.md`) are correct where that file is actually read — the repo root and
    GitHub — and `_rebase_repo_root_doc_links` turns them into working site links. But
    validation is a markdown treeprocessor inside `Page.render()`, so it judges the
    pre-rebase text and reports 13 broken links the built site does not have.

    Filtering the log is the narrow fix. `validation.links.not_found: ignore` in mkdocs.yml
    would hide genuinely missing pages across every other doc, and MkDocs has no per-page
    validation setting; the `inclusion` levels that do suppress warnings (`DRAFT` and below)
    also drop the page from the built site.
    """

    def filter(self, record):
        msg = record.getMessage()
        return not any(f"Doc file '{p}' contains a link" in msg for p in _EMBEDS_REPO_ROOT_FILE)


logging.getLogger("mkdocs.structure.pages").addFilter(_MuteRebasedLinkWarnings())


def on_page_content(html, page, config, files):
    """Post-conversion fixups for a page that embeds a repo-root file: rebase its doc
    links, and replace the embedded `# <filename>` heading with the page's own title.

    The embedded file opens with a heading that names it as a file (`# CLAUDE.md`) —
    right at the repo root and on GitHub, wrong in a docs menu, where the reader wants
    the subject. Front matter `title:` fixes the nav entry and the browser tab but not
    the rendered H1, so it is swapped here rather than by editing the source file."""
    if page.file.src_uri not in _EMBEDS_REPO_ROOT_FILE:
        return html
    html = _rebase_repo_root_doc_links(html)
    if page.title:
        html = _FIRST_H1_RE.sub(
            lambda m: f"{m.group(1)}{page.title}{m.group(3)}", html, count=1)
    return html


_GITHUB_OWNER_RE = re.compile(r"^https://github\.com/([A-Za-z0-9-]+)(?:/|$)")


def attributed_by(markdown: str) -> str:
    """The attribution catalog as the site shows it. Under each name goes its picture: the Wikimedia Commons file the row names, else its GitHub avatar. The Attributed by column lists the modules naming the entry by link or name, read from help.json so the page lists none by hand."""
    rows = {i: (names, url, picture) for i, names, url, picture in catalog_rows(markdown)}
    users = {i: [] for i in rows}
    for t, m in sorted(_help_file().get("modules", {}).items(), key=lambda tm: display_name(tm[0]).lower()):
        attr = m.get("attribution", "")
        urls = set(re.findall(r"\]\((https?://[^)\s]+)\)", attr))
        said = {re.sub(r"^\[([^\]]+)\]\(.*\)$", r"\1", s.strip()).lower() for s in attr.split(",")}
        said |= {s.lower() for s in re.findall(r"\[([^\]]+)\]\(", attr)}
        page, _, anchor = m.get("doc", "").partition("#")
        name = f"[{display_name(t)}](../moonmodules/{page}#{anchor})" if page else display_name(t)
        for i, (names, url, _) in rows.items():
            if (url and url in urls) or any(n.lower() in said for n in names):
                users[i].append(name)
    out = []
    for i, ln in enumerate(markdown.split("\n")):
        if i in rows:
            _, url, picture = rows[i]
            cell = ln.split("|")[1]
            name = cell.partition(" File:")[0].strip()
            owner = _GITHUB_OWNER_RE.match(url or "")
            img = '<img src="{}" width="48" height="48" alt="" loading="lazy" style="width: 48px; height: 48px; object-fit: cover">'
            if picture:
                # The file page credits the picture's author and license, as reuse of a Commons file asks.
                name += f'<br><a href="https://commons.wikimedia.org/wiki/File:{picture}">{img.format(f"https://commons.wikimedia.org/wiki/Special:FilePath/{picture}?width=96")}</a>'
            elif owner:
                name += "<br>" + img.format(f"https://github.com/{owner.group(1)}.png?size=96")
            ln = ln.replace(cell, f" {name} ", 1) + f" {', '.join(users[i])} |"
        elif ln.startswith("| Name "):
            ln += " Attributed by |"
        elif ln.startswith("|---"):
            ln += "---|"
        out.append(ln)
    return "\n".join(out)


def on_page_markdown(markdown, page, config, files):
    """Repoint out-of-docs source links to GitHub blob URLs, then (on the catalog
    pages) render the prose ### blocks as a MoonLight-style 2-column table. Source
    .md stays authored as readable blocks; the table is build-time only."""
    markdown = _rewrite_out_of_docs_links(markdown, page.file.src_uri)
    if page.file.src_uri in _CATALOG_PAGES:
        markdown = _render_catalog_table(fill_cards_from_help(markdown, page.file.src_uri))
    elif page.file.abs_src_path and Path(page.file.abs_src_path) == ATTRIBUTIONS:
        markdown = attributed_by(markdown)
    return markdown


def on_post_build(config):
    """LOCAL PREVIEW ONLY: stage the web installer into the built site under
    /install/, so the local docs preview mirrors the SINGLE-ORIGIN production
    layout (docs at /, installer at /install/). Without it the docs preview
    (:8422) 404s on the Flash link's /MoonLight/install/ target — the installer
    preview is a separate server on :8421, and a production-relative link can't
    reach it. Copying it in makes every /install/ link resolve locally exactly
    as deployed.

    Gated to serve mode via MM_DOCS_SERVE (set by build_docs.py --serve): in CI,
    release.yml does its OWN, more complete install/ staging (incl. the
    releases/<tag>/ firmware binaries), so this must NOT run there and risk
    overlaying the binary-less repo copy over it. Mirrors release.yml's
    `cp -r mooninstaller/. pages/install/` + the install-picker*.js + library.json
    siblings. Best-effort: absent files are skipped."""
    import os
    import shutil

    if os.environ.get("MM_DOCS_SERVE") != "1":
        return  # CI / plain build — release.yml owns install/ staging

    site = Path(config["site_dir"])
    dst = site / "install"
    src = ROOT / "mooninstaller"
    if not src.is_dir():
        return
    dst.mkdir(parents=True, exist_ok=True)
    shutil.copytree(src, dst, dirs_exist_ok=True)
    # The picker's JS halves live in src/ui/ (embedded in firmware too); CI stages
    # them beside the installer. library.json feeds the installer its version.
    for extra in ("src/ui/install-picker.js", "src/ui/install-picker-devices.js", "library.json"):
        p = ROOT / extra
        if p.is_file():
            shutil.copy(p, dst / p.name)

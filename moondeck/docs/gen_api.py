"""Generate per-module technical Markdown from source, at MkDocs build time.

The source `.h` is the single home of technical content; these pages are generated
*views* of the `///` comments in it — nothing is hand-restated. Pipeline: Doxygen
(the de-facto-standard parser — robust on the C++20 that a Tree-sitter tool choked
on) emits XML; moxygen renders it to Markdown with our custom template
(moondeck/docs/moxygen-templates/). Called by mkdocs_hooks.py's on_files, the output
injected into the virtual tree under moonmodules/{core,light}/moxygen/<Module>.md —
the domain-nested layout the § Documentation model standard defines.

**Every** `.h` under src/{core,light} gets a page — core and light, module and
utility alike. Discovery is exhaustive and automatic (no hand-maintained list): a
richly-`///`-commented header yields a full page, a sparsely-commented one a thin
page. Curation is a *separate* layer: only MoonModule subclasses (the things with
controls) appear in the end-user summary tables; a non-module header (a wire-format
struct, a math utility) has no table row but is still reachable as a generated page
and cross-linked from the pages that use it. Catalog effects/modifiers/layouts/
drivers get a page from their `///` too; their *controls* surface stays in the
summary-page cards, since those come from runtime `controls_.add(...)` calls no
static tool sees.

Doxygen (a brew/apt binary) and moxygen (via npx) are NOT uv-installable — the one
justified non-uv dependency (like the ESP-IDF Python exception). If either is absent
the generator returns nothing and the site builds without these pages (they appear
in CI, where both are provisioned); a contributor without doxygen still gets the rest.
"""

import os
import re
import shutil
from functools import lru_cache
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TEMPLATES = Path(__file__).resolve().parent / "moxygen-templates" / "cpp"

# A floor on how many pages a healthy run produces. Below this, something broke
# (doxygen parsed nothing, moxygen emitted nothing, the class→header map is empty) —
# raise rather than write a near-empty API set. Set well under the real count (~114
# today) so it only trips on genuine breakage, not on adding/removing a few headers.
# The tree holds ~190 headers; 150 is a floor that catches a generator regression (a filter
# that drops real pages, an empty XML) while leaving room for headers to come and go.
_MIN_EXPECTED_PAGES = 150


class GenApiError(RuntimeError):
    """The Doxygen/moxygen toolchain was present but failed or produced too few pages.
    Distinct from the toolchain being *absent* (which is a graceful {} skip): this is a
    real failure the caller should surface, so CI doesn't ship a degraded docs site."""

# The domains whose headers are offered to Doxygen. The output URI nests under the matching
# domain dir (moonmodules/<domain>/moxygen/), mirroring src/. Discovery walks these; no
# per-header list to maintain.
#
# `platform` is the interface every module reaches hardware through, so a reader following a
# call out of core hit a dead end while it generated no pages. Its rationale stays in
# explanation/architecture/mooncore.md; this is the reference half.
DOMAINS = ("core", "light", "platform")


def domain_of(header_rel: str) -> str | None:
    """The doc domain for a repo-relative header path, or None when it is under no domain
    directory and so gets no generated page."""
    parts = Path(header_rel).parts
    if len(parts) >= 2 and parts[0] == "src" and parts[1] in DOMAINS:
        return parts[1]
    return None


def _page_stem(header_rel: str) -> str:
    """The generated page's filename stem for a header.

    The bare filename wherever it is unique, which every page has always been. Two headers
    sharing a name (`desktop/platform_config.h` and `esp32/platform_config.h`) would otherwise
    write the same path, and the second silently replaced the first: the larger ESP32 config,
    453 lines of per-chip capability, generated no reachable page at all. A colliding header
    takes its parent directory as a prefix, so both survive and each keeps a stable name."""
    path = Path(header_rel)
    if _stem_counts().get(path.stem, 0) > 1:
        return f"{path.parent.name}_{path.stem}"
    return path.stem


@lru_cache(maxsize=1)
def _stem_counts() -> dict[str, int]:
    """How many discovered headers carry each filename stem."""
    counts: dict[str, int] = {}
    for h in _discover_headers():
        counts[Path(h).stem] = counts.get(Path(h).stem, 0) + 1
    return counts


def _discover_headers() -> list[str]:
    """Every `.h` under the domain directories, repo-relative, sorted. Every header
    gets a generated technical page — exhaustive, no gating. Curation (which modules
    appear in the end-user summary tables) is the summary pages' job, not the
    generator's: only MoonModule subclasses are tabled, but every header is reachable
    as a generated page. A sparsely-commented header just yields a thin page."""
    found: list[str] = []
    for d in DOMAINS:
        for h in sorted((ROOT / "src" / d).rglob("*.h")):
            found.append(str(h.relative_to(ROOT)))
    return found


def available() -> bool:
    """Both tools present? (doxygen binary + npx for moxygen)."""
    return shutil.which("doxygen") is not None and shutil.which("npx") is not None


def _doxyfile(headers: list[str], xml_out: str) -> str:
    # Quote each path so a ROOT (or any parent) containing spaces doesn't get split
    # into separate INPUT entries — Doxygen treats a quoted path as one argument.
    inputs = " ".join(f'"{ROOT / h}"' for h in headers)
    return (
        f'PROJECT_NAME="MoonLight API"\n'
        f"INPUT = {inputs}\n"
        # XML only — moxygen's input. Doxygen defaults GENERATE_HTML *and*
        # GENERATE_LATEX to YES; leaving LaTeX on drops a stray latex/ dir of
        # .tex/.sty files in the cwd every build. We want neither, just the XML.
        f"GENERATE_HTML = NO\nGENERATE_LATEX = NO\nGENERATE_XML = YES\nXML_OUTPUT = {xml_out}\n"
        # documented-only, no privates/statics, hide undoc → the compact public surface.
        "EXTRACT_ALL = NO\nEXTRACT_PRIVATE = NO\nEXTRACT_STATIC = NO\n"
        "HIDE_UNDOC_MEMBERS = YES\nHIDE_UNDOC_CLASSES = YES\n"
        "JAVADOC_AUTOBRIEF = YES\n"          # a leading `///`/`//` line is the brief
        f"STRIP_FROM_PATH = {ROOT}\n"        # relative "Defined in src/…", never an abs path
        "QUIET = YES\nWARN_IF_UNDOCUMENTED = NO\n"
        # A capability macro the compiler sets per target, which Doxygen never sees: left
        # undefined it preprocesses the guarded code away, and a header that is entirely
        # inside one documents nothing at all. Defined here so the page exists on every
        # build, the same way the widest target compiles it.
        "ENABLE_PREPROCESSING = YES\nMACRO_EXPANSION = YES\n"
        "PREDEFINED = MM_HEAVY_COMPUTE=1\n"
    )


# Where the generated pages are written under docs/ (gitignored). Writing them to
# disk — rather than injecting them as in-memory virtual pages — puts them through
# the standard MkDocs flow (MkDocs discovers real files) and lets a human open/preview
# the .md directly, the same as any other doc source.
DOCS_MOONMODULES = ROOT / "docs" / "moonmodules"

def _blob_base() -> str:
    """The GitHub blob URL every source link is built on, pinned to the branch being
    documented rather than a hard-coded `main`.

    `main` is the wrong constant on its own: the deployed site is built from main (so it
    resolves there), but a page generated on a feature branch links a file that main may
    not have yet — a 404 on every source link for any not-yet-merged module. Ask git for
    the current branch and fall back to main when there is no git (a release tarball) or
    the branch is detached."""
    branch = "main"
    try:
        out = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                             cwd=ROOT, capture_output=True, text=True, timeout=5)
        name = out.stdout.strip()
        if out.returncode == 0 and name and name != "HEAD":
            branch = name
    except (OSError, subprocess.SubprocessError):
        pass
    return f"https://github.com/MoonModules/projectMM/blob/{branch}"


_BLOB_BASE = _blob_base()


def _source_header(header_rel: str, domain: str, stem: str) -> str:
    """A one-line banner linking each generated page to its source `.h` on GitHub —
    `src/` isn't published to the site, so this is the only way to reach the header the
    page is generated from."""
    return f"> _Source:_ [`{Path(header_rel).name}`]({_BLOB_BASE}/{header_rel})\n\n"


# A moxygen inter-class link: `](cls_mm-<Class>.md#<anchor>)`, plus the namespace file
# `](cls_mm.md#<anchor>)` (namespace-level free functions). moxygen names these by its
# OWN per-file output names, which don't exist after we recombine into per-header pages
# — so every such link must be repointed (or dropped for the namespace file, which has
# no single per-header home).
_CLS_LINK_RE = re.compile(r'\]\(cls_(?P<key>mm(?:-[\w-]+)?)\.md(?P<frag>#[\w-]+)?\)')

# The same cross-reference, as emitted from a GROUP page. moxygen names a link by the output
# pattern of the file it is CURRENTLY rendering, so a class reference inside a group comes out
# `grp_undefined.md#classmm_1_1_rmt_led_driver` — the filename is a dead end, but the anchor is
# Doxygen's class refid and still says which class it meant. Recover the class from the anchor.
_GRP_CLS_LINK_RE = re.compile(r'\]\(grp_undefined\.md#class(?P<refid>[\w_]+)\)')

# Every OTHER `grp_*.md#anchor` moxygen emits: an enum it rendered into a group page
# (`grp_light_types.md#dim`), or a prose mention Doxygen auto-linked to a group it knows
# (`grp_undefined.md#tasksmodule`). Neither carries a class refid, so the rewrite above cannot
# resolve them, and the `grp_*.md` files are moxygen's own per-group scratch output that never
# ships. Keep the label, drop the target: the same trade _rewrite_cls_links makes for a class
# with no page, and what keeps `--strict` clean.
_GRP_DEAD_LINK_RE = re.compile(r'\]\(grp_[a-z0-9_]*\.md#[\w-]+\)')


def _rewrite_cls_links(md: str, from_domain: str, cls_to_page: dict) -> str:
    """Repoint moxygen's `cls_mm-<Class>.md#anchor` cross-links at the per-header page
    the class actually lands on. Same domain → a sibling `<stem>.md`; cross-domain →
    `../../<domain>/moxygen/<stem>.md`. A class with no generated page (in a class-less
    util header) → drop the link, leaving its label as plain text so nothing dangles.

    The `#anchor` fragment is DROPPED: moxygen numbered anchors per its own per-class
    file (`#onbuildstate-13`), so after recombining several classes into one page those
    numbers no longer match the rendered heading ids — keeping them would emit thousands
    of dead-anchor warnings. Linking to the page (no fragment) lands the reader on the
    right module; the intra-page jump is a fair trade for a clean build."""
    def _sub(m: re.Match) -> str:
        page = cls_to_page.get(m.group("key"))
        if page is None:
            return "]"          # unknown class → strip target, keep the `[label]` text
        domain, stem = page
        rel = f"{stem}.md" if domain == from_domain else f"../../{domain}/moxygen/{stem}.md"
        return f"]({rel})"
    return _CLS_LINK_RE.sub(_sub, md)


def _rewrite_grp_cls_links(md: str, from_domain: str, cls_to_page: dict,
                           refid_to_key: dict) -> str:
    """The group-page counterpart of _rewrite_cls_links: resolve `grp_undefined.md#class<refid>`
    to the class's real page via the refid, or drop the target if it has none."""
    def _sub(m: re.Match) -> str:
        key = refid_to_key.get(m.group("refid"))
        page = cls_to_page.get(key) if key else None
        if page is None:
            return "]"          # no page for that class → keep the label, drop the dead target
        domain, stem = page
        rel = f"{stem}.md" if domain == from_domain else f"../../{domain}/moxygen/{stem}.md"
        return f"]({rel})"
    return _GRP_CLS_LINK_RE.sub(_sub, md)


# moxygen in-page `](#anchor)` self-links that don't survive recombination:
#   - `#_..._8h_source`  — Doxygen's per-header source-file anchor (never rendered here)
#   - `#name-<n>`         — moxygen's numbered member anchor (`#onbuildstate-13`); the
#                          number is a per-original-file dedup counter that no longer
#                          matches the heading id once classes are combined onto one page.
# Both point at nothing on the recombined page, so drop the link, keeping the `[label]`
# text. A *bare* self-link (`#modifylive`, `#drivers`) with no numeric suffix and no
# `_8h_source` shape is left alone — those match a real rendered heading id.
_BAD_ANCHOR_RE = re.compile(r'\]\(#(?:_\w+_8h_source|[\w-]+-\d+)\)')


def _strip_bad_anchor_links(md: str) -> str:
    """Drop the always-dead moxygen self-link shapes (`#_..._8h_source`, numbered `#name-<n>`), keeping
    the `[label]` text. The general 'does this bare `#anchor` resolve to a heading?' check runs LATER, in
    `_strip_unresolved_anchor_links`, once `@moreinfo`/`@xref` have added their headings/links."""
    return _BAD_ANCHOR_RE.sub("]", md)


def _strip_unresolved_anchor_links(md: str) -> str:
    """Final pass: drop any same-page `[label](#anchor)` whose `#anchor` doesn't match a real heading id
    on the finished page, keeping the `[label]` text. Doxygen auto-links a bare method mention
    (`defineDriverControls()`) to a clean `#definedrivercontrols`, but the member's rendered id is
    numbered (or the member lives on a base-class page), so that anchor resolves to nothing → strip it. A
    bare anchor that DOES match a heading (a `@moreinfo` subsection, a manual `## Section`) is kept. Runs
    AFTER `@moreinfo`/`@xref` so their headings and links are already in place."""
    try:
        from markdown.extensions.toc import slugify   # MkDocs' own slugifier — always present in the build env
    except ImportError:
        return md   # no markdown (a bare `python3` outside the docs env) → skip the pass, don't crash the generator
    heading_ids = {slugify(re.sub(r'[`*]', '', h), '-')
                   for h in re.findall(r'^#{1,6}\s+(.+)$', md, re.MULTILINE)}
    return re.sub(r'\]\(#([\w-]+)\)',
                  lambda m: m.group(0) if m.group(1) in heading_ids else "]", md)


# A link moxygen auto-inserted INSIDE a code span: `[label](target)`. Markdown does not
# render links within `code`, so these emit as literal brackets-and-parens in a code chip
# (`[frameBytes](ParallelLedDriver.md)`) — noise at best, and actively wrong when the
# "link" is really notation: a half-open interval `[a, b)` in a `///` comment comes out
# mangled the same way. Either way the target is unreachable, so keep the label text and
# drop the target. Only touches text between backticks; prose links are untouched.
_CODE_SPAN_RE = re.compile(r'`[^`\n]*`')
_LINK_IN_SPAN_RE = re.compile(r'\[([^\]\n]+)\]\([^)\n]*\)')


def _unlink_inside_code_spans(md: str) -> str:
    """Reduce `[label](target)` to `label` wherever it sits inside a code span."""
    return _CODE_SPAN_RE.sub(lambda m: _LINK_IN_SPAN_RE.sub(r'\1', m.group(0)), md)


# A `@card <file>` directive in a class `///` comment — the module's UI-card screenshot.
# Doxygen with GENERATE_HTML=NO drops `\image`/`@htmlonly`/raw `<img>` from the XML, but
# preserves plain text, so `@card foo.png` survives Doxygen → moxygen as-is and we render
# it to an `<img>` here (post-process). The asset lives at docs/assets/<domain>[/<sub>]/<file>;
# from a moxygen page (moonmodules/<domain>/moxygen/) that resolves to ../../../assets/… .
# `@card <file>` can land mid-line: Doxygen flows consecutive `///` lines into one paragraph,
# so in a richly-commented class the directive trails the last text run (`… esp_event.h. @card x.png`)
# rather than sitting on its own line. Match it ANYWHERE, with optional surrounding whitespace, and
# render to an <img> on its own block (a leading newline lifts it out of the trailing paragraph).
_CARD_RE = re.compile(r'[ \t]*@card\s+(?P<file>\S+\.(?:png|jpe?g|gif))[ \t]*')
_ASSETS = ROOT / "docs" / "assets"


# A `@moreinfo` directive in a class `///` comment splits the class description: everything BEFORE it is
# the lead description (renders first, above the attribute/method lists — Doxygen's fixed order), and
# everything AFTER it is deep-dive reference that belongs at the BOTTOM of the page, under the members.
# Doxygen has no "trailing section" slot, so we relocate it here on the rendered markdown, the same
# post-process layer `@card` uses. Like `@card`, the plain-text marker survives Doxygen → moxygen as-is.
_MOREINFO_RE = re.compile(r'^[ \t]*@moreinfo[ \t]*$', re.MULTILINE)
# The first member-section heading moxygen emits. A CLASS page lists `### Public Attributes`,
# `### Public Methods` and friends; a GROUP page lists `### Functions`, `### Variables`,
# `### Enumerations` or `### Classes`. Both shapes are matched, because the detailed description
# ends where the first such heading begins and a group page must not have its members swallowed
# into a relocated tail.
_FIRST_SECTION_RE = re.compile(
    r'^### (?:(?:Public|Protected|Private|Static) |'
    r'(?:Functions|Variables|Enumerations|Classes|Structs|Typedefs|Defines)\s*$)',
    re.MULTILINE)


# An in-`///` cross-reference: `@xref{<anchor>|<label>}` (or `@xref{<anchor>}` — the anchor doubles as the
# label). Renders to a real Markdown link `[label](#anchor)` pointing at a heading on the SAME page (a
# More-info subsection). Doxygen strips relative `[text](../x.md)` links from its XML-only output, so an
# in-text page-local anchor can't be written directly in a `///`; this marker survives Doxygen as plain
# text and becomes the link here. NB: the marker is `@xref`, NOT `@ref` — `@ref` is a real Doxygen command
# (Doxygen consumes the keyword and mangles it); `@xref` is not a command, so it passes through verbatim
# like `@card`. `<anchor>` is the target heading's MkDocs slug (lowercased, spaces→`-`, punctuation
# dropped) — e.g. `@xref{the-ringdbg-instrument-expert-mode-read-only|the ringDbg legend}`.
# The `|` separator may arrive escaped as `\|` — moxygen escapes pipes when the marker lands inside a
# member's brief (a Markdown table cell in the attribute list). Accept either form.
_REF_RE = re.compile(r'@xref\{(?P<anchor>[a-z0-9-]+)(?:\\?\|(?P<label>[^}]+))?\}')


def _render_ref_directives(md: str) -> str:
    """Turn `@xref{anchor|label}` markers into `[label](#anchor)` page-local links (the label defaults to
    the anchor when omitted)."""
    def repl(m: re.Match) -> str:
        anchor = m.group("anchor")
        label = (m.group("label") or anchor).strip()
        return f"[{label}](#{anchor})"
    return _REF_RE.sub(repl, md)


def _relocate_moreinfo(md: str) -> str:
    """Move the class description's `@moreinfo` tail below the member sections, under a `## More info`
    heading. No marker → unchanged. The tail is cut from `@moreinfo` up to the first member-section
    heading (the end of the class description) and re-appended at the end of the page."""
    marker = _MOREINFO_RE.search(md)
    if not marker:
        return md
    # The class description runs until the first member section (or end of page if a class has no members).
    sect = _FIRST_SECTION_RE.search(md, marker.end())
    tail_end = sect.start() if sect else len(md)
    tail = md[marker.end():tail_end].strip()
    # Excise the marker + its tail from the description, then append it (relabeled) after everything else.
    body = (md[:marker.start()] + md[tail_end:]).rstrip()
    return f"{body}\n\n## More info\n\n{tail}\n"


def _render_card_directives(md: str, domain: str, stem: str) -> str:
    """Replace each `@card <file>` directive with an <img> pointing at the resolved asset.

    A moving preview names a `.gif`, and the module's own card screenshot sits beside it under the
    same stem: both are emitted, the animation first and the controls that produced it second, so a
    reader sees what it looks like and what it is made of. A file absent from disk drops silently,
    the same fail-soft as a missing generated page."""
    def repl(m: re.Match) -> str:
        fname = m.group("file")
        # Search under docs/assets/<domain>/ for the file (handles the light/{drivers,effects,…} subdir).
        hits = list((_ASSETS / domain).rglob(fname))
        if not hits:
            return ""   # asset absent → emit nothing rather than a broken link
        shots = [hits[0]]
        # The controls screenshot pairs with a moving preview, and only there: a card that already
        # IS the screenshot has no second image to add.
        if hits[0].suffix == ".gif":
            shot = hits[0].with_suffix(".png")
            if shot.exists():
                shots.append(shot)
        out = ""
        for i, sh in enumerate(shots):
            rel = os.path.relpath(sh, DOCS_MOONMODULES / domain / "moxygen")
            alt = f"{stem} card" if i == 0 else f"{stem} controls"
            out += f'<img src="{rel}" alt="{alt}" width="300">\n'
        return f"\n\n{out}"
    return _CARD_RE.sub(repl, md)


# A member signature line moxygen emits as its own paragraph: a backtick-delimited
# code span alone on a line — an attribute (`uint8_t protocol = 0`) or a method
# (`virtual inline void defineControls() override`). The template wraps each in
# backticks; nothing else on the site opens a line with a bare code span, so this
# anchors the match to member signatures only.
_SIG_LINE_RE = re.compile(r'^`(?P<sig>[^`\n]+)`[ \t]*$', re.MULTILINE)
# A METHOD name is the identifier immediately before the FIRST argument-list `(`
# (`… defineControls(…) override` → `defineControls`) — trailing `const`/`override`
# come after and must NOT win. An ATTRIBUTE name is the identifier before ` =` or at
# the end of the declarator (`uint8_t protocol = 0` → `protocol`; `char pins[24] = ""`
# → `pins`, skipping the `[N]` array bound). Two anchored patterns, method tried first.
_SIG_METHOD_NAME_RE = re.compile(r'(?P<name>[A-Za-z_]\w*)\s*\(')
_SIG_ATTR_NAME_RE = re.compile(r'(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|$)')


def _highlight_signature_names(md: str) -> str:
    """Wrap the declared member NAME in each generated signature so the theme can
    highlight it while the type/args stay muted (CSS: `.mm-sig-name`). moxygen emits
    a flat `<code>` string with no internal markup, so 'color only the name' can't
    be done in CSS alone — we split the code span here into
    `<code>…<span class="mm-sig-name">name</span>…</code>` (raw HTML the markdown
    passes through). The signature reads as one code chip; only the identifier pops."""
    def repl(m: re.Match) -> str:
        sig = m.group("sig")
        # Method (has an arg list) → the id before the FIRST `(`; else attribute →
        # the id before `=` / end. Falls through to the whole span if neither matches.
        h = _SIG_METHOD_NAME_RE.search(sig) if "(" in sig else _SIG_ATTR_NAME_RE.search(sig)
        if not h:
            return m.group(0)
        start, end = h.start("name"), h.end("name")
        wrapped = (_html_escape(sig[:start])
                   + f'<span class="mm-sig-name">{_html_escape(sig[start:end])}</span>'
                   + _html_escape(sig[end:]))
        return f'<code class="mm-sig">{wrapped}</code>'
    return _SIG_LINE_RE.sub(repl, md)


def _html_escape(s: str) -> str:
    # Signatures carry `<`, `>`, `&` (templates, refs) — escape so the raw-HTML
    # <code> we emit renders them as text, not markup.
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _class_to_header(xml_dir: Path) -> dict[str, str]:
    """Map each moxygen class-file key → its source header (repo-relative), read from
    the Doxygen XML `<location file=...>` of every class/struct compound. The key is
    moxygen's `--classes` filename stem: the fully-qualified name with `::` → `-`
    (e.g. `mm::ControlList` → `mm-ControlList`), matching moxygen's `%s` substitution."""
    # Every compound declared INSIDE a class or struct, by qualified name. Doxygen lists them
    # as <innerclass> children of the enclosing compound, which is the only reliable way to
    # tell `mm::Hub75Driver::BoardPins` (nested in a class) from `mm::json::JsonDoc` (in a
    # namespace): the qualified names are the same shape.
    class_nested: set[str] = set()
    for cx in list(xml_dir.glob("class*.xml")) + list(xml_dir.glob("struct*.xml")):
        try:
            cd = ET.parse(cx).getroot().find("compounddef")
        except ET.ParseError:
            continue
        if cd is None or cd.get("kind") not in ("class", "struct"):
            continue
        for inner in cd.findall("innerclass"):
            if inner.text:
                class_nested.add(inner.text.strip())

    mapping: dict[str, str] = {}
    for cx in list(xml_dir.glob("class*.xml")) + list(xml_dir.glob("struct*.xml")):
        try:
            root = ET.parse(cx).getroot()
        except ET.ParseError:
            continue
        cd = root.find("compounddef")
        if cd is None:
            continue
        name = cd.findtext("compoundname") or ""     # e.g. "mm::ControlList"
        loc = cd.find("location")
        if not name or loc is None:
            continue
        # An undocumented CLASS-NESTED compound never reaches a page. Doxygen emits a
        # struct*.xml for every compound it parses, and HIDE_UNDOC_CLASSES only keeps those
        # out of lists and inheritance graphs, so a private helper struct
        # (Hub75Driver::BoardPins, ParallelLedDriver::PeripheralEntry) rendered as a
        # top-level section above the class it belongs to, listing its fields as public API.
        #
        # Nested in a CLASS, read from the enclosing compound's <innerclass>, not guessed from
        # the `::` count: `mm::json::JsonDoc` has just as many and is a real page. A top-level
        # type documented with a file-level `//` block has no brief either, so filtering on the
        # brief alone dropped 22 real pages (AudioFrame, raymarch, crc...).
        if name in class_nested:
            brief = cd.find("briefdescription")
            if brief is None or not "".join(brief.itertext()).strip():
                continue
        header = loc.get("file")                      # e.g. "src/core/module/Control.h"
        if header:
            mapping[name.replace("::", "-")] = header
    return mapping


def _refid_to_class_key(xml_dir: Path) -> dict[str, str]:
    """Map Doxygen's class refid (`classmm_1_1_rmt_led_driver` minus the `class` prefix) →
    moxygen's class-file key (`mm-RmtLedDriver`). A GROUP page's class cross-references carry
    only the refid (see _GRP_CLS_LINK_RE), so this is what turns one back into a real page."""
    mapping: dict[str, str] = {}
    for cx in list(xml_dir.glob("class*.xml")) + list(xml_dir.glob("struct*.xml")):
        try:
            root = ET.parse(cx).getroot()
        except ET.ParseError:
            continue
        cd = root.find("compounddef")
        if cd is None:
            continue
        refid = cd.get("id") or ""
        name = cd.findtext("compoundname") or ""
        if refid and name:
            # the XML id already carries the `class`/`struct` prefix moxygen strips into the anchor
            mapping[re.sub(r'^(class|struct)', '', refid)] = name.replace("::", "-")
    return mapping


def _group_to_header(xml_dir: Path) -> dict[str, str]:
    """Map each moxygen GROUP-file key → its source header, the free-function counterpart
    of _class_to_header.

    A group compound has no `<location>` of its own (a group is a label, not an entity), so
    read it from the group's MEMBERS instead and take the header they agree on. A group that
    spans headers has no single home — skip it rather than guess, and it simply gets no page.
    The key is moxygen's `--groups` filename stem: the group id as written in `@defgroup`."""
    mapping: dict[str, str] = {}
    for gx in xml_dir.glob("group__*.xml"):
        try:
            root = ET.parse(gx).getroot()
        except ET.ParseError:
            continue
        cd = root.find("compounddef")
        if cd is None:
            continue
        name = cd.findtext("compoundname") or ""      # the @defgroup id
        files = {loc.get("file") for loc in cd.iterfind(".//memberdef/location")
                 if loc.get("file")}
        if name and len(files) == 1:
            mapping[name] = files.pop()
    return mapping


def generate() -> dict[str, str]:
    """Write a generated technical page for every documented module under
    src/{core,light} into docs/moonmodules/{domain}/moxygen/<Module>.md (gitignored),
    and return {doc_uri: markdown} for the pages written (empty if the toolchain is
    unavailable). doc_uri nests by domain, e.g. 'moonmodules/core/moxygen/Control.md'.

    ONE Doxygen pass over all headers + ONE moxygen `--classes` call — not per-header.
    Per-header (132×) meant 132 npx cold-starts (~0.95s each ≈ 150s); the single pass
    is ~5s. moxygen `--classes` emits one file per class, so a header's several classes
    (Control.h → Control, ControlList, ControlDescriptor) are recombined here into one
    per-header page via the class→header map from the XML `<location>`.

    Failure model: `available()` false → return {} (a contributor without the tools
    still builds the rest of the site — a *graceful* skip). But if the tools ARE present
    and then fail (npx registry fetch error, doxygen crash, empty output), raise
    GenApiError — silently returning {} there would ship a docs site with ZERO API pages
    and no red X. The caller (mkdocs_hooks) degrades gracefully on absent tools but lets
    the error propagate so CI, where the tools are provisioned, fails loudly."""
    if not available():
        return {}

    headers = [h for h in _discover_headers() if domain_of(h)]
    if not headers:
        return {}

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        xml_dir = tdp / "xml"
        (tdp / "Doxyfile").write_text(_doxyfile(headers, str(xml_dir)))
        r = subprocess.run(["doxygen", str(tdp / "Doxyfile")],
                           cwd=tdp, capture_output=True, text=True, check=False)
        if r.returncode != 0 or not xml_dir.exists():
            raise GenApiError(f"doxygen failed (rc={r.returncode}): {r.stderr[-500:]}")

        # One moxygen call, class-per-file (output name = fully-qualified class, ::→-).
        m = subprocess.run(
            ["npx", "--yes", "moxygen@2.1.10",
             "--templates", str(TEMPLATES), "--classes", "--noindex",
             "--output", str(tdp / "cls_%s.md"), str(xml_dir)],
            cwd=tdp, capture_output=True, text=True, check=False,
        )
        if m.returncode != 0:
            # npx couldn't fetch/run moxygen (registry outage, yanked version, no net).
            raise GenApiError(f"npx moxygen failed (rc={m.returncode}): {m.stderr[-500:]}")

        # A second moxygen call for GROUPS. A header of free functions (the WS2812 slot
        # encoder, the pin parsers) has no class for `--classes` to find, so it would get
        # no page at all — Doxygen files those members under the NAMESPACE compound, which
        # the class→header map never reads. `@defgroup`/`@ingroup` is Doxygen's own answer
        # for "these free functions are one unit", and moxygen renders a group per file the
        # same way it renders a class, so the recombine below treats both alike.
        g = subprocess.run(
            ["npx", "--yes", "moxygen@2.1.10",
             "--templates", str(TEMPLATES), "--groups", "--noindex",
             "--output", str(tdp / "grp_%s.md"), str(xml_dir)],
            cwd=tdp, capture_output=True, text=True, check=False,
        )
        if g.returncode != 0:
            raise GenApiError(f"npx moxygen --groups failed (rc={g.returncode}): {g.stderr[-500:]}")

        cls_to_header = _class_to_header(xml_dir)
        cls_to_header.update(_group_to_header(xml_dir))
        refid_to_key = _refid_to_class_key(xml_dir)

        # moxygen's `--classes` cross-references link to its OWN per-class filenames
        # (`cls_mm-<Class>.md#anchor`). We recombine classes into per-header pages, so
        # those targets don't exist — rewrite each to the header page the class lands
        # on. A class in a non-generated header (e.g. a struct in a class-less util)
        # maps to nothing → strip the link to plain text so it can't dangle.
        # cls-key ("mm-Layer") → (domain, header-stem) of the page it ends up in.
        cls_to_page = {
            key: (domain_of(h), _page_stem(h))
            for key, h in cls_to_header.items() if domain_of(h)
        }

        # Group the per-class markdown by owning header (in header order, so a page's
        # classes appear top-down as declared).
        # A `@defgroup` header emits BOTH per-class files and a group file, and the group file
        # already renders those same classes in full (attributes, methods, statics) under its own
        # lead. Appending both printed every class twice: platform.h came out 18 sections as 36,
        # and Hub75Slots.md had the same fault before any of this. So the group file, where one
        # exists, IS the page; the per-class files are used only for a header without a group.
        grp_headers: dict[str, str] = {}
        cls_blocks: dict[str, list[str]] = {}
        for prefix in ("cls_", "grp_"):
            for part_md in sorted(tdp.glob(f"{prefix}*.md")):
                key = part_md.name[len(prefix):-len(".md")]   # "mm-ControlList" / "parallelslots"
                header = cls_to_header.get(key)
                if header is None or domain_of(header) is None:
                    continue
                text = part_md.read_text(encoding="utf-8")
                if prefix == "grp_":
                    grp_headers[header] = text
                else:
                    cls_blocks.setdefault(header, []).append(text)

        # A class DECLARED OUTSIDE the group is absent from the group file, so appending its
        # per-class block adds it rather than duplicating it. Keyed on the rendered heading: a
        # class the group already covers is skipped, one it never saw is kept. Without this a
        # mixed header, free functions in a group plus a class beside it, published the functions
        # and silently dropped the class, and the card linking it arrived at a page without it.
        by_header: dict[str, list[str]] = {h: [md] for h, md in grp_headers.items()}
        for header, blocks in cls_blocks.items():
            if header not in by_header:
                by_header[header] = blocks
                continue
            grp = by_header[header][0]
            # The group file's OWN lead is its title, which for a `@defgroup JsonSink` is the class
            # name itself: matching against it would read every such class as already rendered.
            # So compare against the headings BELOW that lead, which are what the group really
            # contains, and keep a class the group never rendered.
            # The group's own `# Title` line is dropped before the comparison: for a
            # `@defgroup JsonSink` it carries the class's name and would read as the class
            # already being rendered. What follows it is what the group really contains.
            lines = grp.split("\n")
            for i, ln in enumerate(lines):
                if ln.startswith("# "):
                    lines = lines[i + 1:]
                    break
            grp_members = set(re.findall(r'^#+ (\S+)', "\n".join(lines), re.M))
            for b in blocks:
                m = re.search(r'^#+ (\S+)', b, re.M)
                if m and m.group(1) not in grp_members:
                    by_header[header].append(b)

        pages: dict[str, str] = {}
        for header, blocks in by_header.items():
            domain = domain_of(header)
            stem = _page_stem(header)
            body = _rewrite_cls_links("".join(blocks), domain, cls_to_page)
            body = _rewrite_grp_cls_links(body, domain, cls_to_page, refid_to_key)
            body = _GRP_DEAD_LINK_RE.sub("]", body)   # enum / prose group links: keep the label
            body = _strip_bad_anchor_links(body)
            # After the link rewrites (so it catches their output too): a link inside a
            # code span can't render — keep the label, drop the target.
            body = _unlink_inside_code_spans(body)
            body = _render_card_directives(body, domain, stem)
            body = _render_ref_directives(body)   # @xref{anchor|label} → page-local [label](#anchor)
            # After card rendering (so a @card inside the More-info tail moves with it): relocate the
            # class description's @moreinfo tail to a `## More info` section below the member lists.
            body = _relocate_moreinfo(body)
            # LAST link pass — now every heading (incl. @moreinfo subsections) and every @xref link is in
            # place, so this can honestly check which bare `#anchor`s resolve and drop the ones that don't.
            body = _strip_unresolved_anchor_links(body)
            body = _highlight_signature_names(body)
            md = _source_header(header, domain, stem) + body
            uri = f"moonmodules/{domain}/moxygen/{stem}.md"
            dst = DOCS_MOONMODULES / domain / "moxygen" / f"{stem}.md"
            dst.parent.mkdir(parents=True, exist_ok=True)
            # Write ONLY when the content changed. These files live under docs_dir,
            # which `mkdocs serve` watches — an unconditional write bumps the mtime
            # every build, which the watcher reads as a change and rebuilds, which
            # regenerates, which writes again: an endless rebuild loop that pins the
            # serve at ~7s/request. Skipping an identical write leaves mtime untouched,
            # so the watcher stays quiet.
            if not dst.exists() or dst.read_text(encoding="utf-8") != md:
                dst.write_text(md, encoding="utf-8")
            pages[uri] = md

        # Tools ran but produced far too few pages → something broke upstream (empty
        # XML, an unmatched class→header map). Fail loudly rather than ship a gutted set.
        if len(pages) < _MIN_EXPECTED_PAGES:
            raise GenApiError(
                f"only {len(pages)} API pages generated (expected ≥ {_MIN_EXPECTED_PAGES}) "
                f"— doxygen/moxygen ran but produced almost nothing")

        # Remove a page whose header is gone. The output directory is gitignored and never
        # cleaned, so a renamed header left its old page behind: reachable by URL, linked by
        # nothing, and reporting as an orphan that no source explains. AFTER the count guard,
        # so a broken toolchain cannot empty the tree.
        # Keyed by (DOMAIN, name), not by name alone: core/ and light/ each have their own
        # moxygen tree, so a page sharing a filename across the two would mask the other and
        # a delete could take the wrong one.
        keep = {(uri.split("/")[-3], uri.rsplit("/", 1)[-1]) for uri in pages}
        for domain in DOMAINS:
            for stale in (DOCS_MOONMODULES / domain / "moxygen").glob("*.md"):
                if (domain, stale.name) not in keep:
                    stale.unlink()
        return pages

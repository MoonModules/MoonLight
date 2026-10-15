#!/usr/bin/env python3
"""Check that implemented MoonModules have corresponding promoted specs.

Scans src/ for MoonModule .h files and verifies each has a matching
.md file in docs/moonmodules/. Reports missing or outdated specs.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
SPECS = ROOT / "docs" / "moonmodules"

# The module stems that get a source-generated technical page (every `.h` under
# src/{core,light} — gen_api discovers them). A summary/overview may link its
# `moxygen/<stem>.md` in place of a `## Source` section; this set validates that link
# points at a real generated page. Import by path so this check needs no PYTHONPATH tweak.
#
# `_page_stem` rather than the bare filename, because the generator prefixes a COLLIDING
# stem with its parent directory (`desktop/platform_config.h` becomes
# `desktop_platform_config`). Deriving the set here with `Path(h).stem` instead duplicated
# the naming rule and then disagreed with it: the page existed and was reachable, and this
# check called the link dead. One function owns how a page is named.
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
from gen_api import _discover_headers, _page_stem  # noqa: E402
_API_STEMS = {_page_stem(h) for h in _discover_headers()}

# Map source directories to spec directories
SOURCE_DIRS = {
    "core": "core",
    "light": "light",
}


# Subdirs under docs/moonmodules/ that are NOT hand-written active specs:
#   moxygen/ — generated technical pages (no `## Source`; the page IS the source view)
#   archive/ — the old per-module pages, kept for migration cross-check until Stage 5
#              deletes them; transitional, not maintained (their relative links are
#              stale after the archive move — no reason to fix links on doomed files).
_NON_SPEC_DIRS = {"moxygen", "archive"}


def spec_md_files():
    """Every hand-written, still-active spec `.md` under docs/moonmodules/, excluding
    the generated and archived subtrees (see _NON_SPEC_DIRS)."""
    return (md for md in SPECS.rglob("*.md")
            if _NON_SPEC_DIRS.isdisjoint(md.parts))


def find_moonmodules():
    """Find all .h files that define MoonModule subclasses."""
    modules = []
    for h_file in SRC.rglob("*.h"):
        # Skip platform, ui, and non-module headers
        rel = h_file.relative_to(SRC)
        if str(rel).startswith("platform/") or str(rel).startswith("ui/"):
            continue

        # Explicit utf-8: source files contain non-ASCII (→, µ, ×). Windows'
        # default read_text encoding is cp1252 and rejects those bytes.
        content = h_file.read_text(encoding="utf-8")
        # Check if file defines a class inheriting from MoonModule or its subclasses
        if re.search(r'class\s+\w+\s*:\s*public\s+\w*(MoonModule|EffectBase|DriverBase|ModifierBase|LayoutBase)', content):
            # Skip abstract base classes (pure virtual methods + no controls)
            has_pure_virtual = re.search(r'=\s*0\s*;', content)
            has_controls = re.search(r'controls_\.add', content)
            if has_pure_virtual and not has_controls:
                continue
            # Skip CRTP template bases (e.g. ParallelLedDriver<Derived>): a
            # `template<...> class X : public DriverBase` is shared infrastructure,
            # not a registered module — its controls belong to the concrete derived
            # classes, which carry the docs. The concrete subclass is `class Foo :
            # public X<Foo>` (no leading `template<`), so it is still picked up.
            if re.search(r'template\s*<[^>]*>\s*class\s+\w+\s*:\s*public\s+\w*'
                         r'(MoonModule|EffectBase|DriverBase|ModifierBase|LayoutBase)',
                         content):
                continue
            modules.append(h_file)
    return modules

# Effects, modifiers, layouts, and drivers each document themselves as one compact-row page per type
# rather than a file per module (docs consolidation — see the folder-structure decision). A module
# whose type name ends in one of these suffixes is documented on the matching shared page; everything
# else keeps a per-module page named for the type. The match is purely on the type-name **suffix**, so
# EVERY *Layout module (CarLightsLayout, CubeLayout, PanelLayout, RingLayout, …) routes to layouts.md,
# every *Driver to drivers.md, etc. — not just a hand-picked subset. New effect/modifier/layout/driver
# types fold in automatically. (Layouts/Effects/Drivers are CONTAINERS, not leaf modules — none of those
# stems ends in a suffix below ("Drivers" ≠ "Driver"), so each container keeps its own per-module page;
# the CRTP base ParallelLedDriver is skipped in discover_modules as a template.)
CONSOLIDATED_PAGES = {
    "Effect": SPECS / "light" / "effects" / "effects.md",
    "Modifier": SPECS / "light" / "modifiers" / "modifiers.md",
    "Layout": SPECS / "light" / "layouts" / "layouts.md",
    "Driver": SPECS / "light" / "drivers" / "drivers.md",
}


def find_spec(module_path):
    """Find the matching spec .md for a source .h.

    A module whose type name ends in Effect/Modifier/Layout is documented on the shared
    per-type page (its row must carry every control name — checked by check_spec_freshness).
    Everything else uses the per-module page (stem match), as before.
    """
    name = module_path.stem  # e.g. "NoiseEffect"

    # MoonLive is a live-script module under light/moonlive/, not a normal effect — it keeps its
    # own page despite the *Effect suffix. Only the per-type folders consolidate.
    in_moonlive = "moonlive" in module_path.parts
    if not in_moonlive:
        for suffix, page in CONSOLIDATED_PAGES.items():
            if name.endswith(suffix):
                if page.exists():
                    return page
                break  # consolidated page missing — fall through to the per-module stem search

    # Per-module page: match by filename stem anywhere under docs/moonmodules/.
    for md in spec_md_files():
        if md.stem == name:
            return md
    return None

def check_spec_freshness(source_path, spec_path):
    """Check if spec mentions key elements from the source."""
    issues = []
    source = source_path.read_text(encoding="utf-8")
    spec = spec_path.read_text(encoding="utf-8")
    # A card cut to its heading is drawn from help.json, so the page is read as the build draws it.
    if spec_path in CATALOG_PAGES:
        spec = fill_cards_from_help(spec, spec_path.relative_to(SPECS.parent).as_posix())

    # Extract control names from source
    controls = re.findall(r'controls_\.add\w+\("(\w+)"', source)
    for ctrl in controls:
        if ctrl not in spec:
            issues.append(f"control '{ctrl}' not in spec")

    # On a consolidated catalog page (effects/modifiers/layouts/drivers) the spec
    # documents MANY modules; scope the drift checks to THIS module's own block so a
    # control name shared across modules (fps, fadeRate, speed…) doesn't cross-match
    # another module's prose. The block is the one whose `source [<stem>.h]` link
    # points at this .h. On a per-module page the block is the whole file.
    block = _module_block(source_path, spec)
    issues += _check_range_drift(source, block)
    issues += _check_author_url_drift(source, block)
    return issues


def _module_block(source_path, spec):
    """The slice of `spec` documenting the module in `source_path`. On a consolidated
    page, the `### ` block whose `source [<stem>.h]` link matches; else the whole spec."""
    stem = source_path.name          # e.g. "BlurzEffect.h"
    base = source_path.stem          # e.g. "BlurzEffect"
    lines = spec.splitlines()
    # Which line ties this module to its block. Several link shapes across the catalog
    # pages: a direct `source [<stem>.h]`, a detail-page reference, or the card's own
    # `Detail: [technical](moxygen/<base>.md)`.
    def _match(ln):
        return (f"[{stem}]" in ln or f"[{base}.md]" in ln
                or f"moxygen/{base}.md" in ln
                or f'alt="{base} ' in ln or f'alt="A {base} ' in ln)
    link_i = next((i for i, ln in enumerate(lines) if _match(ln)), None)
    if link_i is None:
        return spec  # per-module page (or no matching link) — whole file
    # WHERE a block starts and ends is the build's rule, not a second copy of it.
    for _title, start, end in split_blocks(spec):
        if start <= link_i < end:
            return "\n".join(lines[start:end])
    return spec


# Range-bearing control forms: addControl("name", var, MIN, MAX).
# (addPin/addSelect/addText/addButton and a bool addControl carry no numeric range — a call with
# no MIN/MAX simply does not match, which is what skips them.)
_RANGE_CTRL_RE = re.compile(
    r'controls_\.addControl\("(?P<name>\w+)"\s*,\s*\w+\s*,\s*'
    r'(?P<lo>-?\d+)\s*,\s*(?P<hi>-?\d+)'
)
# A numeric range as spelled in .md prose: 1–8 / 1-8 / 1 to 8 (en-dash, hyphen, or "to").
_MD_RANGE_RE = re.compile(r'(-?\d+)\s*(?:[-–]|to)\s*(-?\d+)')


def _check_range_drift(source, spec):
    """Warn when the .md states a numeric range for a control that CONFLICTS with the
    range declared in the .h. Deliberately not "every control must restate its range"
    (many legitimately don't — a pin list, an obvious 0–255). We only flag a real
    mismatch: the .md line for a control gives a range, and it's a different one."""
    issues = []
    md_lines = spec.splitlines()
    for m in _RANGE_CTRL_RE.finditer(source):
        name, lo, hi = m.group("name"), int(m.group("lo")), int(m.group("hi"))
        # Find the .md line documenting this control (the `- `name` — …` prose line).
        for ln in md_lines:
            if f"`{name}`" not in ln:
                continue
            ranges = [(int(a), int(b)) for a, b in _MD_RANGE_RE.findall(ln)]
            if ranges and (lo, hi) not in ranges:
                issues.append(
                    f"control '{name}' range in .h is {lo}–{hi}, but the spec line "
                    f"states {', '.join(f'{a}–{b}' for a, b in ranges)} (drift)")
            break  # first line mentioning the control is its doc line
    return issues


# `// Author: … — <url>` (the em-dash separates the credit from the source URL).
_AUTHOR_URL_RE = re.compile(r'//\s*Author:.*?(https?://\S+)')


def _check_author_url_drift(source, spec):
    """Warn when a URL in the .h `// Author:` line is absent from the .md (whose
    `Origin:` line wraps it in a markdown link). Catches a source/credit URL that
    was updated in code but not in the doc. Matches on the bare URL substring."""
    issues = []
    for m in _AUTHOR_URL_RE.finditer(source):
        url = m.group(1).rstrip(".,);")   # trim trailing punctuation the comment may carry
        if url not in spec:
            issues.append(f"author URL {url} (from .h) not found in the spec")
    return issues

# Consolidated catalog pages document one module per `### ` block, and each block
# carries its own `source [Foo.h](...)` link (the docs site renders these blocks as
# a table, so a trailing `## Source` list would just duplicate every row's link —
# removed as redundant). These pages are checked by validating those per-block
# source links resolve, NOT by requiring a `## Source` section.
# From the build's own list rather than a copy. The copy this replaces named four pages at
# paths that stopped existing when docs/ was restructured, so the catalog branch below had
# been unreachable and every catalog page took the `## Source` path instead.
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
from mkdocs_hooks import _CATALOG_PAGES, catalog_markdown, fill_cards_from_help, split_blocks  # noqa: E402
CATALOG_PAGES = {SPECS.parent / rel for rel in _CATALOG_PAGES}


def _resolve_link(md, href):
    """Return an issue string if the relative link doesn't resolve inside the repo, else None."""
    target = href.split("#", 1)[0]  # drop any anchor
    candidate = (md.parent / target).resolve()
    if target.startswith("/") or not candidate.is_relative_to(ROOT):
        return f"source link escapes repo or is absolute: {href}"
    if not candidate.exists():
        return f"source link does not resolve: {href}"
    return None


def check_source_links():
    """Verify every spec page's source back-links resolve.

    Catches a source file renamed/moved out from under a spec's link. Two shapes:
    - Catalog pages (effects/modifiers/layouts/drivers): validate the per-block
      `source [Foo.h](...)` links — one per module, rendered into the table's Links
      column. No `## Source` section required (it would duplicate every row).
    - Every other spec: require a `## Source` section with ≥1 resolving relative
      link (the page's back-reference to the code it documents).

    Returns a list of (spec_rel, issue) tuples — empty when all is well.
    """
    issues = []
    for md in sorted(spec_md_files()):
        spec_rel = md.relative_to(ROOT)
        text = md.read_text(encoding="utf-8")

        if md in CATALOG_PAGES:
            # A catalog page back-references code either directly — each `### ` block
            # carries a `source [Foo.h](...)` link (effects/modifiers/layouts) — or
            # indirectly, linking each entry to its per-driver detail page which holds
            # the real source link (drivers.md, an index). Accept both: validate the
            # `source [..]` links if present, else the detail-page `.md` links; every
            # relative link must resolve. (No `## Source` section — it would duplicate
            # every row; the rows are rendered as a table on the site.)
            # \b before `source` so it matches the `source [Foo.h](…)` link marker,
            # not the tail of a word like "resource [".
            src_links = re.findall(r'\bsource \[[^\]]+\]\(([^)]+)\)', text)
            if not src_links:
                # Consolidated catalog pages now link each block to its generated
                # technical page: `Detail: [technical](../moxygen/<Stem>.md)`.
                src_links = re.findall(r'\]\(([^)]*moxygen/[^)]+\.md)\)', text)
            if not src_links:
                # index page: the per-entry links to detail pages (…Driver.md),
                # same-dir (no ../) — e.g. `](RmtLedDriver.md)`.
                src_links = re.findall(r'\]\(([^)]*Driver\.md[^)]*)\)', text)
            rel_links = [h for h in src_links if not h.startswith(("http://", "https://"))]
            if not rel_links:
                issues.append((spec_rel, "no per-block source or detail-page links"))
                continue
            for href in rel_links:
                issue = _resolve_link(md, href)
                if issue:
                    issues.append((spec_rel, issue))
            continue

        # A card-backed detail page (a driver detail page) opens with a back-link to
        # its catalog-card section — `[drivers.md § …](drivers.md#…)` — and the source
        # link now lives on that card, not repeated here. Validate the back-link
        # resolves (so the card is reachable); no `## Source` section required.
        back = re.search(r'\]\(([\w./-]*drivers\.md#[\w-]+)\)', text)
        if back:
            issue = _resolve_link(md, back.group(1))
            issues += ([(spec_rel, issue)] if issue else [])
            continue

        # A summary page whose technical reference is the source-generated page
        # (moonmodules/{core,light}/moxygen/<Module>.md, built by gen_api.py from the
        # `.h`'s /// comments) links to that page instead of a `## Source` GitHub blob
        # — the generated page *is* the source view. Validate EVERY moxygen link on the
        # page (a summary page tables many modules, so it has many such links) against
        # the discovered-header set; no `## Source` section required (it would
        # duplicate the generated reference).
        moxygen_links = re.findall(r'\]\([\w./-]*moxygen/([\w-]+)\.md(?:#[\w-]+)?\)', text)
        if moxygen_links:
            for stem in moxygen_links:
                if stem not in _API_STEMS:
                    issues.append((spec_rel, f"moxygen link to '{stem}' is not a generated module"))
            continue

        # Capture only the Source section body — stop at the next top-level
        # header or end-of-file, so links in a later section aren't mistaken
        # for source links if a page ever has one after Source.
        m = re.search(r'^## Source\s*$(.*?)(?=^##\s|\Z)', text, re.MULTILINE | re.DOTALL)
        if not m:
            issues.append((spec_rel, "no '## Source' section"))
            continue
        # Markdown links in the Source section: [label](relative/path).
        # Only check relative paths (the repo convention); skip any http(s) URL.
        links = re.findall(r'\]\(([^)]+)\)', m.group(1))
        rel_links = [href for href in links if not href.startswith(("http://", "https://"))]
        if not rel_links:
            issues.append((spec_rel, "'## Source' section has no relative source link"))
            continue
        for href in rel_links:
            issue = _resolve_link(md, href)
            if issue:
                issues.append((spec_rel, issue))
    return issues

# --- registerType docPath resolution (guards the in-UI help links) --------------
# main.cpp registers each module type with a `docPath` (relative to docs/moonmodules/)
# that the web UI turns into a per-card help link (see ModuleFactory.h). A docs rename
# that moves/deletes a page silently breaks those links — the UI 404s, and nothing else
# catches it. This check parses every registerType<T>("Name", "docPath") and asserts the
# docPath's file AND #anchor resolve, the same ground-truth the docs build would.
_REGISTER_RE = re.compile(
    r'registerType<[^>]+>\(\s*"[^"]+"\s*,\s*"(?P<doc>[^"]+)"\s*\)')
# A markdown heading's anchor (the standard python-markdown `toc` slugify: lowercase,
# strip non-word/space/hyphen, spaces→hyphens, collapse repeats) — matches how MkDocs
# generates `id=` from a `##`/`###` heading. Explicit `<a id="x">` anchors are exact.
_HEADING_RE = re.compile(r'^#{1,6}\s+(?P<text>.+?)\s*$', re.MULTILINE)
_ANCHOR_ID_RE = re.compile(r'<a\s+id="(?P<id>[^"]+)"')


def _slugify(text: str) -> str:
    s = text.strip().lower()
    s = re.sub(r'[^\w\s-]', '', s)      # drop punctuation/emoji (·, 💫, (), …)
    s = re.sub(r'[\s]+', '-', s)        # spaces → hyphens
    s = re.sub(r'-+', '-', s).strip('-')
    return s


def _page_anchors(text: str) -> set:
    """Every anchor a page's markdown exposes: explicit `<a id>` (catalog cards) + heading slugs."""
    anchors = set(_ANCHOR_ID_RE.findall(text))
    anchors.update(_slugify(m.group("text")) for m in _HEADING_RE.finditer(text))
    return anchors


def check_registertype_docpaths():
    """Each registerType docPath in the registry must resolve to a page, written or generated, and its anchor."""
    issues = []
    registry = SRC / "module_types.cpp"
    calls = list(_REGISTER_RE.finditer(registry.read_text(encoding="utf-8"))) if registry.exists() else []
    if not calls:   # a registry this check cannot read is a finding, never a clean run
        return [f"no registerType docPath found in {registry.name}"]
    for m in calls:
        doc = m.group("doc")
        rel, _, frag = doc.partition("#")
        text = catalog_markdown(f"moonmodules/{rel}")
        if text is None:
            issues.append(f'registerType docPath "{doc}": file not found: docs/moonmodules/{rel}')
            continue
        if frag and frag not in _page_anchors(text):
            issues.append(f'registerType docPath "{doc}": no anchor #{frag} in {rel}')
    return issues


def main():
    modules = find_moonmodules()
    missing = []
    outdated = []
    ok = []

    for mod in sorted(modules):
        rel = mod.relative_to(SRC)
        spec = find_spec(mod)
        if not spec:
            # docs-v2: a module with no active hand-written .md is still documented if
            # it has a source-generated technical page (its .h is in the moxygen
            # discovery set). The generated page IS its spec; not a miss.
            if mod.stem in _API_STEMS:
                ok.append(rel)
            else:
                missing.append(rel)
        else:
            issues = check_spec_freshness(mod, spec)
            if issues:
                outdated.append((rel, spec.relative_to(ROOT), issues))
            else:
                ok.append(rel)

    source_link_issues = check_source_links()
    docpath_issues = check_registertype_docpaths()

    # Report
    print(f"Spec check: {len(modules)} modules, {len(ok)} ok, {len(missing)} missing, "
          f"{len(outdated)} outdated, {len(source_link_issues)} source-link issues, "
          f"{len(docpath_issues)} docPath issues")

    if missing:
        print("\nMissing specs:")
        for m in missing:
            print(f"  {m} — no .md in docs/moonmodules/")

    if outdated:
        print("\nOutdated specs:")
        for src, spec, issues in outdated:
            print(f"  {src} → {spec}")
            for issue in issues:
                print(f"    {issue}")

    if source_link_issues:
        print("\nSource-link issues:")
        for spec, issue in source_link_issues:
            print(f"  {spec} — {issue}")

    if docpath_issues:
        print("\ndocPath issues (main.cpp registerType → broken UI help link):")
        for issue in docpath_issues:
            print(f"  {issue}")

    if missing or outdated or source_link_issues or docpath_issues:
        print()
        sys.exit(1)
    else:
        print("All specs up to date.")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# /// script
# dependencies = ["markdown"]
# ///
# `markdown` because the docs hooks it reads the catalog pages through slug an anchor with Python-Markdown's own function.
"""Check that every MoonModule is documented and every documentation link resolves.

A module is documented by its header, which gen_help reads into help.json, or as a catalog's base class, whose comment is that catalog's intro. The pages under docs/moonmodules/ must link only to sources that exist, and every registerType docPath must reach its page and anchor.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"
SPECS = ROOT / "docs" / "moonmodules"

sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

def spec_md_files():
    """Every written page under docs/moonmodules/."""
    return SPECS.rglob("*.md")


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

sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
from mkdocs_hooks import _help_file, catalog_markdown  # noqa: E402


def _resolve_link(md, href):
    """Return an issue string if the relative link doesn't resolve inside the repo, else None."""
    target = href.split("#", 1)[0]  # drop any anchor
    candidate = (md.parent / target).resolve()
    if target.startswith("/") or not candidate.is_relative_to(ROOT):
        return f"source link escapes repo or is absolute: {href}"
    # A catalog page drawn from help.json exists only in the build, under its own name.
    generated = candidate.is_relative_to(SPECS) and candidate.relative_to(SPECS).as_posix() in _help_file().get("catalogs", {})
    if not candidate.exists() and not generated:
        return f"source link does not resolve: {href}"
    return None


def check_source_links():
    """Verify every spec page's source back-links resolve.

    Catches a source file renamed or moved out from under a spec's link. A catalog drawn from help.json is skipped, its cards linking the headers gen_help read; a summary page's header links must resolve; any other spec needs a `## Source` section with a resolving link.

    Returns a list of (spec_rel, issue) tuples — empty when all is well.
    """
    issues = []
    for md in sorted(spec_md_files()):
        spec_rel = md.relative_to(ROOT)
        text = md.read_text(encoding="utf-8")

        # A catalog drawn from help.json links each card's header through the help's `source`, which gen_help writes from the header it read, so the page's own markdown holds only the intro.
        if md.relative_to(SPECS).as_posix() in _help_file().get("catalogs", {}):
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

        # A summary page links each header it lists, the technical reference read in place, so every such link must resolve; it needs no `## Source` section beside them.
        header_links = re.findall(r'\]\(([\w./-]*src/[\w./-]+\.h)\)', text)
        if header_links:
            issues += [(spec_rel, issue) for href in header_links if (issue := _resolve_link(md, href))]
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
    ok = []

    for mod in sorted(modules):
        rel = mod.relative_to(SRC)
        # A module the help reads is documented by its header, as is a catalog's base class, whose comment is that catalog's intro.
        if any(m.get("source", "").endswith("/" + mod.relative_to(ROOT).as_posix())
               for m in _help_file().get("modules", {}).values()) \
                or mod.stem in {c.get("base") for c in _help_file().get("catalogs", {}).values()}:
            ok.append(rel)
        else:
            missing.append(rel)

    source_link_issues = check_source_links()
    docpath_issues = check_registertype_docpaths()

    # Report
    print(f"Spec check: {len(modules)} modules, {len(ok)} ok, {len(missing)} missing, "
          f"{len(source_link_issues)} source-link issues, "
          f"{len(docpath_issues)} docPath issues")

    if missing:
        print("\nUndocumented modules:")
        for m in missing:
            print(f"  {m}: help.json does not read its header; register it, or give its class a `///` line")

    if source_link_issues:
        print("\nSource-link issues:")
        for spec, issue in source_link_issues:
            print(f"  {spec} — {issue}")

    if docpath_issues:
        print("\ndocPath issues (main.cpp registerType → broken UI help link):")
        for issue in docpath_issues:
            print(f"  {issue}")

    if missing or source_link_issues or docpath_issues:
        print()
        sys.exit(1)
    else:
        print("All specs up to date.")

if __name__ == "__main__":
    main()

"""The docs hooks build anchors by hand, a module's card from its name, and each must land on the heading id MkDocs renders. Because `validation.links.anchors` is `warn` (not `fail`), a slug that diverges from the real heading id produces a SILENT dead link, not a build failure, so this test pins `_slug()` to the anchor Python-Markdown generates.

`_slug()` delegates to `markdown.extensions.toc.slugify` (the exact function MkDocs' toc uses), so this both guards that delegation and documents the contract. If `_slug` is ever re-hand-rolled, the unicode and consecutive-separator cases below catch the drift.
"""

from pathlib import Path

import pytest

# `markdown` ships with MkDocs and the base test env lacks it, so the file skips where it is absent; CI installs it (--with markdown), so it runs there.
markdown = pytest.importorskip("markdown")
from markdown.extensions.toc import slugify  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent

from mkdocs_hooks import _slug  # noqa: E402


def test_slug_matches_markdown_toc_slugify():
    """_slug is exactly Python-Markdown's toc slugify, including the edge cases a hand-rolled mimic gets wrong: accents, decoration, repeated separators."""
    for text in [
        "LED output — details",
        "Fire2012 — details",
        "GEQ · 3D — details",
        "a  b   c",
        "Émoji 💫 test",
        "Multiple---hyphens",
    ]:
        assert _slug(text) == slugify(text, "-"), text


def test_slug_resolves_to_the_real_rendered_heading_id():
    """A `## <heading>` rendered by the toc extension MkDocs uses gets an `id=` equal to _slug(heading), so the hand-built anchor resolves."""
    for heading in ["LED output — details", "GEQ · 3D — details"]:
        html = markdown.markdown(f"## {heading}", extensions=["toc"])
        assert f'id="{_slug(heading)}"' in html, heading


def test_the_generators_heading_id_is_the_one_mkdocs_renders():
    """gen_help.heading_id builds the anchors of a module's tests and a MoonWalk step's section without Python-Markdown, so it must agree with it on the headings those name."""
    from gen_help import heading_id
    from _test_metadata import collect_unit_files
    for text in [f["module"] for f in collect_unit_files() if f.get("module")] + ["The Homebridge accessory", "Which family?", "LED output: details"]:
        assert heading_id(text) == _slug(text), text


# --- Catalog-page card images resolve on disk ---
# The catalog pages hand-author each card's preview as a raw `<img src="../../assets/…">`.
# MkDocs' --strict validates markdown `![]()` links but NOT raw-HTML `<img src>`, and the
# catalog hook moves the tag into a table cell without touching its src — so a wrong `../`
# depth ships a SILENT 404 on the live site (the exact bug this test pins: the pages need
# `../../assets`, one level shallower than a page nested one folder deeper). This test resolves every catalog `<img src>` against its
# page's own directory and fails on any that doesn't exist on disk.
import re  # noqa: E402

_DOCS = ROOT / "docs"
_IMG_SRC_RE = re.compile(r'<img[^>]+src="([^"]+)"')


def _catalog_pages():
    return sorted(
        (_DOCS / "moonmodules" / domain / f"{group}.md")
        for domain, groups in {
            "core": ("services", "supporting", "ui"),
            "light": ("effects", "modifiers", "layouts", "drivers", "supporting"),
        }.items()
        for group in groups
        if (_DOCS / "moonmodules" / domain / f"{group}.md").exists()
    )


def test_catalog_card_images_resolve_on_disk():
    missing = []
    for page in _catalog_pages():
        for src in _IMG_SRC_RE.findall(page.read_text(encoding="utf-8")):
            if src.startswith(("http://", "https://", "data:", "/")):
                continue
            target = (page.parent / src.split("#")[0]).resolve()
            if not target.exists():
                missing.append(f"{page.relative_to(ROOT)} -> {src}")
    assert not missing, "catalog card image(s) 404 on the site:\n  " + "\n  ".join(missing)

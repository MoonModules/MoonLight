"""A docs link that leaves docs/ for the repo goes to GitHub, which names a folder and a file differently: a folder under /tree/, a file under /blob/. A folder sent to /blob/ works only through GitHub's redirect."""

import pytest

pytest.importorskip("markdown")   # the hooks import it; CI installs it, a bare env skips

from mkdocs_hooks import _rebase_repo_root_doc_links, _rewrite_out_of_docs_links  # noqa: E402


def test_a_folder_link_goes_to_its_tree_and_a_file_link_to_its_blob():
    out = _rewrite_out_of_docs_links("[a](../moonwalk/) [b](../moonwalk/moonwalk.schema.json)", "moonwalk.md")
    assert "/tree/" in out.split(") [b]")[0] and out.split(") [b]")[0].endswith("/moonwalk/")
    assert "/blob/" in out.split(") [b]")[1] and out.endswith("/moonwalk/moonwalk.schema.json)")


def test_an_embedded_root_file_links_from_the_site_root_whatever_folder_embeds_it():
    html = '<a href="docs/moonwalk.md#commit">c</a> <a href="docs/explanation/architecture/index.html">a</a>'
    out = _rebase_repo_root_doc_links(html, "contributing/principles-and-process.md")
    assert 'href="../moonwalk.html#commit"' in out and 'href="../explanation/architecture/index.html"' in out
    assert 'href="moonwalk.html#commit"' in _rebase_repo_root_doc_links(html, "index.md")   # a page at the root climbs nothing


def test_an_embedded_site_link_becomes_the_climb_to_the_site_root():
    html = '<a href="https://moonmodules.org/MoonLight/getting-started.html#three-ways-in">g</a> <a href="https://moonmodules.org/MoonLight/">s</a>'
    out = _rebase_repo_root_doc_links(html, "index.md")
    assert 'href="getting-started.html#three-ways-in"' in out and 'href="index.html"' in out
    assert 'href="../getting-started.html#three-ways-in"' in _rebase_repo_root_doc_links(html, "contributing/principles-and-process.md")


def test_every_site_link_in_a_readme_names_a_page_and_an_anchor_the_site_has():
    """A README links the website by address, so a reader on GitHub lands on the site; the build cannot see those links, so this reads each one back."""
    import re
    from pathlib import Path
    from mkdocs_hooks import _slug, generated_catalog
    root = Path(__file__).resolve().parents[2]
    processes = {m.group(1) for f in (root / "moonwalk").glob("*.yaml")
                 for m in re.finditer(r"^  - id: ([\w-]+)", f.read_text(encoding="utf-8"), re.M)}
    readmes = ["README.md", "moonlive/README.md", "mooncloud/README.md", "mooninstaller/README.md"]
    links = [link for f in readmes for link in re.findall(r"\]\(https://moonmodules\.org/MoonLight/([^)#]*)(?:#([^)]*))?\)", (root / f).read_text(encoding="utf-8"))]
    assert links
    missing = []
    for path, anchor in links:
        if path.startswith("install/"):
            continue   # the web installer, an app of its own beside the site
        uri = (path[:-5] if path.endswith(".html") else (path or "index")) + ".md"
        page = root / "docs" / uri
        text = generated_catalog(uri) or (page.read_text(encoding="utf-8") if page.exists() else None)
        if text is None:
            missing.append(path)
            continue
        if not anchor:
            continue
        if uri == "moonwalk.md":
            ok = anchor in processes
        else:   # a heading's id, or an id a catalog writes for each module
            ok = anchor in {_slug(h) for h in re.findall(r"^#+ (.+)$", text, re.M)} | set(re.findall(r'id="([^"]+)"', text))
        if not ok:
            missing.append(f"{path}#{anchor}")
    assert missing == []

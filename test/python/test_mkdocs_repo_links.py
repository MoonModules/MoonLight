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

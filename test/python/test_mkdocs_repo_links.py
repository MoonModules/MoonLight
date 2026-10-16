"""A docs link that leaves docs/ for the repo goes to GitHub, which names a folder and a file differently: a folder under /tree/, a file under /blob/. A folder sent to /blob/ works only through GitHub's redirect."""

import sys
from pathlib import Path

import pytest

pytest.importorskip("markdown")   # the hooks import it; CI installs it, a bare env skips

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

from mkdocs_hooks import _rewrite_out_of_docs_links  # noqa: E402


def test_a_folder_link_goes_to_its_tree_and_a_file_link_to_its_blob():
    out = _rewrite_out_of_docs_links("[a](../moonwalk/) [b](../moonwalk/moonwalk.schema.json)", "moonwalk.md")
    assert "/tree/" in out.split(") [b]")[0] and out.split(") [b]")[0].endswith("/moonwalk/")
    assert "/blob/" in out.split(") [b]")[1] and out.endswith("/moonwalk/moonwalk.schema.json)")

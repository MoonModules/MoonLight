# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""MoonDeck's build pages: the tempo View serves the harness's latest page, build/tempo/view.md, and the history report its own."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import moondeck  # noqa: E402


def serve(root, route="/api/tempo-view"):
    handler = moondeck.MoonDeckHandler.__new__(moondeck.MoonDeckHandler)
    handler.path, out = route, []
    handler.send_error = lambda code, msg="": out.append(("error", code))
    handler._serve_markdown_as_html = lambda md_path, anchor: out.append(("page", md_path.relative_to(root).as_posix()))
    original = moondeck.ROOT
    moondeck.ROOT = root
    try:
        handler._serve_build_page()
    finally:
        moondeck.ROOT = original
    return out


def test_the_view_serves_the_latest_tempo_page_or_says_it_is_missing(tmp_path):
    assert serve(tmp_path) == [("error", 404)]
    (tmp_path / "build" / "tempo").mkdir(parents=True)
    (tmp_path / "build" / "tempo" / "view.md").write_text("# latest")
    assert serve(tmp_path) == [("page", "build/tempo/view.md")]


def test_the_history_report_is_served_by_the_same_rule_from_its_own_file(tmp_path):
    assert serve(tmp_path, "/api/history-report") == [("error", 404)]
    (tmp_path / "build").mkdir()
    (tmp_path / "build" / "history.md").write_text("# history")
    assert serve(tmp_path, "/api/history-report") == [("page", "build/history.md")]

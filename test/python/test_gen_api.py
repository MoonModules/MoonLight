"""gen_api renders the block tags gen_help reads, so a technical page shows them rather than their raw markers."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import gen_api  # noqa: E402


def test_a_video_tag_plays_its_clip_and_a_missing_clip_leaves_nothing():
    md = gen_api._render_video_directives("Lead.\n@video 05-layouts.webm Layouts at work.\n", "light")
    assert '<video src="../../../assets/moontube/05-layouts.webm" controls playsinline width="720" title="Layouts at work."></video>' in md
    assert "@video" not in gen_api._render_video_directives("Lead.\n@video nosuch.webm Gone.\n", "light")


def test_a_guide_marker_goes_and_its_text_reads_in_place():
    """A class `@guide` line and a member's `@guide` line both lose only the marker on the technical page."""
    assert gen_api._GUIDE_RE.sub("", "Lead.\n@guide\n## Tips\n") == "Lead.\n## Tips\n"
    assert gen_api._GUIDE_RE.sub("", "Master power.\n@guide Off keeps the level.") == "Master power.\nOff keeps the level."


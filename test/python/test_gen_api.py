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

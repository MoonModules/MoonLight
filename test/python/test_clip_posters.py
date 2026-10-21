"""A clip and the deck show a picture: every tracked clip has its poster beside it, the docs build gives each `<video>` that poster, and the effects deck README opens on is recorded to its crop."""

import sys
from pathlib import Path

import pytest

pytest.importorskip("markdown")   # the hooks import it; CI installs it, a bare env skips

ROOT = Path(__file__).resolve().parent.parent.parent

from mkdocs_hooks import _with_posters  # noqa: E402


def test_every_tracked_clip_has_its_poster_beside_it():
    clips = sorted((ROOT / "docs" / "assets" / "moontube").glob("*.webm"))
    assert clips
    assert [c.name for c in clips if not c.with_suffix(".jpg").exists()] == [], "a clip copied in without its poster: run mtvideo.poster on it"


def test_a_clip_takes_the_poster_beside_its_source_and_one_that_names_a_poster_keeps_it():
    assert _with_posters('<video src="../assets/moontube/05-layouts.webm" controls>') == '<video src="../assets/moontube/05-layouts.webm" poster="../assets/moontube/05-layouts.jpg" controls>'
    named = '<video poster="x.jpg" src="a.webm">'
    assert _with_posters(named) == named
    assert _with_posters('<video src="a.mp4">') == '<video src="a.mp4">', "only a MoonTube clip, a .webm, has a poster beside it"


def test_the_deck_gif_is_cropped_to_the_deck_in_whole_pixels_and_scaled_to_readme_width():
    sys.path.insert(0, str(ROOT / "moondeck" / "moontube"))
    import mtdeck
    vf = mtdeck.gif_filter({"x": 146.6, "y": 71.7, "w": 1307.8, "h": 1001.3}, 900, 10)
    assert vf.startswith("crop=1307:1001:146:71,fps=10,scale=900:-1"), "ffmpeg's crop takes whole pixels; the browser's box is fractional"
    assert (ROOT / "docs" / "assets" / "light" / "effects-deck.gif").exists(), "README opens on it"

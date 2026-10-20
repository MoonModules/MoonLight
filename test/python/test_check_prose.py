# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The prose rules under .vale/styles/MoonLight: each fires on its words, and a named link is not flagged."""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.skipif(shutil.which("vale") is None, reason="vale is not installed")
def test_the_link_text_tone_and_vague_closer_rules_fire_on_their_words_and_not_on_a_named_link(tmp_path):
    page = tmp_path / "page.md"
    page.write_text("# T\n\nSee [here](a.md). Please read it, and note that it lists x, etc.\n\nSee [the REST API](api.md) for the routes.\n", encoding="utf-8")
    out = subprocess.run(["vale", "--config", str(ROOT / ".vale.ini"), "--output=line", str(page)], capture_output=True, text=True, cwd=ROOT).stdout
    assert "MoonLight.LinkText" in out and "MoonLight.TonePadding" in out and "MoonLight.VagueClose" in out
    assert out.count("MoonLight.LinkText") == 1   # the named link is not flagged

# /// script
# requires-python = ">=3.11"
# dependencies = ["pytest"]
# ///
"""The prose rules under .vale/styles/MoonLight, each firing on its words and only there, and the ratchet on the whole tree's count."""

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.skipif(shutil.which("vale") is None, reason="vale is not installed")
def test_the_link_text_tone_and_vague_closer_rules_fire_on_their_words_and_not_on_a_named_link(tmp_path):
    page = tmp_path / "page.md"
    vague = "[" + "here](a.md)"   # built, so this file does not read as the vague link it tests
    page.write_text(f"# T\n\nSee {vague}. Please read it, and note that it lists x, etc.\n\nIt holds apples, pears and more\n\nSee [the REST API](api.md) for the routes.\n", encoding="utf-8")
    out = subprocess.run(["vale", "--config", str(ROOT / ".vale.ini"), "--output=line", str(page)], capture_output=True, text=True, cwd=ROOT).stdout
    assert "MoonLight.LinkText" in out and "MoonLight.TonePadding" in out and "MoonLight.VagueClose" in out
    assert out.count("MoonLight.LinkText") == 1   # the named link is not flagged
    assert out.count("MoonLight.VagueClose") == 2, "a closer at the end of its line is one too"


@pytest.mark.skipif(shutil.which("vale") is None, reason="vale is not installed")
def test_a_vague_closer_is_a_list_ending_and_not_a_path_or_a_comparison(tmp_path):
    page = tmp_path / "page.md"
    page.write_text("# T\n\nEdit the file under /etc/docker for it.\n\nThe new one is simpler and more robust than the old.\n", encoding="utf-8")
    out = subprocess.run(["vale", "--config", str(ROOT / ".vale.ini"), "--output=line", str(page)], capture_output=True, text=True, cwd=ROOT).stdout
    assert "MoonLight.VagueClose" not in out, out


def test_the_whole_tree_count_ratchets_per_rule_and_in_total_against_the_committed_counts(monkeypatch):
    import sys
    sys.path.insert(0, str(ROOT / "moondeck" / "check"))
    import _ratchet
    import check_prose
    monkeypatch.setattr(_ratchet, "committed_counts", lambda report: {"MoonLight.EmDash": 3, "MoonLight.Weasel": 2, "(total)": 5})
    assert check_prose.ratchet({"MoonLight.EmDash": 3, "MoonLight.Weasel": 1}, 4) == []
    assert check_prose.ratchet({"MoonLight.EmDash": 1, "MoonLight.Weasel": 3}, 4) == [("MoonLight.Weasel", 2, 3)], "one rule paying for another is caught"
    monkeypatch.setattr(_ratchet, "committed_counts", lambda report: None)
    assert check_prose.ratchet({"MoonLight.EmDash": 9}, 9) == [], "no baseline yet: the first run records it"


def test_a_section_past_its_word_cap_is_a_finding_and_its_tables_and_code_are_not_counted():
    import sys
    sys.path.insert(0, str(ROOT / "moondeck" / "check"))
    import check_prose as c
    words = " ".join(["word"] * (c.MAX_SECTION_WORDS + 1))
    page = f"# Page\n\nIntro.\n\n## Long\n\n{words}\n\n## Short\n\nDone.\n\n## Data\n\n| a | b |\n|---|---|\n" + "| x y z | w |\n" * 400 + "```\n" + "code " * 900 + "\n```\n"
    found = c.section_findings(page)
    assert [(rule, n) for rule, _, n in found] == [(c.SECTION_RULE, 5)]   # only the prose section, at its heading; a short one is no finding
    assert c.section_findings("# T\n\n## A\n\n" + " ".join(["w"] * c.MAX_SECTION_WORDS) + "\n") == []   # at the cap is within it



def test_against_the_page_at_head_only_a_section_the_change_lengthened_is_a_finding():
    import sys
    sys.path.insert(0, str(ROOT / "moondeck" / "check"))
    import check_prose as c
    page = lambda n: "# T\n\n## A\n\n" + " ".join(["w"] * n) + "\n"
    long = c.MAX_SECTION_WORDS + 100
    assert c.section_findings(page(long), page(long)) == [], "a one-word edit leaves a long section as long: no finding"
    assert len(c.section_findings(page(long + 1), page(long))) == 1, "a long section grown by a word is"
    assert len(c.section_findings(page(long), page(10))) == 1, "a section the change took past the cap is"
    assert c.section_findings(page(long - 50), page(long)) == [], "a long section cut but still long is not"
    two = lambda a, b: "# T\n\n## A\n\n### Setup\n\n" + " ".join(["w"] * a) + "\n\n## B\n\n### Setup\n\n" + " ".join(["w"] * b) + "\n"
    assert [n for _, _, n in c.section_findings(two(long + 1, long + 200), two(long, long + 200))] == [5], "two sections of one title count apart: the first grew"
    assert c.section_findings(two(long + 200, long), two(long, long + 200)) != [], "their counts swapped is a growth, not a match"
    assert c.section_findings(two(long, long + 200), two(long, long + 200)) == [], "both unchanged is none"
    only_b = lambda b: "# T\n\n## B\n\n### Setup\n\n" + " ".join(["w"] * b) + "\n"
    assert len(c.section_findings(only_b(long + 1), two(long + 200, long))) == 1, "A deleted, B grown by a word: B compares with its own count"

def test_the_section_count_reads_a_page_whose_name_holds_spaces_and_skips_one_deleted_but_tracked(tmp_path, monkeypatch):
    import sys
    sys.path.insert(0, str(ROOT / "moondeck" / "check"))
    import check_prose as c
    run = lambda *a: subprocess.run(["git", *a], cwd=tmp_path, check=True, capture_output=True)
    run("init", "-q")
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "a page.md").write_text("# T\n\n## Long\n\n" + " ".join(["w"] * (c.MAX_SECTION_WORDS + 1)) + "\n")
    (tmp_path / "docs" / "gone.md").write_text("# Gone\n")
    run("add", ".")
    (tmp_path / "docs" / "gone.md").unlink()   # deleted in the working tree, still in the index
    monkeypatch.setattr(c, "ROOT", tmp_path)
    assert [(f, rule) for f, rule, _, _ in c.section_rows()] == [("docs/a page.md", c.SECTION_RULE)]

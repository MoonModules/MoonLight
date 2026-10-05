"""A commit's flash change splits into new, removed, grown and shrunk symbols, and the parts add up to the net.

The split is what tells a feature from a cleanup when one commit does both, so it is pinned here: what counts as
in the image, how a rename reads, that the remainder makes the sum exact, and that every run inside one commit
compares against the same baseline.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import flash_split  # noqa: E402

NM = """\
400d0000 00000100 T mm::Layer::tick()
400d0100 00000020 t helper
400d0200 00000010 t helper
3ffb0000 00000400 B bigBuffer
3f400000 00000040 R kTable
400d0300 T noSize
"""


def test_nm_counts_what_the_image_carries_and_keeps_names_whole():
    sizes = flash_split.parse_nm(NM)
    assert sizes["mm::Layer::tick()"] == 0x100
    assert sizes["helper"] == 0x30          # two file-local statics of one name add up
    assert sizes["kTable"] == 0x40
    assert "bigBuffer" not in sizes         # bss is zeroed at boot, not stored in flash
    assert "noSize" not in sizes


def test_the_parts_sum_to_the_net_and_a_rename_reads_as_removed_plus_new():
    base = {"kept": 100, "grows": 50, "shrinks": 80, "oldName": 40, "dropped": 30}
    now = {"kept": 100, "grows": 70, "shrinks": 60, "newName": 40, "feature": 500}
    s = flash_split.split(base, now, net=520)
    assert (s["new"], s["removed"], s["grown"], s["shrunk"]) == (540, -70, 20, -20)
    assert s["new"] + s["removed"] + s["grown"] + s["shrunk"] + s["other"] == s["net"]
    assert s["other"] == 50                 # merged strings and alignment, which no symbol owns
    assert s["top"][0] == ["feature", 500]  # the largest move first, so a misreading shows


def test_every_run_inside_one_commit_compares_against_the_last_commits_measurement(tmp_path, monkeypatch):
    tables = iter([{"a": 10}, {"a": 10, "b": 5}, {"a": 10, "b": 5}, {"a": 12, "b": 5}])
    monkeypatch.setattr(flash_split, "symbols", lambda elf, env: next(tables))
    elf = tmp_path / "MoonLight.elf"
    assert flash_split.measure(tmp_path, elf, 1000, "aaa", {}) is None   # first ever: nothing to compare
    assert flash_split.measure(tmp_path, elf, 1005, "aaa", {}) is None   # same commit: still no baseline
    first = flash_split.measure(tmp_path, elf, 1005, "bbb", {})          # committed as bbb: the last run before it is the baseline
    again = flash_split.measure(tmp_path, elf, 1007, "bbb", {})          # a second run at bbb keeps that baseline
    assert (first["since"], first["net"], first["new"]) == ("bbb", 0, 0)   # named after the commit it measured
    assert (again["since"], again["net"], again["grown"], again["other"]) == ("bbb", 2, 2, 0)
    assert json.loads((tmp_path / flash_split.SNAPSHOT).read_text())["head"] == "bbb"


def test_the_commit_line_names_each_firmware_in_kilobytes():
    s = flash_split.split({"x": 1024}, {"y": 3072}, net=2048)
    assert flash_split.line({"esp32": s}) == "Flash split: esp32 +2.0 KB = +3.0 new, -1.0 removed, +0.0 grown, +0.0 shrunk, +0.0 other"
    assert flash_split.line({}).startswith("Flash split: unmeasured")

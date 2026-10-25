"""The size report reads a firmware's symbols the way nm prints them, sums a name defined in several files, and names what changed between two runs."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "report"))

import size_symbols as size  # noqa: E402

NM = """\
400d1000 00000120 T mm::Scheduler::tick()
3ffb0000 00000040 d leafCache
400d2000 00000010 t helper()
400d3000 00000018 t helper()
3f400000 00000200 R mm::ui::file1
         U memcpy
400d4000 00000000 T zeroSized
"""


def test_each_symbol_is_read_with_its_kind_and_size():
    s = size.parse_nm(NM)
    assert s["mm::Scheduler::tick()"] == {"kind": "code", "size": 0x120}
    assert s["leafCache"] == {"kind": "data", "size": 0x40}
    assert s["mm::ui::file1"]["kind"] == "const"


def test_small_data_counts_as_data_and_bss_and_a_weak_object_apart():
    s = size.parse_nm("3fc90000 00000008 G gSmall\n3fc90010 00000004 g sLocal\n"
                      "3fc90020 0000000c S zSmall\n3fc90030 00000002 s zLocal\n3fc90040 00000010 V weakObj\n")
    assert [s[n]["kind"] for n in ("gSmall", "sLocal", "zSmall", "zLocal", "weakObj")] == ["data", "data", "bss", "bss", "weak"]
    assert size.totals(s) == {"data": 12, "bss": 14, "weak": 16}


def test_a_name_defined_in_two_files_counts_both_copies():
    assert size.parse_nm(NM)["helper()"]["size"] == 0x10 + 0x18


def test_undefined_and_empty_symbols_are_left_out():
    s = size.parse_nm(NM)
    assert "memcpy" not in s and "zeroSized" not in s


def test_the_change_between_two_runs_names_what_grew_appeared_and_went():
    before = {"a": {"kind": "code", "size": 100}, "gone": {"kind": "code", "size": 40}, "same": {"kind": "data", "size": 8}}
    after = {"a": {"kind": "code", "size": 160}, "new": {"kind": "code", "size": 20}, "same": {"kind": "data", "size": 8}}
    rows = size.diff(before, after)
    assert rows == [(60, "a", 100, 160), (-40, "gone", 40, 0), (20, "new", 0, 20)]


def test_the_nm_beside_the_compiler_the_cache_names_reads_the_image(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "xtensa-esp32-elf-g++").write_text("")
    (bin_dir / "xtensa-esp32-elf-nm").write_text("")
    (tmp_path / "CMakeCache.txt").write_text(f"CMAKE_CXX_COMPILER:FILEPATH={bin_dir / 'xtensa-esp32-elf-g++'}\n")
    assert size.nm_for(tmp_path) == str(bin_dir / "xtensa-esp32-elf-nm")

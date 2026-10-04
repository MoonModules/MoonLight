"""The code report and the ratchet it shares with docgen and prose."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import check_code  # noqa: E402
import _ratchet  # noqa: E402


def test_a_function_over_both_limits_counts_once_per_rule():
    """One row per rule a function breaks, so the per-rule table says WHICH limit, and a function under both earns nothing."""
    def fn(name, file="src/light/b.h", **k):
        f = {"nloc": 10, "ccn": 2, "params": 2, "file": file, "name": name, "start": 1, "nesting": 1}
        return dict(f, **k)
    funcs = [fn("mm::both", "src/core/a.cpp", nloc=70, ccn=15), fn("mm::branchy", "src/core/a.cpp", ccn=15),
             fn("mm::long", nloc=70), fn("mm::deep", nesting=5), fn("mm::wide", params=8),
             fn("mm::fine", nesting=3, params=7)]
    rows = check_code.findings(funcs)
    assert sorted(r[1] for r in rows) == ["complex function", "complex function", "deeply nested", "long function", "long function", "long parameter list"]
    assert check_code.counts(rows) == {"complex function": 2, "long function": 2, "deeply nested": 1, "long parameter list": 1, "(total)": 6}
    # The KPI and repo health count functions, once each however many limits one breaks.
    assert check_code.functions_over([dict(f, start=i) for i, f in enumerate(funcs)]) == 5


def test_a_large_file_is_a_finding_and_a_vendored_one_is_not(tmp_path, monkeypatch):
    """File length is counted on the files we own; a vendored header is not ours to shrink."""
    monkeypatch.setattr(check_code, "MAX_FILE_LINES", 3)
    rows = check_code.large_files()
    assert all(r[1] == "large file" and r[3] > 3 for r in rows)
    assert any(r[0] == "moondeck/check/check_code.py" for r in rows)
    assert not any(r[0].startswith("src/platform/desktop/vendor/") or r[0] == "test/doctest.h" for r in rows)
    # The same files feed the clone search, so a scenario's recorded JSON or an installer SVG never moves a code count.
    owned = check_code.owned_files()
    assert owned and not any(f.endswith((".json", ".svg", ".css", ".md")) for f in owned)
    # A file directly under a top folder counts, as `**` does in a shell.
    assert "src/main.cpp" in owned and "moondeck/moondeck.py" in owned and "test/scenario_runner.cpp" in owned


def test_the_report_round_trips_through_its_own_committed_parser(tmp_path, monkeypatch):
    """What the writer puts in the `## By rule` table and the headline is what the ratchet reads back, or the comparison compares nothing."""
    monkeypatch.setattr(check_code, "REPORT", tmp_path / "code.md")
    rows = [("src/core/a.cpp", "complex function", "mm::x", 12), ("src/core/a.cpp", "long function", "mm::x", 70),
            ("src/light/b.h", "complex function", "mm::y", 11)]
    check_code.write_report(rows)
    text = (tmp_path / "code.md").read_text()
    monkeypatch.setattr(check_code, "committed", lambda _report: text)
    assert check_code.committed_counts() == {"complex function": 2, "long function": 1, "(total)": 3}
    # Only the rule table feeds the baseline: a file or area row would be a rule that never appears.
    assert "`src/core/a.cpp`" in text and "`core`" in text
    # Every rule says how it is solved, the catalog move by name.
    assert all(rule in text and fix in text for rule, fix in check_code.FIXES.items())


def test_the_ratchet_refuses_a_rise_per_rule_and_in_total_over_the_union():
    """Per rule, so one rule cannot pay for another; in total, so a rule under its own baseline cannot absorb a new finding; over the union, so a rule the baseline never saw starts at zero."""
    base = {"complex function": 4, "long function": 2, "(total)": 6}
    assert _ratchet.risen(base, {"complex function": 4, "long function": 2, "(total)": 6}) == []
    assert _ratchet.risen(base, {"complex function": 2, "long function": 3, "(total)": 5}) == [("long function", 2, 3)]
    assert _ratchet.risen(base, {"complex function": 4, "long function": 2, "big file": 1, "(total)": 7}) == \
        [("(total)", 6, 7), ("big file", 0, 1)]
    assert _ratchet.risen(None, {"complex function": 9, "(total)": 9}) == [], "a first run records rather than refuses"


def test_the_area_is_the_directory_under_src():
    assert check_code.area("src/core/system/NetworkModule.h") == "core"
    assert check_code.area("src/platform/esp32/platform_esp32.cpp") == "platform"
    assert check_code.area("moondeck/check/x.py") == "moondeck"


def test_a_measurement_that_reads_nothing_is_a_failure_not_a_clean_report(tmp_path, monkeypatch):
    """A zero from a tool that read nothing looks like a sweep; lizard over an empty tree must answer None so the run fails loudly."""
    monkeypatch.setattr(check_code, "LIZARD_ARGS", [str(tmp_path), "-l", "cpp"])
    assert check_code.measure() is None


def test_a_clone_counts_under_both_of_its_files_and_names_the_other():
    """Either file's edit clears its row, and the row says where the twin is; the headline carries jscpd's share of duplicated lines."""
    report = {"duplicates": [{"lines": 7, "tokens": 80,
                              "firstFile": {"name": "src/light/effects/A.h", "start": 10, "end": 16},
                              "secondFile": {"name": "src/light/effects/B.h", "start": 40, "end": 46}}],
              "statistics": {"total": {"percentage": 1.18}}}
    rows, pct = check_code.clone_rows(report)
    assert pct == 1.18
    assert [(r[0], r[1], r[3]) for r in rows] == [("src/light/effects/A.h", "duplicated block", 7), ("src/light/effects/B.h", "duplicated block", 7)]
    assert rows[0][2] == "lines 10-16, also in src/light/effects/B.h:40"   # the span from start and length, since jscpd's end field is unreliable
    assert rows[1][2] == "lines 40-46, also in src/light/effects/A.h:10"
    assert check_code.clone_rows({"duplicates": []}) == ([], 0.0)


def test_the_baseline_is_the_committed_report_and_a_report_never_committed_is_a_first_run():
    assert _ratchet.committed(_ratchet.ROOT / "docs/reference/metrics/docgen.md")
    assert _ratchet.committed(_ratchet.ROOT / "docs/reference/metrics/no-such-report.md") is None

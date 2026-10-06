"""The code report and the ratchet it shares with docgen and prose."""

import sys
from pathlib import Path

import pytest

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
    check_code.write_report(rows, (3.11, 4321))
    text = (tmp_path / "code.md").read_text()
    assert check_code.duplicated_lines_in(text) == 4321
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
              "statistics": {"total": {"percentage": 1.18, "duplicatedLines": 14}}}
    rows, duplicated = check_code.clone_rows(report)
    assert duplicated == (1.18, 14)
    assert [(r[0], r[1], r[3]) for r in rows] == [("src/light/effects/A.h", "duplicated block", 7), ("src/light/effects/B.h", "duplicated block", 7)]
    assert rows[0][2] == "lines 10-16, also in src/light/effects/B.h:40"   # the span from start and length, since jscpd's end field is unreliable
    assert rows[1][2] == "lines 40-46, also in src/light/effects/A.h:10"
    assert check_code.clone_rows({"duplicates": []}) == ([], (0.0, 0))


def test_a_commit_account_shows_what_it_added_and_what_it_saved():
    """The commit message carries its own account, so a subtraction shows as a saving in the commit that made it."""
    lines = {"src": [120, 340], "test": [60, 0]}
    base = {"complex function": 5, "long function": 3, "(total)": 8}
    now = {"complex function": 3, "long function": 3, "deeply nested": 1, "(total)": 7}
    assert check_code.account_line(lines, base, now, 4321, 4290) == (
        "Commit: src +120/-340 lines | test +60/-0 lines"
        " | code findings 8 -> 7 (complex function -2, deeply nested +1)"
        " | duplicated lines 4321 -> 4290 (-31)")
    # A report this pass did not rewrite says nothing about the code, so it is left out rather than shown as measured; so is duplication without the clone pass.
    assert check_code.account_line({"src": [1, 1], "test": [0, 0]}, base, base, None, None) == (
        "Commit: src +1/-1 lines | test +0/-0 lines")


def test_a_blocking_call_on_the_render_path_is_a_row_per_site(monkeypatch):
    """Each site the compiler finds counts once under its file, a float conversion at formatTo included."""
    import check_nonblocking as nb
    monkeypatch.setattr(nb, "function_effects_enabled", lambda _d: True)
    monkeypatch.setattr(nb, "build_output", lambda _d: "out")
    monkeypatch.setattr(nb, "collect", lambda _out: [{"file": "src/core/a.h", "callee": "mm::platform::millis", "fn": "tick1s"},
                                                     {"file": "src/core/a.h", "callee": "printf", "fn": None}])
    monkeypatch.setattr(nb, "float_conversions_on_the_hot_path", lambda: ["src/light/b.h:42"])
    monkeypatch.setattr(nb.check_clang_tidy, "_host_build_dir", lambda: _ratchet.ROOT)
    monkeypatch.setattr(check_code.Path, "exists", lambda self: True)
    assert check_code.hotpath_rows(check_code.build_warnings()) == [
        ("src/core/a.h", check_code.HOT_PATH, "mm::platform::millis in tick1s", 1),
        ("src/core/a.h", check_code.HOT_PATH, "printf in ?", 1),
        ("src/light/b.h", check_code.HOT_PATH, "a float conversion at formatTo, line 42", 1)]
    # A host whose compiler cannot measure it skips the rule; a build that fails raises, never a clean zero.
    monkeypatch.setattr(nb, "build_output", lambda _d: None)
    with pytest.raises(check_code.BuildMissing):
        check_code.build_warnings()
    monkeypatch.setattr(nb, "function_effects_enabled", lambda _d: False)
    assert check_code.build_warnings() is None


def test_the_two_boundaries_are_counted_where_the_architecture_draws_them():
    """Platform code belongs in src/platform and core includes nothing of the light domain; a comment or another folder crosses nothing."""
    find = lambda rel, text: [(r[1], r[2]) for r in check_code.boundary_findings(rel, text.splitlines())]
    assert find("src/core/a.h", '#include "light/drivers/Drivers.h"  // the light count') == [(check_code.CORE_LIGHT, 'line 1: #include "light/drivers/Drivers.h"')]
    assert find("src/light/a.h", '#include "light/drivers/Drivers.h"') == []
    assert find("src/light/a.h", "#include <esp_timer.h>\n#ifdef ESP_PLATFORM\nif constexpr (platform::hasWiFi) {}") == \
        [(check_code.PLATFORM, "line 1: #include <esp_timer.h>"), (check_code.PLATFORM, "line 2: #ifdef ESP_PLATFORM")]
    assert find("src/platform/esp32/a.cpp", "#include <esp_timer.h>") == []
    # Quoted SDK headers and #elif branches cross the same boundary.
    assert find("src/core/a.cpp", '#include "esp_timer.h"\n#elif defined(ESP_PLATFORM)') == \
        [(check_code.PLATFORM, 'line 1: #include "esp_timer.h"'), (check_code.PLATFORM, "line 2: #elif defined(ESP_PLATFORM)")]
    assert find("test/unit/a.cpp", "#include <esp_timer.h>") == []
    # The real tree: no platform code outside src/platform, which the old gate held at zero.
    assert not [r for r in check_code.boundary_rows() if r[1] == check_code.PLATFORM]


def _fake_builds(monkeypatch):
    """check_nonblocking with every build succeeding and the warning on, and the list of builds it asked for."""
    import check_nonblocking as nb
    import subprocess
    builds = []
    monkeypatch.setattr(nb.subprocess, "run", lambda cmd, **k: builds.append(cmd) or subprocess.CompletedProcess(cmd, 0, stdout="", stderr=""))
    monkeypatch.setattr(nb, "function_effects_enabled", lambda _d: True)
    return nb, builds


def _build_with(tmp_path, diags):
    """A build dir whose compile database names one object per entry in `diags`, each saved finding written when not None."""
    import json
    entries = []
    for i, text in enumerate(diags):
        obj = f"CMakeFiles/t.dir/f{i}.cpp.o"
        entries.append({"directory": str(tmp_path / "sub"), "file": f"f{i}.cpp", "output": obj, "command": "c++"})   # a subdirectory target: output stays relative to the top
        if text is not None:
            (tmp_path / obj).parent.mkdir(parents=True, exist_ok=True)
            (tmp_path / (obj + ".diag")).write_text(text, encoding="utf-8")
    (tmp_path / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")


def test_the_tree_is_read_from_every_files_saved_findings_after_an_incremental_build(monkeypatch, tmp_path):
    """A file that did not recompile still speaks through its saved findings, so no clean rebuild is needed."""
    nb, builds = _fake_builds(monkeypatch)
    _build_with(tmp_path, ["a.h:1:2: warning: x [-Wfunction-effects]", ""])
    (tmp_path / "CMakeFiles/t.dir/gone.cpp.o.diag").write_text("stale [-Wfunction-effects]", encoding="utf-8")
    out = nb.build_output(tmp_path)
    assert "a.h:1:2" in out and "stale" not in out   # a source the build dropped leaves nothing behind
    assert builds == [["cmake", "--build", str(tmp_path)]]   # incremental only


def test_a_build_from_before_the_launcher_is_rebuilt_clean_once(monkeypatch, tmp_path):
    nb, builds = _fake_builds(monkeypatch)
    _build_with(tmp_path, [None])
    (tmp_path / "CMakeFiles/t.dir").mkdir(parents=True)
    (tmp_path / "CMakeFiles/t.dir/build.make").write_text("diag_launcher.py c++ -c a.cpp", encoding="utf-8")
    assert nb.build_output(tmp_path) is None   # still nothing saved: the build needs reconfiguring, never a silent zero
    assert builds[-1][-1] == "--clean-first" and len(builds) == 2


def test_a_build_running_another_launcher_is_told_to_reconfigure_without_a_clean_rebuild(monkeypatch, tmp_path):
    nb, builds = _fake_builds(monkeypatch)
    _build_with(tmp_path, [None])
    (tmp_path / "CMakeFiles/t.dir").mkdir(parents=True)
    (tmp_path / "CMakeFiles/t.dir/build.make").write_text("ccache c++ -c a.cpp", encoding="utf-8")
    assert nb.build_output(tmp_path) is None
    assert builds == [["cmake", "--build", str(tmp_path)]]   # minutes of clean rebuild that could never save a finding are skipped


def test_silence_with_the_warning_on_is_a_measured_zero_and_without_it_unmeasured(monkeypatch, tmp_path):
    nb, _ = _fake_builds(monkeypatch)
    _build_with(tmp_path, [""])
    assert nb.build_output(tmp_path) == ""
    monkeypatch.setattr(nb, "function_effects_enabled", lambda _d: False)
    assert nb.build_output(tmp_path) is None


def test_the_launcher_keeps_what_the_compiler_said_beside_the_object(tmp_path):
    sys.path.insert(0, str(ROOT / "moondeck" / "build"))
    import diag_launcher
    obj = tmp_path / "f.cpp.o"
    rc = diag_launcher.main([sys.executable, "-c", "import sys; sys.stderr.write('w: x [-Wfunction-effects]')", "-o", str(obj)])
    assert rc == 0
    assert (tmp_path / "f.cpp.o.diag").read_text() == "w: x [-Wfunction-effects]"
    assert diag_launcher.object_path(["cc", "-c", "a.cpp", "-ob.o"]) == "b.o"
    assert diag_launcher.object_path(["cc", "-E", "a.cpp"]) is None


def test_the_baseline_is_the_committed_report_and_a_report_never_committed_is_a_first_run():
    assert _ratchet.committed(_ratchet.ROOT / "docs/reference/metrics/docgen.md")
    assert _ratchet.committed(_ratchet.ROOT / "docs/reference/metrics/no-such-report.md") is None


def _tree(tmp_path, monkeypatch, files: dict):
    """A miniature repository the file rules read, in place of the real one."""
    for rel, text in files.items():
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_text(text)
    monkeypatch.setattr(check_code, "ROOT", tmp_path)
    monkeypatch.setattr(check_code, "owned_files", lambda: list(files))


def test_a_module_type_named_in_the_ui_is_a_finding_and_a_container_is_not(tmp_path, monkeypatch):
    _tree(tmp_path, monkeypatch, {"src/ui/app.js": "if (m.type === 'NoiseEffect') x('Drivers'); // 'RainbowEffect'\n",
                                  "src/core/x.h": "const char* a = \"NoiseEffect\";\n"})
    rows = check_code.ui_type_rows({"NoiseEffect", "RainbowEffect", "Drivers"})
    assert rows == [("src/ui/app.js", check_code.UI_TYPE, "line 1: 'NoiseEffect'", 1)]


def test_a_type_named_beyond_its_own_files_spreads_onto_its_header(tmp_path, monkeypatch):
    monkeypatch.setattr(check_code, "MAX_SPREAD", 2)
    files = {"src/light/Foo.h": "class Foo {};\n", "test/unit/unit_Foo.cpp": "Foo f;\n",
             "src/module_types.cpp": "registerType<Foo>(\"Foo\");\n"}
    files.update({f"src/a{i}.h": "Foo* p;\n" for i in range(3)})
    _tree(tmp_path, monkeypatch, files)
    assert check_code.spread_rows({"Foo"}) == [("src/light/Foo.h", check_code.SPREAD, "Foo in 3 files", 3)]
    monkeypatch.setattr(check_code, "MAX_SPREAD", 3)
    assert check_code.spread_rows({"Foo"}) == []


def test_a_large_frame_is_a_row_per_function_and_only_ours_count(tmp_path, monkeypatch):
    _tree(tmp_path, monkeypatch, {"src/core/a.h": "", "src/platform/desktop/vendor/v.h": ""})
    monkeypatch.setattr(check_code, "owned_files", lambda: ["src/core/a.h"])
    out = "\n".join([
        f"{tmp_path}/src/core/a.h:40:17: warning: stack frame size (1056) exceeds limit (512) in 'bool mm::F::reg<mm::X>(char const*)' [-Wframe-larger-than]",
        f"{tmp_path}/src/core/a.h:40:17: warning: stack frame size (1056) exceeds limit (512) in 'bool mm::F::reg<mm::X>(char const*)' [-Wframe-larger-than]",
        f"{tmp_path}/src/core/a.h:40:17: warning: stack frame size (600) exceeds limit (512) in 'bool mm::F::reg<mm::Y>(char const*)' [-Wframe-larger-than]",
        f"{tmp_path}/src/platform/desktop/vendor/v.h:9:1: warning: stack frame size (4000) exceeds limit (512) in 'f()' [-Wframe-larger-than]",
        "/Library/SDK/thread.h:9:1: warning: stack frame size (700) exceeds limit (512) in 'g()' [-Wframe-larger-than]"])
    rows = sorted(check_code.stack_rows(out))
    assert rows == [("src/core/a.h", check_code.STACK, "F::reg<mm::X>, line 40", 1056),
                    ("src/core/a.h", check_code.STACK, "F::reg<mm::Y>, line 40", 600)]


def test_a_hand_buffer_in_light_is_a_finding_and_a_factory_or_a_comment_is_not(tmp_path, monkeypatch):
    _tree(tmp_path, monkeypatch, {
        "src/light/a.h": "auto* p = new uint8_t[n];\nbuf = static_cast<T*>(platform::alloc(n));\n// malloc(n) in a comment\n"
                         "reg([]() -> P* { return new Peripheral(); });\nnew (slot) T();\n",
        "src/core/b.h": "auto* p = new uint8_t[n];\n"})
    assert [r[2] for r in check_code.raw_alloc_rows()] == ["line 1: auto* p = new uint8_t[n];",
                                                          "line 2: buf = static_cast<T*>(platform::alloc(n));"]

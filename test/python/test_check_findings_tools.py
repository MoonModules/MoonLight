"""Every check keeps its findings in the shared record: CodeQL's alerts by severity, clang-query's rows by rule."""

import json
from pathlib import Path

import _findings
import check_clang_query
import check_codeql

ROOT = Path(__file__).resolve().parents[2]


def test_a_critical_alert_is_an_error_and_a_note_a_warning():
    rows = [{"sev": "critical", "rule": "cpp/overflow", "file": "src/a.cpp", "line": 3, "msg": "bad"},
            {"sev": "note", "rule": "cpp/unused", "file": "src/b.cpp", "line": 9, "msg": "meh"}]
    assert [(f["rule"], f["level"]) for f in check_codeql.alert_findings(rows)] == [("cpp/overflow", "error"), ("cpp/unused", "warning")]


def test_clang_query_rows_become_findings_and_a_clean_run_writes_an_empty_record():
    arrays = [{"file": "src/a.h", "line": 4, "name": "buf", "elem": "uint8_t", "count": 8, "bytes": 8}]
    assert check_clang_query.rule_findings("arrays", arrays)[0]["value"] == 8
    undocumented = [{"file": "src/a.h", "line": 4, "name": "f", "doc_words": 0}, {"file": "src/a.h", "line": 9, "name": "g", "doc_words": 5}]
    assert len(check_clang_query.rule_findings("comments", undocumented)) == 1
    path = _findings.write("check_clang_query", {"heap": {"reason": "x"}}, [])
    assert json.loads(path.read_text())["findings"] == []


def test_every_check_writes_its_findings_except_those_that_find_nothing_of_their_own():
    """The test page shows each check with what it last found, so a check script that does not write its findings is a card that stays empty."""
    measures = {"check_module": "runs the other checks on one module", "check_footprint": "measures bytes against no limit"}
    for path in sorted((ROOT / "moondeck" / "check").glob("check_*.py")):
        writes = "_findings.write(" in path.read_text(encoding="utf-8")
        assert writes != (path.stem in measures), f"{path.stem}: {'writes findings, so it is no longer a measure' if writes else 'does not write its findings'}"


def test_a_firmware_check_replaces_its_own_verdict_and_keeps_the_other_firmwares():
    import check_esp32_built
    stale = {"rule": "stale binary", "message": "esp32: stale", "level": "error"}
    check_esp32_built.record("esp32", [stale])
    check_esp32_built.record("esp32s3", [])
    found = json.loads((_findings.DIR / "check_esp32_built.json").read_text())["findings"]
    assert [(f["firmware"], f["rule"]) for f in found] == [("esp32", "stale binary")], "a fresh S3 does not erase the stale classic"
    check_esp32_built.record("esp32", [])
    assert json.loads((_findings.DIR / "check_esp32_built.json").read_text())["findings"] == []

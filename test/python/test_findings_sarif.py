"""A check's findings reach code scanning as SARIF 2.1.0: a run per check, its rules declared, each finding a located result."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck" / "ci"))
import findings_sarif  # noqa: E402


def test_a_findings_record_becomes_one_sarif_run_with_its_rules_and_located_results(tmp_path):
    record = tmp_path / "findings"
    record.mkdir()
    (record / "check_x.json").write_text(json.dumps({"check": "check_x",
        "rules": {"long function": {"reason": "over 60 lines", "fix": "Extract Function", "standard": "docs/contributing/coding-standards.md#static-checks", "count": 1}},
        "findings": [{"rule": "long function", "level": "error", "message": "f is 70 lines", "file": "src/a.cpp", "line": 3},
                     {"rule": "bugprone-x", "level": "warning", "message": "from the tool"}]}))
    [path] = findings_sarif.write(tmp_path / "sarif", record)
    run = json.loads(path.read_text())["runs"][0]
    assert run["tool"]["driver"]["name"] == "check_x" and run["automationDetails"]["id"] == "check_x/"
    assert [r["id"] for r in run["tool"]["driver"]["rules"]] == ["long function", "bugprone-x"], "a rule only a finding names is declared too"
    assert run["tool"]["driver"]["rules"][0]["helpUri"].endswith("coding-standards.md#static-checks")
    located, bare = run["results"]
    assert located == {"ruleId": "long function", "level": "error", "message": {"text": "f is 70 lines"},
                       "locations": [{"physicalLocation": {"artifactLocation": {"uri": "src/a.cpp"}, "region": {"startLine": 3}}}]}
    assert "locations" not in bare


def test_no_findings_record_is_a_failed_export_not_a_clean_one(tmp_path, monkeypatch):
    monkeypatch.setattr(findings_sarif._findings, "DIR", tmp_path / "none")
    monkeypatch.setattr(sys, "argv", ["findings_sarif.py", "--out", str(tmp_path / "sarif")])
    assert findings_sarif.main() == 1

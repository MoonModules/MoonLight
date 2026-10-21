"""Every test imports the checks and the docs build by module name, and keeps the checks' findings out of the repository's own findings folder, which a real run of a check fills."""

import sys
from pathlib import Path

import pytest

for folder in ("check", "docs"):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "moondeck" / folder))


@pytest.fixture(autouse=True)
def findings_stay_out_of_the_real_folder(monkeypatch, tmp_path):
    import _findings
    monkeypatch.setattr(_findings, "DIR", tmp_path / "findings")

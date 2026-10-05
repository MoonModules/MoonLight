"""The two moments a release moves library.json: after a release, and toward a major."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "build"))

import release_version as rv  # noqa: E402


def test_after_a_release_development_moves_to_the_next_minor():
    assert rv.target("next", "6.0.0", "6.0.0") == "6.1.0-dev"
    assert rv.target("next", "6.1.0-dev", "6.1.0") == "6.2.0-dev"


def test_a_major_release_is_declared_before_it_is_tagged():
    assert rv.target("major", "6.1.0-dev", "6.0.0") == "7.0.0-dev"
    assert rv.target("major", "6.1.0-dev", None) == "7.0.0-dev"   # before the first release: from library.json


def test_only_the_version_field_changes(tmp_path):
    f = tmp_path / "library.json"
    f.write_text('{\n  "name": "MoonLight",\n  "version": "6.1.0-dev",\n  "description": "x"\n}\n')
    rv.set_version("7.0.0-dev", f)
    assert f.read_text() == '{\n  "name": "MoonLight",\n  "version": "7.0.0-dev",\n  "description": "x"\n}\n'
    assert json.loads(f.read_text())["version"] == "7.0.0-dev"

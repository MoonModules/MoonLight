"""Every clip is spoken by a named voice, and the two passes that speak it agree on which.

A clip is spoken twice: `mtmeasure` times each line so the recording can hold the shot, and
`mtvoiceover` lays the narration on afterwards. Two voices do not speak a line at the same speed, so
a clip measured with one and voiced with another drifts off its shots. The run file names the voice
and both passes read it through `voice_of`, which is what these tests pin.

The series is also meant to sound like a team: Luna presents the slides as Alba, and the clips
alternate between the other voices, so the run files themselves are checked for that.
"""

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "moontube"))
import mtnarrate  # noqa: E402

CLIPS = sorted((ROOT / "moontube" / "clips").glob("*.json"))
SLIDES = sorted((ROOT / "moontube" / "slides").glob("*.json"))
PROJECTS = ROOT / "moontube" / "projects"


def test_a_script_is_spoken_in_the_voice_it_names():
    assert mtnarrate.voice_of({"voice": "alan"}) == "alan"


def test_a_script_naming_no_voice_is_spoken_by_luna():
    assert mtnarrate.voice_of({}) == mtnarrate.DEFAULT_VOICE == "alba"


def test_the_command_line_overrides_the_script():
    assert mtnarrate.voice_of({"voice": "alan"}, "cori") == "cori"


def test_an_unknown_voice_stops_the_run_and_lists_the_choices():
    with pytest.raises(SystemExit) as stop:
        mtnarrate.voice_of({"voice": "nobody"})
    assert "alba" in str(stop.value)


def test_every_clip_names_a_voice_that_exists():
    for path in CLIPS:
        voice = json.loads(path.read_text(encoding="utf-8")).get("voice")
        assert voice in mtnarrate.VOICE_PATHS, f"{path.name} names {voice!r}"


def test_the_slides_are_lunas():
    for path in SLIDES:
        script = json.loads(path.read_text(encoding="utf-8"))
        assert "voice" not in script, f"{path.name} names a voice; a slide deck falls to Luna by naming none"
        assert mtnarrate.voice_of(script) == "alba", path.name


def test_no_clip_shares_a_voice_with_its_neighbor_in_the_series():
    series = json.loads((PROJECTS / "full-series.json").read_text(encoding="utf-8"))
    voices = []
    series["clips"] = [entry for entry in series["clips"] if "clip" in entry]   # a loose source file has no narrator
    for entry in series["clips"]:
        for folder in ("clips", "slides"):
            path = ROOT / "moontube" / folder / f"{entry['clip']}.json"
            if path.exists():
                voices.append((entry["clip"], mtnarrate.voice_of(json.loads(path.read_text(encoding="utf-8")))))
    assert len(voices) == len(series["clips"])
    clashes = [f"{a[0]} and {b[0]} are both {a[1]}" for a, b in zip(voices, voices[1:]) if a[1] == b[1]]
    # The two closing decks are both Luna's and sit side by side, which is the one allowed pair.
    assert clashes == ["12-getting-involved and 13-attribution are both alba"]


def test_speakers_of_one_model_share_one_download(tmp_path, monkeypatch):
    monkeypatch.setattr(mtnarrate, "VOICES_DIR", tmp_path)
    model = tmp_path / "en_GB-semaine-medium.onnx"
    model.write_bytes(b"x")
    assert mtnarrate.voice_model("prudence") == mtnarrate.voice_model("obadiah") == model


def test_the_overview_lists_the_clips_after_this_one_from_the_project():
    project = {"clips": [{"clip": "00-intro", "title": "Why"},
                         {"clip": "01-a", "title": "First", "subtitle": "on a computer"},
                         {"clip": "02-b", "title": "Second"}]}
    assert mtnarrate.overview_rows(project, "00-intro") == [["1. First", "on a computer"],
                                                            ["2. Second", ""]]


def test_the_intros_overview_names_a_project_that_exists():
    intro = json.loads((ROOT / "moontube" / "slides" / "00-intro.json").read_text(encoding="utf-8"))
    named = [s["overview"] for s in intro["slides"] if "overview" in s]
    assert named == ["full-series"]
    rows = mtnarrate.overview_rows(json.loads((PROJECTS / "full-series.json").read_text(encoding="utf-8")), intro["name"])
    assert len(rows) == 16

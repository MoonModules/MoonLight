"""The spec-drift checks in check_specs.py must catch a control range or an author
URL that changed in the .h but not the .md — and NOT false-alarm on the common
cases (a control whose prose omits the range, a range spelled with a hyphen vs
en-dash). These are the two Phase-3 drift guards; they run inside check_spec_freshness
on every commit (via the spec-check gate), so a wrong range/URL in a doc is caught at
commit instead of shipping. This test pins them against synthetic .h/.md text.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

from check_specs import _check_range_drift, _check_author_url_drift  # noqa: E402


# ---- range drift ----

def test_range_drift_flags_a_conflicting_range():
    src = 'controls_.addControl("floor", floor, 0, 255);'
    md = "- `floor` — noise floor (0–128)."     # .md says 0–128, .h says 0–255
    issues = _check_range_drift(src, md)
    assert issues and "floor" in issues[0] and "0–255" in issues[0]


def test_range_drift_silent_when_ranges_match():
    src = 'controls_.addControl("freq_x", freq_x, 1, 8);'
    md = "- `freq_x` — wave frequency (1–8)."
    assert _check_range_drift(src, md) == []


def test_range_drift_tolerates_hyphen_and_to_spellings():
    src = 'controls_.addControl("count", count, 1, 255);'
    for prose in ("(1-255)", "1 to 255", "(1–255)"):
        md = f"- `count` — rings {prose}."
        assert _check_range_drift(src, md) == [], prose


def test_range_drift_silent_when_prose_states_no_range():
    # Many controls legitimately don't restate their range (pins, obvious 0–255).
    src = 'controls_.addControl("gain", gain, 1, 255);'
    md = "- `gain` — microphone gain."
    assert _check_range_drift(src, md) == []


def test_range_drift_ignores_non_range_controls():
    # A bool addControl / addSelect / addPin carry no numeric range → nothing to check.
    src = 'controls_.addControl("enabled", enabled);\ncontrols_.addPin("sdPin", sdPin);'
    assert _check_range_drift(src, "- `enabled` — on/off. - `sdPin` — data pin.") == []


# ---- author-URL drift ----

def test_author_url_drift_flags_missing_url():
    src = "// Author: Someone — https://github.com/acme/thing"
    md = "Origin: Someone · a different link https://github.com/acme/OTHER"
    issues = _check_author_url_drift(src, md)
    assert issues and "acme/thing" in issues[0]


def test_author_url_drift_silent_when_url_present():
    src = "// Author: Someone — https://github.com/acme/thing"
    md = "Origin: Someone · source [thing](https://github.com/acme/thing)"
    assert _check_author_url_drift(src, md) == []


def test_author_url_drift_silent_when_no_url_in_header():
    # An Author line without a URL (a plain credit) has nothing to sync.
    src = "// Author: FastLED inoise field (Mark Kriegsman)"
    assert _check_author_url_drift(src, "Origin: FastLED · inoise field") == []


def test_the_registry_page_check_reads_the_registry_and_names_a_missing_anchor(tmp_path, monkeypatch):
    """The check once read a file the registry had moved out of and reported zero; an unreadable registry is a finding now, and a bad anchor is named."""
    import check_specs
    monkeypatch.setattr(check_specs, "SRC", tmp_path)
    assert check_specs.check_registertype_docpaths() == ["no registerType docPath found in module_types.cpp"]
    (tmp_path / "module_types.cpp").write_text('mm::ModuleFactory::registerType<mm::X>("X", "light/effects.md#nosuch");')
    assert check_specs.check_registertype_docpaths() == ['registerType docPath "light/effects.md#nosuch": no anchor #nosuch in light/effects.md']


def test_a_registry_link_into_a_generated_catalog_page_resolves_its_anchors(tmp_path, monkeypatch):
    """The modifiers page has no markdown of its own: its anchors exist on the page generated from help.json, which the check reads."""
    import check_specs
    monkeypatch.setattr(check_specs, "SRC", tmp_path)
    (tmp_path / "module_types.cpp").write_text('mm::ModuleFactory::registerType<mm::B>("BlockModifier", "light/modifiers.md#block");\n'
                                               'mm::ModuleFactory::registerType<mm::X>("X", "light/modifiers.md#nosuch");')
    assert check_specs.check_registertype_docpaths() == ['registerType docPath "light/modifiers.md#nosuch": no anchor #nosuch in light/modifiers.md']

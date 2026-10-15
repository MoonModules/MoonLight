"""check_specs: every registerType docPath reaches its page and its anchor, written or generated from help.json."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))


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

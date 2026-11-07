"""screenshot_modules captures the module cards and previews from a running MoonLight, and leaves that MoonLight's pipeline as it found it.

Each rule here keeps the user's own setup safe from a capture run: the run removes only the modules it added, by a ledger rather than by name, a run cut short undone by the next against the same host, never takes over a module of the user's that shares a capture's name, and puts back the grid size and the modifiers it changed, their on-switch included."""

import sys
import types

# The capture drives a browser through Playwright, which the test environment does not install; the rules
# below never open a browser, so a stand-in for the import is enough.
if "playwright.sync_api" not in sys.modules:
    _pw = types.ModuleType("playwright")
    _api = types.ModuleType("playwright.sync_api")
    _api.sync_playwright = None
    _api.Page = object
    _pw.sync_api = _api
    sys.modules["playwright"] = _pw
    sys.modules["playwright.sync_api"] = _api

import pytest  # noqa: E402

import screenshot_modules as sm  # noqa: E402


class Response:
    """What the HTTP helpers return, as much of it as the capture reads."""

    def __init__(self, body=None, status=200):
        self.body = body or {}
        self.status_code = status
        self.ok = status < 400
        self.headers = {"content-type": "application/json"}

    def json(self):
        return self.body


@pytest.fixture
def ledger(tmp_path, monkeypatch):
    path = tmp_path / "screenshot_added.json"
    monkeypatch.setattr(sm, "LEDGER", path)
    return path


def test_the_ledger_keeps_on_disk_what_the_run_added_and_changed_on_its_host(ledger):
    added = sm.Ledger("board-a")
    added.append("Strings1")
    added.append("Ripples")
    added.remove("Strings1")
    added.pipeline = {"grids": [{"name": "Grid", "width": 16, "height": 16}], "modifiers": []}
    added.save()
    left = sm.Ledger.left_over("board-a")   # what a run cut short leaves for the next against that host
    assert left["added"] == ["Ripples"]
    assert left["pipeline"]["grids"][0]["width"] == 16
    added.close()
    assert sm.Ledger.left_over("board-a") is None


def test_another_host_s_ledger_is_left_for_a_run_against_that_host(ledger):
    added = sm.Ledger("board-a")
    added.append("Strings1")
    assert sm.Ledger.left_over("board-b") is None   # board-b's own Strings1 is not touched
    assert sm.Ledger.left_over("board-a")["added"] == ["Strings1"]


def test_a_missing_or_unreadable_ledger_leaves_nothing_to_undo(ledger):
    assert sm.Ledger.left_over("board-a") is None
    ledger.write_text("not json")
    assert sm.Ledger.left_over("board-a") is None


def test_a_capture_never_takes_the_name_of_a_module_the_user_already_has(monkeypatch):
    state = {"modules": [{"name": "Effects", "type": "Effects", "children": [
        {"name": "Layer", "type": "Layer", "children": [{"name": "Strings", "type": "StringsEffect"}]}]}]}
    sent = {}

    def post(url, json=None, **kwargs):
        sent.update(json)
        return Response({"name": "Strings1"})

    monkeypatch.setattr(sm, "_get", lambda url, **kwargs: Response(state))
    monkeypatch.setattr(sm, "_post", post)
    assert sm.add_module("host", "Strings", "StringsEffect", "Layer", {}) == "Strings1"
    assert "id" not in sent   # the device picks the name, so the user's Strings is not the one the run deletes


def test_a_capture_names_its_module_as_the_ui_does_when_the_name_is_free(monkeypatch):
    sent = {}

    def post(url, json=None, **kwargs):
        sent.update(json)
        return Response({"name": "Strings"})

    monkeypatch.setattr(sm, "_get", lambda url, **kwargs: Response({"modules": []}))
    monkeypatch.setattr(sm, "_post", post)
    assert sm.add_module("host", "Strings", "StringsEffect", "Layer", {}) == "Strings"
    assert sent["id"] == "Strings"


def test_the_grid_and_the_modifiers_a_capture_changes_go_back_as_they_were(monkeypatch):
    state = {"modules": [
        {"name": "Layouts", "type": "Layouts", "children": [
            {"name": "Grid", "type": "GridLayout", "controls": [{"name": "width", "value": 128}, {"name": "height", "value": 96}]}]},
        {"name": "Effects", "type": "Effects", "children": [
            {"name": "Layer", "type": "Layer", "children": [
                {"name": "Strings", "type": "StringsEffect"},
                {"name": "Block", "type": "BlockModifier", "role": "modifier", "enabled": False,
                 "controls": [{"name": "size", "value": 7}]}]}]}]}
    calls = []
    monkeypatch.setattr(sm, "_get", lambda url, **kwargs: Response(state))
    monkeypatch.setattr(sm, "delete_module", lambda host, name: calls.append(("delete", name)) or True)
    monkeypatch.setattr(sm, "set_control", lambda host, name, ctrl, value: calls.append(("set", name, ctrl, value)) or True)
    monkeypatch.setattr(sm, "add_module", lambda host, name, type_, parent, props: calls.append(("add", name, type_, parent)) or name)

    changed = sm.prepare_pipeline("host", drop_modifiers=True, grid=64)
    assert ("delete", "Block") in calls
    assert ("set", "Grid", "width", 64) in calls and ("set", "Grid", "height", 64) in calls

    calls.clear()
    sm.restore_pipeline("host", changed, "Layer")
    assert ("set", "Grid", "width", 128) in calls and ("set", "Grid", "height", 96) in calls
    assert ("add", "Block", "BlockModifier", "Layer") in calls
    assert ("set", "Block", "size", 7) in calls
    assert ("set", "Block", "enabled", False) in calls   # switched off as it was, not back on


def test_a_capture_that_changes_nothing_puts_nothing_back(monkeypatch):
    calls = []
    monkeypatch.setattr(sm, "_get", lambda url, **kwargs: Response({"modules": []}))
    monkeypatch.setattr(sm, "set_control", lambda *a: calls.append(a) or True)
    monkeypatch.setattr(sm, "add_module", lambda *a: calls.append(a) or "x")
    sm.restore_pipeline("host", sm.prepare_pipeline("host", drop_modifiers=True, grid=64), "Layer")
    assert calls == []

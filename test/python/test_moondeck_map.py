"""MoonDeck's Live tab draws the device UI's own map and opens its own node popup: the files it serves from src/ui, and the module that imports them."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import moondeck  # noqa: E402


def test_moondeck_serves_the_device_uis_scripts_and_sheets_and_nothing_else_of_it():
    assert moondeck.shared_ui_file("/ui/devicemap.js") == (ROOT / "src" / "ui" / "devicemap.js", "application/javascript")
    assert moondeck.shared_ui_file("/ui/devicemap.css")[1] == "text/css"
    assert moondeck.shared_ui_file("/ui/ui_embedded.h") is None          # the generated header and every other kind stay private
    assert moondeck.shared_ui_file("/ui/../moondeck.json") is None       # one name, no way out of the folder
    assert moondeck.shared_ui_file("/ui/no-such-module.js") is None


def test_moondecks_static_route_serves_its_own_folders_and_nothing_above_them():
    assert moondeck.static_file("/") == (ROOT / "moondeck" / "moondeck_ui" / "index.html").resolve()
    assert moondeck.static_file("/?tab=host") == moondeck.static_file("/index.html")
    assert moondeck.static_file("/app.js") == (ROOT / "moondeck" / "moondeck_ui" / "app.js").resolve()
    assert moondeck.static_file("/../moondeck.json") is None              # http.server hands the path over unnormalized
    assert moondeck.static_file("/assets/../../moondeck/moondeck.json") is None
    assert moondeck.static_file("/no-such-file.js") is None


def _reached(entry):
    """Every module `entry` reaches by relative imports, transitively, itself included."""
    seen, todo = set(), [entry]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        todo += re.findall(r'^import [^;]*? from "\./([\w.-]+)";', (ROOT / "src" / "ui" / name).read_text(), re.M)
    return seen


def test_every_module_the_map_and_its_popup_import_is_served_beside_them():
    # A browser loads them from /ui/ and resolves their imports there, so each module they reach must be a served path.
    for entry, expected in (("devicemap.js", {"help.js"}), ("devicepopup.js", {"deviceinstall.js", "devicemap.js", "install-picker.js", "releases.js", "semver.js", "uikit.js"})):
        reached = _reached(entry)
        assert expected <= reached, entry   # the walk reads the imports it should
        assert all(moondeck.shared_ui_file(f"/ui/{name}") for name in reached), sorted(n for n in reached if not moondeck.shared_ui_file(f"/ui/{n}"))


def test_the_live_tab_loads_the_map_from_the_served_paths():
    page = (ROOT / "moondeck" / "moondeck_ui" / "index.html").read_text()
    module = (ROOT / "moondeck" / "moondeck_ui" / "map.js").read_text()
    assert '<link rel="stylesheet" href="/ui/devicemap.css">' in page and 'src="map.js"' in page
    assert 'from "/ui/devicemap.js"' in module and 'from "/ui/devicepopup.js"' in module


def test_the_popup_takes_moondecks_own_section_through_its_actions_hook():
    popup = (ROOT / "src" / "ui" / "devicepopup.js").read_text()
    module = (ROOT / "moondeck" / "moondeck_ui" / "map.js").read_text()
    assert "actions: () => {}," in popup and "hooks(opts).actions(row, dlg, opts);" in popup   # a page without one shows the popup as is
    assert 'import { moondeckActions } from "./popup.js";' in module and "actions: moondeckActions," in module

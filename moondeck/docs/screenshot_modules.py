#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["playwright", "requests"]
# ///
"""Capture UI screenshots and preview GIFs of every projectMM module.

Connects to a running projectMM server, adds each module to a minimal
pipeline via REST, screenshots its card in the web UI, then removes it.
For effects and modifiers also captures a 3-second GIF of the preview canvas.

Also captures MoonDeck tab screenshots and the web installer page, and
inserts them into the appropriate docs files.

Saves to (by domain/type, mirroring src):
  docs/assets/light/effects/<TypeName>.png/.gif   — effect card + preview
  docs/assets/light/{modifiers,layouts,drivers}/  — other light modules
  docs/assets/core/<TypeName>.png                 — core modules
  docs/assets/ui/ui_overview.png                  — projectMM full-page screenshot
  docs/assets/ui/moondeck_{desktop,esp32,live}.png     — MoonDeck tabs
  docs/assets/ui/installer.png                    — web installer page

Usage:
    # one effect, raw (no modifier), good preview size, with its GIF, overwriting:
    uv run moondeck/docs/screenshot_modules.py --filter Ripples --no-modifier --grid 32 --gif --force
    # everything, same look:
    uv run moondeck/docs/screenshot_modules.py --no-modifier --grid 32 --gif --force

Preview quality:
    --no-modifier  removes the default MultiplyModifier so effects render RAW, not
                   mirror-folded. Recommended for effect previews.
    --grid N       resizes the grid before capture. 32 is the sweet spot: bigger
                   than the 16 boot default (more detail) but still dense in the
                   preview. NOTE: 128 looks WORSE, not better — the preview index-
                   downsamples to a fixed send budget, so a huge grid spreads sparse
                   effects (Ripples, Game of Life) to scattered dots. Use ~32.

Prerequisites (one-time, machine-local — NOT in the repo or the venv):
    1. ffmpeg on PATH                       brew install ffmpeg
    2. Playwright's chromium browser:
         uv run --with playwright playwright install chromium
       NOTE: plain `uv run playwright …` FAILS — playwright isn't a project
       dependency, it's an inline (PEP 723) dep of THIS script, so the browser
       install needs `--with playwright`. On macOS the browser lands in
       ~/Library/Caches/ms-playwright/ (not ~/.cache/). It survives venv rebuilds
       and repo transfers — if it's "missing" it was simply never installed here.

Running server: the script captures against whatever is on --host (default
:8080). Start ONE fresh server:  uv run moondeck/build/build_desktop.py && ./build/<host>/projectMM
(or via MoonDeck's Desktop tab — same per-host build dir, so they share the binary). GOTCHA: a leftover binary on :8080 captures the WRONG
images silently — e.g. a `build/macos/projectMM` from a MoonDeck run still bound
to the port serves the OLD code, so a renamed/changed effect screenshots as its
previous version no matter how often you rebuild. The script now prints a STALE
SERVER warning when the running types don't match src/main.cpp; if you see it,
`pkill -f projectMM` and start exactly one server. GIFs need --gif (PNG-only
otherwise). Always eyeball the output PNG — the card title + controls should
match the effect you captured.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import requests
from playwright.sync_api import sync_playwright, Page

ROOT = Path(__file__).resolve().parent.parent.parent
ASSETS = ROOT / "docs" / "assets"
UI_DIR = ASSETS / "ui"   # tooling / installer / full-page shots (not per-module)

# Map a module to its asset subfolder (domain/type), mirroring src. Module screenshots live in
# docs/assets/{core, light/{effects,modifiers,layouts,drivers}}/.
def asset_dir_for(type_name: str) -> Path:
    if type_name.endswith("Effect"):
        return ASSETS / "light" / "effects"
    if type_name.endswith("Modifier"):
        return ASSETS / "light" / "modifiers"
    if type_name.endswith("Layout"):
        return ASSETS / "light" / "layouts"
    if type_name.endswith("Driver"):
        return ASSETS / "light" / "drivers"
    if type_name in ("Layouts", "Effects", "Drivers"):
        return ASSETS / "light"
    return ASSETS / "core"   # SystemModule, FilesystemModule, DevicesModule, … and the rest

# ---------------------------------------------------------------------------
# Module catalogue
# Each entry: (type_name, parent_type, extra_props, capture_gif)
#   parent_type: "Layer" = child of Layer, "Drivers" = child of Drivers,
#                "Layouts" = child of Layouts
#   capture_gif: True = also record a preview GIF
# ---------------------------------------------------------------------------
MODULES = [
    # Layouts
    ("GridLayout",          "Layouts",  {}, True),
    # The border layout the screen-follow effect is built for: a ring with the corners shared, so
    # the preview shows the perimeter rather than a filled box.
    ("RectangleLayout",     "Layouts",  {"width": 24, "height": 16}, True),
    # The scripted layout, whose preview is the shape a script places rather than a control
    # panel: it runs `rose.mll`, a rhodonea curve whose petals no compiled layout defines,
    # so the capture shows what a script buys that a C++ class does not.
    ("MoonLiveLayout",      "Layouts",  {"script": "rose.mll"}, True),
    # Effects
    # Paints the live video frame, so with no source it photographs black. The capture points it
    # at the test pattern, whose four coloured bands and sweeping block are what the effect is for.
    ("AmbilightEffect",     "Layer",    {}, True),
    ("RainbowEffect",       "Layer",    {}, True),
    ("NoiseEffect",         "Layer",    {}, True),
    # The generative-fields showcases: each is Dim::D3, so the preview shows a volume.
    ("AuroraEffect",        "Layer",    {}, True),
    ("ColorTrailsEffect",   "Layer",    {}, True),
    ("TrailsEffect",        "Layer",    {}, True),
    ("NebulaEffect",        "Layer",    {}, True),
    ("FluidEffect",         "Layer",    {}, True),
    ("FireEffect",          "Layer",    {}, True),
    ("PlasmaEffect",        "Layer",    {}, True),
    ("PlasmaPaletteEffect", "Layer",    {}, True),
    ("LinesEffect",         "Layer",    {}, True),
    ("MetaballsEffect",     "Layer",    {}, True),
    ("ParticlesEffect",     "Layer",    {}, True),
    ("GlowParticlesEffect", "Layer",    {}, True),
    ("CheckerboardEffect",  "Layer",    {}, True),
    ("RingsEffect",         "Layer",    {}, True),
    ("RipplesEffect",       "Layer",    {}, True),
    ("LavaLampEffect",      "Layer",    {}, True),
    ("SpiralEffect",        "Layer",    {}, True),
    ("GameOfLifeEffect",    "Layer",    {}, True),
    # Modifiers — added as children of Layer
    ("MultiplyModifier",    "Layer",    {}, True),
    ("CheckerboardModifier","Layer",    {}, True),
    # Drivers
    ("NetworkSendDriver",    "Drivers",  {}, False),
    ("PreviewDriver",       "Drivers",  {}, False),
    # These three carry no card image yet. The desktop build links every driver
    # (MM_LINKS_ALL_LED_DRIVERS), so each renders its controls here even where the
    # silicon or the runtime is absent: HUB75 wants LCD_CAM or Parlio, NDI wants a
    # runtime the user installs. The card and its controls are what a screenshot is
    # for, and those are real on any host.
    # The two LED drivers, each its own card: they share only `pins` and `ledsPerPin`, and a
    # screenshot of one describes the other's controls wrongly.
    ("RmtLedDriver",        "Drivers",  {}, False),
    ("ParallelLedDriver",   "Drivers",  {}, False),
    ("Hub75Driver",         "Drivers",  {}, False),
    ("NdiDriver",           "Drivers",  {}, False),
    ("HlsDriver",           "Drivers",  {}, False),
    ("RtspDriver",          "Drivers",  {}, False),
]

# A modifier reshapes what an effect draws, so it has nothing to show on an empty Layer: it
# photographs as a dark panel. Every modifier capture therefore lays this effect down first and
# removes it again afterwards. Rainbow is the bed because a modifier is only visible as a BREAK in
# something regular: its smooth gradient fills every light, so a fold, a rotation or a transpose
# shows as an obvious seam. A noise field fills the panel too but is already irregular, which hides
# the very transform the card is meant to show.
BED_EFFECT = "RainbowEffect"

# Container types that exist in the pipeline but are not added via REST
CONTAINERS = ["Layouts", "Effects", "Drivers"]

# Core modules: always present in the pipeline, never added/deleted via REST.
# Each entry: type_name — the module's type string as reported by /api/types.
# The screenshot navigates to the module by its live name from /api/state.
CORE_MODULES = [
    "FilesystemModule",
    "SystemModule",
    "Services",
    "FirmwareUpdateModule",
    "NetworkModule",
    "HttpServerModule",
    "MqttModule",
    "FileManagerModule",
    "DevicesModule",
    "MoonCloudModule",
    "MoonStatsModule",
    "MoonTalkModule",
    "TasksModule",
    "PinsModule",
    # Present only in an ESP32 tree (services / provisioning) — skipped on desktop
    # (not in state); capture these against a board when needed.
    "ImprovProvisioningModule",
    "AudioService",
    "VideoService",
    "AnalogService",
    "I2cScanModule",
    "InfraredService",
    "ButtonService",
    "MoonLiveService",
]

# Core modules that are CHILDREN of another module (so they have no top-level nav entry
# of their own): {child type → parent type}. The capture navs to the parent's nav root,
# then screenshots the child's own card. Everything not listed here is a top-level card.
CORE_NAV_ROOT = {
    "MqttModule": "NetworkModule",
    # Tasks and Pins are fixed System children (wired-by-code), so they have no nav entry.
    "TasksModule": "SystemModule",
    "PinsModule": "SystemModule",
    # Stats and Talk are MoonCloud children, each carrying its own consent.
    "MoonStatsModule": "MoonCloudModule",
    "MoonTalkModule": "MoonCloudModule",
    "DevicesModule": "NetworkModule",
    "ImprovProvisioningModule": "NetworkModule",
    # Audio / infrared are user-added Services (children of the Services container); I2cScan is a
    # fixed System child (wired-by-code). They're added/present per-board and never exist in
    # the desktop tree — so they're captured against an ESP32, where these entries route the
    # shot to the right nav root.
    "AudioService": "Services",
    "VideoService": "Services",
    "AnalogService": "Services",
    "InfraredService": "Services",
    "ButtonService": "Services",
    "MoonLiveService": "Services",
    "I2cScanModule": "SystemModule",
}
# FileManagerModule, FirmwareUpdateModule, SystemModule, NetworkModule are top-level
# (scheduler.addModule in main.cpp) — NOT listed here, so they capture as standalone cards.

# ---------------------------------------------------------------------------
# Extra shots: MoonDeck tabs + web installer
# Each entry: (filename, url, wait_selector, doc_files, anchor_text)
#   filename:      saved as docs/assets/ui/<filename>
#   url:           full URL to load
#   wait_selector: CSS selector to wait for before screenshotting (or "")
#   doc_files:     list of repo-relative paths to insert image into
#   anchor_text:   text of the line/heading after which to insert (or "")
# ---------------------------------------------------------------------------
class _ExtrasOnlyDone(Exception):
    """Internal sentinel — raised inside the main capture try-block to bail
    out cleanly after the EXTRA_SHOTS loop when --extras-only is set."""


MOONDECK_URL   = "http://localhost:8420"
INSTALLER_URL  = "http://localhost:8000"

EXTRA_SHOTS = [
    (
        "moondeck_desktop.png",
        f"{MOONDECK_URL}/?tab=desktop",
        ".tab-content.active",
        ["README.md", "moondeck/MoonDeck.md"],
        "## Desktop Tab",
    ),
    (
        "moondeck_esp32.png",
        f"{MOONDECK_URL}/?tab=esp32",
        ".tab-content.active",
        ["moondeck/MoonDeck.md"],
        "## ESP32 Tab",
    ),
    (
        "moondeck_live.png",
        f"{MOONDECK_URL}/?tab=live",
        ".tab-content.active",
        ["moondeck/MoonDeck.md"],
        "## Live Tab",
    ),
    (
        "installer.png",
        INSTALLER_URL,
        "body",
        ["README.md", "moondeck/MoonDeck.md"],
        "### preview_installer",
    ),
]

# GIF capture settings
GIF_DURATION_S  = 3      # seconds to record
GIF_FPS         = 10     # frames per second captured
GIF_OUTPUT_FPS  = 10     # frames per second in output GIF

# ---------------------------------------------------------------------------
# REST helpers
# ---------------------------------------------------------------------------

def _get(url: str, **kwargs) -> requests.Response:
    """GET with retries — the embedded server resets connections under load."""
    for attempt in range(4):
        try:
            return requests.get(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _post(url: str, **kwargs) -> requests.Response:
    """POST with retries."""
    for attempt in range(4):
        try:
            return requests.post(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _delete(url: str, **kwargs) -> requests.Response:
    """DELETE with retries."""
    for attempt in range(4):
        try:
            return requests.delete(url, **kwargs)
        except requests.exceptions.ConnectionError:
            if attempt == 3:
                raise
            time.sleep(0.5 * (attempt + 1))
    raise RuntimeError("unreachable")


def _collect_names(modules: list) -> set[str]:
    """Recursively collect all module names from a state tree."""
    names: set[str] = set()
    for m in modules:
        names.add(m.get("name", ""))
        names |= _collect_names(m.get("children", []))
    return names


def add_module(host: str, id_: str, type_: str, parent_id: str | None,
               props: dict) -> str | None:
    """Add a module and return the actual name the server assigned.

    Snapshots state before and after the POST; the new module is whichever
    name appears after that wasn't there before and has the right type.
    Handles server-side name truncation and deduplication transparently.
    """
    before = _get(f"http://{host}/api/state", timeout=5)
    names_before = _collect_names(before.json().get("modules", [])) if before.ok else set()

    body: dict = {"id": id_, "type": type_}
    if parent_id:
        body["parent_id"] = parent_id
    # `props` is deliberately NOT sent: the create endpoint ignores it (no handler reads it), so a
    # caller that needs a control set gets it through /api/control after the module exists.
    r = _post(f"http://{host}/api/modules", json=body, timeout=5)
    if not r.ok:
        return None

    time.sleep(0.15)
    sr = _get(f"http://{host}/api/state", timeout=5)
    if not sr.ok:
        return id_[:16]

    def find_new(modules: list) -> str | None:
        for m in modules:
            n = m.get("name", "")
            if n not in names_before and m.get("type") == type_:
                return n
            found = find_new(m.get("children", []))
            if found:
                return found
        return None

    return find_new(sr.json().get("modules", [])) or id_[:16]


def delete_module(host: str, id_: str) -> bool:
    r = _delete(f"http://{host}/api/modules/{id_}", timeout=5)
    return r.ok


def set_control(host: str, module: str, control: str, value) -> bool:
    """Set a control value via the same /api/control path the UI uses."""
    r = _post(f"http://{host}/api/control",
              json={"module": module, "control": control, "value": value}, timeout=5)
    return r.ok


# Where a script control's picker looks: kScriptDir in MoonLiveScriptFile.h. The USER
# directory, not the factory one, because that is the directory the picker lists and the
# name a `script` control resolves against.
SCRIPT_DIR = "/moonlive"

# Where each script extension lives in the repo, imported rather than restated: catalog_scripts.py
# is the one home for the extension-to-role-to-folder mapping, and a second copy here would drift
# the moment a role is added.
sys.path.insert(0, str(ROOT / "src" / "light" / "moonlive"))
from catalog_scripts import ROLE_BY_EXT, FOLDER_BY_ROLE   # noqa: E402
SCRIPT_SRC = {ext: FOLDER_BY_ROLE[role] for ext, role in ROLE_BY_EXT.items()}


def push_script(host: str, name: str) -> bool:
    """Copy one script from the repo onto the device, so a capture needs no hand-copied file.

    A scripted module captures an empty card reading "script not found" unless the script it
    names is on the device, and the desktop build's filesystem is a build directory nothing
    in the repo seeds. Pushing here keeps the run self-contained: check out, build, capture.

    Byte-exact through /api/file, the same request the File Manager's upload makes.
    """
    src = ROOT / "moonlive" / SCRIPT_SRC.get(Path(name).suffix, "") / name
    if not src.is_file():
        return False
    r = _post(f"http://{host}/api/file?path={SCRIPT_DIR}/{name}",
              headers={"Content-Type": "application/octet-stream"},
              data=src.read_bytes(), timeout=10)
    return r.ok


def prepare_pipeline(host: str, drop_modifiers: bool, grid: int | None) -> None:
    """Make the capture pipeline match the requested look BEFORE adding effects.

    The default boot pipeline is Grid(16x16) + Layer(Noise + MultiplyModifier).
    Two knobs improve effect previews:
      - drop_modifiers: delete any Modifier children of the Layer so each effect
        renders RAW, not folded/mirrored by the default MultiplyModifier.
      - grid: resize the GridLayout (e.g. 128) for crisper, higher-res previews.
    Both are best-effort and logged; a failure here doesn't abort the run.
    """
    try:
        sr = _get(f"http://{host}/api/state", timeout=5)
        if not sr.ok:
            return
        mods = sr.json().get("modules", [])
    except Exception:
        return

    def walk(ms, role_wanted, type_contains):
        out = []
        for m in ms:
            if m.get("role") == role_wanted or type_contains in m.get("type", ""):
                out.append(m)
            out.extend(walk(m.get("children", []), role_wanted, type_contains))
        return out

    if drop_modifiers:
        for m in walk(mods, "modifier", "Modifier"):
            if delete_module(host, m.get("name", "")):
                print(f"  pipeline: removed modifier {m.get('name')!r} (raw effect preview)")

    if grid:
        for g in walk(mods, "", "GridLayout"):
            name = g.get("name", "")
            ok_w = set_control(host, name, "width", grid)
            ok_h = set_control(host, name, "height", grid)
            if ok_w and ok_h:
                print(f"  pipeline: grid {name!r} -> {grid}x{grid}")


def displace_default_layout(host: str) -> dict | None:
    """Take the existing layout out of the pipeline and return what it takes to put it back.

    Two layouts in the pipeline share one set of lights, so the boot GridLayout would overlap
    whichever layout is being photographed. It is removed for the shot and rebuilt afterwards
    from this snapshot, controls and all, so a capture run leaves the pipeline as it found it.
    Returns None when there is nothing to displace, which is also what a failure returns: the
    capture is still worth taking, and the restore then has nothing to undo.
    """
    try:
        sr = _get(f"http://{host}/api/state", timeout=5)
        if not sr.ok:
            return None

        def find(ms):
            for m in ms:
                if m.get("role") == "layout":
                    return m
                hit = find(m.get("children", []))
                if hit:
                    return hit
            return None

        cur = find(sr.json().get("modules", []))
        if not cur:
            return None
        # The NAME as well as the type: the boot pipeline calls its grid "Grid", and coming back
        # as "GridLayout" would leave the pipeline subtly different from the one found.
        snap = {"type": cur.get("type", ""), "name": cur.get("name", ""),
                "controls": {c.get("name"): c.get("value") for c in cur.get("controls", [])}}
        if not delete_module(host, cur.get("name", "")):
            return None
        return snap
    except Exception as e:
        print(f"  displace-layout failed: {e} ", end="", flush=True)
        return None


def restore_default_layout(host: str, snap: dict) -> None:
    """Put back what displace_default_layout took, controls and all."""
    try:
        name = add_module(host, (snap.get("name") or snap["type"])[:16], snap["type"],
                          "Layouts", {})
        if not name:
            print(f"  restore-layout: re-adding {snap['type']} failed ", end="", flush=True)
            return
        for ctrl, value in snap.get("controls", {}).items():
            set_control(host, name, ctrl, value)
    except Exception as e:
        print(f"  restore-layout failed: {e} ", end="", flush=True)


def get_types(host: str) -> set[str]:
    r = _get(f"http://{host}/api/types", timeout=5)
    if not r.ok:
        return set()
    data = r.json()
    types = data.get("types", data) if isinstance(data, dict) else data
    return {t["name"] if isinstance(t, dict) else t for t in types}


def source_registered_types() -> set[str]:
    """The module type names the firmware registers, parsed from registerType<...>("Name", ...).

    Read from module_types.cpp, the one file that registers them: the registry moved out of
    main.cpp so the firmware, the scenario runner and the ESP32 build share one list. Reading the
    old home returns nothing and silently disables the check below rather than failing loudly.

    Used to detect a STALE server binary: if the running server is missing a type
    the source registers, it's an old build (or a different binary), the cause of
    a hard-to-spot bug where screenshots capture the previous version of an effect.
    Returns an empty set if the file can't be read (then the check is skipped).
    """
    registry = ROOT / "src" / "module_types.cpp"
    try:
        text = registry.read_text()
    except OSError:
        return set()
    # registerType<mm::FooEffect>("FooEffect", "...") — capture the quoted name.
    return set(re.findall(r'registerType<[^>]+>\(\s*"([^"]+)"', text))


def check_server_freshness(host: str) -> None:
    """Warn loudly if the running server looks like a stale binary.

    Compares the server's registered types against what src/main.cpp registers.
    The classic failure (seen 2026-06): a leftover `build/macos/projectMM` from a
    MoonDeck run was still bound to :8080, so captures showed the PRE-rename effect
    no matter how many times the dev binary was rebuilt. A missing type is the
    tell — surface it with the fix instead of silently capturing wrong images.
    """
    src = source_registered_types()
    if not src:
        return  # couldn't parse main.cpp — skip rather than false-alarm
    server = get_types(host)
    missing = sorted(src - server)
    if missing:
        print(f"  ⚠️  STALE SERVER: {host} is missing {len(missing)} type(s) the "
              f"source registers: {', '.join(missing[:6])}"
              + (" …" if len(missing) > 6 else ""))
        print("      The running binary is older than the current source. Most likely a")
        print("      second projectMM (a stale build/<host>/projectMM) is still on :8080.")
        print("      Fix:  pkill -f projectMM  then rebuild + run ONE server:")
        print("        uv run moondeck/build/build_desktop.py && ./build/<host>/projectMM")


# ---------------------------------------------------------------------------
# Pipeline discovery
# ---------------------------------------------------------------------------

def find_parent_ids(host: str) -> tuple[dict[str, str], dict[str, str]]:
    """Return (parents, nav_roots) for the existing pipeline containers.

    parents maps role → module name to use as parent_id when adding a child.
    nav_roots maps role → top-level nav sidebar name (what to click).
    Layer is nested inside Effects, so its nav root is "Effects" not "Layer".
    """
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}, {}

    parents: dict[str, str] = {}
    nav_roots: dict[str, str] = {}

    def walk(modules: list, nav_root: str | None = None) -> None:
        for m in modules:
            t = m.get("type", "")
            n = m.get("name", "")
            current_nav = nav_root if nav_root else n
            if t == "Layer" and "Layer" not in parents:
                parents["Layer"] = n
                nav_roots["Layer"] = current_nav
            elif t == "Drivers" and "Drivers" not in parents:
                parents["Drivers"] = n
                nav_roots["Drivers"] = current_nav
            elif t == "Layouts" and "Layouts" not in parents:
                parents["Layouts"] = n
                nav_roots["Layouts"] = current_nav
            if m.get("children"):
                walk(m["children"], current_nav)

    walk(r.json().get("modules", []))
    return parents, nav_roots


def find_container_nav_names(host: str) -> dict[str, str]:
    """Return a map of container type → module name for top-level containers.

    Used to screenshot Layouts, Effects, Drivers cards directly.
    """
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}
    result: dict[str, str] = {}
    for m in r.json().get("modules", []):
        t = m.get("type", "")
        if t in CONTAINERS and t not in result:
            result[t] = m.get("name", "")
    return result


def find_core_module_names(host: str) -> dict[str, str]:
    """Return a map of type → live module name for core (always-present) modules.
    Recurses the tree: some core modules (Mqtt, Devices, Improv) are children of
    NetworkModule, not top-level, so a flat scan would miss them."""
    r = _get(f"http://{host}/api/state", timeout=5)
    if not r.ok:
        return {}
    result: dict[str, str] = {}

    def walk(mods):
        for m in mods:
            t = m.get("type", "")
            if t in CORE_MODULES and t not in result:
                result[t] = m.get("name", "")
            walk(m.get("children", []))

    walk(r.json().get("modules", []))
    return result


# ---------------------------------------------------------------------------
# Screenshot helpers
# ---------------------------------------------------------------------------

def _load_page(page: Page, host: str) -> None:
    """Reload the UI and wait for it to settle.

    Retries on socket errors — the embedded server can momentarily drop
    connections when hammered with rapid reloads.
    """
    url = f"http://{host}/"
    for attempt in range(4):
        try:
            page.goto(url, wait_until="networkidle", timeout=15000)
            page.wait_for_timeout(800)
            return
        except Exception:
            if attempt == 3:
                raise
            page.wait_for_timeout(1000 * (attempt + 1))


def _click_nav(page: Page, nav_root: str) -> None:
    nav_btn = page.query_selector(f'button.nav-item[data-module="{nav_root}"]')
    if nav_btn:
        nav_btn.click()
        page.wait_for_timeout(500)


def _screenshot_card(page: Page, module_id: str, out_path: Path) -> bool:
    """Screenshot the card for module_id. Page must already show the right nav."""
    card_sel = f'.card[data-module="{module_id}"]'
    try:
        page.wait_for_selector(card_sel, timeout=5000)
    except Exception:
        return False
    card = page.query_selector(card_sel)
    if not card:
        return False
    card.scroll_into_view_if_needed()
    page.wait_for_timeout(300)
    box = card.bounding_box()
    if not box:
        return False
    page.screenshot(path=str(out_path), clip=box)
    return True


def _click_child_tab(page: Page, module_id: str) -> None:
    """Open the tab for `module_id`, if its parent shows its children behind a tab strip.

    A TOP-LEVEL module renders one child at a time behind tabs (app.js renderChildTabs), so a
    sibling's card is not merely scrolled out of view: it is not in the DOM at all. Without this the
    capture only ever saw whichever child happened to be the active tab, and every other child of
    Services / System failed with "screenshot failed". A no-op where there is no tab strip, so the
    flat case is unchanged.
    """
    tab = page.query_selector(f'button.tab[data-tab-mid="{module_id}"]')
    if tab:
        tab.click()
        page.wait_for_timeout(500)


def screenshot_module(page: Page, host: str, module_id: str,
                      nav_root: str, out_path: Path) -> bool:
    """Reload the UI, click nav, open the child's tab if there is one, screenshot the card."""
    _load_page(page, host)
    _click_nav(page, nav_root)
    _click_child_tab(page, module_id)
    return _screenshot_card(page, module_id, out_path)


def screenshot_container(page: Page, host: str, container_name: str,
                         out_path: Path) -> bool:
    """Screenshot a top-level container card (Layouts, Effects, Drivers)."""
    _load_page(page, host)
    _click_nav(page, container_name)
    return _screenshot_card(page, container_name, out_path)


def screenshot_fullpage(page: Page, host: str, out_path: Path,
                        nav_root: str = "") -> bool:
    """Capture a full-page screenshot of the UI."""
    _load_page(page, host)
    if nav_root:
        _click_nav(page, nav_root)
    page.screenshot(path=str(out_path), full_page=False)
    return True


def screenshot_url(page: Page, url: str, wait_selector: str,
                   out_path: Path) -> bool:
    """Load an arbitrary URL and screenshot it."""
    for attempt in range(4):
        try:
            page.goto(url, wait_until="networkidle", timeout=15000)
            break
        except Exception:
            if attempt == 3:
                return False
            page.wait_for_timeout(1000 * (attempt + 1))
    if wait_selector:
        try:
            page.wait_for_selector(wait_selector, timeout=5000)
        except Exception:
            pass
    page.wait_for_timeout(600)
    page.screenshot(path=str(out_path), full_page=False)
    return True



# ---------------------------------------------------------------------------
# GIF capture
# ---------------------------------------------------------------------------

def capture_preview_gif(page: Page, host: str, module_id: str,
                        nav_root: str, out_path: Path) -> bool:
    """Capture a GIF of the WebGL preview canvas while module_id is active.

    Reloads the page, navigates to the module, then grabs canvas frames via
    page.screenshot(clip=...) at GIF_FPS for GIF_DURATION_S seconds.
    toDataURL() returns black for WebGL in headless mode; page.screenshot()
    uses the compositor and captures the rendered output correctly.
    """
    if shutil.which("ffmpeg") is None:
        print("(ffmpeg not found, skipping GIF)", end=" ", flush=True)
        return True  # not a hard failure

    _load_page(page, host)
    _click_nav(page, nav_root)
    # The CHILD tab too, as the screenshot path does: the nav root alone leaves whichever child
    # was last open selected, so the wait below can find a card the preview is not rendering.
    _click_child_tab(page, module_id)

    # Wait for the card to be visible (confirms module is rendering).
    card_sel = f'.card[data-module="{module_id}"]'
    try:
        page.wait_for_selector(card_sel, timeout=5000)
    except Exception:
        return False

    # Locate the preview canvas bounding box.
    canvas = page.query_selector("#preview")
    if not canvas:
        return False
    canvas_box = canvas.bounding_box()
    if not canvas_box:
        return False

    # Wait for the effect to warm up before recording.
    page.wait_for_timeout(800)

    n_frames = GIF_DURATION_S * GIF_FPS
    interval_ms = 1000 // GIF_FPS

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        for i in range(n_frames):
            frame_path = tmp_path / f"frame{i:04d}.png"
            page.screenshot(path=str(frame_path), clip=canvas_box)
            page.wait_for_timeout(interval_ms)

        frames = sorted(tmp_path.glob("frame*.png"))
        if not frames:
            return False

        result = subprocess.run(
            [
                "ffmpeg", "-y",
                "-framerate", str(GIF_FPS),
                "-i", str(tmp_path / "frame%04d.png"),
                "-vf", f"fps={GIF_OUTPUT_FPS},scale=320:-1:flags=lanczos,"
                       "split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse",
                str(out_path),
            ],
            capture_output=True,
        )
        return result.returncode == 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="localhost:8080",
                        help="projectMM server host:port (default: localhost:8080)")
    parser.add_argument("--force", action="store_true",
                        help="Re-capture even if screenshot already exists")
    parser.add_argument("--gif", action="store_true",
                        help="Also capture animated GIF previews for effects/modifiers")
    parser.add_argument("--filter", default="",
                        help="Only capture modules whose type name contains this substring (case-insensitive)")
    parser.add_argument("--no-modifier", action="store_true",
                        help="Remove the default MultiplyModifier from the Layer so effects "
                             "render RAW (not folded/mirrored) in their previews.")
    parser.add_argument("--grid", type=int, default=0, metavar="N",
                        help="Resize the GridLayout to NxN before capturing (e.g. 128) for "
                             "higher-resolution previews. Default: leave the boot grid (16).")
    parser.add_argument("--all-registered", action="store_true",
                        help="Also capture every registered effect/modifier that has no MODULES "
                             "entry, on a Layer with default controls. Fixes the list going stale "
                             "the moment a module is added.")
    parser.add_argument("--extras-only", action="store_true",
                        help="Skip projectMM module captures; only run the extra shots "
                             "(MoonDeck tabs, installer). Useful for recapturing the "
                             "MoonDeck UI without needing a built+running projectMM.")
    args = parser.parse_args()

    if not args.extras_only:
        try:
            _get(f"http://{args.host}/api/state", timeout=3)
        except Exception as e:
            print(f"Cannot reach projectMM at {args.host}: {e}")
            print("Start the server first: uv run moondeck/moondeck.py  (then build+run from Desktop tab)")
            return 1

    # Module-related state — only meaningful when projectMM is reachable.
    # In --extras-only mode we skip the discovery/orphan-sweep so the script
    # can run with only MoonDeck up (recapturing MoonDeck tab screenshots
    # shouldn't require a built+running projectMM).
    parents: dict[str, str] = {}
    nav_roots: dict[str, str] = {}
    container_names: dict[str, str] = {}
    core_names: dict[str, str] = {}
    if not args.extras_only:
        server_types = get_types(args.host)
        if server_types:
            print(f"Server reports {len(server_types)} module types.")
        # Catch a stale / wrong server binary before capturing anything against it.
        check_server_freshness(args.host)

        # Catch the MODULES list drifting from registered types — a registered
        # effect/modifier with no list entry is silently never captured (how
        # GameOfLife/MultiplyModifier/CheckerboardModifier went imageless).
        listed = {t for t, *_ in MODULES}
        # Filtered against what the RUNNING server offers, not just what main.cpp registers: a type
        # the server does not have (a capability-gated driver, a different firmware) would be handed
        # to add_module and fail. That is what the three add_module failures in the first full sweep
        # were.
        offered = set(server_types) if server_types else None
        uncaptured = sorted(
            t for t in source_registered_types()
            if ("Effect" in t or "Modifier" in t or "Layout" in t) and t not in listed
            # A container is not a capture subject: "Layouts" and "Effects" hold the modules
            # rather than being one, and each has its own card shot by the CONTAINERS loop.
            and t not in CONTAINERS
            and (offered is None or t in offered))
        if uncaptured and args.all_registered:
            # Capture them anyway: an effect and a modifier go on the Layer, a layout goes under
            # Layouts, and all three want a GIF. The hand-kept list stays for the ones that need
            # special props; everything else needs no entry at all.
            MODULES.extend((t, "Layouts" if "Layout" in t else "Layer", {}, True)
                           for t in uncaptured)
            print(f"  + {len(uncaptured)} registered effect/modifier/layout(s) added from the "
                  f"types registered in src/main.cpp (--all-registered)")
        elif uncaptured:
            print(f"  ⚠️  {len(uncaptured)} registered effect/modifier/layout(s) are NOT in this "
                  f"script's MODULES list, so they get no screenshot: {', '.join(uncaptured)}")
            print("      Run with --all-registered to capture them, or add an entry to MODULES "
                  "if one needs special props.")

        # Optional pipeline tweaks for nicer effect previews (raw, higher-res).
        if args.no_modifier or args.grid:
            prepare_pipeline(args.host, args.no_modifier, args.grid or None)

        # Known name prefixes for orphan sweep (first 16 chars of each type name).
        known_prefixes = {t[:16] for t, _, _, _ in MODULES}

        # Sweep orphans from a previous aborted run. Per-delete try/except
        # so one failed cleanup doesn't abort the whole recursion; outer
        # try/except so a state-fetch failure is reported, not silently
        # swallowed (silent swallow was hiding real network/HTTP issues).
        try:
            sr = _get(f"http://{args.host}/api/state", timeout=5)
            if sr.ok:
                def _sweep_orphans(modules: list) -> None:
                    for m in modules:
                        n = m.get("name", "")
                        if any(n.startswith(p) for p in known_prefixes):
                            try:
                                # delete_module returns False on HTTP failure
                                # (4xx/5xx) — log that the same way as a raised
                                # exception so the orphan stays visible.
                                if not delete_module(args.host, n):
                                    print(f"  orphan-sweep: delete {n!r} on {args.host} returned HTTP failure")
                            except Exception as e:
                                print(f"  orphan-sweep: delete {n!r} on {args.host} failed: {e}")
                        _sweep_orphans(m.get("children", []))
                _sweep_orphans(sr.json().get("modules", []))
        except Exception as e:
            print(f"  orphan-sweep on {args.host} failed: {e}")

        print("Discovering pipeline containers …")
        parents, nav_roots = find_parent_ids(args.host)
        missing = [r for r in ("Layer", "Drivers", "Layouts") if r not in parents]
        if missing:
            print(f"Pipeline containers not found: {missing}")
            print("Build and run projectMM first (Desktop tab → Build → Run).")
            return 1
        print(f"  Layer={parents['Layer']!r} (nav={nav_roots['Layer']!r})")
        print(f"  Drivers={parents['Drivers']!r} (nav={nav_roots['Drivers']!r})")
        print(f"  Layouts={parents['Layouts']!r} (nav={nav_roots['Layouts']!r})")

        container_names = find_container_nav_names(args.host)
        core_names = find_core_module_names(args.host)

    for _d in (UI_DIR, ASSETS/'core', ASSETS/'light', ASSETS/'light'/'effects',
              ASSETS/'light'/'modifiers', ASSETS/'light'/'layouts', ASSETS/'light'/'drivers'):
        _d.mkdir(parents=True, exist_ok=True)

    captured, gif_captured, skipped, failed = [], [], [], []

    filt = args.filter.lower()

    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page(viewport={"width": 1280, "height": 900})

        added_ids: list[str] = []
        # The layout the capture pushed aside, restored once the orphan sweep below has run:
        # the sweep deletes by TYPE PREFIX, and a restored GridLayout matches one, so putting
        # it back any earlier only has it removed again as if it were an orphan.
        displaced_layout: dict | None = None

        try:
            # --- Full-page UI overview screenshot --- (needs projectMM)
            if not args.extras_only:
                overview_path = UI_DIR / "ui_overview.png"
                filter_allows = (not filt or filt in "ui_overview")
                if filter_allows:
                    if not overview_path.exists() or args.force:
                        print("  ui_overview …", end=" ", flush=True)
                        # Use Effects nav to show a populated view
                        nav = nav_roots.get("Layer", "")
                        ok = screenshot_fullpage(page, args.host, overview_path, nav_root=nav)
                        print(f"saved → {overview_path.relative_to(ROOT)}" if ok else "failed")
                        if ok:
                            captured.append("ui_overview")
                        else:
                            failed.append(("ui_overview", "screenshot failed"))
                    else:
                        # File exists, no --force, filter allows → genuinely
                        # skipped because already captured.
                        print("  skip ui_overview (already captured)")
                        skipped.append(("ui_overview", "already exists"))
                # When the filter excludes ui_overview, print nothing and
                # don't pollute the skipped count — same shape as the
                # filtered loop below this block.

            # --- Extra shots: MoonDeck tabs + installer ---
            for filename, url, wait_sel, _doc_files, _anchor in EXTRA_SHOTS:
                if filt and filt not in filename.lower():
                    continue
                out_path = UI_DIR / filename
                if out_path.exists() and not args.force:
                    print(f"  skip {filename} (already captured)")
                    skipped.append((filename, "already exists"))
                    continue
                print(f"  {filename} …", end=" ", flush=True)
                ok = screenshot_url(page, url, wait_sel, out_path)
                if ok:
                    print(f"saved → {out_path.relative_to(ROOT)}")
                    captured.append(filename)
                else:
                    print("failed (is the server running?)")
                    failed.append((filename, "screenshot failed"))

            # Module-card captures all require projectMM. In --extras-only
            # mode we're done after the EXTRA_SHOTS loop above.
            if args.extras_only:
                raise _ExtrasOnlyDone()

            # --- Container cards (Layouts, Effects, Drivers) ---
            for container_type in CONTAINERS:
                if filt and filt not in container_type.lower():
                    continue
                cname = container_names.get(container_type, "")
                if not cname:
                    continue
                out_path = asset_dir_for(container_type) / f"{container_type}.png"
                if out_path.exists() and not args.force:
                    print(f"  skip {container_type} (already captured)")
                    skipped.append((container_type, "already exists"))
                    continue
                print(f"  {container_type} …", end=" ", flush=True)
                ok = screenshot_container(page, args.host, cname, out_path)
                print(f"saved → {out_path.relative_to(ROOT)}" if ok else "failed")
                if ok:
                    captured.append(container_type)
                else:
                    failed.append((container_type, "screenshot failed"))

            # --- Core module cards (always present, never added/deleted) ---
            for type_name in CORE_MODULES:
                if filt and filt not in type_name.lower():
                    continue
                cname = core_names.get(type_name, "")
                if not cname:
                    print(f"  skip {type_name} (not in state — ESP32-only?)")
                    skipped.append((type_name, "not in state"))
                    continue
                out_path = asset_dir_for(type_name) / f"{type_name}.png"
                if out_path.exists() and not args.force:
                    print(f"  skip {type_name} (already captured)")
                    skipped.append((type_name, "already exists"))
                    continue
                print(f"  {type_name} …", end=" ", flush=True)
                # A top-level module is its own nav root (screenshot_container navs + cards by the
                # same name). A CHILD module (Mqtt/Devices/Improv under Network) has no nav entry of
                # its own — nav to its parent root, then screenshot the child's own card by name.
                parent_type = CORE_NAV_ROOT.get(type_name)
                if parent_type:
                    nav_name = core_names.get(parent_type, "")   # live name of the nav root (e.g. "Network")
                    ok = screenshot_module(page, args.host, cname, nav_name, out_path)
                else:
                    ok = screenshot_container(page, args.host, cname, out_path)
                print(f"saved → {out_path.relative_to(ROOT)}" if ok else "failed")
                if ok:
                    captured.append(type_name)
                else:
                    failed.append((type_name, "screenshot failed"))

            # --- Individual module cards ---
            for type_name, parent_type, extra_props, want_gif in MODULES:
                if filt and filt not in type_name.lower():
                    continue
                out_path = asset_dir_for(type_name) / f"{type_name}.png"
                gif_path = asset_dir_for(type_name) / f"{type_name}.gif"
                need_png = not out_path.exists() or args.force
                need_gif = want_gif and args.gif and (not gif_path.exists() or args.force)

                if not need_png and not need_gif:
                    print(f"  skip {type_name} (already captured)")
                    skipped.append((type_name, "already exists"))
                    continue

                parent_id = parents.get(parent_type)
                nav_root = nav_roots.get(parent_type, "")
                req_id = type_name[:16]
                print(f"  {type_name} …", end=" ", flush=True)

                # The bed effect, removed again below, so the pipeline holds exactly one effect
                # and one subject while the camera is open. A modifier reshapes what an effect
                # draws and a layout says where its lights sit, so neither has anything to show
                # on its own: both photograph as a dark panel without a lit effect under them.
                bed_name = None
                if need_gif and (type_name.endswith("Modifier") or type_name.endswith("Layout")):
                    bed_name = add_module(args.host, BED_EFFECT[:16], BED_EFFECT,
                                          parents.get("Layer"), {})
                    if bed_name:
                        added_ids.append(bed_name)
                        time.sleep(0.5)
                    else:
                        print("bed-failed ", end="", flush=True)

                # A second layout would fight the boot GridLayout for the same lights, so the
                # default steps aside for the duration and is restored once the shot is taken.
                if type_name.endswith("Layout") and displaced_layout is None:
                    displaced_layout = displace_default_layout(args.host)

                actual_name = add_module(args.host, req_id, type_name,
                                         parent_id, extra_props)
                if not actual_name:
                    print("add failed")
                    failed.append((type_name, "add_module failed"))
                    if bed_name:
                        delete_module(args.host, bed_name)
                        added_ids.remove(bed_name)
                    continue

                added_ids.append(actual_name)

                # Through /api/control, the path the UI uses: the create endpoint ignores a
                # `props` body, so a module needing a control set (a scripted one needs its
                # script, or it captures an empty card reading "no script") gets it here. The
                # settle is the compile, which a scripted module does on the control change.
                pushed = True
                if extra_props:
                    for ctrl, value in extra_props.items():
                        # A script control names a file that has to BE there: push it first, or
                        # the control is set to a name the device cannot resolve and the capture
                        # records an empty card. A failed push abandons this module rather than
                        # photographing that, so the asset on disk is never a lie.
                        if ctrl == "script" and isinstance(value, str) \
                                and not push_script(args.host, value):
                            print(f"push-{value}-failed")
                            failed.append((type_name, f"script upload failed: {value}"))
                            delete_module(args.host, actual_name)
                            added_ids.remove(actual_name)
                            if bed_name:
                                delete_module(args.host, bed_name)
                                added_ids.remove(bed_name)
                            pushed = False
                            break
                        if not set_control(args.host, actual_name, ctrl, value):
                            print(f"control-{ctrl}-failed ", end="", flush=True)
                    if not pushed:
                        time.sleep(0.15)
                        continue
                    time.sleep(1.0)

                if need_png:
                    ok = screenshot_module(page, args.host, actual_name,
                                           nav_root, out_path)
                    if ok:
                        print("png ", end="", flush=True)
                        captured.append(type_name)
                    else:
                        print("png-failed ", end="", flush=True)
                        failed.append((type_name, "screenshot failed"))

                if need_gif:
                    ok = capture_preview_gif(page, args.host, actual_name,
                                             nav_root, gif_path)
                    if ok:
                        print("gif ", end="", flush=True)
                        gif_captured.append(type_name)
                    else:
                        print("gif-failed ", end="", flush=True)
                        failed.append((type_name, "gif failed"))

                print(f"→ {asset_dir_for(type_name).relative_to(ROOT)}/")
                delete_module(args.host, actual_name)
                added_ids.remove(actual_name)
                if bed_name:
                    delete_module(args.host, bed_name)
                    added_ids.remove(bed_name)
                time.sleep(0.5)

        except _ExtrasOnlyDone:
            # --extras-only: bail out cleanly after the EXTRA_SHOTS loop.
            pass

        finally:
            if not args.extras_only:
                for mid in added_ids:
                    try:
                        delete_module(args.host, mid)
                    except Exception as e:
                        print(f"  cleanup: delete {mid!r} on {args.host} failed: {e}")
                # Final sweep — same per-delete + outer try/except pattern as the
                # opening sweep so cleanup errors surface instead of being hidden.
                try:
                    sr = _get(f"http://{args.host}/api/state", timeout=5)
                    if sr.ok:
                        def _sweep(modules: list) -> None:
                            for m in modules:
                                n = m.get("name", "")
                                if any(n.startswith(p) for p in known_prefixes):
                                    try:
                                        # Same False-return check as the
                                        # opening sweep — HTTP failure is
                                        # silent without it.
                                        if not delete_module(args.host, n):
                                            print(f"  final-sweep: delete {n!r} on {args.host} returned HTTP failure")
                                    except Exception as e:
                                        print(f"  final-sweep: delete {n!r} on {args.host} failed: {e}")
                                _sweep(m.get("children", []))
                        _sweep(sr.json().get("modules", []))
                except Exception as e:
                    print(f"  final-sweep on {args.host} failed: {e}")
                # Last, so the sweep above cannot take it straight back out.
                if displaced_layout:
                    restore_default_layout(args.host, displaced_layout)
                    print(f"  pipeline: restored {displaced_layout['type']}")
            browser.close()

    print(f"\n{'─'*50}")
    print(f"Captured : {len(captured)} PNGs, {len(gif_captured)} GIFs")
    print(f"Skipped  : {len(skipped)}")
    print(f"Failed   : {len(failed)}")
    # GIFs are PNG-only by default — remind when an effect/modifier capture ran
    # without --gif, since "0 GIFs" on an effect run usually means the flag was
    # forgotten, not that nothing animates.
    if not args.gif and not args.extras_only and any(
            (not filt or filt in t.lower()) and want_gif
            for t, _, _, want_gif in MODULES):
        print("\nNote: GIFs were NOT captured — re-run with --gif to also record "
              "animated previews for effects/modifiers.")
    if failed:
        print("\nFailed:")
        for name, reason in failed:
            print(f"  {name}: {reason}")
    if captured:
        print("\nNext steps:")
        print("  Add module screenshots: uv run moondeck/docs/update_module_docs.py")
        if "ui_overview" in captured:
            print("  Add UI overview to docs/explanation/architecture/index.md # Web UI section:")
            print("  ![UI overview](assets/ui/ui_overview.png)")

    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())

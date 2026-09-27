#!/usr/bin/env python3
"""Locally preview the web installer at mooninstaller/index.html.

Stages a small directory (the install page + the shared install-picker
module) and serves it with `python -m http.server`, plus the
`Access-Control-Allow-Origin: *` header that GitHub Pages serves on its
static assets by default — needed so the device UI's
`consumePendingBoardParam` can cross-origin-fetch a localhost-served
deviceModels.json the same way it would fetch the production one. Two modes
depending on what's been built locally:

  - **render-only** (no `build/esp32-*/projectMM.bin` present): the
    picker populates against the real GitHub Releases API, dropdowns
    work, but clicking Install fails because the local server has no
    `releases/` tree. Equivalent to "Recipe A" in mooninstaller/README.md.
    Useful for iterating on HTML/CSS/JS without tagging a release.

  - **flash-ready** (at least one local ESP32 build exists): the
    script additionally stages every `build/esp32-*/projectMM.bin` it
    finds into `releases/local-dev/` and generates matching Pages-
    relative manifests via generate_manifest.py — the same code path
    .github/workflows/release.yml uses. The picker shows `local-dev`
    as the newest release; clicking Install flashes the chip + opens
    the ESP Web Tools Improv WiFi modal, end-to-end.

The flash-ready mode is the developer's test ground for the install
flow before deploying to GitHub Pages: any change to the installer
page or install-picker.js can be verified against a real ESP32 over
`http://localhost:8421/` (Web Serial works on localhost without the
secure-origin requirement that gates the public site).

Long-running. MoonDeck shows a Stop button while this is up; pressing
Stop kills the python process (matched by script name via pkill).
"""

import http.server
import json
import socketserver
import subprocess
import sys
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# build_esp32.py is the single source of truth for firmware variants (same import
# generate_manifest.py and collect_kpi.py use).
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_esp32 import FIRMWARES  # noqa: E402
INSTALL_DIR = ROOT / "mooninstaller"
ASSETS_BOARDS_DIR = ROOT / "docs" / "assets" / "boards"
PICKER_JS = ROOT / "src" / "ui" / "install-picker.js"
# Board-catalog / chip-detection half of the picker — mooninstaller only (not
# embedded in firmware), imported by index.html. Staged alongside PICKER_JS.
PICKER_BOARDS_JS = ROOT / "src" / "ui" / "install-picker-boards.js"
STAGE_DIR = ROOT / "build" / "install-preview"
# The preview mirrors the GitHub Pages layout: the installer at /install/,
# board images under /install/assets/deviceModels/, and the releases tree under
# /install/releases/ — so an "image": "assets/deviceModels/<slug>.jpg" path and the
# "./releases/<tag>/" firmware path resolve identically here and on Pages, no
# per-context special casing. A root redirect (/ → /install/) keeps the old
# entry point working.
STAGE_INSTALL = STAGE_DIR / "install"
BUILD_ROOT = ROOT / "build"
GENERATE_MANIFEST = ROOT / "moondeck" / "build" / "generate_manifest.py"
# Stage under the tag the picker WILL surface from the live GitHub Releases
# API: `latest` is always the newest CI build. Local bins replace the real
# `latest` bins for the duration of the preview — that's the point, we want
# to flash what we've built locally, not what's on GitHub. The picker shows
# `latest` (and any other real tags) regardless; clicking Install on `latest`
# resolves the manifest path to our staged copy, served same-origin from the
# preview server. Other tags (`v1.0.0-rc2` etc.) the picker offers will 404
# in preview because we don't stage them — clearly visible failure if the
# user picks the wrong one. (Picking a `v*` tag in preview is a developer
# mistake; the picker defaults to the newest stable, so if no stable
# release exists yet the default lands on `latest`, which works.)
LOCAL_TAG = "latest"
LOCAL_VERSION = "local-dev"
PORT = 8421


def _stage_runtime_files(src_dir: Path, dst_dir: Path):
    """Copy every browser-loadable file (.html/.js/.css/.json/.png/.ico/.svg) from
    src_dir to dst_dir — mirrors release.yml's `cp -r mooninstaller/. pages/install/`.
    README.md / other .md are docs, skipped. The deploy's `cp -r` is recursive, so a
    subdirectory of static assets (mooninstaller/assets/, the app-store badges) is
    staged too — walk the tree rather than just the top level, or those 404 in preview
    while working in production."""
    exts = (".html", ".js", ".css", ".json", ".png", ".ico", ".svg")
    for src in src_dir.rglob("*"):
        if src.is_file() and src.suffix.lower() in exts:
            out = dst_dir / src.relative_to(src_dir)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, out)


def _stage_referenced_board_images(dst_dir: Path):
    """Stage only the board images a deviceModels.json entry references (mirrors the
    deploy's filtered copy), under <dst_dir>/assets/deviceModels/."""
    boards_json = INSTALL_DIR / "deviceModels.json"
    if not boards_json.exists():
        return
    try:
        boards = json.loads(boards_json.read_text())
    except (ValueError, OSError):
        return
    for b in boards:
        rel = b.get("image")            # e.g. "assets/deviceModels/<slug>.jpg"
        if not rel:
            continue
        src = ROOT / "docs" / rel       # source lives in docs/assets/deviceModels/
        if src.is_file():
            out = dst_dir / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, out)


def stage_install_page():
    """Refresh the preview dir, mirroring the GitHub Pages layout.

    Re-staging on every run keeps the local copy honest — if you edit
    index.html or any sibling JS while the server is up, restart via
    the Stop/Run cycle to pick up changes (or rely on the `?nocache=1`
    query parameter the picker honours).

    Layout matches Pages exactly: the installer under /install/, board images
    under /install/assets/deviceModels/, the releases tree under /install/releases/.
    A root index.html redirects / → /install/ so the historical
    `localhost:8421/` entry point still lands on the installer. The shared
    install-picker.js (under src/ui/, shared with the on-device UI) is copied in.
    """
    if STAGE_DIR.exists():
        shutil.rmtree(STAGE_DIR)
    STAGE_DIR.mkdir(parents=True)

    if not INSTALL_DIR.exists():
        print(f"ERROR: install source not found at {INSTALL_DIR}", file=sys.stderr)
        sys.exit(1)
    if not PICKER_JS.exists():
        print(f"ERROR: install-picker.js not found at {PICKER_JS}", file=sys.stderr)
        sys.exit(1)
    if not PICKER_BOARDS_JS.exists():
        print(f"ERROR: install-picker-boards.js not found at {PICKER_BOARDS_JS}", file=sys.stderr)
        sys.exit(1)

    library_json = ROOT / "library.json"

    # --- installer → /install/ ---
    _stage_runtime_files(INSTALL_DIR, STAGE_INSTALL)
    shutil.copy(PICKER_JS, STAGE_INSTALL / "install-picker.js")
    shutil.copy(PICKER_BOARDS_JS, STAGE_INSTALL / "install-picker-boards.js")
    if library_json.exists():
        shutil.copy(library_json, STAGE_INSTALL / "library.json")
    _stage_referenced_board_images(STAGE_INSTALL)

    # --- root redirect / → /install/ (keeps the old entry point working) ---
    (STAGE_DIR / "index.html").write_text(
        '<!doctype html><meta http-equiv="refresh" content="0; url=./install/">'
        '<a href="./install/">MoonLight installer →</a>\n'
    )


def find_local_builds() -> list[Path]:
    """Return every `build/esp32-*/` dir that has a flashable projectMM.bin.

    Multiple firmware variants can coexist on disk (each in its own
    `build/esp32-<firmware>/` dir per build_esp32.py). We stage them all
    so the install-picker can offer the right one based on the device's
    reported firmware key during Improv probe — same shape as production
    where every release publishes every variant.
    """
    if not BUILD_ROOT.exists():
        return []
    return sorted(
        p for p in BUILD_ROOT.glob("esp32-*")
        if (p / "projectMM.bin").exists()
    )


def stage_local_builds(builds: list[Path]) -> list[str]:
    """Copy bins + generate Pages-relative manifests for each local build.

    Mirrors .github/workflows/release.yml's "Stage release artifacts" +
    "Generate ESP Web Tools manifests" steps, just into the preview
    staging dir and using a fixed `local-dev` tag instead of a git tag.
    Releases live under /install/releases/ (matching Pages). Returns the staged
    firmware keys.
    """
    releases_dir = STAGE_INSTALL / "releases" / LOCAL_TAG
    releases_dir.mkdir(parents=True, exist_ok=True)

    staged: list[str] = []
    for build_dir in builds:
        # build_dir.name is "esp32-<firmware>"; strip the prefix.
        firmware = build_dir.name[len("esp32-"):]
        prefix = f"firmware-{firmware}-v{LOCAL_VERSION}"

        # The four .bin files that go alongside a firmware in the release.
        # Mirrors release.yml's "Stage release artifacts" step: app + bootloader
        # are per-firmware; ota-data + partition-table are SHARED (ota-data once
        # globally, partition-table once per flash-size group), matching the names
        # generate_manifest.py emits so the preview's manifests resolve them.
        try:
            flasher = json.loads((build_dir / "flasher_args.json").read_text())
            size = str(flasher.get("flash_settings", {}).get("flash_size", "")).lower()
            shutil.copy(build_dir / "projectMM.bin",
                        releases_dir / f"{prefix}.bin")
            shutil.copy(build_dir / "bootloader" / "bootloader.bin",
                        releases_dir / f"{prefix}-bootloader.bin")
            shutil.copy(build_dir / "partition_table" / "partition-table.bin",
                        releases_dir / f"partition-table-{size}.bin")
            shutil.copy(build_dir / "ota_data_initial.bin",
                        releases_dir / "shared-ota-data.bin")
            # MoonBase firmwares also ship the shared maintenance image + the slot-0 otadata
            # their manifests reference (same names release.yml stages).
            if FIRMWARES.get(firmware, {}).get("moonbase"):
                from build_esp32 import otadata_slot0_bytes
                chip = FIRMWARES[firmware]["chip"]
                shutil.copy(build_dir.parent / f"moonbase-{chip}" / "projectMM-moonbase.bin",
                            releases_dir / f"shared-moonbase-{chip}.bin")
                (releases_dir / "shared-ota-data-slot0.bin").write_bytes(otadata_slot0_bytes())
        except FileNotFoundError as e:
            # Partial build (bootloader / partition-table missing) — skip this
            # firmware rather than half-stage it, the picker would offer it
            # but Install would fail mid-flash.
            print(f"==> skip {firmware}: missing {e.filename}", file=sys.stderr)
            continue

        # Pages-relative manifest (release-url = "." → siblings of the manifest).
        # Generate via the same script production uses so the manifest shape
        # stays in sync with what ESP Web Tools expects.
        manifest_path = releases_dir / f"manifest-{firmware}.json"
        flasher_args = build_dir / "flasher_args.json"
        if not flasher_args.exists():
            print(f"==> skip {firmware}: missing flasher_args.json", file=sys.stderr)
            continue
        # Run through `uv run` (project standard — see CLAUDE.md "Use uv for
        # every Python invocation"), not the raw interpreter, so the managed
        # venv resolves the manifest generator's deps.
        result = subprocess.run(
            ["uv", "run", "python", str(GENERATE_MANIFEST),
             "--firmware", firmware,
             "--version", LOCAL_VERSION,
             "--release-url", ".",
             "--flasher-args", str(flasher_args),
             "--out", str(manifest_path)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            print(f"==> skip {firmware}: manifest generation failed:\n{result.stderr}",
                  file=sys.stderr)
            continue
        staged.append(firmware)

    return staged


def write_local_firmwares_json(staged: list[str]):
    """Write local-firmwares.json so the picker offers locally-staged firmwares.

    The picker's firmware dropdown is normally driven by the published release's
    GitHub assets, so a brand-new variant (built locally, not yet in any release)
    wouldn't appear even though its bins are staged. install.js fetches this file
    and hands the picker an `extraFirmwaresByTag` override; the URLs are the same
    `./releases/<tag>/...` paths the staged bins already live at, so Install
    resolves them same-origin. Shape: { <tag>: [{firmware, manifestUrl, binaryUrl}] }.
    """
    by_tag = {LOCAL_TAG: [
        {
            "firmware": fw,
            "manifestUrl": f"./releases/{LOCAL_TAG}/manifest-{fw}.json",
            "binaryUrl": f"./releases/{LOCAL_TAG}/firmware-{fw}-v{LOCAL_VERSION}.bin",
        }
        for fw in staged
    ]}
    (STAGE_INSTALL / "local-firmwares.json").write_text(
        json.dumps(by_tag, indent=2) + "\n", encoding="utf-8")


def _port_in_use(port: int) -> bool:
    """True if something is already listening on `port` on this host. Used
    as a pre-flight before the staging step nukes STAGE_DIR — re-running
    while an old preview is up would yank the running server's cwd and
    leave it serving from a deleted directory (requests then hang)."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0
    finally:
        s.close()


def main() -> int:
    if _port_in_use(PORT):
        print(f"ERROR: port {PORT} is in use. Another preview already running? "
              f"Stop it first (MoonDeck → Stop, or `pkill -f preview_installer.py`).",
              file=sys.stderr)
        return 1

    stage_install_page()

    print(f"==> staged {STAGE_DIR}")

    # Look for local ESP32 builds and stage them under LOCAL_TAG (= "latest"
    # — overlays the real `latest` tag the picker surfaces from the GitHub
    # API; clicking Install on `latest` then resolves to our local bins via
    # toLocalUrl). When no builds exist, the preview runs in render-only
    # mode (picker works against the real GitHub API, Install fails for
    # lack of bins).
    local_builds = find_local_builds()
    if local_builds:
        firmwares = stage_local_builds(local_builds)
        if firmwares:
            write_local_firmwares_json(firmwares)
            print(f"==> staged `{LOCAL_TAG}` release with firmwares: {', '.join(firmwares)}")
            print(f"    pick the `{LOCAL_TAG}` tag in the picker to flash a USB-connected ESP32")
        else:
            print("==> no firmwares could be staged (all skipped — see warnings above)")
            print("    falling back to render-only mode")
    else:
        print(f"==> no local ESP32 builds found under {BUILD_ROOT.relative_to(ROOT)}")
        print("    render-only mode — `Install` will fail")
        print("    run `uv run moondeck/build/build_esp32.py --firmware <variant>` first to enable end-to-end flash")

    print(f"==> serving at http://localhost:{PORT}/")
    print(f"    installer: http://localhost:{PORT}/install/")
    print("    open in Chrome / Edge / Opera (Web Serial requires one of these)")
    print("    add ?nocache=1 to bypass the picker's 5-min sessionStorage cache")
    print()
    sys.stdout.flush()

    # Mirror GitHub Pages' default static-asset CORS behavior
    # (`Access-Control-Allow-Origin: *`). The device-UI's
    # `consumePendingBoardParam` fetches deviceModels.json cross-origin
    # (http://<device>/ → https://moonmodules.org/…). In dev we point that
    # constant at this preview server so we can iterate on the schema; the
    # device's browser will refuse the fetch unless we serve the same CORS
    # header production does. Doesn't affect the production firmware — only
    # the developer's local-preview flow.
    class CorsHandler(http.server.SimpleHTTPRequestHandler):
        def end_headers(self):
            self.send_header("Access-Control-Allow-Origin", "*")
            # no-store so edits to deviceModels.json / index.html / install-picker.js
            # show on a normal reload. Python's http.server sends only
            # Last-Modified (no Cache-Control), so browsers heuristically cache
            # and a stale deviceModels.json survives a restage until a hard reload.
            # Dev-preview only; production Pages sets its own cache headers.
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    handler = CorsHandler

    class QuietTCPServer(socketserver.ThreadingTCPServer):
        allow_reuse_address = True   # avoids "Address already in use" on quick restart
        daemon_threads = True

    # cwd=STAGE_DIR equivalent: serve files from STAGE_DIR by chdir'ing first.
    # SimpleHTTPRequestHandler serves from the current working directory.
    import os
    os.chdir(STAGE_DIR)

    try:
        with QuietTCPServer(("", PORT), handler) as httpd:
            httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n==> stopped")
    except OSError as e:
        if e.errno == 48 or "Address already in use" in str(e):
            print(f"ERROR: port {PORT} is in use. Another preview already running?",
                  file=sys.stderr)
            return 1
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""MoonDeck — browser-based developer console for MoonLight."""

import http.server
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import suppress
from datetime import datetime
from pathlib import Path

PORT = 8420
ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = Path(__file__).resolve().parent

# Last run's output per script, so a card can answer "what did this do last time" after the
# stream is gone — a page reload, a different tab, or simply coming back later. Under build/
# because it is derived state, not repo state (build/ is gitignored). One file per script id,
# overwritten each run: this is a "last run" record, not a history.
LOG_DIR = ROOT / "build" / "moondeck-logs"
# Cap per log. An ESP32 build or a docs screenshot run can print megabytes, and this is a
# convenience record, not an archive. The TAIL is what matters (the result, the exit code),
# so once the cap is hit the writer stops and says so — truncating the end would throw away
# the part you came for.
LOG_MAX_BYTES = 1_000_000
# How many times each script has been run, so a card's footer can say "run #7" — the question
# "have I actually re-run this since the fix, or am I reading a stale number?" is otherwise
# unanswerable from a report that looks identical either way. Alongside the logs and equally
# derived: deleting build/ resets the counts, which is the right blast radius for a convenience
# record. One small JSON for every script rather than a file each — it is read and rewritten
# once per run, and a dict of ints stays tiny.
RUNS_FILE = LOG_DIR / "run-counts.json"
UI_DIR = SCRIPTS_DIR / "moondeck_ui"
ASSETS_DIR = ROOT / "docs" / "assets"
STATE_FILE = SCRIPTS_DIR / "moondeck.json"

# Shared test-metadata parsers live next to the doc generator. Both this server
# and moondeck/docs/generate_test_docs.py import from there so the two views of
# the same source files (HTML in MoonDeck, markdown in docs/reference/tests/) can't drift.
sys.path.insert(0, str(SCRIPTS_DIR / "docs"))
import _test_metadata as test_meta  # noqa: E402
# Re-use the doc generator's perf-table formatter so the MoonDeck step view
# and the generated scenario-tests.md show the same shape per step (single
# source of truth — adding/changing a metric updates both surfaces at once).
import generate_test_docs as test_doc_gen  # noqa: E402


def _app_version():
    """Read the project version from library.json. '?' if unavailable."""
    try:
        return json.loads((ROOT / "library.json").read_text(encoding="utf-8")).get("version", "?")
    except Exception:
        return "?"


APP_VERSION = _app_version()

# ---------------------------------------------------------------------------
# Device-model catalog (single source of truth, shared with the web installer)
# ---------------------------------------------------------------------------

DEVICE_MODELS_FILE = ROOT / "mooninstaller" / "deviceModels.json"


def _load_device_models():
    """Load mooninstaller/deviceModels.json. Returns [] on missing/malformed file —
    `_deduce_device_model` then always returns "" (no firmware uniquely identifies
    a board), MoonDeck JS shows only the empty default. The web installer
    Step 2 picker will share this file.
    """
    try:
        return json.loads(DEVICE_MODELS_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []


DEVICE_MODELS = _load_device_models()

FIRMWARES_FILE = ROOT / "mooninstaller" / "firmwares.json"


def _load_firmwares():
    """Shipping firmware-variant names from mooninstaller/firmwares.json — the
    generated projection of build_esp32's FIRMWARES dict (the single source of
    truth, shared with the CI release matrix). Returns [] on missing/malformed
    file, so the MoonDeck UI just shows no firmware entries. Filtering on
    `ships` keeps held-out variants out of the picker.
    """
    try:
        doc = json.loads(FIRMWARES_FILE.read_text(encoding="utf-8"))
        return [f["name"] for f in doc["firmwares"] if f.get("ships")]
    except (OSError, json.JSONDecodeError, KeyError):
        return []


# ---------------------------------------------------------------------------
# Script definitions (loaded from moondeck_config.json)
# ---------------------------------------------------------------------------

SCRIPTS_FILE = SCRIPTS_DIR / "moondeck_config.json"

def load_scripts():
    with open(SCRIPTS_FILE) as f:
        return json.load(f)

_scripts_data = load_scripts()
SCRIPTS = _scripts_data["scripts"]
FIRMWARES = _load_firmwares()


def _duration(seconds: float) -> str:
    """A run's wall time, read at a glance: `4.2s`, `1m12s`, `1h04m`.

    Seconds alone stop being readable somewhere around a minute — `312s` has to be divided in
    your head — and a clean rebuild here runs into the minutes.
    """
    # Decide the sub-minute branch on the value AS DISPLAYED, not on int(): 59.96 truncates to 59
    # but renders "60.0s", a reading that does not exist on this scale.
    s = int(round(seconds, 1))
    if s < 60:
        return f"{seconds:.1f}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


def bump_run_count(script_id):
    """Record one more run of `script_id` and return the new total.

    Best-effort throughout: this is a footer on a report, so a missing or corrupt counts file
    must never fail the run that produced the report. A read error starts from zero rather than
    raising, and a write error is dropped — the count is the only thing lost.
    """
    counts = {}
    with suppress(OSError, ValueError):
        loaded = json.loads(RUNS_FILE.read_text(encoding="utf-8"))
        if isinstance(loaded, dict):
            counts = loaded
    # A hand-edited or half-written file can hold anything; a bad value restarts the count for
    # that script rather than raising in the middle of reporting a successful run. `bool` is
    # excluded explicitly — it IS an int in Python, so `True` would count as run #2 — and a
    # negative restarts rather than counting up from below zero.
    prev = counts.get(script_id, 0)
    valid = type(prev) is int and prev >= 0     # noqa: E721 — bool must NOT satisfy this
    n = (prev if valid else 0) + 1
    counts[script_id] = n
    with suppress(OSError):
        RUNS_FILE.parent.mkdir(parents=True, exist_ok=True)
        RUNS_FILE.write_text(json.dumps(counts, indent=1, sort_keys=True), encoding="utf-8")
    return n

# ---------------------------------------------------------------------------
# Device discovery
# ---------------------------------------------------------------------------

def _lan_ip():
    """This machine's LAN IP. '' if it can't be determined (offline).

    connect() on a UDP socket sends no packet — it just picks the outbound
    interface, whose address is the LAN IP.
    """
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return ""
    finally:
        s.close()


def _get_local_subnet():
    """The local /24 subnet prefix (e.g. '192.168.1'). Falls back to a default."""
    ip = _lan_ip()
    return ".".join(ip.split(".")[:3]) if ip else "192.168.1"


def _walk_modules(modules):
    """Yield every module in the tree (depth-first), including nested children."""
    for m in modules or []:
        yield m
        yield from _walk_modules(m.get("children", []))


def _device_sort_key(d):
    """Sort devices by name (case-insensitive), IP as tiebreaker — the same order the on-device
    DevicesModule uses (src/core/system/DevicesModule.h sortByName / ciLess), so MoonDeck's list and the
    device's own list read the same. Used everywhere a device list is stored, so the persisted
    moondeck.json is already in display order."""
    return (d.get("deviceName", "").lower(), d.get("ip", ""))


def _device_key(d):
    """The device's identity: its MAC (from SystemModule.mac), lower-cased. The MAC is per-chip and
    the only unchangeable identifier — an IP is a DHCP lease, not an identity, so it is NOT used as a
    key. A device without a MAC (a WLED peer, or a MoonLight device on firmware predating the `mac`
    control) has NO stable identity: it's shown while online in a live scan but not persisted. Returns
    "" for such a device — callers persist/dedup only truthy keys."""
    return (d.get("mac") or "").strip().lower()


def _probe_device(ip, port=8080, timeout=0.4):
    """Probe a single IP for /api/state. Returns device info or None.

    Short timeout: on a LAN a live device answers in a few ms and a dead IP
    refuses the connection almost instantly; 0.4s only matters for IPs that
    silently drop packets (firewalled hosts), and a subnet scan should not
    stall seconds on those.

    Returns: { ip, deviceName, firmware, deviceModel }
    - `firmware` is the variant flashed (value of the `firmware` control on
      SystemModule, set from kFirmwareName in build_info.h). Used to deduce
      `deviceModel` when the device hasn't been told its model yet. See
      docs/explanation/architecture/index.md § Firmware vs board.
    - `deviceModel` is the physical-hardware identity (a catalog entry). Preferred source: the device's
      own `deviceModel` control on SystemModule (the value MoonDeck pushed earlier
      and the device persisted). Fall back to firmware-based deduction
      (catalog lookup) when the device hasn't been told yet — then MoonDeck
      pushes the deduced value on next discover, the device persists it,
      and subsequent probes read it back from the device.
    """
    import urllib.request
    import urllib.error
    url = f"http://{ip}:{port}/api/state"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
            modules = data.get("modules", [])
            device_name = ""
            firmware = ""
            device_model = ""
            mac = ""
            for m in _walk_modules(modules):
                # deviceName, deviceModel (the board) and mac live on SystemModule; the flashed
                # firmware-variant lives on FirmwareUpdateModule (`firmware`, alongside version/build).
                if m.get("type") == "SystemModule":
                    for c in m.get("controls", []):
                        if c.get("name") == "deviceName":
                            device_name = c.get("value", "") or ""
                        elif c.get("name") == "deviceModel":
                            device_model = c.get("value", "") or ""
                        elif c.get("name") == "mac":
                            mac = c.get("value", "") or ""
                elif m.get("type") == "FirmwareUpdateModule":
                    for c in m.get("controls", []):
                        if c.get("name") == "firmware":
                            firmware = c.get("value", "") or ""
            return {
                # `mac` is the stable device IDENTITY (per-chip, survives DHCP IP changes); the merge
                # keys on it. `ip` is just the current location — omit the port when it's HTTP's
                # default (80, the ESP32 devices), keep a non-default port (8080, the desktop build).
                "mac": mac,
                "ip": ip if port == 80 else f"{ip}:{port}",
                "deviceName": device_name,
                "firmware": firmware,
                # `deviceModel` is the merged value (device-reported, else the firmware-deduced
                # fallback). `_reportedModel` is what the DEVICE actually said (empty if it hasn't been
                # told) — the merge compares against this to know whether to push the model to the
                # device; using the already-deduced value would make them equal and skip the push.
                "deviceModel": device_model or _deduce_device_model(firmware),
                "_reportedModel": device_model,
            }
    except Exception:
        return None


def _deduce_device_model(firmware: str) -> str:
    """Firmware → deviceModel name when exactly one catalog entry claims this
    firmware. Returns "" when zero (unknown firmware) or multiple device models
    claim it (ambiguous — user picks). Catalog lives at
    mooninstaller/deviceModels.json; see docs/explanation/architecture/index.md § Firmware vs board.
    """
    if not firmware:
        return ""
    matches = [b["name"] for b in DEVICE_MODELS if firmware in b.get("firmwares", [])]
    return matches[0] if len(matches) == 1 else ""


def _push_device(ip: str, model: str) -> bool:
    """POST /api/control on the device for every per-board control in deviceModels.json.

    For device models that have a catalog entry in mooninstaller/deviceModels.json: fans
    out the full `controls.<Module>.<control>` block (matching the web
    installer's and the device-side `?deviceModel=` Inject path — same generic
    iteration, so adding a new field to a deviceModel entry Just Works without
    code changes here). For device models without a catalog entry (custom names,
    unknown firmware): still pushes `System.deviceModel` so the bare name lands —
    keeps the legacy single-field behaviour as the fallback.

    Returns True iff EVERY POST returned 200. False on any failure (timeout,
    non-2xx, network error) — partial state may have been applied; the next
    refresh re-attempts. Same best-effort semantics as the prior single-
    field shape.

    `ip` is the "host:port" string from the device record (already includes
    the port discovery picked). `deviceModel` is the catalog key MoonDeck wants
    the device to remember (empty string means "clear" — no push).
    """
    if not model:
        return True   # nothing to push; not a failure
    # Look up the catalog entry. DEVICE_MODELS is loaded at module init; we don't
    # re-read deviceModels.json per push so a tight discover-refresh cycle
    # doesn't hammer the disk. If the user edits deviceModels.json, restart
    # MoonDeck (same as every other catalog change).
    entry = next((b for b in DEVICE_MODELS if b.get("name") == model), None)
    if entry is not None:
        modules = entry.get("modules") or []
    else:
        # Custom / unknown deviceModel: push the bare name onto System's `deviceModel`
        # control (the identity lives on SystemModule now — no parent_id, it's the
        # boot-wired top-level module, so _apply just sets the control, no add).
        modules = [{"type": "System", "id": "System",
                    "controls": {"deviceModel": model}}]

    return _apply_modules_to_device(ip, modules)


def _apply_modules_to_device(ip: str, modules: list) -> bool:
    """Add-then-configure a list of module-with-controls units on a device.

    Each unit is `{type, id, parent_id?, controls?}` — the SAME shape deviceModels.json
    catalog entries use, so the deviceModel push (_push_device) drives it. Per
    module: add it first when it has a parent_id (a fresh flash has no user-added
    modules like AudioService, so a control write would 404), then set its controls.
    A module without parent_id is boot-wired/top-level (Board under System,
    Network) that already exists — skip the add, just set controls. The add is
    idempotent (an existing id returns 200). Returns True iff EVERY POST returned
    200; best-effort (partial state may apply, the next refresh re-attempts).
    """
    import urllib.request
    import urllib.error

    def _post(path: str, body_obj: dict) -> bool:
        body = json.dumps(body_obj).encode()
        try:
            req = urllib.request.Request(
                f"http://{ip}{path}",
                data=body, method="POST",
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=0.6) as resp:
                return resp.status == 200
        except (urllib.error.URLError, OSError):
            return False

    for m in modules:
        if not isinstance(m, dict):
            continue
        if m.get("parent_id") and m.get("type"):
            if not _post("/api/modules", {
                "type": m.get("type"),
                "id": m.get("id"),
                "parent_id": m.get("parent_id"),
            }):
                return False
        ctrls = m.get("controls") or {}
        for control_name, value in ctrls.items():
            if not _post("/api/control", {
                "module": m.get("id"),
                "control": control_name,
                "value": value,
            }):
                return False
    return True


def _push_devices_in_parallel(pushes):
    """Fire _push_device for each (ip, deviceModel) tuple in parallel.

    Discovery + refresh probe a /24, so the push count is bounded by the
    device count (single digits in practice). A small thread pool keeps
    total latency near the slowest single push instead of summing. Result
    is fire-and-forget: callers don't act on the bool returned by each
    push — failures are recoverable on the next refresh cycle.
    """
    if not pushes:
        return
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        # list() forces all futures to start; the with-block waits for them.
        list(pool.map(lambda p: _push_device(*p), pushes))


def discover_devices(subnet=""):
    """Scan subnet for devices responding to /api/state."""
    if not subnet:
        subnet = _get_local_subnet()

    # .1-.254 on port 80 (ESP32) and 8080 (desktop), plus localhost.
    targets = [(f"{subnet}.{i}", port)
               for i in range(1, 255) for port in (80, 8080)]
    targets.append(("localhost", 8080))

    # Wide thread pool — the probes are I/O-bound (almost always blocked on the
    # socket, not the CPU), so running all ~509 in one wave means the whole /24
    # scan finishes in about one probe-timeout window (~0.4s) instead of
    # batch-serializing. The pool still caps thread churn vs. raw thread spawns.
    from concurrent.futures import ThreadPoolExecutor
    devices = []
    with ThreadPoolExecutor(max_workers=len(targets)) as pool:
        for result in pool.map(lambda t: _probe_device(*t), targets):
            if result:
                devices.append(result)

    # The local app answers on both localhost and this machine's LAN IP — the
    # subnet scan finds the LAN-IP entry, the explicit localhost probe finds the
    # other. Keep the LAN IP (usable from any device) and drop the redundant
    # localhost entry so the discovered list shows real network addresses.
    # Compare on the HOST (ip without port), since a port-80 device is now stored bare.
    localIp = _lan_ip()
    hasLanEntry = localIp and any(d["ip"].split(":", 1)[0] == localIp for d in devices)
    if hasLanEntry:
        devices = [d for d in devices if d["ip"].split(":", 1)[0] != "localhost"]

    # Attribute a recent flash's port to the device it flashed (by MAC) — same as refresh, so
    # a flash → discover also records last_port. Whichever scan finds the device first links it.
    _link_last_flash(devices)

    # Sort by device name, case-insensitive — matching the on-device DevicesModule list
    # (src/core/system/DevicesModule.h sortByName / ciLess) so both lists read the same. IP is the
    # tiebreaker so un-named / duplicate-named devices still have a stable order.
    devices.sort(key=_device_sort_key)
    return devices, subnet


_LAST_FLASH_FILE = SCRIPTS_DIR / ".last_flash.json"
_LAST_FLASH_TTL_S = 5 * 60  # ignore markers older than 5 minutes


def _consume_last_flash() -> dict | None:
    """Read the breadcrumb moondeck/.last_flash.json that flash_esp32.py drops
    after a successful flash. Returns {port, mac, firmware} when the marker is
    recent (< TTL); the caller deletes the file once it links the event so it
    only applies once. Returns None when there's no recent marker."""
    if not _LAST_FLASH_FILE.exists():
        return None
    try:
        data = json.loads(_LAST_FLASH_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    import time
    if time.time() - float(data.get("ts", 0)) > _LAST_FLASH_TTL_S:
        with suppress(OSError):
            _LAST_FLASH_FILE.unlink()
        return None
    return {"port": data.get("port", ""), "mac": data.get("mac", ""),
            "firmware": data.get("firmware", "")}


def refresh_devices(known_devices):
    """Probe known devices to check online/offline status.

    Preserves user-set fields (`deviceModel`, `last_port`) across refreshes: the
    probe result carries fresh `firmware`/`deviceName` and a deduced `deviceModel`
    (set only when firmware unambiguously identifies hardware), but a user-set
    `deviceModel` for a firmware that can run on multiple device models (e.g. `esp32` on
    LOLIN D32 vs generic DevKit) must survive a refresh. Same for `last_port`,
    which is set by the flash-event breadcrumb (see _consume_last_flash).
    """
    def probe(device):
        ip = device.get("ip", "")
        if ":" in ip:
            host, port = ip.rsplit(":", 1)
            fresh = _probe_device(host, int(port))
        else:
            # A bare IP is a port-80 device (that's why it's stored without a port) — probe 80, NOT
            # _probe_device's 8080 default, or the ESP32 refresh would fail and mark it offline.
            fresh = _probe_device(ip, 80)
        if not fresh:
            return None
        # Merge: probe wins for live-readable fields; user-set / flash-tracked
        # fields must survive. Without this, `online` (the device responded
        # → True), `selected` (user checkbox), `board` (user-set when not
        # deducible), and `last_port` (set by the flash breadcrumb) would
        # disappear from the persisted record after every refresh.
        fresh["online"] = True
        if "selected" in device:
            fresh["selected"] = device["selected"]
        if not fresh.get("deviceModel") and device.get("deviceModel"):
            fresh["deviceModel"] = device["deviceModel"]
        if device.get("last_port"):
            fresh["last_port"] = device["last_port"]
        if device.get("usbSerial"):
            fresh["usbSerial"] = device["usbSerial"]
        if device.get("probedChip"):
            fresh["probedChip"] = device["probedChip"]
        return fresh

    if not known_devices:
        return []
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=16) as pool:
        refreshed = [r for r in pool.map(probe, known_devices) if r]

    _link_last_flash(refreshed)
    return refreshed


def _mac_matches(flash_mac: str, device_mac: str) -> bool:
    """True if a probed device's self-reported MAC identifies the just-flashed chip.

    esptool reads the raw 6-byte efuse MAC (e.g. 30:ED:A0:F3:D4:68), which is what the
    breadcrumb stores. Most chips' `esp_efuse_mac_get_default()` returns that same value,
    so an exact compare is the common case. But on some chips (the ESP32-S31) that API
    returns the EUI-64 encoding of the efuse MAC — FF:FE inserted after the 3-byte OUI —
    truncated back to 6 bytes: 30:ED:A0:F3:D4:68 → 30:ED:A0:FF:FE:F3. The device then
    reports *that* everywhere (its `mac` control, its MM-xxxx name), so a raw-efuse
    breadcrumb never matches it. Accept both the raw form and its EUI-64-truncated
    derivation, computed forward from the efuse MAC (the reverse is lossy — the dropped
    bytes are gone). A pure widening of the exact compare: it never breaks a real match.
    """
    flash_mac = (flash_mac or "").strip().lower()
    device_mac = (device_mac or "").strip().lower()
    if not flash_mac or not device_mac:
        return False
    if flash_mac == device_mac:
        return True
    b = flash_mac.split(":")
    if len(b) != 6:
        return False
    # EUI-64(efuse) = OUI(3) + ff:fe + rest(3); the device reports its first 6 bytes.
    eui64_trunc = ":".join([b[0], b[1], b[2], "ff", "fe", b[3]])
    return device_mac == eui64_trunc


def _link_last_flash(probed: list) -> None:
    """Set `last_port` on the device a recent flash targeted. flash_esp32.py writes
    moondeck/.last_flash.json {port, mac, firmware} after a successful flash; the port is
    attributed to the probed device with that MAC (the board's stable identity — unambiguous
    even when two boards share a firmware). A legacy breadcrumb without a MAC falls back to a
    firmware match, kept only for the single-candidate case. The marker is consumed (deleted)
    once linked so it applies once; if the device isn't in this scan yet it's left for the next.
    Called by both discover and refresh — whichever finds the device first links it."""
    last_flash = _consume_last_flash()
    if not last_flash:
        return
    flash_mac = (last_flash.get("mac") or "").strip().lower()
    if flash_mac:
        matches = [d for d in probed if _mac_matches(flash_mac, d.get("mac"))]
    elif last_flash.get("firmware"):   # legacy breadcrumb without a MAC
        matches = [d for d in probed if d.get("firmware") == last_flash["firmware"]]
    else:
        matches = []
    if len(matches) == 1:
        port = last_flash["port"]
        # A physical serial port maps to exactly ONE device at a time — boards get swapped on
        # the same USB port, and ports drift between sessions. So before attributing it, strip
        # this port from any OTHER device that still carries it (a stale link from a previous
        # flash), else two records share a last_port and port→device lookups (baud resolution,
        # the flash chip) can pick the wrong, stale board.
        serial = _port_serial(port)
        for d in probed:
            if d is not matches[0] and d.get("last_port") == port:
                d.pop("last_port", None)
            # usbSerial is the drift-immune key (the adapter's, not the OS path);
            # like last_port it belongs to exactly one board, so clear stale holders.
            if d is not matches[0] and serial and d.get("usbSerial") == serial:
                d.pop("usbSerial", None)
        matches[0]["last_port"] = port
        # Record the stable per-adapter serial so the port dropdown can re-identify
        # this board after the path drifts (see describe_serial_ports).
        if serial:
            matches[0]["usbSerial"] = serial
        with suppress(OSError):
            _LAST_FLASH_FILE.unlink()   # linked → consume so it applies once
    # 0 matches (device hasn't rebooted into the new firmware yet) or 2+ (ambiguous legacy
    # firmware match) → _consume_last_flash read but did NOT delete the file, so the marker
    # stays on disk for the next scan to retry, until its TTL lapses.


# ---------------------------------------------------------------------------
# State management
# ---------------------------------------------------------------------------

def load_state():
    """Load MoonDeck state. Migrates the old flat-list shape (top-level
    `devices` + `port`) to the new networks-grouped shape on first load
    after this commit ships; new-shape files load as-is. See _migrate_to_networks."""
    if STATE_FILE.exists():
        with open(STATE_FILE) as f:
            state = json.load(f)
        if "networks" not in state and ("devices" in state or "port" in state):
            state = _migrate_to_networks(state)
        return state
    return {"networks": [], "active_network": "", "tab": "desktop"}


def _migrate_to_networks(old_state: dict) -> dict:
    """One-shot migration of the pre-networks moondeck.json shape:
        {env, port, devices: [...], tab, firmware, scenario, module, flag_*}
    into the networks-grouped shape:
        {networks: [{name, subnet, wifi, port, devices: [...]}, ...],
         active_network, tab, firmware, scenario, module, flag_*}

    Buckets existing devices by `/24` subnet derived from each device's `ip`.
    Names the largest bucket "Home", subsequent buckets "Network 2", "Network 3",
    ... User can rename via the dropdown. The old top-level `port` migrates
    into the bucket that holds the largest device count (heuristic — usually
    that's where the user was working). Drops the legacy `env` field
    (already migrated to `firmware` in app.js).
    """
    import sys
    devices = old_state.get("devices") or []
    by_subnet: dict[str, list] = {}
    for d in devices:
        ip_port = d.get("ip", "")
        host = ip_port.split(":", 1)[0]
        parts = host.split(".")
        subnet = ".".join(parts[:3]) + ".0/24" if len(parts) == 4 else "unknown"
        by_subnet.setdefault(subnet, []).append(d)

    # Largest bucket first → named "Home"; rest "Network 2", "Network 3", ...
    ordered = sorted(by_subnet.items(), key=lambda kv: -len(kv[1]))
    networks = []
    for i, (subnet, bucket) in enumerate(ordered):
        name = "Home" if i == 0 else f"Network {i + 1}"
        networks.append({
            "name": name,
            "subnet": subnet,
            "wifi": {"ssid": "", "password": ""},
            "port": "",
            "devices": bucket,
        })
    # Old top-level port → largest bucket (which is networks[0] if any).
    if networks and old_state.get("port"):
        networks[0]["port"] = old_state["port"]

    new_state = {k: v for k, v in old_state.items()
                 if k not in ("devices", "port", "env")}
    new_state["networks"] = networks
    new_state["active_network"] = networks[0]["name"] if networks else ""
    new_state.setdefault("tab", "desktop")
    print(f"moondeck: migrated {len(devices)} device(s) into {len(networks)} "
          f"network(s): {', '.join(n['name'] for n in networks)}", file=sys.stderr)
    return new_state


# `modules` is the full module tree from /api/state — kilobytes per device, no
# UI consumer between probes; strip on save to keep moondeck.json small.
# `deviceName` and `firmware` ARE displayed in the device row label, so keep
# them persisted: stripping made the row show only an IP after every server
# restart until the user clicked Discover. Both fields are correctly
# overwritten by the next probe (no staleness drift problem).
_VOLATILE_DEVICE_FIELDS = ("modules",)


# Serializes the full load → mutate → save transaction across the threaded
# HTTP handlers (ThreadingHTTPServer dispatches each request on its own
# thread). Without this, two concurrent /api/discover requests could both
# load_state(), mutate their own copies, and each save_state() — last write
# wins, half the work is lost. RLock (not Lock) so save_state can also be
# called standalone for the no-mutator path (POST /api/state body merge)
# without deadlocking when nested inside mutate_state.
_state_write_lock = threading.RLock()


def mutate_state(mutator):
    """Run a full load → mutator(state) → save cycle under the state lock.
    Returns the post-mutation state so the handler can echo it to the
    client. `mutator` receives the loaded state dict, mutates in place
    (or returns a new dict, which becomes the value to save), and may
    return None to mean "keep the in-place mutation."

    Slow work (subnet scans, device probes) should happen BEFORE calling
    mutate_state — pass already-gathered data in by closure. Holding the
    lock across network I/O would serialize everything behind the slowest
    scan."""
    with _state_write_lock:
        state = load_state()
        result = mutator(state)
        if result is not None:
            state = result
        save_state(state)
        return state


def save_state(state):
    """Persist MoonDeck state. Strips per-device fields that the device itself
    is the source of truth for (`deviceName`, `firmware`) — caching them
    invites stale values when the device is reflashed/renamed via another
    host. They are re-read from `/api/state` on each refresh and live only
    in the in-memory device lists until the next save. User-set fields
    (`deviceModel`, `last_port`, `selected`, `online`) persist. Iterates per network.

    Write is atomic + serialized: a temp file in the same dir → fsync → rename.
    The rename is atomic on POSIX (same filesystem); fsync makes the bytes
    durable before the swap so a crash mid-write never leaves a half-written
    moondeck.json (the previous version stays intact). The lock ensures two
    handler threads don't race on the temp file or the rename."""
    persisted = dict(state)
    networks = persisted.get("networks") or []
    if networks:
        persisted["networks"] = [_strip_network_volatiles(n) for n in networks]
    data = json.dumps(persisted, indent=2)
    with _state_write_lock:
        # NamedTemporaryFile in the same dir so os.replace stays on one
        # filesystem (cross-FS rename is not atomic). delete=False because
        # we hand the path to os.replace ourselves.
        tmp = tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8",
            dir=str(STATE_FILE.parent),
            prefix=STATE_FILE.name + ".",
            suffix=".tmp",
            delete=False,
        )
        try:
            tmp.write(data)
            tmp.flush()
            os.fsync(tmp.fileno())
            tmp.close()
            os.replace(tmp.name, STATE_FILE)
        except Exception:
            # On failure, drop the stray temp file so we don't accumulate
            # .tmp leftovers across crashes. Re-raise so the caller sees it.
            with suppress(OSError):
                os.unlink(tmp.name)
            raise


def _strip_network_volatiles(network: dict) -> dict:
    """Return a copy of a network with volatile per-device fields stripped.

    `deviceModel` is conditionally volatile: when it equals the value `_deduce_device_model`
    produces from the device's current firmware, the next probe will re-derive
    it for free — no need to persist. When the user picked a deviceModel manually
    (firmware doesn't deduce to anything, e.g. `esp32` could be LOLIN D32 or
    generic), the picker's choice is the only source so we must keep it.
    """
    out = dict(network)
    devs = out.get("devices") or []
    cleaned = []
    for d in devs:
        c = {k: v for k, v in d.items() if k not in _VOLATILE_DEVICE_FIELDS}
        # `board` strip: drop it when empty (noise) or when it matches the
        # firmware-deduced value (recomputable). Keep when the user picked
        # it (firmware deduces "" so the picker's value is the only source).
        firmware = d.get("firmware") or ""
        if "deviceModel" in c and (not c["deviceModel"] or c["deviceModel"] == _deduce_device_model(firmware)):
            del c["deviceModel"]
        cleaned.append(c)
    out["devices"] = cleaned
    return out


def _active_network(state: dict) -> dict | None:
    """Return the dict for state['active_network'] from state['networks'],
    or None when the name doesn't match or there's no active selection.
    Every consumer that previously read state['devices'] or state['port']
    routes through this helper."""
    name = state.get("active_network") or ""
    for n in state.get("networks") or []:
        if n.get("name") == name:
            return n
    return None


def _strip_probe_transient(device: dict) -> dict:
    """A copy of `device` without `_reportedModel` — the raw model string a probe attaches
    for the push-compare, which must not be persisted to moondeck.json. One idiom for the
    drop so discover and refresh can't diverge on which key(s) count as probe-transient."""
    return {k: v for k, v in device.items() if k != "_reportedModel"}


def _device_model_for_port(port: str) -> str:
    """The deviceModel of the device last flashed via `port`, or "" if none.

    Maps a serial port back to a board by the `last_port` breadcrumb (set at flash time,
    keyed per-device by MAC) in the active network. Used so a port-based flash can resolve
    its baud by the exact deviceModel rather than the shared firmware. Returns "" when the
    port isn't linked to any device, or the linked device has no deviceModel set."""
    if not port:
        return ""
    net = _active_network(load_state())
    for d in (net.get("devices") or []) if net else []:
        if d.get("last_port") == port:
            return d.get("deviceModel") or ""
    return ""


def _subnet_from_host_subnet(host_subnet: str) -> str:
    """Normalize `_get_local_subnet()` output (e.g. "192.168.1") to the
    network record's `subnet` field shape ("192.168.1.0/24")."""
    if not host_subnet:
        return ""
    return f"{host_subnet}.0/24"


def _auto_select_network(state: dict, host_subnet: str) -> None:
    """In-place: set state['active_network'] to whichever known network's
    subnet matches the host's current subnet — but only if the user hasn't
    pinned a different network. Pinning happens when the user changes the
    dropdown; cleared when the pinned network's subnet stops matching the
    host (next time we land on its LAN, auto-select takes over again)."""
    if not state.get("networks"):
        return
    target_subnet = _subnet_from_host_subnet(host_subnet)
    if not target_subnet:
        return
    pinned = state.get("active_network_user_pinned")
    if pinned:
        active = _active_network(state)
        if active and active.get("subnet") == target_subnet:
            return  # pinned network still matches host — leave as is
        # Pinned network no longer matches host — release the pin so the
        # next auto-select picks the right network for where we are now.
        state["active_network_user_pinned"] = False
    for n in state["networks"]:
        if n.get("subnet") == target_subnet:
            state["active_network"] = n["name"]
            return


# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------

_running: dict[str, subprocess.Popen] = {}
_lock = threading.Lock()
_IS_WIN = sys.platform == "win32"


def _kill_process_by_name(name: str):
    """Kill processes matching name. Cross-platform."""
    if _IS_WIN:
        subprocess.run(["taskkill", "/F", "/IM", name + ".exe"],
                       capture_output=True)
    else:
        subprocess.run(["pkill", "-f", name], capture_output=True)


def _free_port(port: int):
    """SIGTERM whatever already holds `port`, so a re-run of MoonDeck replaces the
    prior instance instead of failing to bind ("Address already in use"). Targets
    the port's owner (not a name match), so it can only hit a stale server, never
    the current process (we haven't bound yet). Cross-platform: `lsof` on
    macOS/Linux, `netstat` on Windows; a no-op if the port is free or the lookup
    tool is absent."""
    pids: set[int] = set()
    try:
        if _IS_WIN:
            out = subprocess.run(["netstat", "-ano", "-p", "TCP"],
                                 capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 5 and parts[1].endswith(f":{port}") and "LISTEN" in line:
                    with suppress(ValueError):
                        pids.add(int(parts[-1]))
        else:
            # -t: pids only, -sTCP:LISTEN: only the listener (not clients connected to it).
            out = subprocess.run(["lsof", "-t", f"-iTCP:{port}", "-sTCP:LISTEN"],
                                 capture_output=True, text=True).stdout
            for tok in out.split():
                with suppress(ValueError):
                    pids.add(int(tok))
    except (OSError, FileNotFoundError):
        return  # no lookup tool → let the bind fail with its normal error
    for pid in pids:
        if pid == os.getpid():
            continue  # never ourselves
        with suppress(OSError, ProcessLookupError):
            if _IS_WIN:
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
            else:
                os.kill(pid, signal.SIGTERM)
    if pids:
        print(f"Freed port {port} from a prior instance (pid {', '.join(map(str, pids))}).")


def kill_script(script_id: str):
    with _lock:
        proc = _running.pop(script_id, None)
    if proc and proc.poll() is None:
        try:
            if _IS_WIN:
                proc.terminate()
            else:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass
    # Close the pty master fd if the SSE stream never attached to close it (a killed-before-
    # streamed proc — e.g. a fast Run→Run double click). Harmless if already closed.
    if proc is not None:
        stream = getattr(proc, "_mm_read_stream", None)
        if stream is not None and getattr(proc, "_mm_master_fd", None) is not None:
            with suppress(OSError):
                stream.close()

    # Clean up any orphaned processes (e.g. MoonLight after os.execv)
    script_def = next((s for s in SCRIPTS if s["id"] == script_id), None)
    pname = script_def.get("process_name") if script_def else None
    if pname:
        _kill_process_by_name(pname)


def is_process_running(name: str) -> bool:
    """Check if a process matching name is running. Cross-platform."""
    if _IS_WIN:
        r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}.exe"],
                           capture_output=True, text=True)
        return name in r.stdout
    else:
        r = subprocess.run(["pgrep", "-f", name], capture_output=True)
        return r.returncode == 0


# ---------------------------------------------------------------------------
# Serial port discovery
# ---------------------------------------------------------------------------

def list_serial_ports() -> list[str]:
    """List available serial ports.

    POSIX hosts: glob the conventional /dev serial device files (no deps).
    Windows: read HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM via winreg (stdlib).
    SERIALCOMM is the authoritative table the OS itself maintains for
    present COM ports — what pyserial reads under the hood — so a registry
    walk is both correct and dependency-free. The previous brute-force
    COM0..COM255 open-and-close loop required pyserial, which MoonDeck did
    not declare, so on Windows the list silently came back empty.
    """
    ports: list[str] = []
    import glob
    # macOS exposes each USB serial as BOTH /dev/tty.X (call-in, blocks on DCD) and /dev/cu.X
    # (call-out, non-blocking). Flashing (esptool / idf.py) uses the cu.* node, and flash_esp32.py
    # records last_port as cu.*, so list cu.* here too — otherwise the port dropdown (tty.*) never
    # matches a stored last_port and the selection silently falls back to "--".
    ports.extend(glob.glob("/dev/cu.usb*"))
    ports.extend(glob.glob("/dev/ttyUSB*"))
    ports.extend(glob.glob("/dev/ttyACM*"))
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        _, port, _ = winreg.EnumValue(key, i)
                        ports.append(port)
                        i += 1
                    except OSError:
                        break
        except FileNotFoundError:
            pass  # no SERIALCOMM key — no ports present
    return sorted(ports)


# ---------------------------------------------------------------------------
# Port identity — three levels of "which board is this?"
#
# The port PATH drifts between sessions (macOS renumbers cu.usbserial-* on
# replug); ports don't self-identify. So we build up to three levels of info,
# each degrading gracefully to the one below:
#   1. path  — always (the cu.* / COM* string itself)
#   2. chip  — the ESP32 family (esp32-s3 / -p4 / classic), from the USB
#              descriptor when the board has native USB (Espressif VID 0x303A);
#              otherwise inferred from the registry's stored firmware.
#   3. board — the specific board name (e.g. "MM-LC16"), from a registry match:
#              on native-USB Espressif chips the USB serial number IS the MAC,
#              so we match MAC→device directly; on external UART adapters
#              (CP210x / CH343) the stable per-adapter serial embedded in the
#              port name keys a device's `usbSerial` field.
# None of this resets or opens the board — it reads the USB descriptor the OS
# already enumerated, so it's safe to run on every dropdown refresh.
# ---------------------------------------------------------------------------

# Espressif chips with *native* USB (no external UART bridge) advertise VID
# 0x303A; the product-descriptor PID names the family. Boards behind a CP210x
# (SiLabs 0x10C4) or CH343 (WCH 0x1A86) UART bridge expose only the adapter, so
# their chip family isn't in USB — it comes from the registry firmware instead.
_ESPRESSIF_VID = 0x303A
# PID 0x1001 is the SHARED USB-Serial-JTAG PID across S3/C3/C6/H2/P4 — it does NOT identify the
# specific chip (the P4-NANO reports it too), and the product string is always the generic "USB
# JTAG/serial debug unit", so there's nothing to disambiguate with. Map it only to "native-USB
# Espressif, family unknown"; the specific chip comes from a registry match (MAC/usbSerial) or the
# Identify probe. Only 0x1002 (the S2's own PID) names a chip. Others fall through to the generic.
_ESP_PID_CHIP = {
    0x1002: "esp32-s2",
}

# VIDs that could front an ESP32: Espressif native USB, plus the USB-UART bridges
# ESP32 dev boards ship (SiLabs CP210x, WCH CH34x, FTDI, Prolific). A port whose
# USB vendor is known but NOT in this set (any other USB serial device on the
# machine — a monitor's control channel, a dongle, a keyboard) is definitely not
# an ESP32, so it's labeled by its own product name and skipped from the probe.
_ESP_CAPABLE_VIDS = frozenset({
    _ESPRESSIF_VID,   # 0x303A Espressif native USB
    0x10C4,           # Silicon Labs CP210x
    0x1A86,           # WCH CH340 / CH343
    0x0403,           # FTDI
    0x067B,           # Prolific PL2303
})


def _chip_from_usb(vid: int, pid: int) -> str:
    """Map a native-USB Espressif (vid, pid) to an ESP32 family, or "" if the
    descriptor doesn't reveal the chip (external UART bridge — vid isn't Espressif's,
    or an unknown PID). The product string is always the generic "USB JTAG/serial
    debug unit" (never the SoC), so the PID table is what identifies the chip. Pure."""
    if vid != _ESPRESSIF_VID:
        return ""  # external adapter — chip family not carried in USB
    return _ESP_PID_CHIP.get(pid, "esp32 (native-usb)")


def _firmware_to_chip(firmware: str) -> str:
    """Best-effort ESP32 family from a registry firmware id (e.g.
    'esp32s3-n8r8' → 'esp32-s3', 'esp32p4rev1-eth' → 'esp32-p4', 'esp32' →
    'esp32 (classic)'). The fallback for boards behind a UART bridge. Pure."""
    f = (firmware or "").lower()
    for key, chip in (("p4", "esp32-p4"), ("s3", "esp32-s3"),
                      ("s2", "esp32-s2"), ("c6", "esp32-c6"), ("c3", "esp32-c3")):
        if key in f:
            return chip
    if f.startswith("esp32"):
        return "esp32 (classic)"
    return ""


def _port_serial(path: str) -> str:
    """The stable per-adapter serial embedded in a macOS port name:
    /dev/cu.usbserial-20213240 → '20213240', /dev/cu.usbmodem5ABA0767291 →
    '5ABA0767291'. This survives path drift (the number is the adapter's, not
    the OS enumeration order). Returns "" when the name carries no serial. Pure."""
    import re
    m = re.search(r"usb(?:serial-|modem)(.+)$", path)
    return m.group(1) if m else ""


def _heal_last_ports(network: dict | None) -> None:
    """Correct `last_port` breadcrumbs that the currently-present ports disprove.

    `last_port` is a flash-time breadcrumb, and macOS renumbers `/dev/cu.usbserial-*`
    paths as adapters come and go — so a record can end up naming a port that no longer
    exists while the board sits on a different one. The Live tab then shows a port the
    ESP32 tab knows is wrong, which is exactly the kind of quietly-stale state that sends
    a flash at the wrong board.

    Two repairs, both driven by `usbSerial` (the ADAPTER's own serial, embedded in the port
    name and immune to path drift), never by the path:

      1. the adapter is present on a different path → move `last_port` to that path;
      2. the adapter is absent and the recorded path is gone → drop `last_port`, so the UI
         shows no port rather than a fictional one.

    A record with no `usbSerial` is left alone: without the drift-immune key there is
    nothing to prove the breadcrumb wrong, and guessing would be worse than stale.
    """
    if not network:
        return
    present = {p: _port_serial(p) for p in list_serial_ports()}
    by_serial = {s: p for p, s in present.items() if s}

    for dev in network.get("devices", []):
        serial = dev.get("usbSerial")
        if not serial:
            continue
        actual = by_serial.get(serial)
        recorded = dev.get("last_port")
        if actual:
            if recorded != actual:
                dev["last_port"] = actual
        elif recorded and recorded not in present:
            dev.pop("last_port", None)


def _resolve_port(path: str, usb: dict, devices: list) -> dict:
    """Build the {path, chip, board, ip} identity for one port. `usb` is that
    port's USB descriptor ({vid,pid,product,serial}) or {} if unknown; `devices`
    is the active network's device list. Pure — all I/O done by the caller."""
    chip = _chip_from_usb(usb.get("vid", 0), usb.get("pid", 0))
    board = ip = ""
    # Level 3: match a registry device. Native-USB Espressif chips report their
    # MAC as the USB serial number, so match that first; then fall back to the
    # per-adapter serial (from the port name) against a stored `usbSerial`.
    usb_serial = usb.get("serial", "")
    serial = _port_serial(path)
    match = None
    for d in devices:
        mac = (d.get("mac") or "").upper()
        if usb_serial and mac and _mac_matches(mac, usb_serial):
            match = d
            break
        if serial and d.get("usbSerial") and serial == d["usbSerial"]:
            match = d
            break
    if match:
        board, ip = match.get("deviceName", ""), match.get("ip", "")
        # A registry match knows the SPECIFIC chip (from an esptool probe, or derived from the
        # flashed firmware). That beats the USB descriptor's family: for a native-USB board the
        # descriptor only ever gives the generic "esp32 (native-usb)" (0x1001 is shared across
        # S3/C3/C6/H2/P4), so a real probe/firmware chip must win over it. Keep a *specific* USB
        # chip (the S2's 0x1002) if the registry has nothing better.
        known = match.get("probedChip", "") or _firmware_to_chip(match.get("firmware", ""))
        if known and (not chip or chip == "esp32 (native-usb)"):
            chip = known
    elif not chip:
        pass   # no match, no USB chip → stays "" (path/level-1 only), unchanged
    # `probeable`: could this port be an ESP32 at all? True unless the USB vendor
    # is known-but-not-ESP-capable (a monitor, dongle, keyboard — resetting those
    # wastes a ~10s reset for nothing). This is a DEV bench where boards move
    # between ports constantly, so Identify re-probes every probeable port every
    # time — even already-labeled ones — because a fresh read of the board
    # actually on the port beats trusting a cache that a swap may have staled.
    vid = usb.get("vid", 0)
    known_not_esp = bool(vid) and vid not in _ESP_CAPABLE_VIDS
    probeable = not known_not_esp
    # Label a non-ESP port with its actual USB product name (whatever the device
    # reports) so you can see what's on the port, not just that it's skipped;
    # fall back to "not an ESP32" when the descriptor has no product string.
    note = ""
    if known_not_esp and not board:
        note = usb.get("product", "").strip() or "not an ESP32"
    return {"path": path, "chip": chip, "board": board, "ip": ip,
            "probeable": probeable, "note": note}


def _read_usb_ports() -> dict:
    """macOS: parse `ioreg` into {port_path: {vid,pid,product,serial}} for every
    USB serial callout device, without opening (resetting) the port. Returns {}
    on non-macOS or if ioreg is unavailable — callers degrade to path-only."""
    if sys.platform != "darwin":
        return {}
    import re
    try:
        # BYTES, then a lenient decode. `ioreg -l` dumps every property in the registry, including
        # raw device data that is not text at all, so a strict UTF-8 decode raises
        # UnicodeDecodeError on whatever happens to be attached: on the bench it fired on every
        # /api/ports request while boards were connected, and since UnicodeDecodeError is neither
        # OSError nor SubprocessError it escaped this handler and 500'd the request, leaving
        # MoonDeck's port dropdown empty with boards plugged in. The parse below only ever reads
        # ASCII keys, so replacing the undecodable bytes costs nothing and keeps the listing.
        raw = subprocess.run(
            ["ioreg", "-l", "-w0"],
            capture_output=True, timeout=5).stdout
        out = raw.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        return {}
    # The IORegistry is a tree: a USB device node holds the descriptor
    # (idVendor/idProduct/USB Serial Number), and a *child* IOSerialBSDClient
    # node holds IOCalloutDevice (the /dev path). They're in different `+-o`
    # blocks, so pair them by tree depth: track a stack of nodes keyed on the
    # indent of their `+-o` marker, and attach each callout to its nearest
    # ancestor that carries a descriptor field.
    def depth(ln):
        i = ln.find("+-o")
        return i if i >= 0 else None
    stack: list = []            # (depth, node-dict)
    cur: dict | None = None
    result: dict = {}
    fields = (("idVendor", "vid", int), ("idProduct", "pid", int),
              ("USB Product Name", "product", str), ("USB Serial Number", "serial", str))
    for ln in out.splitlines():
        d = depth(ln)
        if d is not None:
            while stack and stack[-1][0] >= d:
                stack.pop()
            cur = {"vid": 0, "pid": 0, "product": "", "serial": ""}
            stack.append((d, cur))
        if cur is not None:
            for tag, key, cast in fields:
                m = re.search(rf'"{tag}" = (?:"([^"]*)"|(\d+))', ln)
                if m:
                    cur[key] = cast(m.group(1) if m.group(1) is not None else m.group(2))
        mc = re.search(r'"IOCalloutDevice" = "([^"]+)"', ln)
        if mc:
            desc = {"vid": 0, "pid": 0, "product": "", "serial": ""}
            for _, node in reversed(stack):   # nearest ancestor wins per field
                for key in desc:
                    if not desc[key] and node[key]:
                        desc[key] = node[key]
            result[mc.group(1)] = desc
    return result


def describe_serial_ports(devices: list | None = None) -> list[dict]:
    """The dropdown's data source: every present port enriched with chip family
    and specific board (levels 2 and 3). `devices` is the active network's
    device list (for the registry match); pass [] for path+chip only."""
    usb = _read_usb_ports()
    return [_resolve_port(p, usb.get(p, {}), devices or [])
            for p in list_serial_ports()]


# --- level 2/3 by active probe (opt-in — resets the board) -------------------
#
# The USB descriptor only reveals the chip for *native-USB* boards; a board
# behind a CP210x/CH343 UART bridge hides its family. esptool's connect
# handshake reads the chip's magic register and efuse MAC — the same thing the
# web installer does — which fills both the chip (level 2) and, via the MAC, the
# board (level 3). But the handshake RESETS the board (DTR/RTS into download
# mode), so this is opt-in (an "Identify ports" button), never automatic. The
# result is cached to the registry (usbSerial), so it's a one-time cost per board.

def _normalize_chip(detected: str) -> str:
    """esptool's chip name ('ESP32-S3', 'ESP32-S31', 'ESP32-P4', 'ESP32') → our
    family style ('esp32-s3', 'esp32-p4', 'esp32 (classic)'). Pure."""
    d = (detected or "").strip().lower()
    if not d.startswith("esp32"):
        return ""
    for fam in ("esp32-p4", "esp32-s31", "esp32-s3", "esp32-s2",
                "esp32-c6", "esp32-c5", "esp32-c3", "esp32-h2"):
        if d.startswith(fam):
            return "esp32-s3" if fam == "esp32-s31" else fam   # S31 is an S3 variant
    return "esp32 (classic)"   # bare "esp32"


def _parse_esptool_probe(text: str) -> dict:
    """Extract {chip, mac} from esptool's `read-mac` output. Pure — the caller
    runs esptool. `chip` is normalized to our family style; both "" if absent."""
    import re
    chip = mac = ""
    m = re.search(r"Detecting chip type\.\.\.\s*(ESP32\S*)", text, re.I) \
        or re.search(r"Chip is\s+(ESP32\S*)", text, re.I)
    if m:
        chip = _normalize_chip(m.group(1))
    mm = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", text)
    if mm:
        mac = mm.group(1).lower()
    return {"chip": chip, "mac": mac}


def probe_port_chip(path: str) -> dict:
    """Run esptool against one port to read its chip + MAC. RESETS the board.
    Returns {chip, mac} ("" fields on failure — a busy/monitored port, no chip,
    or esptool unavailable). esptool comes via `uv run --with esptool` so it needs
    no IDF toolchain (the project uv standard, see CLAUDE.md § Use uv)."""
    try:
        out = subprocess.run(
            ["uv", "run", "--with", "esptool", "python", "-m", "esptool",
             "--port", path, "--before", "default-reset", "read-mac"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return {"chip": "", "mac": ""}
    return _parse_esptool_probe(out.stdout + out.stderr)


def _apply_probe_results(devices: list, probed: dict) -> None:
    """Cache probe results ({path: {chip, mac}}) onto the matched devices in
    place. A probe reads the board ACTUALLY on the port, so it's authoritative:
    the port-name serial is moved onto the probed board even if another device's
    cache claimed it (the adapter-reused-for-another-board swap). Pure aside from
    mutating `devices`; the caller runs it inside mutate_state. Unmatched probes
    (no MAC, or a MAC no device reports) are ignored."""
    for path, info in probed.items():
        if not info.get("mac"):
            continue
        dev = next((d for d in devices
                    if _mac_matches(info["mac"], d.get("mac", ""))), None)
        if not dev:
            continue
        serial = _port_serial(path)
        if serial:
            # One port maps to one board: strip a stale claim off any other device.
            for other in devices:
                if other is not dev and other.get("usbSerial") == serial:
                    other.pop("usbSerial", None)
            dev["usbSerial"] = serial
        if info.get("chip"):
            dev["probedChip"] = info["chip"]


# ---------------------------------------------------------------------------
# Perf-table HTML (shared shape with docs/reference/tests/scenario-tests.md)
# ---------------------------------------------------------------------------

def _render_perf_table_html(step: dict) -> str:
    """Render a scenario step's contract+observed data as an HTML table that
    matches the markdown table emitted by test_doc_gen._format_perf_table.
    The doc generator owns the cell formatters; we just translate its pipe-
    delimited markdown to <table><tr><td>. Returns "" when the step has no
    contract/observed data."""
    import html as html_mod
    md_lines = test_doc_gen._format_perf_table(step)
    if not md_lines:
        return ""
    # Lines come in groups:
    #   header text (`**Performance** ...`)
    #   blank
    #   table header row (`| Board | FPS | ... |`)
    #   separator row (`|---|---|...`)
    #   N body rows (`| `target` | ... |`)
    #   blank
    #   optional footer lines (`- \`target\`: contract set ... · observed ...`)
    out: list[str] = []
    in_table = False
    rendered_header = False
    for line in md_lines:
        if not line.strip():
            if in_table:
                out.append("</tbody></table>")
                in_table = False
            continue
        if line.startswith("**"):
            # `**Performance** (contract / observed) — tick stored, FPS shown:`
            # → strip both the leading `**...**` bold marker (just the markup,
            # keep the bolded text inside) and the trailing `:` colon.
            import re as _re
            txt = _re.sub(r"\*\*(.+?)\*\*", r"\1", line.strip()).rstrip(":").strip()
            out.append(f'<div class="perf-head"><strong>{html_mod.escape(txt)}</strong></div>')
            continue
        if line.startswith("|") and "---" in line:
            continue  # markdown separator row
        if line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not in_table:
                out.append('<table class="perf-table"><tbody>')
                in_table = True
            tag = "th" if not rendered_header else "td"
            rendered_header = True
            row = "".join(
                f"<{tag}>{_inline_code_html(html_mod.escape(c))}</{tag}>"
                for c in cells
            )
            out.append(f"<tr>{row}</tr>")
            continue
        if line.lstrip().startswith("-"):
            # Audit footer line.
            txt = line.lstrip().lstrip("-").strip()
            out.append(f'<div class="perf-audit">{_inline_code_html(html_mod.escape(txt))}</div>')
            continue
    if in_table:
        out.append("</tbody></table>")
    return '<div class="perf">' + "".join(out) + '</div>'


def _inline_code_html(s: str) -> str:
    """Tiny markdown-inline-code → <code> translator. The perf table cells
    contain `target` and `field` names wrapped in backticks; render as <code>.
    Doesn't try to be a full markdown parser — just the patterns the perf
    formatter produces."""
    import re as _re
    return _re.sub(r"`([^`]+)`", r"<code>\1</code>", s)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class MoonDeckHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        # Suppress default request logging
        pass

    def handle(self):
        # Browser closing the connection is harmless; suppress the noise.
        with suppress(ConnectionResetError, BrokenPipeError):
            super().handle()

    def _send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    def do_GET(self):
        if self.path == "/api/scripts":
            # Which scripts have a stored last run, so the UI shows the 📄 button only where
            # there is something to show. The absence is informative too: a card with no 📄 is
            # one nobody has run yet.
            have_logs = set()
            if LOG_DIR.exists():
                have_logs = {f.stem for f in LOG_DIR.glob("*.log")}
            scripts = [{**s, "hasLog": s["id"] in have_logs} for s in SCRIPTS]
            self._send_json({"scripts": scripts, "firmwares": FIRMWARES})

        elif self.path == "/api/ports":
            # Enrich each port with chip family + specific board (levels 2/3),
            # resolved against the active network's registered devices, and heal any
            # `last_port` this listing proves wrong (see _heal_last_ports).
            state = mutate_state(lambda s: _heal_last_ports(
                next((n for n in s.get("networks", [])
                      if n.get("name") == s.get("active_network")), None)))
            active = next((n for n in state.get("networks", [])
                           if n.get("name") == state.get("active_network")), None)
            devices = (active or {}).get("devices", [])
            self._send_json({"ports": describe_serial_ports(devices)})

        elif self.path == "/api/scenarios":
            self._send_json({"scenarios": self._list_scenarios()})

        elif self.path == "/api/uiclips":
            self._send_json({"uiclips": self._list_ui_runs("clips")})

        elif self.path == "/api/uiprojects":
            self._send_json({"uiprojects": self._list_ui_runs("projects")})

        elif self.path.startswith("/api/scenarios/"):
            self._serve_scenario_steps()

        elif self.path == "/api/test-modules":
            self._send_json({"modules": test_meta.list_test_modules()})

        elif self.path == "/api/device-models":
            # Serves mooninstaller/deviceModels.json (loaded at startup). The web
            # installer (Step 2) fetches the same file directly from Pages;
            # MoonDeck reads it locally and exposes it here so the JS UI shares
            # one source of truth with the Python deduce path.
            self._send_json({"deviceModels": DEVICE_MODELS})

        elif self.path.startswith("/api/unit-tests/"):
            self._serve_unit_tests_for_module()

        elif self.path == "/api/state":
            # Auto-select the network matching the host's current subnet
            # (unless the user has pinned a different one — see
            # _auto_select_network). Persist the selection back so the next
            # load is stable when the host's subnet hasn't changed.
            state = load_state()
            before = state.get("active_network")
            _auto_select_network(state, _get_local_subnet())
            if state.get("active_network") != before:
                save_state(state)
            self._send_json(state)

        elif self.path == "/api/running":
            running = {}
            for s in SCRIPTS:
                pname = s.get("process_name")
                if pname:
                    running[s["id"]] = is_process_running(pname)
            self._send_json(running)

        elif self.path.startswith("/api/stream/"):
            script_id = self.path.split("/")[-1]
            self._handle_stream(script_id)

        elif self.path.startswith("/api/log/"):
            self._serve_log(self.path.split("/")[-1])

        elif self.path.startswith("/api/help"):
            self._serve_help()

        elif self.path.startswith("/api/docs/"):
            self._serve_doc()

        elif self.path == "/api/history-report":
            self._serve_history_report()

        elif self.path.startswith("/api/doc-asset/"):
            self._serve_doc_asset()

        elif self.path.startswith("/firmware/") and self.path.endswith(".bin"):
            self._serve_firmware_bin()

        else:
            self._serve_static()

    def do_POST(self):
        if self.path.startswith("/api/run/"):
            script_id = self.path.split("/")[-1]
            body = self._read_body()
            params = json.loads(body) if body else {}
            self._handle_run(script_id, params)

        elif self.path.startswith("/api/kill/"):
            script_id = self.path.split("/")[-1]
            kill_script(script_id)
            self._send_json({"status": "killed"})

        elif self.path == "/api/state":
            body = self._read_body()
            patch = json.loads(body) if body else {}
            # mutate_state holds the lock across load + merge + save so two
            # concurrent POSTs can't each load the same snapshot, apply
            # different patches, and clobber each other on save.
            def _merge(s):
                s.update(patch)
            result = mutate_state(_merge)
            self._send_json(result)

        elif self.path == "/api/identify-ports":
            # Opt-in chip probe (the "Identify ports" button). Body: {ports: [...]}
            # — the ports the UI still shows as Level 1. esptool resets each board
            # to read its chip + MAC, so we probe ONLY what was asked, never all.
            # The probe (slow, network/serial I/O) runs BEFORE mutate_state per its
            # contract; results are cached to the matched device (usbSerial +
            # probedChip) so the label survives path drift and future refreshes.
            body = self._read_body()
            req = json.loads(body) if body else {}
            # Probe ONLY paths that are currently-present serial ports — never an arbitrary client
            # string reaching esptool's --port. A drifted/removed port silently drops from the set.
            known = set(list_serial_ports())
            probed = {p: probe_port_chip(p) for p in req.get("ports", []) if p in known}

            def _apply(s):
                active = next((n for n in s.get("networks", [])
                               if n.get("name") == s.get("active_network")), None)
                _apply_probe_results((active or {}).get("devices", []), probed)
            state = mutate_state(_apply)
            active = next((n for n in state.get("networks", [])
                           if n.get("name") == state.get("active_network")), None)
            self._send_json({"ports": describe_serial_ports((active or {}).get("devices", [])),
                             "probed": probed})

        elif self.path == "/api/ota":
            # Wireless flash of a LOCAL build: MoonDeck serves build/esp32-<fw>/projectMM.bin over
            # its own HTTP (the GET /firmware/<fw>.bin route) and hands the device that URL via the
            # device's POST /api/firmware/url — the device pulls + flashes it (esp_https_ota accepts
            # the plain-http LAN URL). Same "flash my local build" as USB, over WiFi. Body: {ip, firmware}.
            import urllib.request
            import urllib.error
            import re
            body = self._read_body()
            params = json.loads(body) if body else {}
            ip = params.get("ip", "")
            firmware = params.get("firmware", "")
            if not ip or not firmware:
                self._send_json({"error": "ip and firmware required"}, 400)
                return
            # Validate both inputs before they reach a filesystem path or an outbound POST. `firmware`
            # is a build-dir name (same "/" + ".." guard _serve_firmware_bin uses); `ip` must be a
            # bare host/IP (a device on the LAN), not an arbitrary URL/authority — this handler POSTs
            # to `http://<ip>/…`, so an unchecked value would let a caller aim MoonDeck at any host.
            if "/" in firmware or ".." in firmware:
                self._send_json({"error": "bad firmware name"}, 400)
                return
            if not re.fullmatch(r"[A-Za-z0-9.\-]{1,253}", ip):
                self._send_json({"error": "bad ip/host"}, 400)
                return
            bin_path = ROOT / "build" / f"esp32-{firmware}" / "projectMM.bin"
            if not bin_path.exists():
                self._send_json({"error": f"no build for {firmware!r} — run Build first"}, 404)
                return
            host_ip = _lan_ip()
            if not host_ip:
                self._send_json({"error": "MoonDeck can't determine its LAN IP (offline?)"}, 502)
                return
            bin_url = f"http://{host_ip}:{PORT}/firmware/{firmware}.bin"
            # POST the URL to the device; it fetches + flashes. The device 202/200s immediately and
            # reboots on success — the ota status is polled from the device's Firmware module.
            try:
                req = urllib.request.Request(
                    f"http://{ip}/api/firmware/url",
                    data=json.dumps({"url": bin_url}).encode(),
                    method="POST", headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=5) as resp:
                    self._send_json({"ok": True, "url": bin_url, "device_status": resp.status})
            except urllib.error.HTTPError as e:
                self._send_json({"error": f"device rejected OTA (HTTP {e.code})"}, 502)
            except (urllib.error.URLError, OSError) as e:
                self._send_json({"error": f"device unreachable: {e}"}, 502)

        elif self.path == "/api/push-device":
            # Push a single (ip, deviceModel) to a device. Called by the JS when the
            # user picks a deviceModel from the per-device dropdown — saveState
            # alone persists the value in moondeck.json but the device also
            # needs to hear about it (the device persists its `deviceModel` control,
            # now on SystemModule, to /.config/SystemModule.json). The bulk push from discover /
            # refresh covers the multi-device case; this covers the
            # one-device-at-a-time UI mutation.
            body = self._read_body()
            params = json.loads(body) if body else {}
            ip = params.get("ip", "")
            model = params.get("deviceModel", "")
            if not ip:
                self._send_json({"error": "ip required"}, 400)
                return
            ok = _push_device(ip, model)
            self._send_json({"ok": ok})

        elif self.path == "/api/discover":
            body = self._read_body()
            params = json.loads(body) if body else {}
            subnet = params.get("subnet", "")
            # Slow part — subnet scan — happens OUTSIDE the state lock so
            # parallel discovers on different subnets don't serialize behind
            # each other. The merge into the active network record happens
            # under the lock via mutate_state.
            devices, scanned_subnet = discover_devices(subnet)
            target_subnet = _subnet_from_host_subnet(scanned_subnet)
            pushes = []   # (ip, deviceModel) tuples populated by _merge_discover

            def _merge_discover(state):
                # Attribute found devices to the network whose subnet matches
                # the scanned one. Creates a new "Network N" if none matches
                # (a future rename in the UI can adjust the name). Returns
                # the full updated state so the JS reloads the authoritative
                # shape.
                net = next((n for n in (state.get("networks") or [])
                            if n.get("subnet") == target_subnet), None)
                if net is None and devices:
                    existing = state.setdefault("networks", [])
                    name = "Home" if not existing else f"Network {len(existing) + 1}"
                    net = {"name": name, "subnet": target_subnet,
                           "wifi": {"ssid": "", "password": ""},
                           "port": "", "devices": []}
                    existing.append(net)
                if net is None:
                    return  # nothing to merge — state unchanged
                # Merge found devices into the network, keyed by MAC (the only persistent identity —
                # see _device_key). A found device with a MAC updates/creates its record, carrying the
                # user fields (deviceModel, last_port, selected) forward. A found device WITHOUT a MAC is
                # shown this scan but not persisted (nothing stable to track it by).
                by_key = {_device_key(d): d for d in net.get("devices", []) if _device_key(d)}
                merged = []
                for fresh in devices:
                    key = _device_key(fresh)
                    if not key:
                        # No MAC → not a MoonDeck-manageable device (a WLED peer, or MoonLight
                        # firmware predating the `mac` control). Dropped from the list entirely — not
                        # shown, not persisted — since MoonDeck can't flash it anyway. This is
                        # deliberate (the product-owner "MAC or it's not a device" rule), not an omission.
                        continue
                    keep = by_key.get(key, {})
                    out = {**_strip_probe_transient(fresh), "online": True,
                           "selected": keep.get("selected", False)}
                    if keep.get("deviceModel") and not out.get("deviceModel"):
                        out["deviceModel"] = keep["deviceModel"]
                    if keep.get("last_port"):
                        out["last_port"] = keep["last_port"]
                    merged.append(out)
                    # Compare the device's OWN reported model (`_reportedModel`, empty if it hasn't
                    # been told) against the merged value. When they differ — typically MoonDeck just
                    # deduced a model from firmware, or the user picked one, for a device that hasn't
                    # heard it — schedule a push so the device persists it and reports it back next scan.
                    reported_model = (fresh.get("_reportedModel") or "")
                    merged_model = (out.get("deviceModel") or "")
                    if merged_model and merged_model != reported_model:
                        pushes.append((fresh.get("ip", ""), merged_model))
                # Previously-stored (MAC'd) devices not found in this scan stay as offline, so a
                # known board isn't lost just because it's powered off. Discover is additive; refresh
                # prunes. (MAC-less entries never entered by_key, so they simply fall away.)
                found_keys = {_device_key(d) for d in devices if _device_key(d)}
                for key, dev in by_key.items():
                    if key not in found_keys:
                        merged.append({**dev, "online": False})
                # Sort the FULL list (online + carried-over offline) by name so the persisted
                # moondeck.json is stored in the same order it displays — matching the on-device
                # DevicesModule (sortByName). IP is the tiebreaker for un-named devices.
                merged.sort(key=_device_sort_key)
                net["devices"] = merged

            result = mutate_state(_merge_discover)
            # Fire pushes outside the lock — the state write has already
            # landed; pushes are best-effort device-side mirroring.
            _push_devices_in_parallel(pushes)
            # Report WHICH subnet was scanned + how many answered, so a "found
            # nothing" is distinguishable from "scanned the wrong network" (the
            # scan uses the machine's auto-detected /24, not the active network's
            # subnet). Transient, not persisted (the save already happened above).
            result = {**result, "_scanned_subnet": scanned_subnet,
                      "_found_count": len(devices)}
            self._send_json(result)

        elif self.path == "/api/refresh":
            body = self._read_body()
            params = json.loads(body) if body else {}
            network_name = params.get("network", "")
            # Read the device list snapshot under the lock, release, do the
            # slow probes outside, then re-enter mutate_state for the merge.
            # Holding the lock across the probes would serialize every refresh.
            with _state_write_lock:
                state = load_state()
                net = next((n for n in (state.get("networks") or [])
                            if n.get("name") == network_name), None)
                snapshot = list(net.get("devices") or []) if net else None
            if snapshot is None:
                # No-op when the named network doesn't exist (e.g. it was
                # renamed mid-flight). Return state so the JS can re-sync.
                self._send_json(state)
                return
            refreshed = refresh_devices(snapshot)
            pushes = []   # (ip, deviceModel) tuples populated by _merge_refresh

            def _merge_refresh(state):
                # Re-resolve `net` under the second lock — the network may have
                # been renamed / re-added by another handler while the probes
                # ran. If it's gone, drop the refresh result (the user will
                # see the empty list and re-discover).
                target = next((n for n in (state.get("networks") or [])
                               if n.get("name") == network_name), None)
                if target is None:
                    return
                # refresh_devices returns only devices that responded — devices
                # marked offline (didn't respond) are dropped from the list it
                # returns. Carry them forward as offline so the UI doesn't lose
                # known-but-unreachable entries. Persist only MAC'd devices (the identity rule):
                # a responding device without a MAC is shown but not stored; a prior MAC'd device
                # not seen this refresh stays as offline.
                refreshed_keys = {_device_key(d) for d in refreshed if _device_key(d)}
                merged = [_strip_probe_transient(d)
                          for d in refreshed if _device_key(d)]
                for prior in (target.get("devices") or []):
                    pk = _device_key(prior)
                    if pk and pk not in refreshed_keys:
                        merged.append({**prior, "online": False})
                # Sort by name so the persisted list matches the display + the on-device
                # DevicesModule order (see the discover merge above).
                merged.sort(key=_device_sort_key)
                target["devices"] = merged
                # Schedule a deviceModel push for every online device with a
                # non-empty board. Redundant writes are cheap on the device
                # (Text-control write hits a 2s debounce — repeated identical
                # writes coalesce into one disk write). Catches the case
                # where the device lost its persisted value but MoonDeck
                # still has the user-set / deduced one.
                for dev in merged:
                    if dev.get("online") and dev.get("deviceModel") and dev.get("ip"):
                        pushes.append((dev["ip"], dev["deviceModel"]))

            result = mutate_state(_merge_refresh)
            _push_devices_in_parallel(pushes)
            self._send_json(result)

        else:
            self.send_error(404)

    def _handle_run(self, script_id: str, params: dict):
        """Start a script and return immediately. Client uses SSE to stream."""
        script_def = next((s for s in SCRIPTS if s["id"] == script_id), None)
        if not script_def:
            self._send_json({"error": "unknown script"}, 404)
            return

        # Refuse to launch with a genuinely-required selector unset: the underlying
        # script declares the arg `required=True`, so running it bare just leaks
        # argparse's "the following arguments are required" usage error into the log.
        # Surface a clear message naming what to pick instead. Only port + firmware
        # are mandatory; `scenario` and `module` are OPTIONAL filters whose empty /
        # "all" value the scenario scripts accept as "run everything" (run_scenario.py
        # / run_live_scenario.py both `--name`/`--module default=None`), so they are
        # NOT in this list — requiring them here wrongly blocks the "all" case.
        REQUIRED = [("needs_port", "port", "a serial port"),
                    ("needs_firmware", "firmware", "a firmware variant")]
        missing = [label for flag, key, label in REQUIRED
                   if script_def.get(flag) and not params.get(key)]
        if missing:
            self._send_json({"error": f"Select {' and '.join(missing)} first."}, 400)
            return

        kill_script(script_id)  # Kill previous if still running

        script_path = SCRIPTS_DIR / script_def["script"]
        cmd = ["uv", "run", str(script_path)]

        # Fixed args a card always passes to its script (e.g. build_docs runs
        # with `--serve`). Distinct from `flags` (user checkboxes) and the
        # `needs_*` selectors (UI-driven values) — these are constant per card.
        cmd.extend(script_def.get("args", []))

        # Forward selector state (firmware / port / host) when the script
        # declares it needs them. The UI maintains a single Firmware dropdown
        # on the ESP32 tab driving every needs_firmware script; the older
        # per-firmware buttons + extra_args plumbing was collapsed into this.
        if script_def.get("needs_firmware") and params.get("firmware"):
            cmd.extend(["--firmware", params["firmware"]])
        if script_def.get("needs_port") and params.get("port"):
            cmd.extend(["--port", params["port"]])
            # For a port-based flash, hand the script the deviceModel of the device last
            # flashed via that port (matched by last_port in the active network), so its
            # baud resolves by the EXACT board — a per-model flashBaud opt-down (the LOLIN's
            # 460800) then can't leak to a firmware-sibling with a fine bridge (the Dig-Uno).
            # No-op when the port maps to no known device or that device has no model.
            if script_def.get("pass_port_device_model"):
                model = _device_model_for_port(params["port"])
                if model:
                    cmd.extend(["--device-model", model])
        if script_def.get("needs_scenario") and params.get("scenario"):
            cmd.extend(["--name", params["scenario"]])
        # The UI video tools take a PATH, not a name: the dropdown carries the stem and
        # the folder is implied by which selector it is, so the card cannot point at the
        # wrong kind of run file.
        # Checked against the listing, not trusted: the value arrives from a client and
        # is spliced into a path, so a name that is not one of the run files this server
        # just offered has no business reaching the filesystem.
        for flag, key, kind, arg in (("needs_uiclip", "uiclip", "clips", "--run"),
                                     ("needs_uiproject", "uiproject", "projects",
                                      "--project")):
            if not (script_def.get(flag) and params.get(key)):
                continue
            name = params[key]
            if name not in {r["name"] for r in self._list_ui_runs(kind)}:
                self._send_json({"error": f"unknown {kind[:-1]}: {name}"}, 400)
                return
            cmd.extend([arg, f"test/uiscenarios/{kind}/{name}.json"])
        if script_def.get("needs_module") and params.get("module"):
            cmd.extend(["--module", params["module"]])
        # pass_device_model: forward the deviceModel picked in the UI's provisioning
        # dropdown (state.provisionDeviceModel) so improv_provision.py injects that
        # board's TX-power cap BEFORE provisioning (the weak-power brown-out fix).
        # No firmware-deduce fallback: the only pass_device_model script
        # (improv_provision) doesn't declare needs_firmware, so params never
        # carries a firmware to deduce from — the dropdown is the sole source.
        if script_def.get("pass_device_model"):
            # The UI sends this as `device_model` (app.js sets params.device_model); accept the
            # camelCase form too so either key reaches the flag. Without the snake_case key the
            # value was silently dropped — the TX-power cap then never injected before provisioning.
            device_model = params.get("device_model") or params.get("deviceModel")
            if device_model:
                cmd.extend(["--device-model", device_model])   # improv_provision.py's CLI flag
        if params.get("host"):
            cmd.extend(["--host", params["host"]])
        for flag in script_def.get("flags", []):
            if params.get("flag_" + flag["id"]):
                cmd.append(flag["arg"])

        try:
            # Force live output: a child (build_esp32 → idf.py → ninja, esptool, …) block-buffers
            # its stdout when it detects a PIPE instead of a terminal, so nothing reaches the SSE
            # stream until it exits — a long ESP32 build dumps its whole log at once. On Unix we
            # run it under a pseudo-terminal (pty) so every process in the tree sees a TTY and
            # line-buffers naturally (the standard `unbuffer`/`script` trick); the SSE reader reads
            # the pty master. PYTHONUNBUFFERED belts-and-suspenders the Python layer, and is the
            # only lever on Windows (no pty there — ninja still chunks, but flash/monitor improve).
            env = {**os.environ, "PYTHONUNBUFFERED": "1"}
            popen_kwargs = dict(cwd=str(ROOT), env=env)
            read_stream = None       # what the SSE reader reads (pty master, or proc.stdout)
            master_fd = None         # pty master to close on teardown (Unix only)
            slave_fd = None          # pty slave — closed in parent right after spawn (or on failure)
            if _IS_WIN:
                popen_kwargs["stdout"] = subprocess.PIPE
                popen_kwargs["stderr"] = subprocess.STDOUT
                popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            else:
                import pty
                master_fd, slave_fd = pty.openpty()
                popen_kwargs["stdout"] = slave_fd
                popen_kwargs["stderr"] = slave_fd
                popen_kwargs["start_new_session"] = True
            try:
                proc = subprocess.Popen(cmd, **popen_kwargs)
            except Exception:
                # Popen failed (bad cmd, uv not on PATH): close both pty fds so a failed launch
                # doesn't leak two fds per attempt in this long-running server, then re-raise.
                for fd in (master_fd, slave_fd):
                    if fd is not None:
                        with suppress(OSError):
                            os.close(fd)
                raise
            if _IS_WIN:
                read_stream = proc.stdout
            else:
                os.close(slave_fd)   # parent doesn't write; child owns the slave end
                # Buffered binary reader over the pty master. Its read raises OSError (EIO) when
                # the child exits and closes the slave — the SSE loop treats that as clean EOF.
                read_stream = os.fdopen(master_fd, "rb", buffering=0)
            # Stash the read stream + master fd on the proc so _handle_stream can find them (kept in
            # _running keyed by proc). kill_script closes the master fd too, so a killed-before-
            # streamed proc doesn't orphan it.
            proc._mm_read_stream = read_stream
            proc._mm_master_fd = master_fd
            with _lock:
                _running[script_id] = proc
            self._send_json({"status": "started", "pid": proc.pid})
        except Exception as e:
            self._send_json({"error": str(e)}, 500)

    def _handle_stream(self, script_id: str):
        """SSE endpoint: stream stdout of a running script."""
        with _lock:
            proc = _running.get(script_id)

        if not proc:
            self.send_error(404, "No running process")
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        # Read from the pty master (Unix) or the pipe (Windows) — see _handle_run. On a pty, the
        # master read raises OSError (EIO) once the child exits and closes the slave; treat that as
        # EOF, same as the pipe's b"" sentinel.
        stream = getattr(proc, "_mm_read_stream", None) or proc.stdout

        # Wall clock, from the first read to the child's exit. Monotonic, so an NTP step or a DST
        # change mid-run cannot report a negative or wildly wrong duration.
        started = time.monotonic()

        # Tee to disk as we stream, rather than buffering and writing at the end: a long run
        # killed with Stop (or a crashed server) still leaves everything it printed up to
        # that point, which is exactly when you want the log.
        log = None
        log_bytes = 0
        with suppress(OSError):
            LOG_DIR.mkdir(parents=True, exist_ok=True)
            log = (LOG_DIR / f"{script_id}.log").open("w", encoding="utf-8")
            # Timezone-aware: a bare local timestamp is ambiguous when the log is read from
            # another machine or after a DST change.
            log.write(f"# {script_id} — {datetime.now().astimezone().isoformat(timespec='seconds')}\n")

        try:
            while True:
                try:
                    line = stream.readline()
                except OSError:
                    break            # pty EIO on child exit → EOF
                except ValueError:
                    # The stop button: kill_script closes the pty master fd while this thread is
                    # blocked in readline, and reading a CLOSED Python file raises ValueError, not
                    # OSError. Uncaught it takes down the whole request handler with a traceback,
                    # so stopping a long-running card looked like a MoonDeck crash.
                    break
                if not line:
                    break            # pipe EOF
                text = line.decode("utf-8", errors="replace").rstrip("\r\n")
                if log:
                    # Every write is best-effort: a full disk or a removed build/ must not kill
                    # the stream the user is watching.
                    try:
                        if log_bytes < LOG_MAX_BYTES:
                            log_bytes += log.write(text + "\n")
                            log.flush()
                            if log_bytes >= LOG_MAX_BYTES:
                                log.write(f"\n[log truncated at {LOG_MAX_BYTES} bytes]\n")
                                log.flush()
                    except OSError:
                        with suppress(OSError):
                            log.close()
                        log = None
                self.wfile.write(f"data: {json.dumps(text)}\n\n".encode())
                self.wfile.flush()

            proc.wait()
            exit_msg = (f"[exit code: {proc.returncode}] [{_duration(time.monotonic() - started)}]"
                        f" [run #{bump_run_count(script_id)}]")
            if log:
                with suppress(OSError):
                    log.write(exit_msg + "\n")   # always recorded, even past the cap
            self.wfile.write(f"data: {json.dumps(exit_msg)}\n\n".encode())
            done_data = json.dumps({"exitCode": proc.returncode})
            self.wfile.write(f"event: done\ndata: {done_data}\n\n".encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            if log:
                with suppress(OSError):
                    log.close()
            # Close the pty master fd (Unix) so the kernel reclaims it; the pipe closes with proc.
            master_fd = getattr(proc, "_mm_master_fd", None)
            if master_fd is not None:
                with suppress(OSError):
                    stream.close()
            # Pop only if THIS proc is still the registered one — a re-run under the same id
            # (fast Run→Run) may have replaced it while this stream was finishing; popping then
            # would drop the NEW proc and break its Stop button.
            with _lock:
                if _running.get(script_id) is proc:
                    _running.pop(script_id, None)

    def _serve_doc(self):
        """Serve any docs/**/*.md file as styled HTML with deep-link anchor support.
        URL: /api/docs/<path>[?#anchor] — e.g. /api/docs/reference/testing.md, /api/docs/reference/tests/unit-tests.md"""
        import re as _re
        raw_path = self.path[len("/api/docs/"):]
        parts = raw_path.split("?", 1)
        filename = parts[0].strip("/")
        raw_anchor = parts[1] if len(parts) > 1 else ""
        anchor = raw_anchor if _re.fullmatch(r"[A-Za-z0-9._-]+", raw_anchor) else ""
        # Restrict to .md files and resolve under docs/ with a traversal guard:
        # build the candidate path, resolve symlinks, then verify it sits inside docs/.
        # Allows subpaths like tests/unit-tests.md while still rejecting ../escape attempts.
        if not filename.endswith(".md") or ".." in filename.split("/"):
            self.send_error(400, "Only .md files under docs/ are served here")
            return
        docs_root = (ROOT / "docs").resolve()
        md_path = (docs_root / filename).resolve()
        try:
            md_path.relative_to(docs_root)
        except ValueError:
            self.send_error(400, "Path escapes docs/")
            return
        if not md_path.exists():
            self.send_error(404, f"{filename} not found")
            return
        self._serve_markdown_as_html(md_path, anchor)

    def _list_scenarios(self):
        """Return [{name, module, also}] for every scenario JSON.

        The list endpoint surfaces module so MoonDeck's dropdown can filter
        without an extra round-trip per scenario."""
        return [
            {"name": s["path"].stem, "module": s["module"] or "", "also": s["also"]}
            for s in test_meta.collect_scenario_files()
        ]

    def _list_ui_runs(self, kind: str):
        """Return [{name, description}] for every UI clip or project.

        Both live under test/uiscenarios/, the same split the tooling uses: a CLIP is
        performed against a device and recorded, a PROJECT cuts clips together. The
        description rides along so the dropdown can say what each one does rather than
        only naming a file.
        """
        folder = SCRIPTS_DIR.parent / "test" / "uiscenarios" / kind
        out = []
        for path in sorted(folder.glob("*.json")):
            try:
                data = json.loads(path.read_text())
            except (OSError, json.JSONDecodeError):
                continue          # a malformed run must not empty the whole dropdown
            if not isinstance(data, dict):
                continue          # a JSON list or string has no description to read
            out.append({"name": path.stem,
                        "description": data.get("description", "")})
        return out

    def _serve_unit_tests_for_module(self):
        """Render a per-module list of unit-test cases as an HTML view.
        URL: /api/unit-tests/<Module> — `Module` is the CamelCase @module name."""
        import html as html_mod

        raw = self.path[len("/api/unit-tests/"):].split("?", 1)[0].strip("/")
        if not raw or not all(c.isalnum() or c in "-_" for c in raw):
            self.send_error(400, "Bad module name")
            return

        cases = test_meta.cases_for_module(raw)
        if not cases:
            self.send_error(404, f"No unit tests found for module {raw}")
            return

        rows = []
        for i, c in enumerate(cases):
            desc_html = html_mod.escape(c["desc"]) if c["desc"] else f'<em>{html_mod.escape(c["name"])}</em>'
            tag = '' if c["primary"] else ' <span class="also">(also)</span>'
            rows.append(
                f'<div class="case"><div class="case-head">'
                f'<span class="case-num">{i + 1}.</span> '
                f'<span class="case-name">{html_mod.escape(c["name"])}</span>{tag}'
                f'</div><div class="case-desc">{desc_html}</div>'
                f'<div class="case-file"><code>{html_mod.escape(c["file"])}</code></div></div>'
            )

        body_html = "\n".join(rows)
        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; margin: 0 0 4px 0; }}
.sub {{ color: #9aa6ba; margin: 0 0 18px 0; font-size: 12px; }}
.case {{ margin: 6px 0 10px 0; padding: 6px 10px; background: #161b22;
         border-radius: 4px; border-left: 3px solid #0f3460; }}
.case-head {{ font-size: 13px; }}
.case-num {{ color: #6a7a99; }}
.case-name {{ color: #e94560; font-weight: 600; }}
.case-desc {{ margin-top: 2px; color: #c0c0c0; }}
.case-file {{ margin-top: 2px; color: #6a7a99; font-size: 11px; }}
.also {{ color: #6a7a99; font-size: 11px; margin-left: 4px; }}
code {{ background: transparent; color: #8aa6ba; padding: 0; }}
</style></head><body>
<h1>{html_mod.escape(raw)} unit tests</h1>
<div class="sub">{len(cases)} test case(s). "(also)" marks cases from files whose primary @module is a different module.</div>
{body_html}
</body></html>"""

        data_bytes = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data_bytes)))
        self.end_headers()
        self.wfile.write(data_bytes)

    def _serve_scenario_steps(self):
        """Render a single test/scenarios/<name>.json as an HTML view of its steps.
        URL: /api/scenarios/<name> — `name` is the file stem (no .json suffix), same
        names /api/scenarios returns. The view pane gets one card per step showing
        op, name, and the rest of the step's keys/values verbatim. Lightweight on
        purpose — the test runner is the source of truth for what each op means."""
        import html as html_mod

        raw = self.path[len("/api/scenarios/"):].split("?", 1)[0].strip("/")
        # Restrict to file-stem characters (no path traversal, no .json suffix expected)
        if not raw or not all(c.isalnum() or c in "-_" for c in raw):
            self.send_error(400, "Bad scenario name")
            return
        # Scenarios live in subfolders (core/, light/, …) — find by stem.
        path = test_meta.find_scenario_path(raw)
        if not path:
            self.send_error(404, f"{raw} not found")
            return
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            self.send_error(500, f"Invalid JSON in {raw}.json: {e}")
            return

        # Build a compact per-step list. Each step has at minimum an `op`; we render
        # whatever other keys it carries (id, type, parent_id, key, value, props, bounds, …)
        # as a small definition list so the schema can evolve without code change.
        scen_name = html_mod.escape(str(data.get("name", raw)))
        scen_desc = html_mod.escape(str(data.get("description", "")))
        scen_module = html_mod.escape(str(data.get("module", "")))
        scen_also = data.get("also") or []
        scen_also_html = (
            f'<div class="also">Also touches: {html_mod.escape(", ".join(scen_also))}</div>'
            if scen_also else ""
        )
        scen_module_html = (
            f'<div class="module">Module: <strong>{scen_module}</strong></div>'
            if scen_module else ""
        )
        steps = data.get("steps", []) or []

        rows = []
        for i, step in enumerate(steps):
            op = html_mod.escape(str(step.get("op", "?")))
            step_name = html_mod.escape(str(step.get("name", "")))
            step_desc = html_mod.escape(str(step.get("description", "")))
            # `contract` and `observed` are the per-target performance data
            # and render as a single shared table (same shape as
            # docs/reference/tests/scenario-tests.md — see test_doc_gen._format_perf_table).
            # Everything else stays in the JSON-dump key/value list below.
            perf_html = _render_perf_table_html(step)
            other = {k: v for k, v in step.items()
                     if k not in ("op", "name", "description", "contract", "observed")}
            kv_html = ""
            if other:
                parts = []
                for k, v in other.items():
                    v_str = json.dumps(v) if not isinstance(v, str) else v
                    parts.append(
                        f'<div><code>{html_mod.escape(k)}</code> = '
                        f'<code>{html_mod.escape(v_str)}</code></div>'
                    )
                kv_html = '<div class="step-kv">' + "".join(parts) + "</div>"
            desc_html = f'<div class="step-desc">{step_desc}</div>' if step_desc else ""
            rows.append(
                f'<div class="step"><div class="step-head">'
                f'<span class="step-num">{i + 1}.</span> '
                f'<span class="step-op">{op}</span>'
                f'{f" <span class=\"step-name\">{step_name}</span>" if step_name else ""}'
                f'</div>{desc_html}{kv_html}{perf_html}</div>'
            )
        body_html = "\n".join(rows) if rows else "<p><em>(no steps)</em></p>"

        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; margin: 0 0 4px 0; }}
.desc {{ color: #9aa6ba; margin: 0 0 18px 0; font-size: 12px; }}
.step {{ margin: 6px 0 10px 0; padding: 6px 10px; background: #161b22;
         border-radius: 4px; border-left: 3px solid #0f3460; }}
.step-head {{ font-size: 13px; }}
.step-num {{ color: #6a7a99; }}
.step-op {{ color: #e94560; font-weight: 600; }}
.step-name {{ color: #9aa6ba; margin-left: 6px; }}
.step-desc {{ margin-top: 2px; color: #c0c0c0; }}
.step-kv {{ margin-top: 4px; padding-left: 14px; font-size: 12px; }}
.step-kv > div {{ margin: 2px 0; }}
.module {{ color: #9aa6ba; font-size: 12px; margin: 0 0 2px 0; }}
.module strong {{ color: #e94560; }}
.also {{ color: #6a7a99; font-size: 11px; margin: 0 0 12px 0; }}
code {{ background: transparent; color: #c0c0c0; padding: 0; }}
.step-kv code:first-child {{ color: #8aa6ba; }}
/* Perf table — same shape as docs/reference/tests/scenario-tests.md per-step table */
.perf {{ margin-top: 6px; }}
.perf-head {{ font-size: 12px; color: #9aa6ba; margin: 4px 0 2px 0; }}
.perf-table {{ border-collapse: collapse; font-size: 12px; margin: 2px 0; }}
.perf-table th, .perf-table td {{ padding: 2px 8px; text-align: left;
                                   border-bottom: 1px solid #1c2535; }}
.perf-table th {{ color: #8aa6ba; font-weight: 500; }}
.perf-audit {{ font-size: 11px; color: #6a7a99; margin: 2px 0 0 8px; }}
</style></head><body>
<h1>{scen_name}</h1>
{scen_module_html}
{f'<div class="desc">{scen_desc}</div>' if scen_desc else ''}
{scen_also_html}
{body_html}
</body></html>"""

        data_bytes = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data_bytes)))
        self.end_headers()
        self.wfile.write(data_bytes)

    def _serve_log(self, script_id: str) -> None:
        """The last run's output for one script, as plain text.

        404 when a script has not run since the server had a log dir — the UI treats that as
        "no previous run" rather than an error. Reads at most LOG_MAX_BYTES: the writer caps
        too, but a log from an older build could predate that cap.
        """
        import re as _re
        if not _re.fullmatch(r"[A-Za-z0-9_-]+", script_id or ""):
            self.send_error(400, "bad script id")
            return
        path = LOG_DIR / f"{script_id}.log"
        if not path.exists():
            self.send_error(404, "no log for this script yet")
            return
        with path.open("rb") as f:
            body = f.read(LOG_MAX_BYTES)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_help(self):
        """Serve MoonDeck.md as styled HTML with deep-link anchor support."""
        md_path = SCRIPTS_DIR / "MoonDeck.md"
        if not md_path.exists():
            self.send_error(404, "MoonDeck.md not found")
            return
        raw = self.path.split("?", 1)[1] if "?" in self.path else ""
        import re as _re
        anchor = raw if _re.fullmatch(r"[A-Za-z0-9._-]+", raw) else ""
        self._serve_markdown_as_html(md_path, anchor)

    def _serve_history_report(self):
        """Serve build/history.md (generated by history_report.py) as HTML
        through the same renderer the help pages use. Iframes can't load
        file:// from an http:// parent, so we serve the file through the
        MoonDeck origin instead — same trick /api/help already uses."""
        md_path = ROOT / "build" / "history.md"
        if not md_path.exists():
            self.send_error(
                404,
                "build/history.md not found — run the History Report button first.",
            )
            return
        self._serve_markdown_as_html(md_path, "")

    def _serve_doc_asset(self):
        """Serve a static asset (image, etc.) referenced from a rendered doc.

        Path: /api/doc-asset/<ROOT-relative-path>
        The renderer resolves relative image src values to ROOT-relative paths
        before building the URL, so this handler only needs a simple join."""
        import mimetypes
        from urllib.parse import unquote
        # URL-decode the path: a doc image with a space in its name is written `Hue%20driver.png` in
        # the markdown, so the request path carries `%20`; without decoding, the file lookup would seek
        # a literal "%20" in the name and 404.
        rel = unquote(self.path[len("/api/doc-asset/"):])
        # Resolve against ROOT and ensure no escape.
        try:
            asset_path = (ROOT / rel).resolve()
            ROOT.resolve()  # ensure ROOT itself is resolved
            asset_path.relative_to(ROOT.resolve())  # raises ValueError if escape
        except (ValueError, OSError):
            self.send_error(403, "Forbidden")
            return
        if not asset_path.exists() or not asset_path.is_file():
            self.send_error(404, f"Asset not found: {rel}")
            return
        mime, _ = mimetypes.guess_type(str(asset_path))
        data = asset_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime or "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_firmware_bin(self):
        """Serve a local firmware image for a device OTA: GET /firmware/<fw>.bin →
        build/esp32-<fw>/projectMM.bin. The device (handed this URL by /api/ota) fetches it over
        the LAN and flashes it. Only the exact <fw>.bin shape is served, mapped to the known build
        dir — no arbitrary path, so this can't read outside build/esp32-*/."""
        fw = self.path[len("/firmware/"):-len(".bin")]
        if not fw or "/" in fw or ".." in fw:
            self.send_error(400, "bad firmware name")
            return
        bin_path = ROOT / "build" / f"esp32-{fw}" / "projectMM.bin"
        if not bin_path.exists():
            self.send_error(404, f"no build for {fw}")
            return
        data = bin_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_markdown_as_html(self, md_path, anchor):
        """Render a markdown file to HTML for the View pane. Handles
        headings (with id slugs for deep-linking), fenced code blocks,
        tables, list items, blockquotes, and the inline forms
        (**bold**, `code`, [text](url), italic-via-_underscore_).
        Deliberately not a full CommonMark renderer — just the subset the
        repo's markdown files actually use."""
        import html as html_mod
        import re

        text = md_path.read_text(encoding="utf-8")
        lines: list[str] = []
        in_code = False
        in_list = False           # top-level <ul> open?
        in_quote = False          # <blockquote> open?
        in_quote_list = False     # nested <ul> inside a blockquote open?
        in_table = False

        def render_inline(s: str) -> str:
            """Apply inline markdown to one already-HTML-escaped string.

            Order matters: code first (fenced text inside backticks must
            not get bold/italic-rendered); then links; then bold; then
            italics. Each pass uses placeholder-free regexes that are
            safe to chain because we only ever replace md tokens with
            HTML tags (no nested-markup confusion in the inputs we see)."""
            # `code` — exclude backticks themselves
            s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
            # ![alt](url) — images (must come before link regex to avoid partial match)
            def _img_tag(m):
                alt_, src_ = m.group(1), m.group(2)
                # Resolve relative path from md file's directory to a
                # ROOT-relative path, then serve via /api/doc-asset/.
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        root_rel = abs_src.relative_to(ROOT.resolve())
                        src_ = str(root_rel)
                    except ValueError:
                        pass  # outside ROOT — keep original path
                return f'<img src="/api/doc-asset/{src_}" alt="{html_mod.escape(alt_)}" style="max-width:100%;margin:4px 0;">'
            s = re.sub(r'!\[([^\]]*)\]\(([^)]+)\)', _img_tag, s)
            # [text](url) — same-origin /api/ links post a message to the
            # parent frame (iframe nav is sandboxed); external links open in
            # a new tab. Relative `.md` links are rewritten to /api/docs/<path>
            # so the rendered page stays navigable when served through MoonDeck
            # (the docs are also valid when read straight from the repo: same
            # paths, different host).
            def _link_tag(m):
                import urllib.parse as _up
                text_, url_ = m.group(1), m.group(2)
                if url_.startswith("/api/"):
                    return f'<a href="{url_}" data-moondeck-nav="1">{text_}</a>'
                # Relative .md link (with optional #anchor) → resolve against the
                # current file's directory, re-anchor under docs/, serve via /api/docs/.
                parsed = _up.urlparse(url_)
                if (not parsed.scheme and not url_.startswith("/")
                        and parsed.path.endswith(".md")):
                    try:
                        abs_md = (md_path.parent / parsed.path).resolve()
                        rel = abs_md.relative_to((ROOT / "docs").resolve())
                        api_url = "/api/docs/" + str(rel)
                        if parsed.fragment:
                            api_url += "?" + parsed.fragment
                        return f'<a href="{api_url}" data-moondeck-nav="1">{text_}</a>'
                    except ValueError:
                        pass  # outside docs/ — fall through to default handling
                scheme = parsed.scheme
                if scheme not in ("", "http", "https", "mailto"):
                    return html_mod.escape(text_)  # strip unsafe schemes (e.g. javascript:)
                return f'<a href="{url_}" target="_blank" rel="noopener">{text_}</a>'
            s = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', _link_tag, s)
            # **bold**
            s = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', s)
            # _italic_ (underscore form only — asterisk-italic is ambiguous
            # next to **bold** and the repo doesn't use it)
            s = re.sub(r'(?<!\w)_([^_]+)_(?!\w)', r'<em>\1</em>', s)
            return s

        def _render_cell(c: str) -> str:
            """Render one table cell. The compact module pages (effects/modifiers/layouts)
            use raw <img src=… width=…> previews and <a id=…></a> row anchors inside cells —
            two tags GitHub/VS Code honor but the default escape path would turn to literal
            text. Pass those two through (resolving an <img> src to /api/doc-asset/ like the
            markdown-image path does); escape + inline-render the rest."""
            def _img_attrs(tag: str) -> dict:
                """Pull src/width/alt/style from an <img …> tag regardless of attribute ORDER — each
                is matched by its own name="value" search, so an author may write them in any sequence
                (the previous fixed src→width→alt→style regex silently dropped out-of-order attrs)."""
                def attr(name):
                    m = re.search(rf'\b{name}="([^"]*)"', tag)
                    return m.group(1) if m else None
                return {k: attr(k) for k in ("src", "width", "alt", "style")}
            def _img(a):
                src_ = a.get("src") or ""
                width = a.get("width")
                style = a.get("style")
                alt_ = a.get("alt")
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        src_ = str(abs_src.relative_to(ROOT.resolve()))
                    except ValueError:
                        pass
                    src_ = f"/api/doc-asset/{src_}"
                # Escape every attribute value that reaches the HTML — src/width/style as well as alt —
                # so a doc-page value with a quote or angle bracket can't break the tag or inject markup.
                wattr = f' width="{html_mod.escape(width)}"' if width else ""
                altattr = f' alt="{html_mod.escape(alt_)}"' if alt_ else ""
                # Preserve an author-set width style (the cross-renderer size lever) and append our
                # margin so the preview isn't flush against the cell edges.
                style = (style + ";" if style else "") + "margin:4px 0"
                return f'<img src="{html_mod.escape(src_)}"{wattr}{altattr} style="{html_mod.escape(style)}">'
            # No raw HTML → ordinary escaped+inline cell (the common case).
            if "<img" not in c and "<a id=" not in c and "<br" not in c:
                return render_inline(html_mod.escape(c))
            # Protect the few tags the module-doc cells use (img preview, row anchor, <br> line
            # breaks in a "card" cell), escape the rest, render markdown, then restore them.
            tokens = []
            def _stash(html: str) -> str:
                tokens.append(html)
                return f"\x00{len(tokens)-1}\x00"
            # Match the <img …> envelope, then extract attributes by name (order-independent).
            c = re.sub(r'<img\b[^>]*>', lambda m: _stash(_img(_img_attrs(m.group(0)))), c)
            c = re.sub(r'<a id="([a-z0-9-]+)"></a>', lambda m: _stash(f'<a id="{m.group(1)}"></a>'), c)
            c = re.sub(r'<br\s*/?>', lambda _: _stash("<br>"), c)
            out = render_inline(html_mod.escape(c))
            for i, html in enumerate(tokens):
                out = out.replace(f"\x00{i}\x00", html)
            return out

        def close_list_if_open():
            nonlocal in_list
            if in_list:
                lines.append("</ul>")
                in_list = False

        def close_quote_if_open():
            nonlocal in_quote, in_quote_list
            if in_quote_list:
                lines.append("</ul>")
                in_quote_list = False
            if in_quote:
                lines.append("</blockquote>")
                in_quote = False

        def close_table_if_open():
            nonlocal in_table
            if in_table:
                lines.append("</tbody></table>")
                in_table = False

        def close_blocks():
            close_list_if_open()
            close_quote_if_open()
            close_table_if_open()

        _explicit_id_re = re.compile(r'\{#([A-Za-z0-9._-]+)\}\s*$')
        _allowed_html_re = re.compile(
            r'^</?(?:div|p|span|table|thead|tbody|tr|td|th|ul|ol|li|br|hr'
            r'|strong|em|code|pre|a|h[1-6])[\s>"/]'
        )

        def _heading_slug(text: str) -> tuple[str, str]:
            m_id = _explicit_id_re.search(text)
            if m_id:
                return m_id.group(1), text[:m_id.start()].strip()
            return text.lower().replace(" ", "_"), text

        for raw_line in text.splitlines():
            # Fenced code block toggle. Strip the optional language tag.
            stripped = raw_line.strip()
            if stripped.startswith("```"):
                close_blocks()
                if in_code:
                    lines.append("</code></pre>")
                else:
                    lines.append("<pre><code>")
                in_code = not in_code
                continue
            if in_code:
                lines.append(html_mod.escape(raw_line))
                continue

            # Tables — `| col | col |` lines + a separator row `|---|---|`.
            # We detect the table by the leading `|`; the separator row is
            # skipped (it's just markdown formatting, not data).
            if raw_line.startswith("|") and raw_line.rstrip().endswith("|"):
                close_list_if_open()
                close_quote_if_open()
                # Separator row: |---|---| (all cells are dashes / colons)
                inner = raw_line.strip().strip("|")
                cells = [c.strip() for c in inner.split("|")]
                if all(re.fullmatch(r":?-+:?", c) for c in cells):
                    continue  # skip the alignment row
                if not in_table:
                    lines.append('<table><tbody>')
                    in_table = True
                # First content row → header row (the row above the
                # separator); subsequent rows are body. We don't track
                # which is which precisely — render all as <td> and let
                # CSS handle the first-row styling.
                cell_tag = "td"
                cell_html = "".join(
                    f"<{cell_tag}>{_render_cell(c)}</{cell_tag}>"
                    for c in cells
                )
                lines.append(f"<tr>{cell_html}</tr>")
                continue
            close_table_if_open()

            # Blockquote — `> text` (with optional leading whitespace from
            # nested list-item quotes like the history report's two-space
            # indent before `>`). Inside a blockquote we recognize `- foo`
            # rows as a nested `<ul>` so commit bodies with dashed-list
            # paragraphs render as real bullet lists. Non-list lines get
            # a trailing `<br>` so source-level newlines survive (the
            # browser otherwise collapses all-but-paragraph whitespace
            # and the commit body becomes one long flowing string).
            quote_match = re.match(r"^(\s*)> ?(.*)$", raw_line)
            if quote_match:
                close_list_if_open()
                if not in_quote:
                    lines.append("<blockquote>")
                    in_quote = True
                quote_content = quote_match.group(2)
                if quote_content.startswith("- "):
                    # Nested list item. Open the <ul> the first time.
                    if not in_quote_list:
                        lines.append("<ul>")
                        in_quote_list = True
                    item = quote_content[2:]
                    lines.append(f"<li>{render_inline(html_mod.escape(item))}</li>")
                    continue
                # Not a list item — close any open nested list before
                # rendering the line as flowing text.
                if in_quote_list:
                    lines.append("</ul>")
                    in_quote_list = False
                if quote_content == "":
                    lines.append("<br>")
                else:
                    lines.append(render_inline(html_mod.escape(quote_content)) + "<br>")
                continue
            # If a blockquote just ended, close any nested <ul> too.
            if in_quote and in_quote_list:
                lines.append("</ul>")
                in_quote_list = False
            close_quote_if_open()

            # Unordered list — `- text` at column 0.
            if raw_line.startswith("- "):
                if not in_list:
                    lines.append("<ul>")
                    in_list = True
                item = raw_line[2:]
                lines.append(f"<li>{render_inline(html_mod.escape(item))}</li>")
                continue
            close_list_if_open()

            stripped_check = raw_line.strip()

            # A standalone <img …> line (the per-module doc pages put the preview gif on its own
            # line above the description). Resolve a relative src to /api/doc-asset/ and keep the
            # width, like the table-cell path does — the allowlist below doesn't cover <img>.
            if re.fullmatch(r'<img\b[^>]*>(?:\s*<!--.*-->)?', stripped_check):
                # Extract each attribute by name, order-independent (an author may write src/alt/width
                # in any sequence — a fixed-order regex would silently drop the out-of-order ones).
                def _attr(name):
                    m = re.search(rf'\b{name}="([^"]*)"', stripped_check)
                    return m.group(1) if m else None
                src_ = _attr("src") or ""
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        src_ = f"/api/doc-asset/{abs_src.relative_to(ROOT.resolve())}"
                    except ValueError:
                        pass
                w_ = _attr("width")
                # Escape every attribute value before it reaches the HTML (like _render_cell._img does)
                # so a doc-page src/width/alt with a quote or bracket can't break the tag or inject markup.
                wattr = f' width="{html_mod.escape(w_)}"' if w_ else ""
                alt_ = _attr("alt")
                aattr = f' alt="{html_mod.escape(alt_)}"' if alt_ is not None else ""
                lines.append(f'<img src="{html_mod.escape(src_)}"{wattr}{aattr} style="margin:4px 0">')
                continue

            # Pass-through for a fixed allowlist of structural HTML tags used
            # by history_report.py's combined graph+commits output. Narrowed
            # to known safe tags so arbitrary doc content can't inject scripts.
            if (stripped_check.startswith("<")
                    and stripped_check.endswith(">")
                    and _allowed_html_re.match(stripped_check)):
                lines.append(raw_line)
                continue

            # Headings, then blank → spacer, then plain paragraph.
            # {#explicit-id} suffix overrides the auto-slug.
            if raw_line.startswith("### "):
                slug, heading_text = _heading_slug(raw_line[4:].strip())
                lines.append(f'<h3 id="{slug}">{render_inline(html_mod.escape(heading_text))}</h3>')
            elif raw_line.startswith("## "):
                slug, heading_text = _heading_slug(raw_line[3:].strip())
                lines.append(f'<h2 id="{slug}">{render_inline(html_mod.escape(heading_text))}</h2>')
            elif raw_line.startswith("# "):
                lines.append(f'<h1>{render_inline(html_mod.escape(raw_line[2:]))}</h1>')
            elif raw_line.strip() == "":
                lines.append("<br>")
            else:
                lines.append(f"<p>{render_inline(html_mod.escape(raw_line))}</p>")

        close_blocks()
        if in_code:
            # Defensive — unbalanced fence in source shouldn't crash.
            lines.append("</code></pre>")

        body_html = "\n".join(lines)
        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; }}
h2 {{ color: #e94560; font-size: 15px; border-bottom: 1px solid #0f3460; padding-bottom: 4px; margin-top: 22px; }}
h3 {{ color: #e94560; font-size: 13px; margin-top: 18px; }}
pre {{ background: #161b22; padding: 10px; border-radius: 4px; overflow-x: auto;
       font-size: 11px; line-height: 1.35; }}
code {{ font-size: 12px; background: #161b22; padding: 0 4px; border-radius: 3px; }}
pre code {{ background: transparent; padding: 0; }}
p {{ margin: 2px 0; }}
ul {{ margin: 4px 0 8px 0; padding-left: 22px; }}
li {{ margin: 4px 0; }}
blockquote {{ margin: 4px 0 8px 22px; padding-left: 10px;
              border-left: 2px solid #0f3460; color: #9aa6ba; }}
table {{ border-collapse: collapse; margin: 8px 0; }}
td {{ padding: 4px 10px; border: 1px solid #0f3460; }}
tr:first-child td {{ background: #0f3460; color: #e94560; font-weight: 600; }}
a {{ color: #e94560; text-decoration: none; }}
a:hover {{ text-decoration: underline; }}
strong {{ color: #fff; }}

/* History report: combined graph + commits section. The rail is monospace
 * (matches git log --graph's ASCII characters); each commit's body
 * blockquote already has a left border that visually extends the rail's
 * vertical lines into the description. */
.hr-line {{ font-family: ui-monospace, monospace; color: #6a7a99;
            font-size: 11px; line-height: 1.4; margin: 0; }}
.hr-commit {{ margin: 6px 0; }}
.hr-head {{ font-family: ui-monospace, monospace; font-size: 12px;
            line-height: 1.4; }}
.hr-rail {{ color: #6a7a99; white-space: pre; }}
.hr-merge {{ color: #e94560; }}
.hr-date {{ color: #6a7a99; font-size: 11px; }}
.hr-commit blockquote {{ margin-left: 30px; }}
</style></head><body>
{body_html}
{f'''<script>
// Wait for images so the anchor lands at the right position. scrollIntoView
// fired before image load left the viewport on an earlier section once the
// images finished loading and pushed content down.
(function() {{
  var anchor = "{anchor}";
  function jump() {{
    var el = document.getElementById(anchor);
    if (el) el.scrollIntoView();
  }}
  var imgs = Array.from(document.images || []);
  var pending = imgs.filter(function(i) {{ return !i.complete; }});
  if (pending.length === 0) {{ jump(); return; }}
  var left = pending.length;
  pending.forEach(function(img) {{
    img.addEventListener("load",  function() {{ if (--left === 0) jump(); }});
    img.addEventListener("error", function() {{ if (--left === 0) jump(); }});
  }});
  // Safety net: never wait more than 1.5s for images.
  setTimeout(jump, 1500);
}})();
</script>''' if anchor else ''}
<script>
document.addEventListener("click", function(e) {{
    var a = e.target.closest("a[data-moondeck-nav]");
    if (!a) return;
    e.preventDefault();
    window.parent.postMessage({{type:"moondeck-nav", url: a.getAttribute("href")}}, "*");
}});
</script>
</body></html>"""

        data = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_static(self):
        """Serve files from moondeck_ui/ and docs/assets/."""
        # Strip query string before resolving path (e.g. /?tab=desktop → /)
        raw = self.path.split("?", 1)[0].lstrip("/")
        path = raw if raw else "index.html"

        # Serve /assets/* from docs/assets/
        if path.startswith("assets/"):
            file_path = ASSETS_DIR / path.removeprefix("assets/")
        else:
            file_path = UI_DIR / path

        if not file_path.exists() or not file_path.is_file():
            self.send_error(404)
            return

        content_types = {
            ".html": "text/html",
            ".css": "text/css",
            ".js": "application/javascript",
            ".json": "application/json",
            ".png": "image/png",
            ".svg": "image/svg+xml",
        }
        ext = file_path.suffix.lower()
        content_type = content_types.get(ext, "application/octet-stream")

        data = file_path.read_bytes()
        # index.html carries a {{VERSION}} placeholder filled at serve time.
        if path == "index.html":
            data = data.replace(b"{{VERSION}}", APP_VERSION.encode())
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # A prior MoonDeck already bound to PORT would make the fresh bind fail
    # ("Address already in use"); replace it so a re-run just works. Also picks up
    # any deviceModels.json / firmwares.json edits, since the catalog is loaded at
    # module import (see _load_device_models). Set MOONDECK_NO_KILL=1 to opt out and
    # run a second instance on a different PORT instead.
    if not os.environ.get("MOONDECK_NO_KILL"):
        _free_port(PORT)
    # ThreadingHTTPServer binds to "" → all interfaces, so MoonDeck is reachable
    # from other devices on the LAN, not just this machine.
    server = http.server.ThreadingHTTPServer(("", PORT), MoonDeckHandler)
    print(f"MoonDeck running at http://localhost:{PORT}")
    ip = _lan_ip()
    if ip:
        print(f"  on the network:   http://{ip}:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        # Kill any running scripts
        for sid in list(_running.keys()):
            kill_script(sid)
        server.server_close()


if __name__ == "__main__":
    main()

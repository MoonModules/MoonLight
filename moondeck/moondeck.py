#!/usr/bin/env python3
"""MoonDeck — browser-based developer console for MoonLight."""

import http.server
import json
import os
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from contextlib import suppress
from datetime import datetime
from pathlib import Path

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
SHARED_UI_KINDS = {".js": "application/javascript", ".css": "text/css"}


def shared_ui_file(path: str) -> tuple[Path, str] | None:
    """The device UI's own file a /ui/<name> request names, with its type: a script or a sheet in src/ui, the kinds a desktop app serves
    from a checkout (HttpServerModule's isBundledName), so the Live tab's map and popup load the modules they import, a new one included,
    and the folder's other files stay private. None for any other path."""
    m = re.fullmatch(r"/ui/([\w.-]+)", path)
    file = ROOT / "src" / "ui" / m.group(1) if m else None
    return (file, SHARED_UI_KINDS[file.suffix]) if file and file.suffix in SHARED_UI_KINDS and file.is_file() else None


def static_file(path: str) -> Path | None:
    """The file a static request's path names: /assets/* from docs/assets, anything else from moondeck_ui, "" its index.html. None for a
    file missing or outside its folder, since http.server hands the path over unnormalized and a "../" climbs out of it."""
    path = path.split("?", 1)[0].lstrip("/") or "index.html"
    base, rel = (ASSETS_DIR, path.removeprefix("assets/")) if path.startswith("assets/") else (UI_DIR, path)
    file = (base / rel).resolve()
    return file if file.is_relative_to(base.resolve()) and file.is_file() else None


# Pages a script writes under build/, served into the View pane: route -> (file, what makes it when missing).
BUILD_PAGES = {
    "/api/history-report": ("build/history.md", "run the History Report button first."),
    "/api/tempo-view": ("build/tempo/view.md", "a run of bench_tempo.py makes it."),
    "/api/notes-view": ("build/notes/view.md", "a run of bench_notes.py makes it."),
}
ASSETS_DIR = ROOT / "docs" / "assets"
STATE_FILE = SCRIPTS_DIR / "moondeck.json"

# Shared test-metadata parsers live next to the test registry. Both this server and
# moondeck/docs/gen_tests.py read the test files through them, so the two views cannot drift.
sys.path.insert(0, str(SCRIPTS_DIR / "docs"))
import _test_metadata as test_meta  # noqa: E402
# The perf table of a scenario step, one home for its shape.
import perf_table  # noqa: E402
# The name ESP-IDF gives a build's app image has one home, beside the firmware list.
sys.path.insert(0, str(SCRIPTS_DIR / "build"))
from build_esp32 import APP_BIN, FIRMWARES as ESP32_BUILDS, build_dir_for  # noqa: E402
# The presence MoonLight devices announce, heard for the Live tab's map while MoonDeck runs.
sys.path.insert(0, str(SCRIPTS_DIR))
import _moondeck_config  # noqa: E402
import _presence  # noqa: E402
import _usb  # noqa: E402
PRESENCE = _presence.Listener()
PORT = _presence.MOONDECK_PORT


def _app_version():
    """The version a build of this tree carries, from compute_version's local channel, the one place a version is derived (7.1.0-dev.146);
    '?' when git cannot say."""
    try:
        import compute_version   # moondeck/build is on the path above
        return compute_version.compute("local")
    except Exception:
        return "?"


APP_VERSION = _app_version()

# ---------------------------------------------------------------------------
# Device-model catalog (single source of truth, shared with the web installer)
# ---------------------------------------------------------------------------

DEVICE_MODELS_FILE = ROOT / "mooninstaller" / "deviceModels.json"


def _load_device_models():
    """Load mooninstaller/deviceModels.json. Returns [] on missing/malformed file —
    MoonDeck JS then shows only the empty default. The web installer's
    picker reads the same file.
    """
    try:
        return json.loads(DEVICE_MODELS_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []


DEVICE_MODELS = _load_device_models()

FIRMWARES_FILE = ROOT / "mooninstaller" / "firmwares.json"


def _load_firmwares():
    """Shipping ESP32 firmware-variant names from mooninstaller/firmwares.json, the
    generated projection of the firmware registry (firmwares.py, shared with the
    CI release matrix); the Teensy firmware has the Teensy tab. Returns [] on missing/malformed
    file, so the MoonDeck UI just shows no firmware entries. Filtering on
    `ships` keeps held-out variants out of the picker.
    """
    try:
        doc = json.loads(FIRMWARES_FILE.read_text(encoding="utf-8"))
        return [f["name"] for f in doc["firmwares"] if f.get("ships") and f.get("platform") == "esp32"]
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

sys.path.insert(0, str(SCRIPTS_DIR / "docs"))
from gen_moonwalk import regenerate as regenerate_moonwalk  # noqa: E402  the one home of regenerating MoonWalk's map on request
import walk_status  # noqa: E402  MoonWalk's rule for which steps a change needs and which passes hold
WALK_JSON = ROOT / "build" / "moonwalk" / "moonwalk.json"
WALK_RUNS = walk_status.WALK_RUNS
IN_CHECKOUT = walk_status.in_checkout()   # outside a git checkout a run counts for nothing on the map


def card_call(card: dict) -> str:
    """A card's own call, its script with its fixed arguments: the key its measured runs are kept under."""
    return walk_status.run_key(f"moondeck/{card['script']}", " ".join(card.get("args", [])))


def card_duration(card: dict, durations: dict) -> dict:
    """How long a card's call takes, the longest of its recent passing runs, and whether that is slow. A server left running has no duration."""
    if card.get("long_running"):
        return {}
    # Every call the card has run counts, its selectors included (`build_esp32.py --firmware esp32` is the ESP32 card's), but not another card's own call.
    call = card_call(card)
    others = {card_call(c) for c in SCRIPTS if c is not card}
    mine = [k for k in durations if k == call or (k.startswith(call + " ") and k not in others)]
    seconds = max((x for k in mine for x in [walk_status.longest(durations, k)] if x is not None), default=None)
    return {"seconds": seconds, "slow": seconds is not None and seconds > walk_status.SLOW_SECONDS}


def walk_model() -> dict | None:
    """MoonWalk's map as the stage files are now, or None when they do not build."""
    return json.loads(WALK_JSON.read_text(encoding="utf-8")) if regenerate_moonwalk(WALK_JSON) else None


def gates(base: str | None = None, every: bool = False, with_model: bool = False) -> dict:
    """MoonWalk's steps that run a script, per process: triggered by the change (against HEAD, against `base` for a merge, every step for a release), still fresh, how long they took, and the card that runs each; with `with_model`, the map they were read from as well, so the map draws without generating it a second time."""
    model = walk_model()
    if model is None:
        return {"error": "gen_moonwalk failed; its error is in this console"}
    # A step's card is the one whose own call it is, else the first card for its script: Compile Tests runs `build_host.py --tests`, not Build.
    by_call, by_script = {}, {}
    for c in SCRIPTS:
        by_call.setdefault(card_call(c), c["id"])
        by_script.setdefault(f"moondeck/{c['script']}", c["id"])
    with _lock:
        running = set(_running)
    try:
        processes = walk_status.status(model, walk_status.read_runs(WALK_RUNS), walk_status.read_runs(walk_status.DURATIONS), base=base, every=every)
    except subprocess.CalledProcessError as e:
        return {"error": f"git: {e.stderr.strip() or e}"}   # a base git does not know
    for steps in processes.values():
        for step in steps:
            step["card"] = by_call.get(walk_status.run_key(step["script"], step["args"])) or by_script.get(step["script"])
            step["running"] = step["card"] in running
    return {"processes": processes, **({"model": model} if with_model else {})}


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
# This computer on the network, and the network install of a local build
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


# ---------------------------------------------------------------------------
# State management
# ---------------------------------------------------------------------------

def load_state():
    """MoonDeck's state from moondeck.json, an older file's shape carried over by _moondeck_config.migrate_state; a fresh state when there is none."""
    if STATE_FILE.exists():
        with open(STATE_FILE) as f:
            return _moondeck_config.migrate_state(json.load(f))
    return {"networks": [], "active_network": "", "tab": "repo"}


_MAC = re.compile(r"([0-9a-f]{2}:){5}[0-9a-f]{2}")   # a MAC as the map's rows carry it


# What MoonDeck's server writes and the page only reads: a page posting the whole state it loaded earlier must not undo an Identify or a
# popup's live-test tick made since.
_SERVER_KEYS = ("usbMacs", "liveTests")


def merge_page_state(state: dict, page: dict, host_subnet: str) -> None:
    """The page's state merged into `state`, in place, the server's own keys kept; a network the bar added without a subnet takes
    `host_subnet`, the one this computer is on, which is how the network is chosen again when the computer returns to it."""
    state.update({k: v for k, v in page.items() if k not in _SERVER_KEYS})
    for n in state.get("networks") or []:
        if not n.get("subnet"):
            n["subnet"] = host_subnet


def select_live_test(state: dict, mac: str, selected: bool) -> None:
    """The device with `mac` in or out of the live tests, in place, the selection in the order it was made."""
    macs = [m for m in state.get("liveTests") or [] if m != mac]
    state["liveTests"] = macs + [mac] if selected else macs


# Serializes the full load → mutate → save transaction across the threaded
# HTTP handlers (ThreadingHTTPServer dispatches each request on its own
# thread). Without this, two concurrent requests could both load_state(),
# mutate their own copies, and each save_state(): last write wins, half the
# work is lost. RLock (not Lock) so save_state can also be
# called standalone for the no-mutator path (POST /api/state body merge)
# without deadlocking when nested inside mutate_state.
_state_write_lock = threading.RLock()


def mutate_state(mutator):
    """Run a full load → mutator(state) → save cycle under the state lock.
    Returns the post-mutation state so the handler can echo it to the
    client. `mutator` receives the loaded state dict, mutates in place
    (or returns a new dict, which becomes the value to save), and may
    return None to mean "keep the in-place mutation."

    Slow work (an esptool probe) happens BEFORE calling mutate_state, its
    result passed in by closure: holding the lock across it would serialize
    every request behind the slowest probe."""
    with _state_write_lock:
        state = load_state()
        result = mutator(state)
        if result is not None:
            state = result
        save_state(state)
        return state


def save_state(state):
    """Persist MoonDeck state, atomic and serialized: a temp file in the same dir, fsync, rename. The rename is atomic on POSIX (same
    filesystem) and fsync makes the bytes durable before the swap, so a crash mid-write leaves the previous moondeck.json intact; the lock
    keeps two handler threads off the temp file and the rename at once."""
    data = json.dumps(state, indent=2)
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


def _active_network(state: dict) -> dict | None:
    """Return the dict for state['active_network'] from state['networks'],
    or None when the name doesn't match or there's no active selection."""
    name = state.get("active_network") or ""
    for n in state.get("networks") or []:
        if n.get("name") == name:
            return n
    return None


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
    # A signal does not end a process at once, and binding before it has let go of the port fails with "Address already in use". Wait for each to exit, up to five seconds.
    deadline = time.monotonic() + 5
    while not _IS_WIN and time.monotonic() < deadline and any(_alive(pid) for pid in pids if pid != os.getpid()):
        time.sleep(0.1)
    if pids:
        print(f"Freed port {port} from a prior instance (pid {', '.join(map(str, pids))}).")


def _alive(pid: int) -> bool:
    """Whether a process still exists, by the signal-zero probe that checks without signaling."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True   # it exists, under another user
    return True


def kill_script(script_id: str, orphans: bool = True):
    """Stop the card's run, and with `orphans` any process named like the server the card starts (MoonLight after os.execv, a docs preview)."""
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
    pname = script_def.get("process_name") if script_def and orphans else None
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
# The boards on this computer's USB ports, by MAC (moondeck/_usb.py)
# ---------------------------------------------------------------------------

def _usb_cache() -> dict:
    """This computer's adapter serial to MAC cache, which Identify writes."""
    return load_state().get("usbMacs") or {}


def usb_boards() -> list[dict]:
    """The boards on this computer's USB ports that could be an ESP32, each with the MAC its USB serial or the cache gives it."""
    return _usb.usb_boards(_usb.present_ports(), _usb_cache())


def describe_serial_ports() -> list[dict]:
    """The ESP32 tab's port dropdown: each listed port with its chip, MAC and the name the device with that MAC announces."""
    usb = _usb.read_usb_ports()
    names = {r["mac"]: r["name"] for r in PRESENCE.rows() if r.get("mac")}
    idents = _usb.port_identities({p: usb.get(p, {}) for p in _usb.list_serial_ports()}, _usb_cache())
    return [{**i, "board": next((n for m, n in names.items() if _usb.mac_matches(i["mac"], m)), "")} for i in idents]


def identify_ports(paths: list[str]) -> dict:
    """Probe each present port of `paths` with esptool, which resets its board, and remember each MAC learned by its adapter's serial."""
    present = _usb.present_ports()
    probed = {}
    for p in paths:
        if p not in present:   # only a present port reaches esptool's --port, not an arbitrary client string
            continue
        info = _usb.probe_port_chip(p)
        if _usb.read_usb_ports().get(p, {}) == present[p]:   # the adapter that was probed is the one the key names
            probed[p] = info

    def remember(state):
        cache = state.setdefault("usbMacs", {})
        for path, info in probed.items():
            _usb.remember_probe(cache, _usb.usb_key(path, present[path]), info)
    mutate_state(remember)
    return probed


def live_tests(state: dict) -> dict:
    """The live tests' devices, selected by MAC: {devices: [{mac, name, host}] at the address each answers on now, missing: [mac]}."""
    macs = state.get("liveTests") or []
    found = _presence.resolve(macs, _presence.known_rows(PRESENCE.rows(), want=macs)) if macs else {}
    return {"devices": [found[m] for m in macs if m in found], "missing": [m for m in macs if m not in found]}


def local_builds() -> list[dict]:
    """The ESP32 builds on this computer: each its firmware, chip, size and age in seconds."""
    now = time.time()
    out = []
    for fw in sorted(ESP32_BUILDS):
        image = build_dir_for(fw) / APP_BIN
        if image.is_file():
            try:
                st = image.stat()
            except OSError:   # a rebuild removed it since is_file
                continue
            out.append({"firmware": fw, "chip": _usb.firmware_to_chip(fw), "size": st.st_size, "age": int(now - st.st_mtime)})
    return out


# ---------------------------------------------------------------------------
# Perf-table HTML (the shape the test page draws)
# ---------------------------------------------------------------------------

def _render_perf_table_html(step: dict) -> str:
    """Render a scenario step's contract+observed data as an HTML table that
    matches the markdown table emitted by perf_table._format_perf_table.
    The doc generator owns the cell formatters; we just translate its pipe-
    delimited markdown to <table><tr><td>. Returns "" when the step has no
    contract/observed data."""
    import html as html_mod
    md_lines = perf_table._format_perf_table(step)
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
            durations = walk_status.read_runs(walk_status.DURATIONS)
            scripts = [{**s, "hasLog": s["id"] in have_logs, **card_duration(s, durations)} for s in SCRIPTS]
            self._send_json({"scripts": scripts, "firmwares": FIRMWARES})

        elif self.path == "/api/ports":
            self._send_json({"ports": describe_serial_ports()})

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
            subnet = _get_local_subnet()   # slow, so before the lock
            with _state_write_lock:   # a POST's write lands before this read or after this save, one at a time
                state = load_state()
                before = state.get("active_network")
                _auto_select_network(state, subnet)
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

        elif self.path in BUILD_PAGES:
            self._serve_build_page()

        elif self.path.startswith("/api/doc-asset/"):
            self._serve_doc_asset()
        # MoonWalk: the scripts' last runs, the map as the stage files are now, and the help's renderer the map imports.
        elif self.path.split("?", 1)[0] == "/api/gates":
            q = urllib.parse.parse_qs(self.path.partition("?")[2])
            self._send_json(gates(q.get("base", [None])[0], q.get("every", ["0"])[0] == "1", q.get("model", ["0"])[0] == "1"))
        elif self.path.split("?", 1)[0] == "/moonwalk/moonwalk.json":
            if regenerate_moonwalk(WALK_JSON):
                self._send_file(WALK_JSON, "application/json")
            else:
                self.send_error(500, "gen_moonwalk failed; its error is in this console")
        elif re.fullmatch(r"/moonwalk/text/[a-z0-9-]+\.json", self.path.split("?", 1)[0]):
            # A process's prose, written beside the map by the run that answered the map's request.
            text = WALK_JSON.parent / "text" / self.path.split("?", 1)[0].rsplit("/", 1)[1]
            if text.exists():
                self._send_file(text, "application/json")
            else:
                self.send_error(404)
        elif self.path.split("?", 1)[0] == "/assets/help.js":
            self._send_file(ROOT / "src" / "ui" / "help.js", "application/javascript")
        elif self.path.split("?", 1)[0] == "/api/presence":
            # What MoonDeck heard and sees itself: the presence packets lately, and the boards on this computer's USB ports.
            self._send_json({"listening": PRESENCE.listening, "rows": PRESENCE.rows(), "usb": usb_boards()})
        elif self.path == "/api/live-tests":
            # The devices selected for the live tests, each at the address it answers on now, and the selected MACs no device answers with.
            self._send_json(live_tests(load_state()))
        elif self.path == "/api/local-builds":
            self._send_json({"builds": local_builds()})
        elif m := re.fullmatch(r"/api/local-build/([\w-]+)", self.path.split("?", 1)[0]):
            # A local build's app image, which a node's popup hands to the shared install flow as a file (src/ui/deviceinstall.js).
            image = build_dir_for(m.group(1)) / APP_BIN if m.group(1) in ESP32_BUILDS else None
            if image and image.is_file():
                self._send_file(image, "application/octet-stream")
            else:
                self.send_error(404, "no local build of that firmware")
        elif shared := shared_ui_file(self.path.split("?", 1)[0]):
            self._send_file(*shared)   # the device UI's own file, from the checkout, so the Live tab's map is the device's map

        else:
            self._serve_static()

    def _send_file(self, path, content_type, data=None):
        """One file, whole, with its type, revalidated on every load as every file MoonDeck serves is."""
        try:
            data = path.read_bytes() if data is None else data
        except OSError:   # a file a rebuild removed since the request named it
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

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
            # mutate_state holds the lock across load + merge + save so two
            # concurrent POSTs can't each load the same snapshot, apply
            # different patches, and clobber each other on save.
            host_subnet = _subnet_from_host_subnet(_get_local_subnet())
            result = mutate_state(lambda s: merge_page_state(s, json.loads(body) if body else {}, host_subnet))
            self._send_json(result)

        elif self.path == "/api/live-tests":
            # One device in or out of the live tests, by MAC: {mac, selected}.
            body = self._read_body()
            req = json.loads(body) if body else {}
            mac = str(req.get("mac", "")).strip().lower()
            if not _MAC.fullmatch(mac):
                self._send_json({"error": "a MAC is required"}, 400)
                return
            state = mutate_state(lambda s: select_live_test(s, mac, bool(req.get("selected"))))
            self._send_json({"liveTests": state.get("liveTests", [])})

        elif self.path == "/api/identify-ports":
            # Identify, from the ESP32 tab or a node's popup: {ports: [...]}. esptool resets each board to read its chip and MAC, so only
            # what was asked is probed; each MAC learned is remembered by its adapter's serial (usbMacs).
            body = self._read_body()
            probed = identify_ports((json.loads(body) if body else {}).get("ports", []))
            self._send_json({"ports": describe_serial_ports(), "probed": probed})


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
        # A board named by its MAC, as a map node's popup names it, runs on the port it sits on right now, found from the present ports.
        if params.get("mac") and script_def.get("needs_port") and not params.get("port"):
            if not _MAC.fullmatch(str(params["mac"]).lower()):
                self._send_json({"error": "a MAC is xx:xx:xx:xx:xx:xx"}, 400)
                return
            params = {**params, "port": _usb.port_for_mac(params["mac"], _usb.present_ports(), _usb_cache())}
            if not params["port"]:
                self._send_json({"error": f"No USB port of this computer holds the board {params['mac']}: plug it in, "
                                          "or Identify it once from its node if it sits behind a USB bridge."}, 400)
                return
        REQUIRED = [("needs_port", "port", "a serial port"),
                    ("needs_firmware", "firmware", "a firmware variant")]
        missing = [label for flag, key, label in REQUIRED
                   if script_def.get(flag) and not params.get(key)]
        # A MoonWalk step runs exactly its own call, read from the process file by the step's name: a request names a step, never arguments.
        step_args = None
        if params.get("step"):
            run = walk_status.step_run(walk_model() or {}, params["step"])
            if not run or run["script"] != f"moondeck/{script_def['script']}":
                self._send_json({"error": f"no step {params['step']} runs this card's script"}, 400)
                return
            step_args = run.get("args", "")
        if missing and step_args is None:
            self._send_json({"error": f"Select {' and '.join(missing)} first."}, 400)
            return

        # A MoonWalk step's own call, `build_docs --strict` say, shares the card with its server: while the server runs, the step asks for it to stop rather than stopping it unasked.
        if step_args is not None and script_def.get("long_running"):
            with _lock:
                serving = _running.get(script_id)
            if serving and serving.poll() is None and getattr(serving, "_mm_long_running", False):
                self._send_json({"error": f"{script_def['name']} is running: stop it first, then run this step."}, 409)
                return
        # Kill the previous run if still going; a step's call leaves a server the card started elsewhere alone.
        kill_script(script_id, orphans=step_args is None)

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
            if script_def.get("mac_guard") and params.get("mac"):
                cmd.extend(["--mac", params["mac"]])   # the script reads the board's MAC before it writes and stops on another board
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
            cmd.extend([arg, f"moontube/{kind}/{name}.json"])
        if script_def.get("needs_module") and params.get("module"):
            cmd.extend(["--module", params["module"]])
        # pass_device_model: forward the deviceModel of the board, the one its map node reports or the ESP32 tab's pick
        # (state.provisionDeviceModel), so improv_provision.py injects that board's TX-power cap before provisioning (the
        # weak-power brown-out fix) and flash_esp32.py resolves its flash baud by the exact board.
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
        if isinstance(step_args, str):
            cmd = ["uv", "run", str(script_path), *shlex.split(step_args)]

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
            proc._mm_key = walk_status.run_key(f"moondeck/{script_def['script']}", " ".join(cmd[3:]))   # the call that ran, the key a step is matched by
            proc._mm_long_running = bool(script_def.get("long_running")) and step_args is None   # a server card's step call, `build_docs --strict`, finishes and is timed
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
            # A passing run's seconds join the call's measured durations; a server's are how long it was left on, and a failure may have stopped early.
            if proc.returncode == 0 and hasattr(proc, "_mm_key") and not getattr(proc, "_mm_long_running", False):
                walk_status.record_duration(walk_status.DURATIONS, proc._mm_key, time.monotonic() - started)
            if IN_CHECKOUT:
                with suppress(OSError, subprocess.CalledProcessError):
                    walk_status.record_finished(WALK_RUNS, proc._mm_key, proc.returncode == 0)
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
        URL: /api/docs/<path>[?#anchor] — e.g. /api/docs/reference/testing.md, /api/docs/reference/moonwalk.md"""
        import re as _re
        raw_path = self.path[len("/api/docs/"):]
        parts = raw_path.split("?", 1)
        filename = parts[0].strip("/")
        raw_anchor = parts[1] if len(parts) > 1 else ""
        anchor = raw_anchor if _re.fullmatch(r"[A-Za-z0-9._-]+", raw_anchor) else ""
        # Restrict to .md files and resolve under docs/ with a traversal guard:
        # build the candidate path, resolve symlinks, then verify it sits inside docs/.
        # Allows subpaths like tests/foo.md while still rejecting ../escape attempts.
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

        Both live under moontube/, the same split the tooling uses: a CLIP is
        performed against a device and recorded, a PROJECT cuts clips together. The
        description rides along so the dropdown can say what each one does rather than
        only naming a file.
        """
        folder = SCRIPTS_DIR.parent / "moontube" / kind
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
        # whatever other keys it carries (id, key, value, document, …)
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
            # the test page; see perf_table._format_perf_table).
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
/* Perf table — the per-step table the test page draws */
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

    def _serve_build_page(self):
        """Serve a page a script writes under build/, as HTML through the renderer the help pages use.
        Iframes cannot load file:// from an http:// parent, so the file comes through MoonDeck's origin, as /api/help does."""
        relpath, missing = BUILD_PAGES[self.path]
        md_path = ROOT / relpath
        if not md_path.exists():
            self.send_error(404, f"{relpath} not found: {missing}")
            return
        self._serve_markdown_as_html(md_path, "")

    def _serve_doc_asset(self):
        """Serve a static asset, such as an image, referenced from a rendered doc.

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
        """Serve files from moondeck_ui/ and docs/assets/ (static_file)."""
        file_path = static_file(self.path)
        if not file_path:
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
        if file_path == (UI_DIR / "index.html").resolve():
            data = data.replace(b"{{VERSION}}", APP_VERSION.encode())
        self._send_file(file_path, content_type, data)


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
    PRESENCE.start()
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

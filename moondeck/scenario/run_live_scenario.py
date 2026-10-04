#!/usr/bin/env python3
"""Run scenario tests against a live device via HTTP.

Same JSON format as the in-process runner. Executes steps via REST API
and collects per-step performance measurements.
"""

import argparse
import atexit
import signal
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error
import urllib.parse
from pathlib import Path


def _mod_path(name: str) -> str:
    """`/api/modules/<name>` with the name URL-encoded. Module names can contain
    spaces (ensureUniqueName disambiguates duplicates as "Layer 2"), which urllib
    rejects in a raw URL: encode so delete/replace/clear can address them."""
    return "/api/modules/" + urllib.parse.quote(name, safe="")

ROOT = Path(__file__).resolve().parent.parent.parent
SCENARIOS_DIR = ROOT / "test" / "scenarios"
BASELINE_FILE = ROOT / "test" / "scenario-baseline.json"

# Reuse the shared test-metadata parser so scenario discovery stays in one place.
sys.path.insert(0, str(ROOT / "moondeck"))
from _host import desktop_target  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import _test_metadata as test_meta  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
import _observed  # noqa: E402


# A REDIRECTED Windows stdout takes the locale encoding, cp1252, and this runner prints an arrow
# per step, so a scenario that passed would be recorded as FAILED by the print rather than by the
# device. An attached console is UTF-8 since Python 3.6 (PEP 528), so the failing mode is a pipe
# or a CI log, which is exactly how a gate runs it. errors=replace so a stray glyph never costs
# a result.
if sys.stdout is not None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if sys.stderr is not None:
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


class Client:
    # Mutating ops (add/delete/replace/control) trigger a full prepareTree on the
    # device: at 128x128 that frees/reallocates a large buffer + LUT and can
    # take several seconds on a busy ESP32. 5s was too tight (deletes timed out
    # mid-teardown, leaving a half-mutated tree). 15s clears the worst case while
    # still catching a genuinely hung device.
    TIMEOUT_S = 15

    def __init__(self, host: str):
        self.base = f"http://{host}"
        # Whether the device is a process on THIS machine. A desktop build elsewhere is named after
        # its own platform and restarts itself, where a local one is named after this host and is
        # relaunched from here. Decided by name alone, so this machine reached by its own LAN
        # address or hostname counts as another one.
        name = urllib.parse.urlsplit(f"//{host}").hostname or host
        self.local = name in ("localhost", "127.0.0.1", "::1")

    def _send(self, req):
        # A mutating call triggers prepareTree; while the device is mid-rebuild it
        # can drop the TCP connection (ConnectionResetError / "remote end closed")
        # or briefly refuse one. The device recovers in well under a second, so a
        # single transient drop shouldn't cascade-fail the run: retry once after
        # a short settle. A genuine HTTPError (4xx/5xx from the handler) is a real
        # result and is NOT retried; it propagates to the caller.
        for attempt in range(2):
            try:
                with urllib.request.urlopen(req, timeout=self.TIMEOUT_S) as resp:
                    return json.loads(resp.read())
            except urllib.error.HTTPError:
                raise  # a real handler response: let the caller decide
            except (urllib.error.URLError, ConnectionError, OSError):
                if attempt == 0:
                    time.sleep(1.0)
                    continue
                raise

    def get(self, path: str):
        return self._send(urllib.request.Request(f"{self.base}{path}"))

    def post(self, path: str, data: dict):
        body = json.dumps(data).encode()
        return self._send(urllib.request.Request(
            f"{self.base}{path}", data=body,
            headers={"Content-Type": "application/json"}))

    def get_text(self, path: str) -> str:
        """GET a raw body. /api/file returns the file's CONTENTS, not JSON, so it cannot go
        through get() which parses what it reads."""
        req = urllib.request.Request(f"{self.base}{path}")
        with urllib.request.urlopen(req, timeout=self.TIMEOUT_S) as resp:
            return resp.read().decode("utf-8", "replace")

    def post_text(self, path: str, text: str):
        """POST a raw text body. /api/file takes the file's CONTENTS, not JSON: the body IS
        the payload, which is why it cannot go through post() like every control write."""
        return self._send(urllib.request.Request(
            f"{self.base}{path}", data=text.encode("utf-8"),
            headers={"Content-Type": "text/plain"}))

    def delete(self, path: str):
        return self._send(urllib.request.Request(f"{self.base}{path}", method="DELETE"))

    def patch(self, path: str, data: dict):
        return self._send(urllib.request.Request(
            f"{self.base}{path}", data=json.dumps(data).encode(), method="PATCH",
            headers={"Content-Type": "application/json"}))


def _error_body(he: urllib.error.HTTPError) -> dict:
    """An error response's JSON object, or an empty one when the body is not JSON, such as a proxy's HTML page."""
    try:
        body = json.loads(he.read() or b"{}")
    except ValueError:
        return {}
    return body if isinstance(body, dict) else {}


def _today_iso() -> str:
    """ISO date stamp for set_by fields. Local timezone is fine: set_by is a
    coarse "around when did this contract get blessed" marker, not a timestamp."""
    import datetime
    return datetime.date.today().isoformat()


def collect_metrics(client: Client, settle_s: float = 1.5) -> dict:
    """Wait for system to settle, then collect metrics. Augments /api/system with
    `dynamicBytesTotal` (sum from the module tree) so callers can compare the
    model's allocation prediction against the observed free heap."""
    time.sleep(settle_s)
    metrics = client.get("/api/system")
    try:
        state = client.get("/api/state")
        metrics["dynamicBytesTotal"] = _sum_dynamic_bytes(state)
    except Exception:
        metrics["dynamicBytesTotal"] = None
    return metrics


def _control_value(module: dict, name: str):
    """Return the value of a named control on a module, or None.

    `enabled` is a field of the module itself rather than one of its controls, so it is read from there.
    """
    if name == "enabled":
        return module.get("enabled")
    for ctrl in module.get("controls", []):
        if ctrl.get("name") == name:
            return ctrl.get("value")
    return None


def count_lights(client: Client) -> int:
    """Derive the total light count from layout modules in the state tree.

    Layout modules expose width/height/depth controls; their product is the
    grid's light count. Used to scale the FPS-throughput floor to the grid.
    The module tree is nested (children[]), so walk it recursively.
    """
    def walk(module: dict) -> int:
        total = 0
        w = _control_value(module, "width")
        h = _control_value(module, "height")
        d = _control_value(module, "depth")
        if w is not None and h is not None and d is not None:
            try:
                total += int(w) * int(h) * int(d)
            except (ValueError, TypeError):
                print(f"  WARN  count_lights: non-numeric w/h/d "
                      f"({w!r}/{h!r}/{d!r}) on module {module.get('name','?')}, skipped")
        for child in module.get("children", []):
            total += walk(child)
        return total

    state = client.get("/api/state")
    return sum(walk(m) for m in state.get("modules", []))


def _detect_target(state: dict, local: bool = True) -> str:
    """Identify the build target so per-step contract values can be looked up.

    ESP32: read FirmwareUpdateModule.firmware (`esp32`, `esp32-eth`, `esp32-eth-wifi`,
    `esp32s3-n16r8`, …): set at compile time from MM_FIRMWARE_NAME and
    exposed through the `firmware` control. Desktop: same key but reports
    `unknown`, so we substitute desktop-<host-os> using the runtime os name (still
    distinguishes macOS vs Linux vs Windows builds, which can differ in tick
    noticeably). See docs/explanation/architecture/index.md § Firmware vs board.

    A desktop build on ANOTHER machine is named after the platform it reports about itself
    (`desktop-docker-arm64`), because this host's name says nothing about it: a NanoPi measured
    from a Mac was recorded as `desktop-macos`, into the Mac's own figures.
    """
    firmware = model = None
    for m in state.get("modules", []):
        for c in m.get("controls", []):
            if m.get("type") == "FirmwareUpdateModule" and c.get("name") == "firmware":
                firmware = c.get("value")
            elif m.get("type") == "SystemModule" and c.get("name") == "deviceModel":
                model = c.get("value")
    if firmware and firmware != "unknown":
        return firmware
    if not local:
        return "desktop-" + str(model or "remote").strip().lower().replace(" ", "-")
    # Desktop fallback, from the one home every script shares (moondeck/_host.py).
    return desktop_target()


# --- The host's own WiFi, for a scenario that walks a device's first setup from its access point. ---
# The host joins the device's access point, loses its own network meanwhile, and is always put back:
# a step does it, and atexit does it again for a run that stops in between.

REGISTRY_FILE = ROOT / "moondeck" / "moondeck.json"
# The access point's address, which the firmware's captive portal fixes (src/core/util/CaptivePortal.h).
ACCESS_POINT_ADDRESS = "4.3.2.1"


def _registry_network(name: str) -> dict:
    """A network from the bench registry, `{name, ssid, password}`; the registry is not in git, so credentials stay local."""
    with open(REGISTRY_FILE, encoding="utf-8") as f:
        for net in json.load(f).get("networks", []):
            if net.get("name") == name:
                wifi = net.get("wifi") or {}
                return {"name": name, "ssid": wifi.get("ssid", ""), "password": wifi.get("password", "")}
    raise KeyError(f"no network named {name!r} in {REGISTRY_FILE.name}")


def _wifi_device() -> str:
    """The host's WiFi interface (en0 on most Macs), read rather than assumed."""
    out = subprocess.run(["networksetup", "-listallhardwareports"], capture_output=True, text=True, timeout=30).stdout
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.strip() in ("Hardware Port: Wi-Fi", "Hardware Port: AirPort") and i + 1 < len(lines):
            return lines[i + 1].split(":", 1)[1].strip()
    raise RuntimeError("no WiFi interface found (this op needs macOS networksetup)")


def _reachable(url: str, timeout_s: float) -> bool:
    """Whether `url` answers within the time, polled, since a join takes a few seconds to route."""
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            with urllib.request.urlopen(url, timeout=3):
                return True
        except urllib.error.HTTPError:
            return True   # any HTTP answer is an answer
        except Exception:
            time.sleep(1)
    return False


def _join_failed(out) -> bool:
    """Whether networksetup reported a failure: it exits 0 either way, so its words decide."""
    text = out.stdout + out.stderr
    return out.returncode != 0 or any(w in text for w in ("Could not find network", "Failed", "Error"))


def _host_join(ssid: str, password: str, probe_url: str, timeout_s: float) -> str:
    """Join `ssid` and wait until `probe_url` answers: "" on success, else why.

    The join is retried only while networksetup reports a failure, since an access point still starting is not in the scan yet; once it took, the probe is polled rather than the join repeated, which would drop the link it made. The password rides networksetup's arguments, which is the one way that tool takes it.
    """
    device = _wifi_device()
    end = time.time() + timeout_s
    last = ""
    while time.time() < end:
        cmd = ["networksetup", "-setairportnetwork", device, ssid] + ([password] if password else [])
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            last = "networksetup did not answer"
            continue
        last = (out.stdout + out.stderr).strip()
        if not _join_failed(out):
            if _reachable(probe_url, max(1.0, end - time.time())):
                return ""
            return f"joined {ssid!r}, but {probe_url} did not answer in time"
        time.sleep(2)
    return f"could not join {ssid!r} ({last or 'no message'})"


class HostWifi:
    """Where the host's WiFi is, so a run that moved it to a device's access point always moves it back."""

    def __init__(self):
        self.home = None   # the registry network to return to, set once the host leaves it

    def restore(self):
        """Rejoin the home network, retried as a join is, since macOS often misses a network the moment it leaves an access point; home is kept until it took, so the exit hook can try again."""
        if not self.home:
            return
        home = self.home
        print(f"  HOST  back to {home['ssid']}")
        end = time.time() + 30
        last = ""
        while time.time() < end:
            try:
                out = subprocess.run(["networksetup", "-setairportnetwork", _wifi_device(), home["ssid"], home["password"]],
                                     capture_output=True, text=True, timeout=30)
            except subprocess.TimeoutExpired:
                last = "networksetup did not answer"
                continue
            if not _join_failed(out):
                self.home = None
                return
            last = (out.stdout + out.stderr).strip()
            time.sleep(2)
        print(f"  HOST  WARNING: could not rejoin {home['ssid']} ({last or 'no message'}); rejoin it by hand")


def _fill(value, ctx: dict):
    """Fill `{network.ssid}`, `{network.password}` and `{device}` in a step's strings, so a scenario names no credential."""
    if isinstance(value, str):
        for key, sub in ctx.items():
            value = value.replace("{" + key + "}", sub)
        return value
    if isinstance(value, dict):
        return {k: _fill(v, ctx) for k, v in value.items()}
    if isinstance(value, list):
        return [_fill(v, ctx) for v in value]
    return value


def _http_request(url: str, base: str, method: str = "GET") -> urllib.request.Request:
    """The request an `expect_http` step sends: a path starting with / goes to the device under test, and a POST carries an empty body, as the UI's buttons send."""
    if url.startswith("/"):
        url = base.rstrip("/") + url
    return urllib.request.Request(url, data=b"" if method == "POST" else None, method=method)


def _typed_names(doc) -> list:
    """The module names a state document can create: every object carrying a `type`, depth first."""
    names = []
    if isinstance(doc, dict):
        for key, value in doc.items():
            if isinstance(value, dict):
                if "type" in value:
                    names.append(key)
                names.extend(_typed_names(value))
    return names


def _list_rows(client, module_id: str, key: str) -> list:
    mod = client.get(_mod_path(module_id))
    for c in mod.get("controls") or []:
        if c.get("name") == key:
            return c.get("value") or []
    raise KeyError(f"{module_id} has no list {key!r}")


def _uptime_seconds(client):
    """The System card's uptime as seconds, or None when it cannot be read.

    The value is `H:MM:SS`, and its only use here is comparing two readings across a restart.
    """
    try:
        mod = client.get(_mod_path("System"))
    except Exception:
        return None
    for c in (mod.get("controls") or []):
        if c.get("name") == "uptime":
            parts = str(c.get("value") or "").split(":")
            if len(parts) != 3:
                return None
            try:
                h, m, sec = (int(x) for x in parts)
            except ValueError:
                return None
            return h * 3600 + m * 60 + sec
    return None


def _restarted(before: int, now: int, elapsed: float) -> bool:
    """Whether an uptime reading proves a restart happened since `before` was read, `elapsed` seconds ago.

    A device that kept running has gained as many seconds of uptime as the wall clock has, so one
    that gained fewer restarted in between. Comparing against the wall clock rather than against
    `before` alone is what makes this hold when the device booted moments ago: `now < before` reads
    a fresh boot as "did not restart" whenever the new uptime has already passed the old one, which
    is every time two scenarios restart a board back to back. The two seconds cover the uptime being
    whole seconds on both readings.
    """
    return now + 2 < before + elapsed


def _remember_control(prior: dict, client, mod_id: str, key: str) -> None:
    """Note what a control held before a scenario first writes it, so the run can put it back.

    Only the first write counts: a later one would record the scenario's own value.
    A control that cannot be read is left out, and the scenario's value then stays.
    A password is left out too: the module read returns it obfuscated, and writing that back would set the obfuscated text as the password.
    """
    if (mod_id, key) in prior:
        return
    try:
        module = client.get(_mod_path(mod_id))
    except Exception:
        return
    if any(c.get("name") == key and c.get("type") == "Password" for c in module.get("controls", [])):
        return
    value = _control_value(module, key)
    if value is not None:
        prior[(mod_id, key)] = value


def _remember_type(replaced: dict, client, mod_id: str) -> None:
    """Note what type a slot held before a scenario first replaces it, so the run can swap it back.

    Only the first replace counts, as with controls. A slot that cannot be read is left out.
    """
    if mod_id in replaced:
        return
    try:
        typ = client.get(_mod_path(mod_id)).get("type")
    except Exception:
        return
    if typ:
        replaced[mod_id] = typ


def _restore_types(client, replaced: dict) -> int:
    """Swap every slot a scenario replaced back to the type it held, keeping its name.

    The re-created tree cannot do this: a replaced slot still exists under its name, so the snapshot restore sees it as present and leaves the scenario's type standing.
    A slot the scenario itself created is gone by now, and that 404 stays quiet; any other failure is reported, as `_restore_tree` reports one.
    Returns how many the device confirmed.
    """
    done = 0
    for mod_id, typ in reversed(list(replaced.items())):
        try:
            if client.post(_mod_path(mod_id) + "/replace", {"type": typ, "name": mod_id}).get("ok"):
                done += 1
            else:
                print(f"  WARN  restore: the device declined to swap {mod_id} back to {typ}")
        except urllib.error.HTTPError as e:
            if e.code != 404:
                print(f"  WARN  restore: could not swap {mod_id} back to {typ}: {e}")
        except Exception as e:
            print(f"  WARN  restore: could not swap {mod_id} back to {typ}: {e}")
    return done


def _restore_controls(client, prior: dict) -> int:
    """Set every control a scenario wrote back to what it held before, newest write first.

    A scenario that names a device to prove the name survives a restart otherwise leaves that name on every device it runs on, and two bench devices answering to one name is how a restore landed on the wrong one.
    A control whose module the scenario itself created is gone by now, and that 404 stays quiet; any other failure is reported, as `_restore_tree` reports one.
    Returns how many the device confirmed.
    """
    done = 0
    for (mod_id, key), value in reversed(list(prior.items())):
        try:
            if client.post("/api/control", {"module": mod_id, "control": key, "value": value}).get("ok"):
                done += 1
            else:
                print(f"  WARN  restore: the device declined {mod_id}.{key}={value!r}")
        except urllib.error.HTTPError as e:
            if e.code != 404:
                print(f"  WARN  restore: could not set {mod_id}.{key}={value!r}: {e}")
        except Exception as e:
            print(f"  WARN  restore: could not set {mod_id}.{key}={value!r}: {e}")
    return done


def _reboot_and_wait(client, target: str, timeout_s: float = 60.0) -> str:
    """Restart the device and wait for it to answer again, so a scenario can prove what survives.

    On a board this is the reboot the endpoint performs. On a desktop the same endpoint EXITS the
    process and nothing restarts it, so the runner relaunches the binary itself, with the data
    directory the exiting instance was using: a restart that came back on different files would
    prove nothing about persistence. Returns "" on success, or the reason it did not come back.
    """
    data_dir = os.environ.get("MM_DATA_DIR")
    # Only a desktop build on THIS machine is relaunched from here. One on another host is brought
    # back by whatever runs it there, a container's restart policy or a service manager, and
    # launching a local binary for it would start a second, unrelated device on this machine.
    is_desktop = target.startswith("desktop-") and client.local

    # Uptime before the restart, so the recovered instance can be told from the one still running.
    # Without it the first answering /api/state is accepted, which on a board is routinely the OLD
    # instance replying before it goes down: every persistence assertion after that proves nothing.
    before = _uptime_seconds(client)
    read_at = time.time()

    try:
        client.post("/api/reboot", {})
    except urllib.error.HTTPError as re_:
        # A refused reboot is a failed step: the device is up, so the poll below would pass at once.
        return f"/api/reboot returned HTTP {re_.code}"
    except Exception:
        pass                  # the device goes away mid-response, which is the expected shape

    if is_desktop:
        # The same resolver run_desktop.py uses, which picks the NEWEST candidate rather than the
        # first that exists: picking by existence served a stale build whose changes read as no-ops,
        # and it names the per-host directory, so this works on Linux and Windows too.
        sys.path.insert(0, str(ROOT / "moondeck" / "run"))
        from run_desktop import _resolve_executable            # noqa: E402
        binary = _resolve_executable()
        if not binary.exists():
            return f"no desktop binary to relaunch (looked for {binary})"
        time.sleep(1.0)       # let the old process release the port before the new one binds it
        env = dict(os.environ)
        if data_dir:
            env["MM_DATA_DIR"] = data_dir
        subprocess.Popen([str(binary)], cwd=str(ROOT), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Evidence of a restart, not merely of an answer: either the device went away and came back, or
    # it answers with an uptime lower than before. One of the two must hold before this reports success.
    deadline = time.time() + timeout_s
    went_away = False
    while time.time() < deadline:
        try:
            client.get("/api/state")
        except Exception:
            went_away = True          # the old instance is gone, so the next answer is the new one
            time.sleep(1.0)
            continue
        if went_away:
            return ""
        now = _uptime_seconds(client)
        if before is not None and now is not None and _restarted(before, now, time.time() - read_at):
            return ""                 # the clock fell behind the wall, which only a reboot does
        time.sleep(1.0)
    if not went_away:
        if before is None:
            return (f"still answering after {timeout_s:.0f}s, and the uptime could not be read before "
                    "the restart, so nothing here can tell whether it happened")
        return (f"still answering after {timeout_s:.0f}s with no uptime reset: "
                "the reboot did not take effect")
    return f"did not answer within {timeout_s:.0f}s"


def _sum_dynamic_bytes(state: dict) -> int:
    """Sum dynamicBytes across the live module tree. Returned by /api/state per
    module; the sum is the model's prediction for how much heap the tree owns.

    NOT the same as (boot_heap - free_heap): the framework (lwIP, WiFi stack,
    FreeRTOS, HTTP server kernel buffers) consumes heap outside the model.
    Printed alongside the contract for sanity-checking: a regression here
    means a module started allocating something the contract didn't budget for.
    """
    total = 0
    def walk(modules):
        nonlocal total
        for m in modules:
            try:
                total += int(m.get("dynamicBytes", 0))
            except (TypeError, ValueError):
                pass
            walk(m.get("children", []))
    walk(state.get("modules", []))
    return total


def _collect_module_names(state: dict) -> set:
    """Collect every module name in the live tree, including nested children.
    Used by mutate scenarios to pre-flight that every id they touch is wired."""
    names = set()
    def walk(modules):
        for m in modules:
            n = m.get("name")
            if n:
                names.add(n)
            walk(m.get("children", []))
    walk(state.get("modules", []))
    return names


def _child_names_of(state: dict, container_name: str) -> list:
    """Direct-child names of the named container in the live tree (depth-1 only).
    Used by clear_children to enumerate what to delete; the device tears down each
    child's whole subtree, so only the immediate children need naming."""
    def find(modules):
        for m in modules:
            if m.get("name") == container_name:
                return [c.get("name") for c in m.get("children", []) if c.get("name")]
            hit = find(m.get("children", []))
            if hit is not None:
                return hit
        return None
    return find(state.get("modules", [])) or []


# Containers whose USER-ADDED children a scenario may clear/rebuild: the tree the
# snapshot/restore protects. A scenario that clear_children's one of these destroys
# the board's real config; restoring the snapshot afterward leaves the board as found.
_SNAPSHOT_CONTAINERS = {"Layouts", "Effects", "Drivers", "Services", "Layer"}


def _snapshot_tree(state: dict) -> list:
    """Capture the user-added modules a scenario might clear or remove, in tree order (parents before children), so they can be re-created after the scenario runs.

    Each entry is {type, id, parent_id, controls}: everything /api/state exposes to reconstruct a module via POST /api/modules + /api/control.
    Boot-wired singletons (the containers themselves, Preview, FixtureProfiles) are not captured: the device re-creates them itself, and re-adding is a no-op or an error.
    The snapshot holds the children of the snapshot containers and their descendants, which are the modules a scenario's clear_children or remove takes away."""
    snap = []

    def controls_of(m: dict) -> dict:
        return {c["name"]: c.get("value") for c in m.get("controls", [])
                if c.get("name") and not c.get("readonly")}

    def walk(modules: list, parent_name, *, inside_container: bool) -> None:
        for m in modules:
            name = m.get("name")
            typ = m.get("type")
            # Capture a module that sits INSIDE a snapshot container and is user-editable
            # (a driver/effect/modifier/layout/service the scenario could clear). Skip the
            # boot-wired ones the device owns (userEditable false is not in /api/state, so
            # gate on the known singletons by name instead).
            if inside_container and name and typ and name not in ("Preview", "FixtureProfiles"):
                snap.append({"type": typ, "id": name,
                             "parent_id": parent_name, "controls": controls_of(m)})
            walk(m.get("children", []), name,
                 inside_container=inside_container or name in _SNAPSHOT_CONTAINERS)

    walk(state.get("modules", []), None, inside_container=False)
    return snap


def _restore_tree(client, snapshot: list, current_state: dict) -> None:
    """Re-create any snapshotted module that a scenario removed, restoring the board to
    the tree it had before the run. Adds parents before children (snapshot order) and
    re-applies control values. A module still present is left untouched. A restore that
    fails is reported (which module/control + the error), not silently swallowed: a
    board left partially restored is a signal worth seeing, but one failure must not stop
    the rest, so we log and continue."""
    present = _collect_module_names(current_state)
    restored = 0
    for entry in snapshot:
        if entry["id"] in present:
            continue
        try:
            client.post("/api/modules", {"type": entry["type"], "id": entry["id"],
                                         "parent_id": entry["parent_id"]})
        except Exception as e:
            print(f"  WARN  restore: could not re-create {entry['id']} "
                  f"({entry['type']} under {entry['parent_id']}): {e}")
            continue
        for cname, val in entry["controls"].items():
            try:
                client.post("/api/control", {"module": entry["id"],
                                             "control": cname, "value": val})
            except Exception as e:
                print(f"  WARN  restore: could not set {entry['id']}.{cname}={val!r}: {e}")
        restored += 1
    if restored:
        print(f"  restored {restored} module(s) the scenario had cleared")


def run_scenario(client: Client, scenario_path: Path, settle_s: float = 1.5,
                 update_contract: bool = False,
                 update_reason: str | None = None, named: bool = True,
                 network: str | None = None) -> dict:
    """Run a scenario against a live device and return results.

    Mode handling (see docs/reference/testing.md § Scenario modes):
      construct : scenario builds the pipeline from scratch. Live device's
                   main.cpp owns the top-level shape, so construct scenarios
                   only run in-process. Skip here with a clear note.
      mutate    : scenario assumes a wired pipeline. Skip the fixture array
                   (the device IS the fixture) and run only the steps. Steps
                   that touch ids not present on the device hard-fail (instead
                   of the old WARN-and-continue which silently produced
                   meaningless passes).
    """
    # encoding="utf-8" EXPLICITLY, and it is not cosmetic here: _observed.py writes this file back
    # with the observation block appended, so reading it as cp1252 on Windows and writing it as
    # UTF-8 double-encodes every non-ASCII character in the scenario's own prose. One run turned
    # "16²→32²" into "16Â²â†→32Â²" in every file it touched.
    with open(scenario_path, encoding="utf-8") as f:
        scenario = json.load(f)

    name = scenario.get("name", scenario_path.stem)
    mode = scenario.get("mode", "construct")  # back-compat default
    print(f"\n=== Scenario: {name} ===")
    print(scenario.get("description", ""))

    results = {"name": name, "steps": [], "passed": True, "skipped": False}
    created_modules = []  # mutate scenarios rarely add modules but the existing cleanup path is still useful
    created_files = []    # files a write_file step created, removed at the end even when a later step stopped the run
    prior_files = {}      # files a write_file step overwrote, as they were before its first write, written back at the end
    wrote_observations = [False]  # sentinel; flipped by each measure step that runs
    # Keyed by (step, target): the original contract block before --update-contract
    # mutated it (or None if no prior block existed). Used by the post-run gate to
    # roll mutations back when the run fails, so a renegotiated promise only
    # lands on a clean run.
    pending_contract_originals: dict = {}

    if scenario.get("on_request") and not named:
        print("\n  SKIP (on_request: runs only when named, since it takes the host off its network)")
        results["skipped"] = True
        return results
    if mode == "construct":
        print("\n  SKIP (mode=construct: runs in-process only; the live device's "
              "main.cpp owns the top-level shape)")
        results["skipped"] = True
        return results
    if mode != "mutate":
        print(f"\n  FAIL: unknown mode: {mode!r} (expected construct or mutate)")
        results["passed"] = False
        return results

    # Pre-flight: every id touched by a step must be reachable: either already
    # on the device, OR added by an earlier add_module in this scenario. A
    # canvas-preparing scenario clears the containers and builds its own tree, so
    # its set_control/replace ids won't exist on the device yet; they're created
    # mid-run. A still-unreachable id is a real typo / wrong-wiring bug.
    target = "unknown"
    live_state = None   # bound before the try so the snapshot below can't NameError if /api/state fails
    try:
        live_state = client.get("/api/state")
        target = _detect_target(live_state, client.local)
        # Walk the steps in order, growing the reachable set as add_module steps
        # create ids. The containers (Layouts/Effects/Drivers) are always present.
        reachable = _collect_module_names(live_state)
        # The FIXTURE runs before the steps and creates the wired pipeline, so the ids it adds
        # are reachable by the time any step runs. Walking only `steps` reported a fixture-added
        # module as missing the moment a scenario's first step targeted one.
        for step in scenario.get("fixture", []):
            if step.get("op") == "add_module" and step.get("id"):
                reachable.add(step["id"])
        missing = []
        for step in scenario.get("steps", []):
            sid = step.get("id")
            opn = step.get("op")
            if opn == "add_module" and sid:
                reachable.add(sid)
            elif opn in ("set_control", "delete_module", "remove_module", "replace_module", "clear_children") and sid:
                # `optional` steps are best-effort (e.g. shrink the grid before a
                # clear, if a grid exists): the executor skips them on a missing
                # target, so they don't count as a wiring bug in the pre-flight.
                if sid not in reachable and not step.get("optional"):
                    missing.append(sid)
        # Reset-block ids must exist before steps run (no add can precede them).
        for r in scenario.get("reset", []):
            if r.get("op") == "set_control" and r.get("id") and r["id"] not in reachable:
                missing.append(r["id"])
        missing = sorted(set(missing))
        if missing:
            print(f"\n  FAIL: scenario references ids that are neither on the live "
                  f"device nor added by an earlier step: {', '.join(missing)}. "
                  f"Fix the wiring or add the module first.")
            results["passed"] = False
            return results
    except Exception as e:
        print(f"\n  WARN: couldn't pre-flight live module names: {e}")
    print(f"  Target: {target}")
    results["target"] = target

    # A scenario that moves the host's WiFi runs on a registry network the host and the device share, named by --network, and its steps fill in from it.
    ctx: dict = {}
    host = HostWifi()
    home_base = client.base
    net = None
    if scenario.get("host_network"):
        try:
            if not network:
                raise KeyError("this scenario moves the host's WiFi: name the network it returns to with --network")
            net = _registry_network(network)
        except Exception as e:
            print(f"\n  FAIL: {e}")
            results["passed"] = False
            return results
        ctx["network.ssid"], ctx["network.password"] = net["ssid"], net["password"]
        for m in (live_state or {}).get("modules", []):
            for c in m.get("controls", []):
                if m.get("type") == "SystemModule" and c.get("name") == "deviceName":
                    ctx["device"] = str(c.get("value") or "")
        atexit.register(host.restore)   # a run that stops on the access point still puts the host back
        # MoonDeck's Stop sends SIGTERM, whose default skips atexit; exiting through it runs the hook.
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))

    # Snapshot the board's user-added tree so we can restore it after the scenario:
    # a scenario that clear_children's a container (to get a known canvas) destroys the
    # board's real config, and the created_modules cleanup only removes what the scenario
    # ADDED, not what it CLEARED. Restoring the snapshot leaves the bench board as found.
    prior_controls: dict = {}   # (module, control) -> the value it held before this scenario wrote it
    replaced_types: dict = {}   # slot name -> the type it held before this scenario replaced it
    tree_snapshot = []
    if live_state is not None:   # skip restore if the pre-flight /api/state fetch failed (nothing to snapshot)
        try:
            tree_snapshot = _snapshot_tree(live_state)
        except Exception as e:
            print(f"  WARN: couldn't snapshot tree for restore: {e}")

    # Reset block: scenarios that mutate shared controls (Mirror toggles, grid
    # size, Preview detail, …) declare a `reset` array of set_control steps that
    # restores those controls to production defaults BEFORE the scenario runs.
    # Without this each scenario's measurements depend on whatever the previous
    # scenario left behind, so contract assertions become coupled to run order.
    # Reset failures fail-fast: a swallowed reset means the baseline reflects
    # the wrong state, which silently produces false-positive contract passes
    # (or false-negative failures) downstream. Better to abort cleanly here.
    reset_steps = scenario.get("reset", [])
    if reset_steps:
        print(f"\n  --- reset ({len(reset_steps)} steps) ---")
        for r_step in reset_steps:
            if r_step.get("op") != "set_control":
                continue
            try:
                client.post("/api/control", {
                    "module": r_step["id"],
                    "control": r_step["key"],
                    "value": r_step["value"]
                })
                print(f"  SET   {r_step.get('id','?')}.{r_step.get('key','?')} = {r_step.get('value','?')}")
            except Exception as e:
                print(f"  FAIL  reset {r_step.get('name','?')}: {e}", file=sys.stderr)
                results["passed"] = False
                results["reset_failed"] = f"{r_step.get('name','?')}: {e}"
                # Stop the scenario before collect_metrics: baseline would
                # otherwise reflect an unknown/partial state. No cleanup
                # needed: created_modules only fills inside the steps loop
                # below, which hasn't run yet.
                return results

    # Collect baseline AFTER reset so it reflects the normalized state.
    baseline = collect_metrics(client, settle_s=settle_s)
    print(f"\n  Baseline: tick={baseline.get('tickTimeUs', '?')}us (FPS={baseline.get('fps', '?')})  heap={baseline.get('freeHeap', '?')}")

    # ids whose optional add_module was skipped (a platform-gated module absent on this
    # target: e.g. the Parlio driver on a non-P4 board). A later optional measure/remove
    # that names a skipped id is itself skipped, so an absent driver leaves no trace rather
    # than failing the run. (perf_full's add/measure/remove driver triples are all optional.)
    skipped_ids = set()

    # Live runs `steps` only: `fixture` is the in-process equivalent of what
    # main.cpp already wired on the device.
    for step_index, step in enumerate(scenario.get("steps", [])):
        step_name = step.get("name", "?")
        op = step.get("op", "")
        fstep = _fill(step, ctx) if ctx else step   # a filled copy, so credentials never reach the file the run writes back
        step_result = {"name": step_name, "op": op}

        # An optional measure/control on a module whose optional add was skipped is a
        # no-op: the module isn't there to measure. Skip before any REST call.
        if step.get("optional") and step.get("id") in skipped_ids and op in ("measure", "set_control"):
            step_result["status"] = "ok"
            print(f"  {op:5} {step.get('id','?')}: skipped (optional, module not present on {target})")
            results["steps"].append(step_result)
            continue

        try:
            if op == "add_module":
                data = {"type": step["type"], "id": step.get("id", ""),
                        "parent_id": step.get("parent_id", "")}
                # An `optional` add of a type this target doesn't have is a SKIP, not a
                # fail: perf_full adds every LED driver (RMT/LCD/Parlio), but each is
                # platform-gated (LCD/RMT on classic+S3, Parlio on P4), so the absent
                # ones return "unknown type". The device replies either 400 (HTTPError)
                # or 200 + ok:false depending on the path; treat both as skip when the
                # step is optional. Mirrors the optional set_control handling below.
                try:
                    resp = client.post("/api/modules", data)
                    if resp.get("ok"):
                        step_result["status"] = "ok"
                        if resp.get("note") == "already exists":
                            print(f"  =     {step.get('id', '?')} (exists)")
                        else:
                            print(f"  +     {step.get('id', '?')} ({step['type']})")
                            created_modules.append(step.get("id", ""))
                        # The step's declared PROPS, applied whether the module was newly created or
                        # already existed. /api/modules takes the shape but not the values, so a
                        # scenario saying `{"width": 32}` measured a module at its defaults; and an
                        # existing module measured whatever the last run left on it.
                        for key, value in (step.get("props") or {}).items():
                            ok = False
                            try:
                                pr = client.post("/api/control",
                                                 {"module": step.get("id", ""), "control": key,
                                                  "value": value})
                                ok = bool(pr.get("ok"))
                            except urllib.error.HTTPError as pe:
                                if not step.get("optional"):
                                    raise
                                print(f"  SET   {step.get('id','?')}.{key}: skipped "
                                      f"(optional, not offered on {target}: {pe.code})")
                                continue
                            # A 200 with ok:false is a REJECTION, the same as a 400: the device
                            # refused the value. Silently accepting it measured a configuration the
                            # scenario never got.
                            if not ok:
                                if not step.get("optional"):
                                    raise RuntimeError(
                                        f"{step.get('id','?')}.{key} = {value!r} was rejected")
                                print(f"  SET   {step.get('id','?')}.{key}: skipped "
                                      f"(optional, rejected on {target})")
                        # The step's declared PROPS, applied after creation. /api/modules takes the
                        # shape but not the values, so a scenario saying `{"width": 32}` created a
                        # module at its defaults and every later measurement was of a pipeline the
                        # scenario never asked for. The desktop runner applies them; without this
                        # the same scenario measured two different things on the two runners.

                    elif step.get("optional"):
                        step_result["status"] = "ok"
                        skipped_ids.add(step.get("id", ""))
                        print(f"  +     {step.get('id','?')} ({step['type']}): skipped (optional, type unavailable on {target})")
                    else:
                        step_result["status"] = "error"
                except urllib.error.HTTPError:
                    if step.get("optional"):
                        step_result["status"] = "ok"
                        skipped_ids.add(step.get("id", ""))
                        print(f"  +     {step.get('id','?')} ({step['type']}): skipped (optional, type unavailable on {target})")
                    else:
                        raise

            elif op == "write_file":
                # Stage a script file, the same op the desktop runner has. Without it a migrated
                # scenario ran its set_control against a file that was never written, and every
                # script step failed on hardware while passing on the desktop.
                # A failed write FAILS THE SCENARIO. Every later step runs against a stale or
                # absent file, so reporting PASSED afterwards is the silent pass this op exists
                # to prevent: the same rule the desktop runner applies.
                path_ = step.get("path")
                body = step.get("value", "")
                if not path_:
                    print(f"  WRITE {step_name}: missing path")
                    step_result["status"] = "skipped" if step.get("optional") else "error"
                    if not step.get("optional"):
                        step_result["error"] = "write_file step has no `path`"
                        results["passed"] = False
                else:
                    try:
                        # Only a file the scenario creates is removed at the end: one that was already there is the device's own.
                        try:
                            before = client.get_text(f"/api/file?path={urllib.parse.quote(path_)}")
                            existed = True
                            prior_files.setdefault(path_, before)
                        except urllib.error.HTTPError as he:
                            existed = he.code != 404
                        resp = client.post_text(f"/api/file?path={urllib.parse.quote(path_)}", body)
                        # The device answers {"ok":true}; anything else is a failure, including a
                        # body that is not the JSON object this endpoint documents.
                        ok = isinstance(resp, dict) and resp.get("ok") is True
                        if ok and not existed:
                            created_files.append(path_)
                        step_result["status"] = "ok" if ok else "error"
                        if ok:
                            print(f"  WRITE {path_} ({len(body)} bytes)")
                        else:
                            step_result["error"] = f"unexpected response: {resp!r}"
                            print(f"  WRITE {path_}: FAILED: unexpected response {resp!r}")
                            results["passed"] = False
                    except Exception as we:
                        step_result["status"] = "error"
                        step_result["error"] = str(we)
                        print(f"  WRITE {path_}: FAILED: {we}")
                        results["passed"] = False

            elif op == "delete_file":
                # Remove a file the scenario staged, so a run leaves the device as it found it.
                # A file already gone counts as removed, the same rule the desktop runner applies.
                path_ = step.get("path")
                if not path_:
                    print(f"  DELETE {step_name}: missing path")
                    step_result["status"] = "error"
                    step_result["error"] = "delete_file step has no `path`"
                    results["passed"] = False
                else:
                    if path_ in created_files:
                        created_files.remove(path_)   # removed here, so the end-of-run cleanup leaves it be
                    try:
                        client.delete(f"/api/dir?path={urllib.parse.quote(path_)}")
                    except Exception:
                        pass   # a 500 for a file that was never there is the case this tolerates
                    # Only a 404 proves it gone: an existing file answers with its text, which is not JSON.
                    gone = False
                    try:
                        client.get(f"/api/file?path={urllib.parse.quote(path_)}")
                    except urllib.error.HTTPError as he:
                        gone = he.code == 404
                    except Exception:
                        pass
                    if gone:
                        step_result["status"] = "ok"
                        print(f"  DELETE {path_}")
                    else:
                        step_result["status"] = "error"
                        step_result["error"] = f"{path_} is still there"
                        print(f"  DELETE {path_}: FAILED: still there")
                        results["passed"] = False

            elif op == "reboot" and step.get("wait") is False:
                # Restart without waiting, for a device that comes back somewhere the host cannot reach yet, such as on its own access point.
                # A refusal is retried, since an answering device is up; its own words are kept for the report.
                refusal = ""
                for _ in range(3):
                    try:
                        client.post("/api/reboot", {})
                        refusal = ""
                        break
                    except urllib.error.HTTPError as re_:
                        refusal = f"/api/reboot returned HTTP {re_.code}: {re_.read().decode('utf-8', 'replace')[:120]}"
                        time.sleep(1)
                    except Exception:
                        refusal = ""
                        break   # the device goes away mid-response
                if refusal:
                    raise RuntimeError(refusal)
                step_result["status"] = "ok"
                print("  REBOOT: not waiting")

            elif op == "reboot":
                # Restart and wait, so a later expect_control proves what SURVIVED rather than what
                # is merely still in memory. The one op that can tell a written setting from a kept one.
                why = _reboot_and_wait(client, target, float(step.get("timeout", 60)))
                if why:
                    step_result["status"] = "error"
                    print(f"  REBOOT: {why}")
                    results["passed"] = False
                else:
                    step_result["status"] = "ok"
                    print("  REBOOT: back up")

            elif op == "expect_file":
                # Read a file back, which is the only way to prove a write reached the filesystem
                # rather than a cache: the card that owns the filesystem is proven by its contents.
                path_ = step["path"]
                # `contains` matches a substring, `equals` the whole file: the two tiers agree, and
                # neither accepts a step that names neither, which would pass on any file at all.
                exact = "equals" in step
                if not exact and "contains" not in step:
                    step_result["status"] = "error"
                    print(f"  EXPECT {path_}: needs `contains` or `equals`")
                    results["passed"] = False
                    continue
                want = str(step["equals"] if exact else step["contains"])
                try:
                    raw = client.get_text(f"/api/file?path={urllib.parse.quote(path_, safe='/')}")
                except Exception as fe:
                    step_result["status"] = "error"
                    print(f"  EXPECT {path_}: could not read ({fe})")
                    results["passed"] = False
                else:
                    holds = (raw == want) if exact else (bool(want) and want in raw)
                    step_result["status"] = "ok" if holds else "error"
                    verb = "is" if exact else "holds"
                    print(f"  EXPECT {path_} {verb if holds else 'does not ' + verb} {want!r}")
                    if not holds:
                        results["passed"] = False

            elif op == "apply_state":
                # A state document in one PATCH /api/state; `error` and `at` name the failure a step expects, and without them it must apply.
                # The modules it creates join the cleanup, as an add_module step's do, so a live device is left as found.
                absent = []
                for name in _typed_names(fstep["document"]):
                    try:
                        client.get(_mod_path(name))
                    except urllib.error.HTTPError:
                        absent.append(name)
                try:
                    resp = client.patch("/api/state", fstep["document"])
                    got_error, got_at = None, None
                except urllib.error.HTTPError as he:
                    body = _error_body(he)
                    got_error, got_at = body.get("error") or f"HTTP {he.code}", body.get("at")
                for name in absent:
                    try:
                        client.get(_mod_path(name))
                        created_modules.append(name)
                    except urllib.error.HTTPError:
                        pass   # the document did not get as far as creating it
                if "error" in step:
                    holds = got_error == step["error"] and ("at" not in step or got_at == step["at"])
                else:
                    holds = got_error is None
                step_result["status"] = "ok" if holds else "error"
                print(f"  STATE {step['name']}: {'applied' if got_error is None else got_error + ' at ' + str(got_at)}{'' if holds else '  (expected ' + str(step.get('error', 'success')) + ')'}")
                if not holds:
                    results["passed"] = False

            elif op == "round_trip_state":
                # Every card read back as its document and sent as one: what the device shows applies as it is, and the body outgrows the request buffer, so the streaming route runs.
                whole = {}
                for mod in client.get("/api/state").get("modules", []):
                    whole.update(client.get(_mod_path(mod["name"]) + "/document"))
                try:
                    client.patch("/api/state", whole)
                    failed = None
                except urllib.error.HTTPError as he:
                    body = _error_body(he)
                    failed = f"{body.get('error') or 'HTTP ' + str(he.code)} at {body.get('at')}"
                step_result["status"] = "ok" if failed is None else "error"
                print(f"  STATE {step['name']}: {len(whole)} cards, {len(json.dumps(whole))} bytes, {'applied' if failed is None else failed}")
                if failed is not None:
                    results["passed"] = False

            elif op == "expect_control":
                # Assert a control reads what the scenario says it must, the only op that fails a
                # scenario on a VALUE rather than on a timing contract. It exists for the strings a
                # rename would change silently, where nothing else can see the break.
                # Read through /api/modules/<id>, the same view a client gets, so an assertion can
                # never pass against a value the device would report differently.
                mod_id, key = step["id"], step["key"]
                # JSON spells a boolean `true`, Python spells it `True`, and the in-process runner
                # renders the JSON form: comparing str() of either would make one tier disagree
                # with the other about the same scenario.
                def _as_written(v):
                    return {True: "true", False: "false"}.get(v, str(v)) if isinstance(v, bool) else str(v)
                # `not_equals` pins a value that moves per release, where the only stable claim is
                # that it is not the empty string a missing source would render. Both tiers agree.
                negated = "equals" not in step and "not_equals" in step
                if "equals" not in step and not negated:
                    step_result["status"] = "error"
                    print(f"  EXPECT {mod_id}.{key}: needs `equals` or `not_equals`")
                    results["passed"] = False
                    continue
                want = _as_written(fstep["not_equals" if negated else "equals"])
                # `within` polls until it holds, for a value the device reaches on its own time, such as after a join.
                deadline = time.time() + float(step.get("within", 0))
                while True:
                    read_error = None
                    try:
                        mod = client.get(_mod_path(mod_id))
                        got = next((c.get("value") for c in (mod.get("controls") or [])
                                    if c.get("name") == key), None)
                    except Exception as ce:
                        got, read_error = None, ce
                    holds = got is not None and ((_as_written(got) != want) if negated else (_as_written(got) == want))
                    if holds or time.time() >= deadline:
                        break
                    time.sleep(1)
                if read_error is not None:
                    step_result["status"] = "error"
                    print(f"  EXPECT {mod_id}.{key}: could not read ({read_error})")
                    results["passed"] = False
                    continue
                if got is None:
                    step_result["status"] = "error"
                    print(f"  EXPECT {mod_id}.{key}: no such control")
                    results["passed"] = False
                elif (_as_written(got) != want) if negated else (_as_written(got) == want):
                    step_result["status"] = "ok"
                    print(f"  EXPECT {mod_id}.{key} {'!=' if negated else '=='} {want}")
                else:
                    step_result["status"] = "error"
                    print(f'  EXPECT {mod_id}.{key} is "{_as_written(got)}", '
                          f'expected {"not " if negated else ""}"{want}"')
                    results["passed"] = False

            elif op == "set_control":
                data = {"module": step["id"], "control": step["key"],
                        "value": step["value"]}
                _remember_control(prior_controls, client, step["id"], step["key"])
                try:
                    resp = client.post("/api/control", data)
                    step_result["status"] = "ok" if resp.get("ok") else "error"
                    print(f"  SET   {step.get('id', '?')}.{step.get('key', '?')} = {step.get('value', '?')}")
                except urllib.error.HTTPError as ce:
                    if step.get("optional") and ce.code == 404:
                        # An `optional` set_control on a missing module (e.g. shrink
                        # a grid a prior run's cleanup removed) is a no-op, not a fail.
                        step_result["status"] = "ok"
                        print(f"  SET   {step.get('id','?')}.{step.get('key','?')}: skipped (optional, not present)")
                    elif step.get("optional") and ce.code == 400:
                        # An `optional` set_control the device REJECTS with 400 (e.g.
                        # "value out of range") is a not-available-here skip, not a
                        # failure: e.g. selecting a `peripheral` value this chip doesn't
                        # offer (Parlio on an S3, MoonI80 on a classic): the Select's max
                        # is the board-filtered option count, so the value is out of range
                        # and returns 400. A REQUIRED set_control that 400s still fails.
                        step_result["status"] = "skipped"
                        # And the MODULE is unavailable from here on. Marking only the step left
                        # every later measure running against a module configured for a peripheral
                        # this chip does not have: the numbers came out, looked like data, and
                        # described a configuration that never applied. skipped_ids is the same set
                        # an optional add uses, so the measures already know to skip it.
                        skipped_ids.add(step.get("id", ""))
                        print(f"  SET   {step.get('id','?')}.{step.get('key','?')} = {step.get('value','?')}: skipped (optional, value not offered on this target; later steps on it skip too)")
                    elif ce.code == 404:
                        # Transient: a set_control issued right after a structural
                        # change (replace/add) can race the device's prepareTree and
                        # briefly see "module not found" while the tree rebuilds.
                        # Settle and retry once before treating it as a real failure.
                        time.sleep(1.0)
                        resp = client.post("/api/control", data)
                        step_result["status"] = "ok" if resp.get("ok") else "error"
                        print(f"  SET   {step.get('id','?')}.{step.get('key','?')} = {step.get('value','?')} (retried)")
                    else:
                        raise
                # If this step doesn't measure (so `collect_metrics` won't wait
                # for us), still give the device a moment: a set_control that
                # triggers prepareTree briefly mutates the module tree, and the
                # very next API call can hit a transient "module not found".
                # 500 ms is empirically enough on the classic board; cheap insurance.
                if not (step.get("measure") or op == "measure"):
                    time.sleep(0.5)

            elif op in ("delete_module", "remove_module"):
                # Both names mean the same thing: accept either so a scenario
                # reads identically on the in-process runner (which uses
                # `remove_module`) and here. The two runners must never diverge
                # on op names, or a scenario silently no-ops on one tier.
                # An `optional` remove of a module that was never added (its
                # optional add was skipped: a platform-gated driver absent on this
                # target) is a SKIP, not a fail: the device returns 404 "module not
                # found" or ok:false. Pairs with the optional add above.
                try:
                    resp = client.delete(_mod_path(step["id"]))
                    if resp.get("ok") or not step.get("optional"):
                        step_result["status"] = "ok" if resp.get("ok") else "error"
                        print(f"  -     {step.get('id', '?')}")
                    else:
                        step_result["status"] = "ok"
                        print(f"  -     {step.get('id','?')}: skipped (optional, not present)")
                except urllib.error.HTTPError:
                    if step.get("optional"):
                        step_result["status"] = "ok"
                        print(f"  -     {step.get('id','?')}: skipped (optional, not present)")
                    else:
                        raise

            elif op == "clear_children":
                # Delete every child of a container, leaving the container.
                # The "prepare my own canvas" primitive: a scenario assumes
                # nothing about the device's starting tree. Enumerate children
                # from /api/state, DELETE each by name. The device tears down the
                # whole subtree per delete (handleDeleteModule), so clearing a
                # Layer's effect also drops any modifier under it.
                container_id = step["id"]
                state = client.get("/api/state")
                child_names = _child_names_of(state, container_id)
                cleared = skipped = 0
                for cn in child_names:
                    try:
                        client.delete(_mod_path(cn))
                        cleared += 1
                    except urllib.error.HTTPError as de:
                        # Non-deletable submodules (Preview, Board, Improv) return
                        # 400 "module not deletable": that's expected, skip them.
                        # Mirrors the in-process op, which skips !userEditable().
                        # Re-raise anything that isn't a clean deletability refusal.
                        if de.code == 400:
                            skipped += 1
                        else:
                            raise
                step_result["status"] = "ok"
                tail = f", {skipped} kept" if skipped else ""
                print(f"  clr   {container_id} ({cleared} cleared{tail})")
                time.sleep(0.5)  # let prepareTree settle before the next add

            elif op == "replace_module":
                # Swap a child for a fresh module of another type at the same slot.
                # The name is passed explicitly: the device otherwise renames a slot that carried its old type's default name, and the scenario, like the in-process op, keeps addressing the slot by its id.
                _remember_type(replaced_types, client, step["id"])
                resp = client.post(_mod_path(step["id"]) + "/replace",
                                   {"type": step["type"], "name": step["id"]})
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  ~     {step.get('id', '?')} → {step.get('type', '?')}")
                time.sleep(0.5)

            elif op == "host_wifi":
                # Move the host onto the device's access point or back to the registry network, and the runner's target with it.
                if net is None:
                    raise RuntimeError("host_wifi needs `host_network` on the scenario and --network on the run")
                if step.get("join") == "access_point":
                    if not ctx.get("device"):
                        raise RuntimeError("the device's name is unknown, so its access point cannot be named to join")
                    host.home = host.home or net
                    why = _host_join(ctx.get("device", ""), fstep.get("password", ""), f"http://{ACCESS_POINT_ADDRESS}/api/system",
                                     float(step.get("timeout", 60)))
                    if not why:
                        client.base = f"http://{ACCESS_POINT_ADDRESS}"
                else:
                    client.base = home_base
                    why = _host_join(net["ssid"], net["password"], f"{home_base}/api/system",
                                     float(step.get("timeout", 60)))
                    if not why:
                        host.home = None   # home: nothing left for the exit hook to restore
                step_result["status"] = "error" if why else "ok"
                print(f"  HOST  {'FAILED: ' + why if why else 'on ' + (ctx.get('device', '') if step.get('join') == 'access_point' else net['ssid'])}")
                if why:
                    results["passed"] = False

            elif op == "list_row" and "add" in step:
                # Add a row with the given fields, and with `to` move it to that position, as the card's Add and drag do.
                mod_id, key = step["id"], step["key"]
                route = f"/api/list/{urllib.parse.quote(mod_id, safe='')}/{urllib.parse.quote(key, safe='')}"
                row_id = client.post(route, {}).get("id")
                if row_id is None:
                    raise RuntimeError(f"{mod_id}.{key} gave no id for the new row")
                for field, value in fstep["add"].items():
                    client.patch(f"{route}/{row_id}", {"field": field, "value": value})
                if "to" in step:
                    client.patch(f"{route}/{row_id}", {"to": step["to"]})
                step_result["status"] = "ok"
                print(f"  ROW   {mod_id}.{key} added {step['add']}{' at ' + str(step['to']) if 'to' in step else ''}")

            elif op == "list_row":
                # Find a row by its fields, then write one of its fields, press one of its buttons, delete it, or, with none of those, only expect it.
                mod_id, key, match = step["id"], step["key"], fstep["match"]
                deadline = time.time() + float(step.get("within", 0))
                while True:
                    try:
                        rows = _list_rows(client, mod_id, key)
                    except Exception:
                        rows = []
                    row = next((r for r in rows if all(str(r.get(k)) == str(v) for k, v in match.items())), None)
                    if row is not None or time.time() >= deadline:
                        break
                    time.sleep(1)
                where = f"{mod_id}.{key}[{step.get('match')}]"
                if row is None:
                    step_result["status"] = "ok" if step.get("optional") else "error"
                    print(f"  ROW   {where}: {'absent, skipped (optional)' if step.get('optional') else 'no such row'}")
                    if not step.get("optional"):
                        results["passed"] = False
                else:
                    route = f"/api/list/{urllib.parse.quote(mod_id, safe='')}/{urllib.parse.quote(key, safe='')}/{row['id']}"
                    if step.get("delete"):
                        client.delete(route)
                        print(f"  ROW   {where} deleted")
                    elif "field" not in step:
                        print(f"  ROW   {where} present")
                    else:
                        client.patch(route, {"field": step["field"], "value": fstep.get("value", "")})
                        print(f"  ROW   {where}.{step['field']} set")
                    step_result["status"] = "ok"

            elif op == "expect_http":
                # Redirects are not followed, so a captive portal's own answer is what is checked.
                class _NoRedirect(urllib.request.HTTPRedirectHandler):
                    def redirect_request(self, *args, **kwargs):
                        return None
                url = fstep["url"]
                request = _http_request(url, client.base, step.get("method", "GET"))
                if step.get("resolve") == "access_point":
                    # Resolve at the access point's own DNS, as a phone does: macOS keeps a captive network's answers from ordinary apps until its sign-in completes.
                    parts = urllib.parse.urlsplit(url)
                    dig = subprocess.run(["dig", "+time=2", "+tries=3", "+short", f"@{ACCESS_POINT_ADDRESS}", parts.hostname or ""],
                                         capture_output=True, text=True)
                    resolved = (dig.stdout.strip().splitlines() or ["0.0.0.0"])[-1]
                    print(f"  DNS   {ACCESS_POINT_ADDRESS} resolves {parts.hostname} to {resolved}")
                    request = urllib.request.Request(urllib.parse.urlunsplit(parts._replace(netloc=resolved)),
                                                     data=request.data, method=request.get_method(),
                                                     headers={"Host": parts.netloc})
                # `within` polls too: a host that has joined a network resolves names only once its resolver took that network's server.
                deadline = time.time() + float(step.get("within", 0))
                while True:
                    try:
                        with urllib.request.build_opener(_NoRedirect).open(request, timeout=10) as resp:
                            status, location = resp.status, resp.headers.get("Location", "")
                    except urllib.error.HTTPError as he:
                        status, location = he.code, he.headers.get("Location", "")
                    except Exception as ue:
                        status, location = 0, str(ue)
                    holds = status == step["status"] and ("location" not in step or location == fstep["location"])
                    if holds or time.time() >= deadline:
                        break
                    time.sleep(1)
                if not holds and status == 0:
                    # A name that did not resolve: show which servers the host asks, and what the device's own address answers.
                    name = urllib.parse.urlsplit(url).hostname or ""
                    servers = subprocess.run(["scutil", "--dns"], capture_output=True, text=True).stdout
                    print("  DNS   host asks: " + ", ".join(sorted({l.split(":", 1)[1].strip() for l in servers.splitlines() if "nameserver[" in l})))
                    dig = subprocess.run(["dig", "+time=2", "+tries=1", "+short", f"@{ACCESS_POINT_ADDRESS}", name], capture_output=True, text=True)
                    print(f"  DNS   {ACCESS_POINT_ADDRESS} answers {name}: {(dig.stdout or dig.stderr).strip() or 'nothing'}")
                step_result["status"] = "ok" if holds else "error"
                print(f"  HTTP  {step.get('method', 'GET')} {url} → {status}{' ' + location if location else ''}{'' if holds else '  (expected ' + str(step['status']) + ' ' + fstep.get('location', '') + ')'}")
                if not holds:
                    results["passed"] = False

            elif op == "measure":
                # Pure measurement step (introduced for the build-up scenario shape).
                # No REST call; the measure block below picks it up via step["measure"]
                # or the implicit-measure clause we add to the same dispatcher.
                step_result["status"] = "ok"

            else:
                step_result["status"] = "skipped"
                print(f"  SKIP  {step_name} (unknown op: {op})")

        except urllib.error.HTTPError as e:
            # Read the JSON error body for a friendly message
            try:
                body = json.loads(e.read())
                msg = body.get("error", str(e))
            except Exception:
                msg = str(e)
            step_result["status"] = "error"
            step_result["error"] = msg
            # Every rejected step is a real failure. The old policy WARN'd on
            # add_module which silently turned "top-level rejected" into a
            # missing test step: meaningless passes. Mutate scenarios shouldn't
            # add top-level anyway; if they do, treat it as a scenario bug.
            print(f"  FAIL  {step_name}: {msg}")
            results["passed"] = False
        except Exception as e:
            step_result["status"] = "error"
            step_result["error"] = str(e)
            print(f"  FAIL  {step_name}: {e}")
            results["passed"] = False

        # Measure after this step if requested (explicit "measure": true OR op == "measure").
        # Skip the measurement block when the step itself failed: writing observed
        # values from a failed-step state would persist garbage as the "latest
        # reading" and the contract assertion would compare against an
        # untrustworthy measurement.
        if step_result.get("status") == "error":
            results["steps"].append(step_result)
            continue
        if step.get("measure") or op == "measure":
            # collect_metrics hits /api/state: a missing measurement is a
            # failed run, not a no-op to skip. Silent-skip would let a broken
            # device pass a scenario that asserts on observed/contract data
            # the step never gathered. Fail loudly, record the error on the
            # step, and break out of the step loop so end-of-run cleanup
            # (delete created modules) still fires.
            try:
                metrics = collect_metrics(client, settle_s)
            except Exception as e:
                print(f"  FAIL  {step_name}: collect_metrics failed: {e}")
                step_result["status"] = "error"
                step_result["error"] = f"collect_metrics: {e}"
                step_result["metrics"] = {}
                results["passed"] = False
                results["steps"].append(step_result)
                break  # stop step loop; cleanup runs below
            step_result["metrics"] = metrics
            tick_us = metrics.get("tickTimeUs", 0)
            fps = 1000000 // tick_us if tick_us > 0 else metrics.get("fps", 0)
            heap = metrics.get("freeHeap", 0)
            max_block = metrics.get("maxBlock", 0)
            model_bytes = metrics.get("dynamicBytesTotal")
            print(f"  MEASURE  tick={tick_us}us (FPS={fps})  heap={heap}  "
                  f"block={max_block}  model={model_bytes}")

            # Per-step contract: { "contract": { "<target>": { "tick_us": N,
            #   "free_heap": M, "tick_tolerance_pct": P, "heap_tolerance_pct": Q,
            #   "set_by": "YYYY-MM-DD", "reason": "..." } } }
            # Contracts are hand-set promises: see docs/reference/testing.md § Performance
            # contracts. `--update-contract --reason "..."` rewrites them.
            contract_block = step.get("contract", {}).get(target) if step.get("contract") else None
            if contract_block:
                # Defaults reflect run-to-run variance, not "I don't care":
                #   pc-*      : multi-process OS jitter, 20% pct + 200us absolute
                #                floor. The floor dominates below ~1ms tick (the
                #                realistic case for desktop scenarios today).
                #   esp32-*   : bounded RTOS but lwIP/EMAC jitter, 10% pct + 5us
                #                absolute floor.
                # KEEP IN SYNC: the in-process runner re-declares the same defaults
                # at test/scenario_runner.cpp contract-block handler: tuning one
                # without the other silently desyncs the two tiers.
                is_desktop = target.startswith("desktop-")
                tick_tol_pct = contract_block.get("tick_tolerance_pct",
                                                  20 if is_desktop else 10)
                heap_tol_pct = contract_block.get("heap_tolerance_pct",
                                                  20 if is_desktop else 10)
                tol_us_abs = contract_block.get("tolerance_us", 200 if is_desktop else 5)
                exp_tick = contract_block.get("tick_us")
                exp_heap = contract_block.get("free_heap")
                if exp_tick is not None and exp_tick > 0:
                    # tick contract is a *ceiling*: faster than contract is good
                    # news (mirror of heap being a floor). Tolerance absorbs
                    # upward jitter only; speedups never fail.
                    overshoot = tick_us - exp_tick
                    allowed = max(exp_tick * tick_tol_pct / 100.0, tol_us_abs)
                    if overshoot <= 0:
                        print(f"  PASS  tick {tick_us}us <= contract {exp_tick}us "
                              f"(margin {-overshoot:.0f}us)")
                    elif overshoot > allowed:
                        print(f"  FAIL  tick {tick_us}us vs contract {exp_tick}us "
                              f"(over by {overshoot:.0f}us > allowed {allowed:.0f}us)")
                        results["passed"] = False
                    else:
                        print(f"  PASS  tick {tick_us}us vs contract {exp_tick}us "
                              f"(over by {overshoot:.0f}us within {allowed:.0f}us)")
                if exp_heap is not None and exp_heap > 0:
                    # Contract is a *floor*: the device must deliver at least this
                    # much free heap. More is better; less by more than tolerance is
                    # a regression. Tolerance applies because of legitimate run-to-
                    # run drift in lwIP/TCP buffer pools.
                    drop_pct = (exp_heap - heap) * 100.0 / exp_heap if heap < exp_heap else 0
                    if drop_pct > heap_tol_pct:
                        print(f"  FAIL  free_heap {heap} dropped {drop_pct:.1f}% "
                              f"below contract {exp_heap}")
                        results["passed"] = False
                    else:
                        print(f"  PASS  free_heap {heap} >= contract {exp_heap} "
                              f"(within -{heap_tol_pct}% tolerance)")
                # max_alloc_block contract is also a *floor*: opt-in per scenario.
                # The LUT/buffer allocators need a single contiguous chunk; on a
                # fragmented heap the largest block can be much smaller than free
                # heap, and Layer silently degrades to 1:1 (mirror disappears) when
                # the LUT won't fit. Scenarios that depend on that allocation
                # succeeding assert a minimum block here.
                exp_block = contract_block.get("max_alloc_block")
                if exp_block is not None and exp_block > 0:
                    # max_block of 0 always fails when a positive floor is
                    # asserted: maxBlock is always served by current firmware
                    # (src/core/system/HttpServerModule.cpp), so 0 means the device
                    # reports zero contiguous heap: a real failure, not a
                    # missing field. (Contrast with free_heap on desktop where 0
                    # is the "unlimited" sentinel: that's a desktop-only
                    # convention not used by the live runner.)
                    if max_block <= 0:
                        print(f"  FAIL  max_alloc_block {max_block} (device reports no "
                              f"contiguous heap) vs contract {exp_block}")
                        results["passed"] = False
                    else:
                        drop_pct = (exp_block - max_block) * 100.0 / exp_block if max_block < exp_block else 0
                        if drop_pct > heap_tol_pct:
                            print(f"  FAIL  max_alloc_block {max_block} dropped {drop_pct:.1f}% "
                                  f"below contract {exp_block}")
                            results["passed"] = False
                        else:
                            print(f"  PASS  max_alloc_block {max_block} >= contract {exp_block} "
                                  f"(within -{heap_tol_pct}% tolerance)")

            # observed.<target> keeps a rolling window of samples per scalar and
            # reports p50/p95/min/max/n over it, so a single slow run shifts the tail
            # rather than the headline. When --update-contract is set, the window
            # described the PREVIOUS promise, so reset to the current point.
            # See moondeck/scenario/_observed.py.
            sample = {
                "tick_us": int(tick_us),
                "free_heap": int(heap),
                "max_alloc_block": int(max_block),
            }
            existing_obs = step.get("observed", {}).get(target)
            if update_contract:
                new_obs = _observed.reset(sample, _today_iso())
                obs_changed = True
            else:
                new_obs, obs_changed = _observed.widen(existing_obs, sample, _today_iso())
            if obs_changed:
                step.setdefault("observed", {})[target] = new_obs
                wrote_observations[0] = True

            # --update-contract: rewrite the contract in the scenario JSON for the
            # active target. This is *renegotiating* a contract, not refreshing a
            # last-reading baseline: set_by + reason are stamped so the diff
            # records when and why the promise changed. Caller is responsible for
            # committing the diff intentionally.
            #
            # Originals are stashed in pending_contract_originals so the
            # post-run gate (see below) can roll the in-memory tree back to
            # disk shape if the run failed: only successful runs get to
            # commit a renegotiated promise.
            if update_contract:
                # Preserve any per-step tolerance overrides already in place.
                # Key by step INDEX, not the step dict: a dict is unhashable, so
                # `(step, target)` as a key raised TypeError (this whole path is
                # only reached with --update-contract, which the gates don't pass).
                existing = step.get("contract", {}).get(target, {})
                if (step_index, target) not in pending_contract_originals:
                    # Deep enough copy: existing is a flat dict of scalars.
                    pending_contract_originals[(step_index, target)] = (
                        dict(existing) if existing else None
                    )
                new_block = {
                    "tick_us": int(tick_us),
                    "free_heap": int(heap),
                    "set_by": _today_iso(),
                    "reason": update_reason or existing.get("reason", "updated"),
                }
                for k in ("tick_tolerance_pct", "heap_tolerance_pct", "tolerance_us"):
                    if k in existing:
                        new_block[k] = existing[k]
                # max_alloc_block: opt-in (only carry it over if the existing
                # contract had it), but refresh the value from this run rather
                # than copying the stale one. Mirrors run_scenario.py's update
                # path: keep both files in sync if you change one.
                if "max_alloc_block" in existing:
                    new_block["max_alloc_block"] = int(max_block)
                step.setdefault("contract", {})[target] = new_block

            # Check bounds
            bounds = step.get("bounds", {})
            if "fps" in bounds:
                # Absolute minimum
                if "min" in bounds["fps"]:
                    min_fps = bounds["fps"]["min"]
                    if fps < min_fps:
                        print(f"  FAIL  fps {fps} < {min_fps}")
                        results["passed"] = False
                    else:
                        print(f"  PASS  fps >= {min_fps}")
                # Relative to baseline (percentage)
                if "min_pct" in bounds["fps"] and baseline.get("fps", 0) > 0:
                    min_pct = bounds["fps"]["min_pct"]
                    threshold = int(baseline["fps"] * min_pct / 100)
                    if fps < threshold:
                        print(f"  FAIL  fps {fps} < {threshold} ({min_pct}% of baseline {baseline['fps']})")
                        results["passed"] = False
                    else:
                        print(f"  PASS  fps {fps} >= {min_pct}% of baseline")
                # FPS×lights throughput floor: compared against the measured
                # tick *time* (the device's native unit), not derived FPS.
                # Per-grid budget: max_tick_us = lights * 1e6 / product.
                if "min_fps_led_product" in bounds["fps"]:
                    product = bounds["fps"]["min_fps_led_product"]
                    lights = count_lights(client)
                    if not isinstance(product, (int, float)) or product <= 0:
                        print(f"  WARN  min_fps_led_product: invalid value "
                              f"{product!r}, skipped")
                    elif lights > 0 and tick_us > 0:
                        max_tick = round(lights * 1_000_000 / product)
                        if tick_us > max_tick:
                            print(f"  FAIL  tick {tick_us}us > {max_tick}us "
                                  f"(throughput budget for {lights} lights)")
                            results["passed"] = False
                        else:
                            print(f"  PASS  tick {tick_us}us <= {max_tick}us ({lights} lights)")
                    else:
                        print("  WARN  min_fps_led_product: no layout lights / tick, skipped")

        results["steps"].append(step_result)

    # The host back on its own network before anything else talks to the device, which is then there.
    if host.home:
        host.restore()
    client.base = home_base

    # Cleanup: delete modules that were created by this scenario
    for module_id in reversed(created_modules):
        try:
            client.delete(_mod_path(module_id))
            print(f"  -     {module_id} (cleanup)")
        except Exception:
            pass

    # Swap every replaced slot back to its type before the tree restore, which only re-creates what is missing.
    swapped_back = _restore_types(client, replaced_types)
    if swapped_back:
        print(f"  restored {swapped_back} module(s) the scenario had replaced")

    # Restore: re-create any pre-existing module the scenario removed (a clear_children of a real container, or a remove).
    # Combined with the cleanup and the swap-back above, the board ends the run in the tree it started with: no residue, no lost config.
    if tree_snapshot:
        try:
            after_state = client.get("/api/state")
            _restore_tree(client, tree_snapshot, after_state)
        except Exception as e:
            print(f"  WARN: couldn't restore snapshot: {e}")
    # And the controls it wrote, after the tree, so a module the restore re-created takes its values.
    put_back = _restore_controls(client, prior_controls)
    if put_back:
        print(f"  restored {put_back} control(s) the scenario had set")
    # And the files it created that no step removed, after the controls, so nothing still selects a file as it goes.
    for path_ in created_files:
        try:
            client.delete(f"/api/dir?path={urllib.parse.quote(path_)}")
            print(f"  -     {path_} (cleanup)")
        except Exception:
            pass
    for path_, before in prior_files.items():
        try:
            client.post_text(f"/api/file?path={urllib.parse.quote(path_)}", before)
            print(f"  ~     {path_} (restored)")
        except Exception as e:
            print(f"  WARN  restore of {path_} failed: {e}")

    # Write the scenario JSON back if anything changed:
    #   - observed.<target> was updated by any measure step (every run); OR
    #   - --update-contract renegotiated the contract: AND the run passed
    #     (don't persist a renegotiated promise from a half-broken run; the
    #     observed values still land so drift is visible either way).
    # If the contract was renegotiated but the run failed, the in-memory
    # mutations stay in `scenario` only until the process exits; the on-disk
    # contract is preserved.
    contract_safe_to_write = update_contract and results["passed"]
    if not contract_safe_to_write and update_contract:
        # Revert any contract mutations we made to the in-memory tree so the
        # JSON write below (for observed) doesn't leak them to disk.
        for step_index, step in enumerate(scenario.get("steps", [])):
            contract = step.get("contract")
            if contract and target in contract:
                if (step_index, target) in pending_contract_originals:
                    orig = pending_contract_originals[(step_index, target)]
                    if orig is None:
                        del contract[target]
                    else:
                        contract[target] = orig
        print(f"  contract[{target}] NOT written (run failed; observed still saved)")

    if wrote_observations[0] or contract_safe_to_write:
        _observed.save_scenario(scenario_path, scenario)
        what = []
        if wrote_observations[0]:
            what.append(f"observed[{target}]")
        if contract_safe_to_write:
            what.append(f"contract[{target}]")
        print(f"  WROTE  {scenario_path.name} ({' + '.join(what)})")

    # Summary
    print("\n---")
    if results["passed"]:
        print("PASSED")
    else:
        print("FAILED")

    return results


def load_baseline() -> dict:
    if BASELINE_FILE.exists():
        with open(BASELINE_FILE, encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_baseline(data: dict):
    with open(BASELINE_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def compare_baseline(results: dict, baseline: dict):
    """Compare results against baseline, report regressions."""
    name = results["name"]
    if name not in baseline:
        print(f"  No baseline for '{name}': run with --update-baseline first")
        return

    base = baseline[name]
    regressions = []

    for step, base_step in zip(results.get("steps", []), base.get("steps", [])):
        if "metrics" not in step or "metrics" not in base_step:
            continue
        m = step["metrics"]
        bm = base_step["metrics"]

        step_name = step.get("name", "?")

        # FPS regression > 10%
        if bm.get("fps", 0) > 0 and m.get("fps", 0) > 0:
            pct = (bm["fps"] - m["fps"]) / bm["fps"] * 100
            if pct > 10:
                regressions.append(f"{step_name}: FPS dropped {pct:.0f}% ({bm['fps']} → {m['fps']})")

        # Heap regression > 10KB
        if bm.get("freeHeap", 0) > 0 and m.get("freeHeap", 0) > 0:
            delta = bm["freeHeap"] - m["freeHeap"]
            if delta > 10240:
                regressions.append(f"{step_name}: heap dropped {delta // 1024}KB")

    if regressions:
        print(f"\n  REGRESSIONS for '{name}':")
        for r in regressions:
            print(f"    {r}")
    else:
        print(f"\n  No regressions for '{name}'")


def main():
    parser = argparse.ArgumentParser(description="Run live scenario tests")
    parser.add_argument("--host", default="localhost:8080",
                        help="Device host:port (default: localhost:8080)")
    parser.add_argument("--name", default=None,
                        help="Scenario name (without .json), or several separated by commas. Runs all if omitted.")
    parser.add_argument("--module", default=None,
                        help="Filter to scenarios whose top-level module / also matches.")
    parser.add_argument("--network", default=None,
                        help="Registry network (moondeck.json) the host and device share, for a scenario that moves the host's WiFi.")
    parser.add_argument("--settle", type=float, default=3.0,
                        help="Settle time in seconds between step and measurement")
    parser.add_argument("--update-baseline", action="store_true",
                        help="Save results as new baseline")
    parser.add_argument("--compare-baseline", action="store_true",
                        help="Compare results against stored baseline")
    parser.add_argument("--update-contract", action="store_true",
                        help=("Renegotiate the per-step performance contract: write "
                              "observed tick/heap into contract[<target>] and stamp "
                              "set_by + reason. Requires --reason. Overwrites existing "
                              "values for the active target only; other targets untouched."))
    parser.add_argument("--reason", default=None,
                        help=("Why the contract is being renegotiated (required with "
                              "--update-contract). Examples: 'tighter Layer LUT copy', "
                              "'accepted DMX driver overhead'. Written into each updated "
                              "contract block."))
    args = parser.parse_args()

    if args.update_contract and not args.reason:
        parser.error("--update-contract requires --reason "
                     "(e.g. --reason 'tightened after Layer optimization')")

    client = Client(args.host)

    # Verify connection
    try:
        state = client.get("/api/state")
        module_count = len(state.get("modules", []))
        print(f"Connected to {args.host} ({module_count} modules)")
    except Exception as e:
        print(f"Cannot connect to {args.host}: {e}")
        sys.exit(1)

    # Find scenarios via the shared metadata module (recursive: scenarios live under core/, light/, …)
    if args.name:
        paths = []
        for name in (n.strip() for n in args.name.split(",") if n.strip()):
            match = test_meta.find_scenario_path(name)
            if not match:
                print(f"Scenario not found: {name}.json under {test_meta.SCENARIO_DIR}")
                sys.exit(1)
            paths.append(match)
    else:
        paths = [s["path"] for s in test_meta.collect_scenario_files()]

    if args.module and args.module.lower() != "all":
        module_paths = set(test_meta.paths_for_module(args.module))
        filtered = [p for p in paths if p in module_paths]
        if not filtered:
            print(f"No scenarios match module: {args.module}")
            sys.exit(1)
        paths = filtered
        print(f"Module filter: {args.module} ({len(paths)} scenario(s))")

    if not paths:
        print("No scenarios found")
        sys.exit(1)

    # Run scenarios
    all_results = {}
    all_passed = True
    for path in paths:
        if not path.exists():
            print(f"Scenario not found: {path}")
            continue
        result = run_scenario(client, path, args.settle,
                              update_contract=args.update_contract,
                              update_reason=args.reason, named=bool(args.name), network=args.network)
        all_results[result["name"]] = result
        if not result["passed"]:
            all_passed = False

    # Baseline
    if args.update_baseline:
        save_baseline(all_results)
        print(f"\nBaseline saved to {BASELINE_FILE}")

    if args.compare_baseline:
        baseline = load_baseline()
        for name, result in all_results.items():
            compare_baseline(result, baseline)

    print(f"\n=== {len(all_results)} scenario(s), "
          f"{'all passed' if all_passed else 'SOME FAILED'} ===")
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()

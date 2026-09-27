#!/usr/bin/env python3
"""Run scenario tests against a live device via HTTP.

Same JSON format as the in-process runner. Executes steps via REST API
and collects per-step performance measurements.
"""

import argparse
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
    rejects in a raw URL — encode so delete/replace/clear can address them."""
    return "/api/modules/" + urllib.parse.quote(name, safe="")

ROOT = Path(__file__).resolve().parent.parent.parent
SCENARIOS_DIR = ROOT / "test" / "scenarios"
BASELINE_FILE = ROOT / "test" / "scenario-baseline.json"

# Reuse the shared test-metadata parser so scenario discovery stays in one place.
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import _test_metadata as test_meta  # noqa: E402
sys.path.insert(0, str(ROOT / "moondeck" / "scenario"))
import _observed  # noqa: E402


class Client:
    # Mutating ops (add/delete/replace/control) trigger a full prepareTree on the
    # device — at 128x128 that frees/reallocates a large buffer + LUT and can
    # take several seconds on a busy ESP32. 5s was too tight (deletes timed out
    # mid-teardown, leaving a half-mutated tree). 15s clears the worst case while
    # still catching a genuinely hung device.
    TIMEOUT_S = 15

    def __init__(self, host: str):
        self.base = f"http://{host}"

    def _send(self, req):
        # A mutating call triggers prepareTree; while the device is mid-rebuild it
        # can drop the TCP connection (ConnectionResetError / "remote end closed")
        # or briefly refuse one. The device recovers in well under a second, so a
        # single transient drop shouldn't cascade-fail the run — retry once after
        # a short settle. A genuine HTTPError (4xx/5xx from the handler) is a real
        # result and is NOT retried; it propagates to the caller.
        for attempt in range(2):
            try:
                with urllib.request.urlopen(req, timeout=self.TIMEOUT_S) as resp:
                    return json.loads(resp.read())
            except urllib.error.HTTPError:
                raise  # a real handler response — let the caller decide
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
        """POST a raw text body. /api/file takes the file's CONTENTS, not JSON — the body IS
        the payload, which is why it cannot go through post() like every control write."""
        return self._send(urllib.request.Request(
            f"{self.base}{path}", data=text.encode("utf-8"),
            headers={"Content-Type": "text/plain"}))

    def delete(self, path: str):
        return self._send(urllib.request.Request(f"{self.base}{path}", method="DELETE"))


def _today_iso() -> str:
    """ISO date stamp for set_by fields. Local timezone is fine — set_by is a
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
    """Return the value of a named control on a module, or None."""
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


def _detect_target(state: dict) -> str:
    """Identify the build target so per-step contract values can be looked up.

    ESP32: read FirmwareUpdateModule.firmware (`esp32`, `esp32-eth`, `esp32-eth-wifi`,
    `esp32s3-n16r8`, …) — set at compile time from MM_FIRMWARE_NAME and
    exposed through the `firmware` control. Desktop: same key but reports
    `unknown`, so we substitute desktop-<host-os> using the runtime os name (still
    distinguishes macOS vs Linux vs Windows builds, which can differ in tick
    noticeably). See docs/explanation/architecture/index.md § Firmware vs board.
    """
    import platform
    firmware = None
    for m in state.get("modules", []):
        if m.get("type") != "FirmwareUpdateModule":
            continue
        for c in m.get("controls", []):
            if c.get("name") == "firmware":
                firmware = c.get("value")
                break
        break
    if firmware and firmware != "unknown":
        return firmware
    # Desktop fallback
    osmap = {"Darwin": "desktop-macos", "Linux": "desktop-linux", "Windows": "desktop-windows"}
    return osmap.get(platform.system(), "desktop-unknown")


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


def _reboot_and_wait(client, target: str, timeout_s: float = 60.0) -> str:
    """Restart the device and wait for it to answer again, so a scenario can prove what survives.

    On a board this is the reboot the endpoint performs. On a desktop the same endpoint EXITS the
    process and nothing restarts it, so the runner relaunches the binary itself, with the data
    directory the exiting instance was using: a restart that came back on different files would
    prove nothing about persistence. Returns "" on success, or the reason it did not come back.
    """
    data_dir = os.environ.get("MM_DATA_DIR")
    is_desktop = target.startswith("desktop-")

    # Uptime before the restart, so the recovered instance can be told from the one still running.
    # Without it the first answering /api/state is accepted, which on a board is routinely the OLD
    # instance replying before it goes down: every persistence assertion after that proves nothing.
    before = _uptime_seconds(client)

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
        if before is not None and now is not None and now < before:
            return ""                 # the clock restarted, which only a reboot does
        time.sleep(1.0)
    if not went_away:
        return (f"still answering after {timeout_s:.0f}s with no uptime reset: "
                "the reboot did not take effect")
    return f"did not answer within {timeout_s:.0f}s"


def _sum_dynamic_bytes(state: dict) -> int:
    """Sum dynamicBytes across the live module tree. Returned by /api/state per
    module; the sum is the model's prediction for how much heap the tree owns.

    NOT the same as (boot_heap - free_heap): the framework (lwIP, WiFi stack,
    FreeRTOS, HTTP server kernel buffers) consumes heap outside the model.
    Printed alongside the contract for sanity-checking — a regression here
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


# Containers whose USER-ADDED children a scenario may clear/rebuild — the tree the
# snapshot/restore protects. A scenario that clear_children's one of these destroys
# the board's real config; restoring the snapshot afterward leaves the board as found.
_SNAPSHOT_CONTAINERS = {"Layouts", "Effects", "Drivers", "Services", "Layer"}


def _snapshot_tree(state: dict) -> list:
    """Capture the user-added modules a scenario might clear or replace, in tree order
    (parents before children), so they can be re-created after the scenario runs.

    Each entry is {type, id, parent_id, controls} — everything /api/state exposes to
    reconstruct a module via POST /api/modules + /api/control. Boot-wired singletons
    (the containers themselves, Preview, LightPresets) are NOT captured: the device
    re-creates them itself and re-adding is a no-op or an error. We snapshot the
    CHILDREN of the snapshot containers (and their descendants) — the modules a
    scenario's clear_children / replace actually removes."""
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
            if inside_container and name and typ and name not in ("Preview", "LightPresets"):
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
    fails is reported (which module/control + the error), not silently swallowed — a
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
            print(f"  WARN — restore: could not re-create {entry['id']} "
                  f"({entry['type']} under {entry['parent_id']}): {e}")
            continue
        for cname, val in entry["controls"].items():
            try:
                client.post("/api/control", {"module": entry["id"],
                                             "control": cname, "value": val})
            except Exception as e:
                print(f"  WARN — restore: could not set {entry['id']}.{cname}={val!r}: {e}")
        restored += 1
    if restored:
        print(f"  restored {restored} module(s) the scenario had cleared")


def run_scenario(client: Client, scenario_path: Path, settle_s: float = 1.5,
                 update_contract: bool = False,
                 update_reason: str | None = None) -> dict:
    """Run a scenario against a live device and return results.

    Mode handling (see docs/reference/testing.md § Scenario modes):
      construct  — scenario builds the pipeline from scratch. Live device's
                   main.cpp owns the top-level shape, so construct scenarios
                   only run in-process. Skip here with a clear note.
      mutate     — scenario assumes a wired pipeline. Skip the fixture array
                   (the device IS the fixture) and run only the steps. Steps
                   that touch ids not present on the device hard-fail (instead
                   of the old WARN-and-continue which silently produced
                   meaningless passes).
    """
    with open(scenario_path) as f:
        scenario = json.load(f)

    name = scenario.get("name", scenario_path.stem)
    mode = scenario.get("mode", "construct")  # back-compat default
    print(f"\n=== Scenario: {name} ===")
    print(scenario.get("description", ""))

    results = {"name": name, "steps": [], "passed": True, "skipped": False}
    created_modules = []  # mutate scenarios rarely add modules but the existing cleanup path is still useful
    wrote_observations = [False]  # sentinel; flipped by each measure step that runs
    # Keyed by (step, target): the original contract block before --update-contract
    # mutated it (or None if no prior block existed). Used by the post-run gate to
    # roll mutations back when the run fails, so a renegotiated promise only
    # lands on a clean run.
    pending_contract_originals: dict = {}

    if mode == "construct":
        print("\n  SKIP (mode=construct — runs in-process only; the live device's "
              "main.cpp owns the top-level shape)")
        results["skipped"] = True
        return results
    if mode != "mutate":
        print(f"\n  FAIL — unknown mode: {mode!r} (expected construct or mutate)")
        results["passed"] = False
        return results

    # Pre-flight: every id touched by a step must be reachable — either already
    # on the device, OR added by an earlier add_module in this scenario. A
    # canvas-preparing scenario clears the containers and builds its own tree, so
    # its set_control/replace ids won't exist on the device yet; they're created
    # mid-run. A still-unreachable id is a real typo / wrong-wiring bug.
    target = "unknown"
    live_state = None   # bound before the try so the snapshot below can't NameError if /api/state fails
    try:
        live_state = client.get("/api/state")
        target = _detect_target(live_state)
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
                # clear, if a grid exists) — the executor skips them on a missing
                # target, so they don't count as a wiring bug in the pre-flight.
                if sid not in reachable and not step.get("optional"):
                    missing.append(sid)
        # Reset-block ids must exist before steps run (no add can precede them).
        for r in scenario.get("reset", []):
            if r.get("op") == "set_control" and r.get("id") and r["id"] not in reachable:
                missing.append(r["id"])
        missing = sorted(set(missing))
        if missing:
            print(f"\n  FAIL — scenario references ids that are neither on the live "
                  f"device nor added by an earlier step: {', '.join(missing)}. "
                  f"Fix the wiring or add the module first.")
            results["passed"] = False
            return results
    except Exception as e:
        print(f"\n  WARN — couldn't pre-flight live module names: {e}")
    print(f"  Target: {target}")
    results["target"] = target

    # Snapshot the board's user-added tree so we can restore it after the scenario:
    # a scenario that clear_children's a container (to get a known canvas) destroys the
    # board's real config, and the created_modules cleanup only removes what the scenario
    # ADDED, not what it CLEARED. Restoring the snapshot leaves the bench board as found.
    tree_snapshot = []
    if live_state is not None:   # skip restore if the pre-flight /api/state fetch failed (nothing to snapshot)
        try:
            tree_snapshot = _snapshot_tree(live_state)
        except Exception as e:
            print(f"  WARN — couldn't snapshot tree for restore: {e}")

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
                # Stop the scenario before collect_metrics — baseline would
                # otherwise reflect an unknown/partial state. No cleanup
                # needed: created_modules only fills inside the steps loop
                # below, which hasn't run yet.
                return results

    # Collect baseline AFTER reset so it reflects the normalized state.
    baseline = collect_metrics(client, settle_s=settle_s)
    print(f"\n  Baseline: tick={baseline.get('tickTimeUs', '?')}us (FPS={baseline.get('fps', '?')})  heap={baseline.get('freeHeap', '?')}")

    # ids whose optional add_module was skipped (a platform-gated module absent on this
    # target — e.g. the Parlio driver on a non-P4 board). A later optional measure/remove
    # that names a skipped id is itself skipped, so an absent driver leaves no trace rather
    # than failing the run. (perf_full's add/measure/remove driver triples are all optional.)
    skipped_ids = set()

    # Live runs `steps` only — `fixture` is the in-process equivalent of what
    # main.cpp already wired on the device.
    for step_index, step in enumerate(scenario.get("steps", [])):
        step_name = step.get("name", "?")
        op = step.get("op", "")
        step_result = {"name": step_name, "op": op}

        # An optional measure/control on a module whose optional add was skipped is a
        # no-op — the module isn't there to measure. Skip before any REST call.
        if step.get("optional") and step.get("id") in skipped_ids and op in ("measure", "set_control"):
            step_result["status"] = "ok"
            print(f"  {op:5} {step.get('id','?')} — skipped (optional, module not present on {target})")
            results["steps"].append(step_result)
            continue

        try:
            if op == "add_module":
                data = {"type": step["type"], "id": step.get("id", ""),
                        "parent_id": step.get("parent_id", "")}
                # An `optional` add of a type this target doesn't have is a SKIP, not a
                # fail — perf_full adds every LED driver (RMT/LCD/Parlio), but each is
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
                        # The step's declared PROPS, applied whether the module was just created or
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
                        print(f"  +     {step.get('id','?')} ({step['type']}) — skipped (optional, type unavailable on {target})")
                    else:
                        step_result["status"] = "error"
                except urllib.error.HTTPError:
                    if step.get("optional"):
                        step_result["status"] = "ok"
                        skipped_ids.add(step.get("id", ""))
                        print(f"  +     {step.get('id','?')} ({step['type']}) — skipped (optional, type unavailable on {target})")
                    else:
                        raise

            elif op == "write_file":
                # Stage a script file, the same op the desktop runner has. Without it a migrated
                # scenario ran its set_control against a file that was never written, and every
                # script step failed on hardware while passing on the desktop.
                # A failed write FAILS THE SCENARIO. Every later step runs against a stale or
                # absent file, so reporting PASSED afterwards is the silent pass this op exists
                # to prevent — the same rule the desktop runner applies.
                path_ = step.get("path")
                body = step.get("value", "")
                if not path_:
                    print(f"  WRITE {step_name} — missing path")
                    step_result["status"] = "skipped" if step.get("optional") else "error"
                    if not step.get("optional"):
                        step_result["error"] = "write_file step has no `path`"
                        results["passed"] = False
                else:
                    try:
                        resp = client.post_text(f"/api/file?path={urllib.parse.quote(path_)}", body)
                        # The device answers {"ok":true}; anything else is a failure, including a
                        # body that is not the JSON object this endpoint documents.
                        ok = isinstance(resp, dict) and resp.get("ok") is True
                        step_result["status"] = "ok" if ok else "error"
                        if ok:
                            print(f"  WRITE {path_} ({len(body)} bytes)")
                        else:
                            step_result["error"] = f"unexpected response: {resp!r}"
                            print(f"  WRITE {path_} — FAILED: unexpected response {resp!r}")
                            results["passed"] = False
                    except Exception as we:
                        step_result["status"] = "error"
                        step_result["error"] = str(we)
                        print(f"  WRITE {path_} — FAILED: {we}")
                        results["passed"] = False

            elif op == "reboot":
                # Restart and wait, so a later expect_control proves what SURVIVED rather than what
                # is merely still in memory. The one op that can tell a written setting from a kept one.
                why = _reboot_and_wait(client, target, float(step.get("timeout", 60)))
                if why:
                    step_result["status"] = "error"
                    print(f"  REBOOT — {why}")
                    results["passed"] = False
                else:
                    step_result["status"] = "ok"
                    print("  REBOOT — back up")

            elif op == "expect_file":
                # Read a file back, which is the only way to prove a write reached the filesystem
                # rather than a cache: the card that owns the filesystem is proven by its contents.
                path_ = step["path"]
                # `contains` matches a substring, `equals` the whole file: the two tiers agree, and
                # neither accepts a step that names neither, which would pass on any file at all.
                exact = "equals" in step
                if not exact and "contains" not in step:
                    step_result["status"] = "error"
                    print(f"  EXPECT {path_} — needs `contains` or `equals`")
                    results["passed"] = False
                    continue
                want = str(step["equals"] if exact else step["contains"])
                try:
                    raw = client.get_text(f"/api/file?path={urllib.parse.quote(path_, safe='/')}")
                except Exception as fe:
                    step_result["status"] = "error"
                    print(f"  EXPECT {path_} — could not read ({fe})")
                    results["passed"] = False
                else:
                    holds = (raw == want) if exact else (bool(want) and want in raw)
                    step_result["status"] = "ok" if holds else "error"
                    verb = "is" if exact else "holds"
                    print(f"  EXPECT {path_} {verb if holds else 'does not ' + verb} {want!r}")
                    if not holds:
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
                    print(f"  EXPECT {mod_id}.{key} — needs `equals` or `not_equals`")
                    results["passed"] = False
                    continue
                want = _as_written(step["not_equals" if negated else "equals"])
                try:
                    mod = client.get(_mod_path(mod_id))
                    got = next((c.get("value") for c in (mod.get("controls") or [])
                                if c.get("name") == key), None)
                except Exception as ce:
                    step_result["status"] = "error"
                    print(f"  EXPECT {mod_id}.{key} — could not read ({ce})")
                    results["passed"] = False
                    continue
                if got is None:
                    step_result["status"] = "error"
                    print(f"  EXPECT {mod_id}.{key} — no such control")
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
                try:
                    resp = client.post("/api/control", data)
                    step_result["status"] = "ok" if resp.get("ok") else "error"
                    print(f"  SET   {step.get('id', '?')}.{step.get('key', '?')} = {step.get('value', '?')}")
                except urllib.error.HTTPError as ce:
                    if step.get("optional") and ce.code == 404:
                        # An `optional` set_control on a missing module (e.g. shrink
                        # a grid a prior run's cleanup removed) is a no-op, not a fail.
                        step_result["status"] = "ok"
                        print(f"  SET   {step.get('id','?')}.{step.get('key','?')} — skipped (optional, not present)")
                    elif step.get("optional") and ce.code == 400:
                        # An `optional` set_control the device REJECTS with 400 (e.g.
                        # "value out of range") is a not-available-here skip, not a
                        # failure — e.g. selecting a `peripheral` value this chip doesn't
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
                # for us), still give the device a moment — a set_control that
                # triggers prepareTree briefly mutates the module tree, and the
                # very next API call can hit a transient "module not found".
                # 500 ms is empirically enough on the classic board; cheap insurance.
                if not (step.get("measure") or op == "measure"):
                    time.sleep(0.5)

            elif op in ("delete_module", "remove_module"):
                # Both names mean the same thing — accept either so a scenario
                # reads identically on the in-process runner (which uses
                # `remove_module`) and here. The two runners must never diverge
                # on op names, or a scenario silently no-ops on one tier.
                # An `optional` remove of a module that was never added (its
                # optional add was skipped — a platform-gated driver absent on this
                # target) is a SKIP, not a fail: the device returns 404 "module not
                # found" or ok:false. Pairs with the optional add above.
                try:
                    resp = client.delete(_mod_path(step["id"]))
                    if resp.get("ok") or not step.get("optional"):
                        step_result["status"] = "ok" if resp.get("ok") else "error"
                        print(f"  -     {step.get('id', '?')}")
                    else:
                        step_result["status"] = "ok"
                        print(f"  -     {step.get('id','?')} — skipped (optional, not present)")
                except urllib.error.HTTPError:
                    if step.get("optional"):
                        step_result["status"] = "ok"
                        print(f"  -     {step.get('id','?')} — skipped (optional, not present)")
                    else:
                        raise

            elif op == "clear_children":
                # Delete every child of a container, leaving the container.
                # The "prepare my own canvas" primitive — a scenario assumes
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
                        # 400 "module not deletable" — that's expected, skip them.
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
                # Swap a child for a fresh module of another type at the same
                # slot, keeping its name — mirrors the in-process op and the
                # device's POST /api/modules/<name>/replace endpoint.
                resp = client.post(_mod_path(step["id"]) + "/replace",
                                   {"type": step["type"]})
                step_result["status"] = "ok" if resp.get("ok") else "error"
                print(f"  ~     {step.get('id', '?')} → {step.get('type', '?')}")
                time.sleep(0.5)

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
            # missing test step — meaningless passes. Mutate scenarios shouldn't
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
            # collect_metrics hits /api/state — a missing measurement is a
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
            # Contracts are hand-set promises — see docs/reference/testing.md § Performance
            # contracts. `--update-contract --reason "..."` rewrites them.
            contract_block = step.get("contract", {}).get(target) if step.get("contract") else None
            if contract_block:
                # Defaults reflect run-to-run variance, not "I don't care":
                #   pc-*       — multi-process OS jitter, 20% pct + 200us absolute
                #                floor. The floor dominates below ~1ms tick (the
                #                realistic case for desktop scenarios today).
                #   esp32-*    — bounded RTOS but lwIP/EMAC jitter, 10% pct + 5us
                #                absolute floor.
                # KEEP IN SYNC: the in-process runner re-declares the same defaults
                # at test/scenario_runner.cpp contract-block handler — tuning one
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
                    # tick contract is a *ceiling* — faster than contract is good
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
                    # Contract is a *floor* — the device must deliver at least this
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
                # max_alloc_block contract is also a *floor* — opt-in per scenario.
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
                    # reports zero contiguous heap — a real failure, not a
                    # missing field. (Contrast with free_heap on desktop where 0
                    # is the "unlimited" sentinel — that's a desktop-only
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
            # last-reading baseline — set_by + reason are stamped so the diff
            # records when and why the promise changed. Caller is responsible for
            # committing the diff intentionally.
            #
            # Originals are stashed in pending_contract_originals so the
            # post-run gate (see below) can roll the in-memory tree back to
            # disk shape if the run failed — only successful runs get to
            # commit a renegotiated promise.
            if update_contract:
                # Preserve any per-step tolerance overrides already in place.
                # Key by step INDEX, not the step dict — a dict is unhashable, so
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
                # path — keep both files in sync if you change one.
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
                # FPS×lights throughput floor — compared against the measured
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

    # Cleanup: delete modules that were created by this scenario
    for module_id in reversed(created_modules):
        try:
            client.delete(_mod_path(module_id))
            print(f"  -     {module_id} (cleanup)")
        except Exception:
            pass

    # Restore: re-create any pre-existing module the scenario removed (a clear_children
    # of a real container, or a replace). Combined with the created-module cleanup above,
    # the board ends the run in the tree it started with — no residue, no lost config.
    if tree_snapshot:
        try:
            after_state = client.get("/api/state")
            _restore_tree(client, tree_snapshot, after_state)
        except Exception as e:
            print(f"  WARN — couldn't restore snapshot: {e}")

    # Write the scenario JSON back if anything changed:
    #   - observed.<target> was updated by any measure step (every run); OR
    #   - --update-contract renegotiated the contract — AND the run passed
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
        with open(BASELINE_FILE) as f:
            return json.load(f)
    return {}


def save_baseline(data: dict):
    with open(BASELINE_FILE, "w") as f:
        json.dump(data, f, indent=2)


def compare_baseline(results: dict, baseline: dict):
    """Compare results against baseline, report regressions."""
    name = results["name"]
    if name not in baseline:
        print(f"  No baseline for '{name}' — run with --update-baseline first")
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
                        help="Scenario name (without .json). Runs all if omitted.")
    parser.add_argument("--module", default=None,
                        help="Filter to scenarios whose top-level module / also matches.")
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
        match = test_meta.find_scenario_path(args.name)
        if not match:
            print(f"Scenario not found: {args.name}.json under {test_meta.SCENARIO_DIR}")
            sys.exit(1)
        paths = [match]
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
                              update_reason=args.reason)
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

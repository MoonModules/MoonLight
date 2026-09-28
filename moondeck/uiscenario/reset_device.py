#!/usr/bin/env python3
"""Put a device back in the state a fresh boot leaves it in, over REST.

A clip must not open on the previous take's ending. Doing that reset INSIDE a run file costs screen
time the clip is not about and shows the tidying rather than the subject, so it happens here, before
the camera rolls, and every run file starts on the same picture.

The target is what `src/main.cpp` wires at boot: Layouts holds one GridLayout at its default size,
Effects holds one Layer running PulseEffect, and Drivers holds the two modules the boot path adds
(LightPresetsModule and PreviewDriver) and nothing else.

Usage: uv run moondeck/uiscenario/reset_device.py [--host localhost:8080]
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# What boot leaves under each container, by module type. A child of one of these types is kept where
# boot would have made it; anything else a take added is removed.
BOOT_CHILDREN = {
    "Layouts": ["GridLayout"],
    # Effects holds ONE Layer at boot. Without this a clip that adds a second layer leaves it
    # behind, and the next take opens with one more than the one before: five of them after four
    # takes, every one compositing into the shot.
    "Effects": ["Layer"],
    "Layer": ["PulseEffect"],
    "Drivers": ["LightPresetsModule", "PreviewDriver"],
    # Services holds ONE AudioService at boot, which main.cpp wires. Everything else under it is a
    # take's leftover: the services clip opened on an Analog and a MoonLive service it had added
    # itself on an earlier run, so its first shot showed the state it was about to demonstrate.
    "Services": ["AudioService"],
}

# The grid a fresh GridLayout self-initializes to, `defaultGridSize` in src/light/layouts/GridLayout.h.
DEFAULT_GRID = 16

# Controls whose value is part of the boot state, because a take can leave one set and the next one
# then opens on it. Consent is the case that found this: nothing leaves the device until someone
# says so, so it is off at boot, and a take that turned it on made the scenario suite fail on the
# NEXT recording with a state no run file had asked for.
BOOT_CONTROLS = {
    "Stats": {"consent": False},
    "Talk":  {"consent": False},
}


# A device serves from a small embedded stack, and it closes a kept-alive socket whenever it needs
# the memory. That arrives here as a dropped connection rather than a status, and it says nothing
# about whether the device is healthy: the retry below costs a second and turns a failed recording
# session into a completed one.
_TRANSIENT = (urllib.error.URLError, ConnectionError, OSError, http.client.HTTPException)


def _attempt(what: str, send, tries: int = 4):
    """Run one request, retrying a dropped connection but never a refusal.

    An HTTPError is the device ANSWERING with a no, so it is returned rather than retried; only a
    connection that died before an answer is worth sending again.
    """
    for n in range(1, tries + 1):
        try:
            return send()
        except urllib.error.HTTPError:
            raise
        except _TRANSIENT as e:
            if n == tries:
                print(f"  ! {what}: {type(e).__name__} after {tries} attempts")
                return None
            time.sleep(0.5 * n)
    return None


def _get(host: str, path: str):
    def send():
        with urllib.request.urlopen(f"http://{host}{path}", timeout=10) as r:
            return json.loads(r.read().decode())
    out = _attempt(f"GET {path}", send)
    if out is None:
        raise RuntimeError(f"GET {path} never answered")
    return out


def _post(host: str, path: str, payload: dict) -> bool:
    body = json.dumps(payload).encode()

    def send():
        req = urllib.request.Request(f"http://{host}{path}", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return 200 <= r.status < 300
    try:
        return bool(_attempt(f"POST {path}", send))
    except urllib.error.HTTPError as e:
        print(f"  ! POST {path} -> HTTP {e.code}")
        return False


def _delete(host: str, name: str) -> bool:
    """A module is deleted by PATH, which is the contract handleDeleteModule reads."""
    def send():
        req = urllib.request.Request(f"http://{host}/api/modules/{urllib.parse.quote(name)}",
                                     method="DELETE")
        with urllib.request.urlopen(req, timeout=10) as r:
            return 200 <= r.status < 300
    try:
        return bool(_attempt(f"DELETE {name}", send))
    except urllib.error.HTTPError as e:
        print(f"  ! DELETE {name} -> HTTP {e.code}")
        return False


def _walk(mods, parent=None):
    """Every module as (module, parent-module), depth first."""
    for m in mods:
        yield m, parent
        yield from _walk(m.get("children") or [], m)


def apply_setup(host: str, setup: list[dict]) -> int:
    """Set the controls a clip needs before it is recorded.

    The boot state is what every clip shares; this is what one clip needs on top of it. Doing it
    over REST rather than on camera keeps the clip about its subject: the MoonLive clip spent its
    first three shots typing a grid size into two boxes.
    """
    applied = 0
    for item in setup:
        if _post(host, "/api/control", {"module": item["module"],
                                        "control": item["control"],
                                        "value": item["value"]}):
            applied += 1
            print(f"  = {item['module']}.{item['control']} -> {item['value']}")
        else:
            # SAID rather than counted: a clip whose premise never applied still records, and
            # the grid it was supposed to demonstrate is silently the default one.
            print(f"  ! {item['module']}.{item['control']} refused")
    if applied:
        time.sleep(0.6)       # let the tree settle before the run starts driving it
    return applied


def reset(host: str) -> int:
    """Zero when the device reached its boot state, non-zero when any write was refused.

    A refused write is REPORTED rather than absorbed: the caller records a clip against this
    state, and a half-reset device looks exactly like a correct one until the take is watched.
    """
    failed = 0
    state = _get(host, "/api/state")
    mods = state.get("modules", [])

    # DELETE FIRST, top down: a container's extra children go before anything is added back, so a
    # second Grid from a previous take cannot survive beside the one this script ensures.
    removed = 0
    for container, keep_types in BOOT_CHILDREN.items():
        node = next((m for m, _ in _walk(mods) if m.get("name") == container
                     or m.get("type") == container), None)
        if not node:
            continue
        seen: list[str] = []
        for child in list(node.get("children") or []):
            t, n = child.get("type"), child.get("name")
            # One of each boot type is kept; a duplicate of it is still a leftover.
            if t in keep_types and t not in seen:
                seen.append(t)
                continue
            if _delete(host, n):
                removed += 1
                print(f"  - {container}/{n} ({t})")
            else:
                failed += 1

    # ADD BACK what boot wires and the take removed.
    added = 0
    state = _get(host, "/api/state")
    mods = state.get("modules", [])
    for container, keep_types in BOOT_CHILDREN.items():
        node = next((m for m, _ in _walk(mods) if m.get("name") == container
                     or m.get("type") == container), None)
        if not node:
            continue
        have = {c.get("type") for c in (node.get("children") or [])}
        for t in keep_types:
            if t in have:
                continue
            if _post(host, "/api/modules", {"type": t, "parent_id": node.get("name")}):
                added += 1
                print(f"  + {container}/{t}")
            else:
                failed += 1

    # The grid's size is a control rather than a module, so it is set rather than recreated.
    state = _get(host, "/api/state")
    grid = next((m for m, _ in _walk(state.get("modules", []))
                 if m.get("type") == "GridLayout"), None)
    if grid:
        for key in ("width", "height"):
            cur = next((c.get("value") for c in (grid.get("controls") or [])
                        if c.get("name") == key), None)
            if cur != DEFAULT_GRID:
                if _post(host, "/api/control",
                         {"module": grid.get("name"), "control": key, "value": DEFAULT_GRID}):
                    print(f"  = Grid.{key} -> {DEFAULT_GRID}")
                else:
                    failed += 1

    # Controls that boot in a known state, set the same way the grid is rather than recreated.
    state = _get(host, "/api/state")
    for name, wanted in BOOT_CONTROLS.items():
        node = next((m for m, _ in _walk(state.get("modules", [])) if m.get("name") == name), None)
        if not node:
            continue
        for key, value in wanted.items():
            cur = next((c.get("value") for c in (node.get("controls") or [])
                        if c.get("name") == key), None)
            if cur is None or cur == value:
                continue
            if _post(host, "/api/control", {"module": name, "control": key, "value": value}):
                print(f"  = {name}.{key} -> {value}")
            else:
                failed += 1

    if removed or added:
        time.sleep(1.0)       # let the tree settle before a run starts driving it
    print(f"reset: {removed} removed, {added} added"
          + (f", {failed} FAILED" if failed else ""))
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Reset a device to its boot state.")
    ap.add_argument("--host", default="localhost:8080", help="the device to reset")
    args = ap.parse_args()
    try:
        return reset(args.host)
    except Exception as e:
        print(f"reset failed: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

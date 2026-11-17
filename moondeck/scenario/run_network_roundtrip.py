#!/usr/bin/env python3
"""Measure host→device→host round-trip latency per protocol on the checked boards.

The simplest honest latency probe: the host sends one solid-color frame to a
device, then times how long until that color shows up in the device's preview
WebSocket stream. The path is host → device's NetworkReceiveEffect (writes the
layer buffer) → PreviewDriver (broadcasts the buffer over /ws) → host. It sweeps
all three protocols — ArtNet, E1.31, DDP — per device (the receiver autodetects
each on its own port, so no reconfig between them), and prints a per-device
median-per-protocol comparison. Runs against every device CHECKED in MoonDeck's
Live tab (the `selected` flag); `--host` overrides to a single explicit device.
Works with as few as one board reachable.

Deliberately minimal. The measured time includes the PreviewDriver's own
rate-limit (default 24 fps ≈ up to 42 ms of quantisation), so treat the number
as "state-change visible within" latency, not wire latency — raise the device's
Preview fps to tighten it. Across repeats the spread (min/median/max) is the
useful signal for the hiccups/latency symptom.

Extend later (hooks left intentionally simple): per-frame sequence matching,
the device→device forwarding chain, jitter/drop histograms. This version sweeps
the three protocols with one probe color, N repeats each.

Exit codes follow the other live scripts: 0 = measured ok, 1 = no frame came
back (a real failure), 2 = environment problem (no device, moondeck.json
missing).

Run:  uv run moondeck/scenario/run_network_roundtrip.py [--host IP] [--repeats N]
"""

import argparse
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_live_scenario import Client  # shared HTTP wrapper  # noqa: E402
from _net_probe import (  # shared lights-over-UDP surface  # noqa: E402
    ARTNET_PORT, E131_PORT, DDP_PORT, CHANNELS_PER_UNIVERSE,
    build_artdmx, build_e131, build_ddp, load_selected_devices,
)
import socket  # noqa: E402
import _preview_ws  # noqa: E402

# A color the idle effects are unlikely to paint by themselves, so a preview
# match means OUR frame arrived, not a coincidence. Distinct on all 3 channels.
PROBE_RGB = (0x11, 0x22, 0x33)

# The three industry protocols, each on its own port — the receiver autodetects
# on all of them at once. Order is the comparison order in the output.
PROTOCOLS = ("ArtNet", "E1.31", "DDP")


def seed_once(ip: str, rgb, universes: int, seq: int, protocol: str = "ArtNet"):
    """Send one full frame of `rgb` across the universes via `protocol`
    (fire-and-forget). Builders + ports mirror run_network_live's send_solid."""
    payload = bytes(rgb) * (CHANNELS_PER_UNIVERSE // 3)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for u in range(universes):
            if protocol == "DDP":
                last = u == universes - 1   # push flag on the final chunk
                sock.sendto(build_ddp(u * CHANNELS_PER_UNIVERSE, last, payload),
                            (ip, DDP_PORT))
            elif protocol == "E1.31":
                sock.sendto(build_e131(u, seq, payload), (ip, E131_PORT))
            else:
                sock.sendto(build_artdmx(u, seq, payload), (ip, ARTNET_PORT))
    finally:
        sock.close()


def measure_roundtrip(host: str, repeats: int, timeout_s: float, protocol: str):
    """Send the probe color `repeats` times over `protocol`; time
    send→preview-arrival each time. Returns the list of latencies in
    milliseconds (one per successful repeat)."""
    ip = host.partition(":")[0]
    # 16×16 grid → 256 lights → 2 universes, the same shape the matrix test uses.
    universes = 2
    latencies = []
    for i in range(repeats):
        # Send the probe, then immediately start waiting for it in the preview.
        # The preview holds the last frame, so arriving slightly after the send
        # is fine — we time from just before the send.
        t0 = time.monotonic()
        seed_once(ip, PROBE_RGB, universes, seq=i & 0xFF, protocol=protocol)
        # 60% match = "our frame is clearly here". A latency probe only needs to
        # confirm arrival, not full coverage: the 2-universe probe payload fills
        # most of the grid but not always 100% (the universe→buffer mapping and
        # grid size leave a deterministic tail of pixels the probe doesn't cover
        # — ~75% on a 16×16 grid here; full-coverage correctness is the matrix
        # test's job, not this one's). The preview WebSocket can also drop a
        # connection transiently (device busy mid-tick) — that's one bad probe,
        # not a reason to abort the whole device, so catch it and record a miss.
        try:
            ok, best_pct, points, detail = _preview_ws.wait_for_solid(
                host, PROBE_RGB, tolerance=0, min_match_pct=60.0, timeout_s=timeout_s)
        except (ConnectionError, OSError) as e:
            ok, best_pct, points, detail = False, 0.0, 0, f"WS error: {e}"
        dt_ms = (time.monotonic() - t0) * 1000.0
        if ok:
            latencies.append(dt_ms)
            print(f"    repeat {i + 1}/{repeats}: {dt_ms:6.1f} ms "
                  f"({points} pts, {best_pct:.0f}% match)", flush=True)
        else:
            print(f"    repeat {i + 1}/{repeats}: NO FRAME (best {best_pct:.0f}% "
                  f"of {points} pts; {detail})", flush=True)
        # Clear the probe color between repeats so the next match is a fresh
        # arrival, not the held last frame. Send black on the same protocol;
        # don't time it.
        seed_once(ip, (0, 0, 0), universes, seq=(i + 128) & 0xFF, protocol=protocol)
        time.sleep(0.3)
    return latencies


def _grid_value(client: Client, axis: str):
    """Read the device's current Grid.<axis>, or None if it can't be read — so
    the caller restores only what it actually captured (restoring a guessed
    default could change a grid we never measured)."""
    try:
        for m in client.get("/api/state").get("modules", []):
            for stack in ([m], m.get("children", []) or []):
                for mod in stack:
                    if mod.get("name") == "Grid":
                        for c in mod.get("controls", []):
                            if c.get("name") == axis and c.get("value") is not None:
                                return int(c["value"])
    except Exception:
        pass
    return None


def run_one(host: str, repeats: int, timeout_s: float) -> bool:
    """Measure one device across all three protocols; restore its grid
    afterwards. Returns True if any protocol returned a measurement."""
    client = Client(host)
    added = False
    # Capture the grid BEFORE any mutation, so the finally block can restore it
    # whether or not the receiver add succeeds (the grid POSTs run first, so the
    # grid can be left changed even if the add fails). None = couldn't read it →
    # don't restore that axis (better than forcing a guessed value).
    orig_w = _grid_value(client, "width")
    orig_h = _grid_value(client, "height")
    try:
        # 16×16 → 256 lights → 2 universes, the shape the matrix test uses.
        client.post("/api/control", {"module": "Grid", "control": "width", "value": 16})
        client.post("/api/control", {"module": "Grid", "control": "height", "value": 16})
        client.post("/api/modules", {"type": "NetworkReceiveEffect",
                                     "id": "NetworkReceive", "parent_id": "Layer"})
        added = True
        time.sleep(2.0)  # prepareTree settle

        # Sweep all three protocols so they can be compared head to head — the
        # receiver autodetects each on its own port, so no device reconfig
        # between protocols. Collect each protocol's median for the summary.
        any_ok = False
        summary = []
        for proto in PROTOCOLS:
            print(f"  {proto}: host→device→host round-trip ({repeats} probes)…", flush=True)
            latencies = measure_roundtrip(host, repeats, timeout_s, proto)
            if not latencies:
                print(f"    no {proto} frame came back", flush=True)
                summary.append((proto, None))
                continue
            any_ok = True
            latencies.sort()   # for the min/max below
            median = statistics.median(latencies)
            print(f"    {proto}: min {latencies[0]:.1f} / median {median:.1f} / "
                  f"max {latencies[-1]:.1f} ms over {len(latencies)}/{repeats}", flush=True)
            summary.append((proto, (latencies[0], median, latencies[-1], len(latencies))))
        # Per-device comparison line across the three protocols.
        parts = []
        for proto, stats in summary:
            parts.append(f"{proto} {stats[1]:.0f}ms" if stats else f"{proto} —")
        print(f"  comparison (median): {'  '.join(parts)} "
              f"(incl. Preview fps quantisation — raise Preview fps to tighten)", flush=True)
        if not any_ok:
            print("  FAIL  no protocol returned a frame — receive or preview path down")
        return any_ok
    finally:
        # Remove the temporary receiver only if it was added; restore the grid
        # regardless (the grid POSTs run before the add, so it can be changed
        # even when the add failed). Skip an axis we couldn't read (None).
        if added:
            try:
                client.delete("/api/modules/NetworkReceive")
            except Exception:
                pass
        for axis, val in (("width", orig_w), ("height", orig_h)):
            if val is None:
                continue
            try:
                client.post("/api/control",
                            {"module": "Grid", "control": axis, "value": val})
            except Exception:
                print(f"  WARN  could not restore Grid {axis} to {val}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="run only this device host[:port]; default = "
                                   "every device checked in MoonDeck's Live tab")
    ap.add_argument("--repeats", type=int, default=10, help="probes per protocol per device")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="seconds to wait for the probe color per repeat")
    args = ap.parse_args()

    if args.host:
        hosts = [(args.host, args.host)]
    else:
        devices = load_selected_devices()
        if not devices:
            print("FAIL  no selected+reachable devices — check device boxes in the Live tab")
            return 2
        hosts = [(d["ip"], d.get("deviceName", "?")) for d in devices]
        print(f"devices: {len(hosts)} selected — "
              + ", ".join(f"{n} ({h})" for h, n in hosts), flush=True)

    measured = 0
    for host, name in hosts:
        print(f"\n--- {name} ({host}) ---", flush=True)
        if run_one(host, args.repeats, args.timeout):
            measured += 1

    if measured == 0:
        print("\nFAIL  no device returned a measurement")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

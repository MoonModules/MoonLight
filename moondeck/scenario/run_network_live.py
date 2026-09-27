#!/usr/bin/env python3
"""Live lights-over-UDP matrix test across every online board in moondeck.json.

Proves the multi-protocol network path (ArtNet, E1.31/sACN, DDP) end-to-end on
real firmware. Devices come from moondeck/moondeck.json (the MoonDeck device
list, active network, online only). Each round one device is the SENDER and
every other device LISTENS:

  1. The desktop seeds the sender three times — once per protocol, each with its own
     color — to the sender's protocol ports (6454/5568/4048); the
     NetworkReceiveEffect (added to each device's Layer for the run) listens on
     all three at once. The sender's /ws preview stream must show each color —
     proves desktop → device receive per protocol.
  2. The sender's own NetworkSendDriver is pointed at each listener in turn,
     with its protocol control cycled round-robin so all three send paths get
     exercised across a matrix run; the listener's preview must show the
     sender's CORRECTED color (the send driver applies brightness + channel
     order) — proves device → device over real firmware send + receive.

With one online device only step 1 runs (the matrix needs ≥2 boards). All
mutated state (grid size, NetworkSend ip/protocol/enabled, the added effects)
is restored in a finally block. Exit codes follow improv_smoke_test.py: 0 =
all legs passed, 1 = a leg failed, 2 = environment problem (no devices,
moondeck.json missing).

Run:  uv run moondeck/scenario/run_network_live.py [--device NAME] [--host IP]
"""

import argparse
import json
import sys
import time
import socket
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_live_scenario import Client, _control_value  # shared HTTP wrapper  # noqa: E402
import _preview_ws  # noqa: E402
# Shared lights-over-UDP surface (ports, packet builders, device set) — see
# _net_probe.py; the matrix-only color-correction/Board logic stays below.
from _net_probe import (  # noqa: E402
    ARTNET_PORT, E131_PORT, DDP_PORT, CHANNELS_PER_UNIVERSE, PROTOCOLS,
    MOONDECK_STATE, build_artdmx, build_e131, build_ddp, load_selected_devices,
)

# Round colors: channel values far apart so they stay distinct after the
# sender's brightness scale (default 20/255 → e.g. (255,128,0) → (20,10,0)),
# and distinct per round so a stale frame from an earlier round can't pass.
ROUND_COLORS = [(255, 128, 0), (0, 255, 128), (128, 0, 255),
                 (255, 0, 128), (128, 255, 0), (0, 128, 255)]

# 3-channel presets only: an RGBW sender emits 4 bytes/light, which misaligns a
# 3-channel listener buffer, so those legs are skipped.
PRESET_ORDER = {"RGB": (0, 1, 2), "RBG": (0, 2, 1), "GRB": (1, 0, 2),
                "GBR": (1, 2, 0), "BRG": (2, 0, 1), "BGR": (2, 1, 0)}
PRESET_NAMES = ["RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW"]


def corrected(rgb, brightness, preset):
    """Mirror src/light/drivers/Correction.h so a listener's expected color matches the
    sender's corrected bytes. Keep in sync.

    Models the plain brightness scale only: `curve` Linear and white-balance trims at 255.
    A sender on another curve or with a trim set would have to be modeled here too.
    """
    scaled = [(v * int(brightness)) // 255 for v in rgb]
    order = PRESET_ORDER[preset]
    return tuple(scaled[order[i]] for i in range(3))


# build_artdmx / build_e131 / build_ddp now live in _net_probe.py (imported
# above) — shared with the latency probe.


def send_solid(host: str, rgb, protocol: str = "ArtNet", universes: int = 2,
               repeats: int = 10, pace_ms: int = 50):
    """Send `repeats` full frames of solid color to the device via the given
    protocol (the receiver autodetects on all three ports). Repeats absorb WiFi
    power-save first-packet loss; the receiver's hold-last-frame staging means
    one arrival suffices."""
    ip = host.partition(":")[0]
    payload = bytes(rgb) * (CHANNELS_PER_UNIVERSE // 3)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        seq = 0
        for _ in range(repeats):
            if protocol == "DDP":
                # One byte-addressed chunk per universe-sized slice keeps the
                # frame shape identical across protocols.
                for u in range(universes):
                    last = u == universes - 1
                    sock.sendto(build_ddp(u * CHANNELS_PER_UNIVERSE, last, payload),
                                (ip, DDP_PORT))
            elif protocol == "E1.31":
                for u in range(universes):
                    sock.sendto(build_e131(u, seq, payload), (ip, E131_PORT))
            else:
                for u in range(universes):
                    sock.sendto(build_artdmx(u, seq, payload), (ip, ARTNET_PORT))
            seq = (seq + 1) & 0xFF
            time.sleep(pace_ms / 1000.0)
    finally:
        sock.close()


# load_selected_devices now lives in _net_probe.py (imported above) — shared
# with the latency probe.


def find_module(state: dict, name: str):
    def walk(mod):
        if mod.get("name") == name:
            return mod
        for child in mod.get("children", []):
            found = walk(child)
            if found:
                return found
        return None
    for mod in state.get("modules", []):
        found = walk(mod)
        if found:
            return found
    return None


# --- per-device setup / restore ----------------------------------------------

class Board:
    """One device under test: HTTP client + the original state we mutate."""

    def __init__(self, dev: dict):
        self.name = dev.get("deviceName") or dev["ip"]
        self.host = dev["ip"]
        self.client = Client(self.host)
        state = self.client.get("/api/state")
        grid = find_module(state, "Grid") or {}
        drivers = find_module(state, "Drivers") or {}
        artnet = find_module(state, "NetworkSend") or {}
        self.orig_w = _control_value(grid, "width")
        self.orig_h = _control_value(grid, "height")
        self.orig_ip = _control_value(artnet, "ip")
        self.orig_protocol = _control_value(artnet, "protocol")
        # The user may have NetworkSend disabled (e.g. while testing LED output);
        # relay legs need it on, so remember the original state to restore.
        self.artnet_enabled = bool(artnet.get("enabled", False))
        self.brightness = _control_value(drivers, "brightness") or 0
        preset_idx = _control_value(drivers, "lightPreset") or 0
        self.preset = PRESET_NAMES[int(preset_idx)] if int(preset_idx) < len(PRESET_NAMES) else "RGB"
        self.added_receiver = False
        self.ip_changed = False
        self.enable_changed = False
        self.protocol_changed = False

    def set_control(self, module: str, control: str, value):
        self.client.post("/api/control",
                         {"module": module, "control": control, "value": value})

    def setup(self):
        # Same grid everywhere: a solid frame must fill every listener's buffer
        # (16×16 → 256 lights → 2 universes, preview stride 1).
        self.set_control("Grid", "width", 16)
        self.set_control("Grid", "height", 16)
        self.client.post("/api/modules", {"type": "NetworkReceiveEffect",
                                          "id": "NetworkReceive", "parent_id": "Layer"})
        self.added_receiver = True

    def restore(self):
        if self.added_receiver:
            try:
                self.client.delete("/api/modules/NetworkReceive")
            except Exception as e:
                print(f"  WARN  {self.name}: could not remove NetworkReceive: {e}")
        if self.ip_changed and self.orig_ip is not None:
            try:
                self.set_control("NetworkSend", "ip", self.orig_ip)
            except Exception as e:
                print(f"  WARN  {self.name}: could not restore NetworkSend.ip: {e}")
        if self.enable_changed:
            try:
                self.set_control("NetworkSend", "enabled", self.artnet_enabled)
            except Exception as e:
                print(f"  WARN  {self.name}: could not restore NetworkSend.enabled: {e}")
        if self.protocol_changed and self.orig_protocol is not None:
            try:
                self.set_control("NetworkSend", "protocol", self.orig_protocol)
            except Exception as e:
                print(f"  WARN  {self.name}: could not restore NetworkSend.protocol: {e}")
        for key, val in (("width", self.orig_w), ("height", self.orig_h)):
            if val is not None:
                try:
                    self.set_control("Grid", key, val)
                except Exception as e:
                    print(f"  WARN  {self.name}: could not restore Grid.{key}: {e}")


# --- the matrix ----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", help="only run rounds where this deviceName is the sender")
    ap.add_argument("--host", help="only run rounds where this host is the sender "
                                   "(MoonDeck forwards the selected device here)")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="per-channel color tolerance (default 0 — preview is byte-exact)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for a matching preview frame per leg")
    ap.add_argument("--packets", type=int, default=10, help="desktop seed frame repeats")
    ap.add_argument("--pace-ms", type=int, default=50, help="pause between seed frames")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds after a mutating call before asserting")
    args = ap.parse_args()

    devices = load_selected_devices()
    if not devices:
        print("FAIL  no selected+reachable devices in moondeck.json's active "
              "network — check device boxes in the Live tab")
        return 2
    print(f"devices: {len(devices)} selected — "
          + ", ".join(f"{d.get('deviceName', '?')} ({d['ip']})" for d in devices), flush=True)
    if len(devices) == 1:
        print("note: only one device selected — running the desktop→device leg; "
              "the device↔device matrix needs ≥2 boards", flush=True)

    boards = [Board(d) for d in devices]
    passed, failed, skipped = 0, 0, 0
    relay_count = 0   # global counter so relay protocols cycle across ALL legs
                      # (a per-round formula repeats the same protocol when the
                      # board count and protocol count share a factor)
    try:
        for b in boards:
            b.setup()
        time.sleep(args.settle)  # one settle for the buildStates above

        for k, sender in enumerate(boards):
            if args.device and sender.name != args.device:
                continue
            if args.host and sender.host != args.host:
                continue
            color = ROUND_COLORS[k % len(ROUND_COLORS)]
            print(f"== round {k + 1}/{len(boards)}: sender {sender.name}, "
                  f"color {color}", flush=True)

            # Leg 1 — the seed sweep: the desktop seeds the sender once per protocol,
            # each with a rotated color (a stale frame from the previous
            # protocol can't false-pass); the receiver autodetects all three.
            # The sender's preview shows the RAW color (uncorrected buffer).
            seeded_color = None
            for pi, proto in enumerate(PROTOCOLS):
                proto_color = color[pi:] + color[:pi]
                send_solid(sender.host, proto_color, protocol=proto,
                           repeats=args.packets, pace_ms=args.pace_ms)
                ok, pct, pts, detail = _preview_ws.wait_for_solid(
                    sender.host, proto_color, args.tolerance, 100.0, args.timeout)
                if ok:
                    print(f"PASS  desktop → {sender.name} [{proto}] (preview solid, {pts} points)",
                          flush=True)
                    passed += 1
                    seeded_color = proto_color
                else:
                    print(f"FAIL  pc → {sender.name} [{proto}] (best {pct:.0f}% of {pts} points"
                          f"{', ' + detail if detail else ''})"
                          " — desktop listeners: check the OS firewall allows UDP 6454/5568/4048",
                          flush=True)
                    failed += 1
            if seeded_color is None:
                continue  # without a seeded sender the relay legs can't mean anything
            color = seeded_color  # the sender's buffer now holds the last seeded color

            # Legs 2..N — sender relays to each listener via its own
            # NetworkSendDriver, cycling the protocol control round-robin so a
            # full matrix run exercises all three firmware send paths;
            # listeners see the sender's CORRECTED color.
            for listener in boards:
                if listener is sender:
                    continue
                if self_skip := _relay_skip_reason(sender):
                    print(f"SKIP  {sender.name} → {listener.name} ({self_skip})", flush=True)
                    skipped += 1
                    continue
                relay_proto = relay_count % len(PROTOCOLS)
                relay_count += 1
                expected = corrected(color, sender.brightness, sender.preset)
                if not sender.artnet_enabled and not sender.enable_changed:
                    sender.set_control("NetworkSend", "enabled", True)
                    sender.enable_changed = True
                sender.set_control("NetworkSend", "protocol", relay_proto)
                sender.protocol_changed = True
                sender.set_control("NetworkSend", "ip", listener.host.partition(":")[0])
                sender.ip_changed = True
                ok, pct, pts, detail = _preview_ws.wait_for_solid(
                    listener.host, expected, args.tolerance, 100.0, args.timeout)
                if ok:
                    print(f"PASS  {sender.name} → {listener.name} [{PROTOCOLS[relay_proto]}] "
                          f"(expected {expected} after sender correction, {pts} points)", flush=True)
                    passed += 1
                else:
                    print(f"FAIL  {sender.name} → {listener.name} [{PROTOCOLS[relay_proto]}] "
                          f"(expected {expected}, best {pct:.0f}% of {pts} points"
                          f"{', ' + detail if detail else ''})", flush=True)
                    failed += 1
            if sender.ip_changed and sender.orig_ip is not None:
                sender.set_control("NetworkSend", "ip", sender.orig_ip)
                sender.ip_changed = False
            if sender.protocol_changed and sender.orig_protocol is not None:
                sender.set_control("NetworkSend", "protocol", sender.orig_protocol)
                sender.protocol_changed = False
    finally:
        for b in boards:
            b.restore()

    print(f"SUMMARY: {passed} passed, {failed} failed, {skipped} skipped", flush=True)
    return 1 if failed else 0


def _relay_skip_reason(sender: "Board"):
    """A relay leg is meaningless when the sender's correction destroys the
    signal: RGBW presets emit 4 bytes/light (misaligns a 3-channel listener),
    and brightness 0 corrects every color to black — black also matches a
    listener that received NOTHING (staging zero-fill), a guaranteed false pass."""
    if sender.preset not in PRESET_ORDER:
        return f"sender preset {sender.preset} is 4-channel — relay assert supports 3-channel presets"
    if all(c == 0 for c in corrected((255, 255, 255), sender.brightness, "RGB")):
        return "sender Drivers.brightness too low — corrected color is black (raise brightness)"
    return None


if __name__ == "__main__":
    sys.exit(main())

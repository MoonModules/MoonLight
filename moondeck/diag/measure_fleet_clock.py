#!/usr/bin/env python3
"""Measure how closely the devices' fleet clocks agree.

Every MoonLight device answers the fleet clock's request (`/moonlight/clock`, UDP 65511) with its fleet time, whoever asks. This script asks
the devices in rounds from this computer, every device once per round, back to back, so the devices of one round are compared at nearly
one instant and this computer's clock, and its drift against the fleet, cancel out. Each device is compared with the first in the round
where their two round trips add up least, NTP's clock filter for a pair; its error bound is half those two round trips. The verdict takes
the widest spread every device's bound allows.

Read-only: it sends requests and changes nothing on any device. A device that does not answer runs no fleet clock (older firmware).
"""

import argparse
import json
import socket
import struct
import sys
import time
import urllib.request

PORT = 65511
ASK = "/moonlight/clock"
REPLY = "/moonlight/clock/reply"
AIM_MS = 1.0   # the plan's aim: the fleet within a millisecond on the LAN


def osc_str(s: str) -> bytes:
    """An OSC string: NUL-terminated, padded to four bytes."""
    b = s.encode() + b"\0"
    return b + b"\0" * (-len(b) % 4)


def read_str(data: bytes, at: int):
    """An OSC string from `data` at `at`, and where the next field starts."""
    end = data.index(b"\0", at)
    return data[at:end].decode(errors="replace"), (end + 4) & ~3


def now_us() -> int:
    """This computer's monotonic clock, in microseconds."""
    return time.monotonic_ns() // 1000


def ask_packet(t1: int) -> bytes:
    return osc_str(ASK) + osc_str(",h") + struct.pack(">q", t1)


def parse_reply(data: bytes):
    """t1, t2 and t3 of a reply, or None."""
    try:
        addr, at = read_str(data, 0)
        tags, at = read_str(data, at)
        if addr != REPLY or tags != ",hhh" or len(data) < at + 24:
            return None
        return struct.unpack(">qqq", data[at:at + 24])
    except ValueError:
        return None


def exchange(sock: socket.socket, ip: str):
    """One exchange with `ip`: (delay_us, offset_us), or None when it did not answer within half a second."""
    t1 = now_us()
    sock.sendto(ask_packet(t1), (ip, PORT))
    deadline = time.monotonic() + 0.5
    while time.monotonic() < deadline:
        sock.settimeout(max(0.01, deadline - time.monotonic()))
        try:
            data, src = sock.recvfrom(256)
        except socket.timeout:
            return None
        t4 = now_us()
        reply = parse_reply(data)
        if src[0] != ip or not reply or reply[0] != t1:
            continue   # a late reply to an earlier request
        _, t2, t3 = reply
        delay = (t4 - t1) - (t3 - t2)
        return (delay, ((t2 - t1) + (t3 - t4)) // 2) if delay >= 0 else None
    return None


def discovered(source: str):
    """The MoonLight devices `source`'s Discovery list holds: (name, ip)."""
    with urllib.request.urlopen(f"http://{source}/api/modules/Devices", timeout=4) as r:
        tree = json.load(r)
    disc = next(c for c in tree["children"] if c["name"] == "Discovery")
    rows = next(c for c in disc["controls"] if c["name"] == "devices")["value"]
    return [(row.get("name", row["ip"]), row["ip"]) for row in rows if row.get("type") == "MoonLight" and row.get("ip")]


def main() -> int:
    ap = argparse.ArgumentParser(description="Measure how closely the devices' fleet clocks agree.")
    ap.add_argument("hosts", nargs="*", help="device addresses; default: every MoonLight device in --from's Discovery list")
    ap.add_argument("--from", dest="source", default="localhost:8080", help="the device whose Discovery list names the devices")
    ap.add_argument("--count", type=int, default=40, help="exchanges per device (default 40)")
    ap.add_argument("--gap", type=float, default=0.05, help="seconds between exchanges (default 0.05)")
    args = ap.parse_args()

    devices = [(h, h) for h in args.hosts] if args.hosts else discovered(args.source)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))   # any free port: the devices answer the port a request came from

    # Rounds: every device once per round, so each round's samples share one moment.
    rounds = []
    answered = [0] * len(devices)
    for _ in range(args.count):
        rounds.append([exchange(sock, ip) for _, ip in devices])
        for i, sample in enumerate(rounds[-1]):
            answered[i] += sample is not None
        time.sleep(args.gap)
    live = [i for i in range(len(devices)) if answered[i]]
    for i, (name, ip) in enumerate(devices):
        print(f"  {name:24} {ip:16} " + (f"answered {answered[i]}/{args.count}" if answered[i] else "no fleet clock (no answer)"))
    if len(live) < 2:
        print("Fewer than two devices answered: nothing to compare.")
        return 1

    # Each device against the first live one, in the round where their round trips add up least: (difference, bound), in microseconds.
    first = live[0]
    compared = {first: (0, 0)}
    for i in live[1:]:
        pairs = [(r[i][0] + r[first][0], r[i][1] - r[first][1]) for r in rounds if r[i] and r[first]]
        if pairs:
            both, diff = min(pairs)
            compared[i] = (diff, both / 2)
    print(f"\nEach device's fleet time against {devices[first][0]}'s, with its error bound (half the two round trips):")
    for i, (diff, bound) in compared.items():
        print(f"  {devices[i][0]:24} {diff / 1000:+8.2f} ms  ±{bound / 1000:.2f} ms")
    lo = min(compared.values(), key=lambda c: c[0])
    hi = max(compared.values(), key=lambda c: c[0])
    spread = (hi[0] - lo[0]) / 1000
    widest = (max(d + b for d, b in compared.values()) - min(d - b for d, b in compared.values())) / 1000
    print(f"\nSpread: {spread:.2f} ms between the estimates; at most {widest:.2f} ms within every device's bound.")
    within = widest <= AIM_MS
    print(f"Aim {AIM_MS:.0f} ms: {'met' if within else 'not shown'} (at most {widest:.2f} ms).")
    return 0 if within else 2


if __name__ == "__main__":
    sys.exit(main())

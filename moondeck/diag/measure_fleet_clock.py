#!/usr/bin/env python3
"""Measure how closely the devices' fleet clocks agree.

Every MoonLight device answers the fleet clock's request (`/moonlight/clock`, UDP 65511) with its fleet time, whoever asks. This script asks
each device in turn from this computer, many times, and keeps for each the sample with the least round trip, NTP's clock filter. The
difference between two devices' offsets against this computer's clock is how far their fleet clocks disagree, this computer's clock
cancelling out; the error bound is half the two round trips added.

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


def probe(sock: socket.socket, ip: str, count: int, gap: float):
    """The sample with the least round trip of `count` exchanges with `ip`: (delay_us, offset_us, answered), or None when none answered."""
    best = None
    answered = 0
    for _ in range(count):
        t1 = now_us()
        sock.sendto(ask_packet(t1), (ip, PORT))
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                data, src = sock.recvfrom(256)
            except socket.timeout:
                break
            t4 = now_us()
            reply = parse_reply(data)
            if src[0] != ip or not reply or reply[0] != t1:
                continue   # a late reply to an earlier request
            _, t2, t3 = reply
            offset = ((t2 - t1) + (t3 - t4)) // 2
            delay = (t4 - t1) - (t3 - t2)
            answered += 1
            if delay >= 0 and (best is None or delay < best[0]):
                best = (delay, offset)
            break
        time.sleep(gap)
    return None if best is None else (best[0], best[1], answered)


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

    results = []
    for name, ip in devices:
        r = probe(sock, ip, args.count, args.gap)
        if r is None:
            print(f"  {name:24} {ip:16} no fleet clock (no answer)")
            continue
        delay, offset, answered = r
        results.append((name, ip, offset, delay))
        print(f"  {name:24} {ip:16} answered {answered}/{args.count}, best round trip {delay / 1000:.2f} ms")

    if len(results) < 2:
        print("Fewer than two devices answered: nothing to compare.")
        return 1
    offsets = sorted(o for _, _, o, _ in results)
    median = offsets[len(offsets) // 2]
    print("\nEach device's fleet time against the median, with its error bound (half its best round trip):")
    for name, ip, offset, delay in results:
        print(f"  {name:24} {(offset - median) / 1000:+8.2f} ms  ±{delay / 2000:.2f} ms")
    lo = min(results, key=lambda r: r[2])
    hi = max(results, key=lambda r: r[2])
    spread = (hi[2] - lo[2]) / 1000
    bound = (hi[3] + lo[3]) / 2000
    print(f"\nSpread: {spread:.2f} ms between {lo[0]} and {hi[0]}, ±{bound:.2f} ms measurement error.")
    within = spread + bound <= AIM_MS
    print(f"Aim {AIM_MS:.0f} ms: {'met' if within else 'not shown'} ({spread:.2f} + {bound:.2f} ms).")
    return 0 if within else 2


if __name__ == "__main__":
    sys.exit(main())

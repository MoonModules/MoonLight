#!/usr/bin/env python3
"""Send one OSC message to a MoonLight device: the bench tool for the OSC ingest module.

No dependency on purpose: OSC 1.0 is a dozen lines to emit, and a pip install would be a build
requirement for everyone to run one bench check.

    uv run moondeck/check/send_osc.py 192.168.1.164 /mm/fader/1 0.75
    uv run moondeck/check/send_osc.py 192.168.1.164 /mm/control/Drivers/brightness 128

A value containing '.' is sent as an OSC float (what TouchOSC and Resolume send, 0..1); anything
else as an int (what a hardware bridge sends, 0..255). Both reach the same control value.
"""
import argparse
import socket
import struct
import sys


def osc_message(address: str, value) -> bytes:
    """One OSC 1.0 message: address, type tags, argument, each padded to 4 bytes, big-endian."""
    def osc_string(text: str) -> bytes:
        raw = text.encode("ascii") + b"\0"
        return raw + b"\0" * ((4 - len(raw) % 4) % 4)

    if isinstance(value, float):
        return osc_string(address) + osc_string(",f") + struct.pack(">f", value)
    return osc_string(address) + osc_string(",i") + struct.pack(">i", value)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="device IP")
    ap.add_argument("address", help="OSC address, e.g. /mm/fader/1")
    ap.add_argument("value", help="float (0..1) if it contains a '.', else int (0..255)")
    ap.add_argument("--port", type=int, default=9000, help="OSC port (default 9000)")
    args = ap.parse_args()

    value = float(args.value) if "." in args.value else int(args.value)
    packet = osc_message(args.address, value)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(packet, (args.host, args.port))
    kind = "float" if isinstance(value, float) else "int"
    print(f"sent {args.address} = {value} ({kind}, {len(packet)} bytes) to {args.host}:{args.port}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

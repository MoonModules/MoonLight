"""Play an FSEQ show to a device over DDP, or write a test show: the reference sender for FSEQ on MoonLight.

FSEQ is xLights' and Falcon Player's file of channel data, frame by frame in wiring order with a step time. This reads and writes uncompressed v2 as FPP's FSEQ_Sequence_File_Format.txt lays it out, little-endian, and plays a file the way a mapping sender streams: each frame as DDP packets, the push flag on the last, paced, at the file's step time. The device's Network Receive takes it with `mapped_by` set to sender.

    uv run moondeck/run/fseq_play.py --host 192.168.1.158                       # play the test sweep, written on first use
    uv run moondeck/run/fseq_play.py --host 192.168.1.158 --file show.fseq --loops 3
    uv run moondeck/run/fseq_play.py --write sweep.fseq --width 64 --height 64 --serpentine
    uv run moondeck/run/fseq_play.py --info show.fseq
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DDP_PORT = 4048
DDP_HEADER = 10
DDP_PAYLOAD = 1440       # whole RGB and RGBW lights, as Network Send chunks
HEADER = struct.Struct("<4sHBBHIIBBBBBBQ")   # the 32-byte fixed header


def wiring_index(x: int, y: int, width: int, serpentine: bool) -> int:
    """A light's index in wiring order: rows from the top, every other row reversed when serpentine."""
    return y * width + ((width - 1 - x) if serpentine and y % 2 else x)


def sweep_frame(width: int, height: int, frame: int, serpentine: bool) -> bytes:
    """A diagonal sweep in wiring order with a white light at logical (0, 0), so a wrong mapping shows."""
    out = bytearray(width * height * 3)
    span = width + height
    for y in range(height):
        for x in range(width):
            v = (x + y + frame) % span * 255 // span
            i = wiring_index(x, y, width, serpentine) * 3
            out[i:i + 3] = bytes((v, 0, 255 - v))
    corner = wiring_index(0, 0, width, serpentine) * 3
    out[corner:corner + 3] = b"\xff\xff\xff"
    return bytes(out)


def write_fseq(path: Path, frames: list[bytes], step_ms: int) -> None:
    """An uncompressed v2 file with one `sp` variable header naming its producer."""
    producer = b"MoonLight fseq_play\0"
    variable = struct.pack("<H2s", 4 + len(producer), b"sp") + producer
    header_len = HEADER.size + len(variable)
    data_offset = (header_len + 3) & ~3
    channels = len(frames[0]) if frames else 0
    head = HEADER.pack(b"PSEQ", data_offset, 0, 2, header_len, channels, len(frames), step_ms,
                       0, 0, 0, 0, 0, int(time.time() * 1e6))
    path.write_bytes(head + variable + bytes(data_offset - header_len) + b"".join(frames))


def read_header(path: Path) -> dict:
    """The fixed header's fields, or SystemExit for a file that is not FSEQ."""
    with open(path, "rb") as f:
        raw = f.read(HEADER.size)
    if len(raw) < HEADER.size or raw[:4] != b"PSEQ":
        raise SystemExit(f"{path}: not an FSEQ file")
    (_, data_offset, minor, major, header_len, channels, frames, step,
     _, compression, blocks, sparse, _, uid) = HEADER.unpack(raw)
    return {"version": f"{major}.{minor}", "data_offset": data_offset, "header_len": header_len,
            "channels": channels, "frames": frames, "step_ms": step,
            "compression": compression & 0x0F, "blocks": blocks | (compression >> 4) << 8,
            "sparse_ranges": sparse, "uid": uid}


def ddp_packets(frame: bytes) -> list[bytes]:
    """One frame as DDP data packets, byte-addressed, the push flag on the last."""
    packets = []
    for seq, offset in enumerate(range(0, len(frame), DDP_PAYLOAD)):
        chunk = frame[offset:offset + DDP_PAYLOAD]
        last = offset + DDP_PAYLOAD >= len(frame)
        header = bytes((0x40 | (0x01 if last else 0), seq & 0x0F, 0x0B, 0x01)) \
            + offset.to_bytes(4, "big") + len(chunk).to_bytes(2, "big")
        packets.append(header + chunk)
    return packets


def play(path: Path, host: str, loops: int, gap_us: int) -> None:
    """Stream the file at its step time, pacing a frame's packets `gap_us` apart."""
    info = read_header(path)
    if info["compression"]:
        raise SystemExit("compressed FSEQ: export with compression off")
    if info["sparse_ranges"]:
        raise SystemExit("sparse ranges are not played by this tool")
    size, step = info["channels"], info["step_ms"] / 1000
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"playing {path.name} to {host}: {info['frames']} frames, {size} channels, {info['step_ms']} ms", flush=True)
    with open(path, "rb") as f:
        for loop in range(loops):
            f.seek(info["data_offset"])
            start = time.time()
            for n in range(info["frames"]):
                packets = ddp_packets(f.read(size))
                for i, packet in enumerate(packets):
                    sock.sendto(packet, (host, DDP_PORT))
                    if gap_us and i + 1 < len(packets):
                        time.sleep(gap_us / 1e6)
                time.sleep(max(0.0, start + (n + 1) * step - time.time()))
            print(f"loop {loop + 1}: {info['frames']} frames in {time.time() - start:.1f} s", flush=True)


def main() -> None:
    p = argparse.ArgumentParser(description="Play an FSEQ show to a device over DDP, or write a test show.")
    p.add_argument("--host", help="the device to play to")
    p.add_argument("--file", type=Path, help="the show; the test sweep when omitted")
    p.add_argument("--loops", type=int, default=1)
    p.add_argument("--gap-us", type=int, default=500, help="microseconds between a frame's packets, as Network Send paces")
    p.add_argument("--write", type=Path, help="write the test sweep to this file and stop")
    p.add_argument("--info", type=Path, help="print a file's header and stop")
    p.add_argument("--width", type=int, default=64)
    p.add_argument("--height", type=int, default=64)
    p.add_argument("--serpentine", action="store_true", help="every other row reversed, as most panels are wired")
    p.add_argument("--frames", type=int, default=400)
    p.add_argument("--step", type=int, default=25, choices=range(1, 256), metavar="1-255",
                   help="milliseconds per frame, one byte in the header")
    a = p.parse_args()

    if a.info:
        for key, value in read_header(a.info).items():
            print(f"{key}: {value}")
        return
    def sweep(path: Path) -> None:
        write_fseq(path, [sweep_frame(a.width, a.height, f, a.serpentine) for f in range(a.frames)], a.step)
        print(f"wrote {path}: {a.width}x{a.height} {'serpentine' if a.serpentine else 'rows'}, {a.frames} frames")
    if a.write:
        sweep(a.write)
        return
    if not a.host:
        p.error("--host names the device to play to")
    path = a.file
    if path is None:
        path = ROOT / "build" / "fseq" / f"sweep-{a.width}x{a.height}{'-serpentine' if a.serpentine else ''}.fseq"
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            sweep(path)
    play(path, a.host.split(":")[0], a.loops, a.gap_us)


if __name__ == "__main__":
    sys.exit(main())

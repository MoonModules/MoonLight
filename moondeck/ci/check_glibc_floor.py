#!/usr/bin/env python3
# /// script
# requires-python = ">=3.14"
# ///
"""Fail when a Linux package's binary needs a newer glibc than the oldest system MoonLight supports.

A binary needs at least the glibc of the machine that built it, and a runner image that moves
raises that silently: the amd64 package once came to need glibc 2.43 and died at startup on
Debian 13, Ubuntu 24.04 and in the container, while it installed cleanly everywhere. The release
runs this after packaging, so a moved builder fails the release instead of a user's board.

The floor is glibc 2.35, the one docs/reference/installing-on-linux.md promises: Ubuntu 22.04,
where both packages build, Raspberry Pi OS Bookworm and Debian 12 (2.36), and everything newer.

It reads the .deb itself, an `ar` archive holding a data tarball, so it needs no dpkg. The tarball
is zstd, dpkg-deb's default on the builders, which Python reads from 3.14 on: hence that version,
which uv fetches once per job.

    uv run moondeck/ci/check_glibc_floor.py dist/moonlight_6.1.0_amd64.deb
"""

import argparse
import io
import re
import sys
import tarfile
from pathlib import Path

FLOOR = (2, 35)   # docs/reference/installing-on-linux.md
BINARY = "usr/bin/MoonLight"


def ar_members(data: bytes):
    """The members of an `ar` archive as (name, bytes), the container format of a .deb."""
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("not an ar archive")
    pos = 8
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        yield name, data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)   # members are padded to an even offset


def binary_of(deb: Path) -> bytes:
    """The MoonLight binary inside a .deb."""
    for name, body in ar_members(deb.read_bytes()):
        if name.startswith("data.tar"):
            with tarfile.open(fileobj=io.BytesIO(body)) as tar:
                for member in tar.getmembers():
                    if member.name.removeprefix("./") == BINARY:
                        return tar.extractfile(member).read()
    raise ValueError(f"{deb.name} holds no {BINARY}")


def needed(binary: bytes) -> tuple:
    """The newest glibc symbol version the binary asks for."""
    versions = {tuple(int(p) for p in m.split(b".")) for m in re.findall(rb"GLIBC_(\d+\.\d+)", binary)}
    return max(versions) if versions else (0, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("debs", nargs="+", type=Path, help="the packages to check")
    args = ap.parse_args()
    failed = False
    for deb in args.debs:
        need = needed(binary_of(deb))
        ok = need <= FLOOR
        failed |= not ok
        print(f"{'OK  ' if ok else 'FAIL'} {deb.name} needs glibc {need[0]}.{need[1]}, "
              f"the floor is {FLOOR[0]}.{FLOOR[1]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

"""Shared lights-over-UDP test helpers — the surface both network live tests
build on (the matrix test run_network_live.py and the latency probe
run_network_roundtrip.py).

Kept in its own module (like _preview_ws.py) so the shared bits — protocol
ports, the three packet builders, and the MoonDeck device set — have one clear
home instead of living inside one of the two consumers. Nothing here is
specific to either test; the matrix-only color-correction and Board
orchestration stay in run_network_live.py.

The packet builders mirror the firmware encoders byte for byte — cross-language
duplication is unavoidable here; keep each in sync with its `.h`:
  build_artdmx ↔ src/light/util/ArtNetPacket.h::buildArtDmxPacket
  build_e131   ↔ src/light/util/E131Packet.h::buildE131Packet
  build_ddp    ↔ src/light/util/DdpPacket.h::buildDdpPacket
"""

import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MOONDECK_STATE = ROOT / "moondeck" / "moondeck.json"

ARTNET_PORT = 6454
E131_PORT = 5568
DDP_PORT = 4048
CHANNELS_PER_UNIVERSE = 510
PROTOCOLS = ["ArtNet", "E1.31", "DDP"]   # mirrors NetworkSendDriver.kProtocolOptions


def build_artdmx(universe: int, sequence: int, data: bytes) -> bytes:
    pkt = bytearray(b"Art-Net\0")
    pkt += bytes([0x00, 0x50])                        # OpDmx, little-endian
    pkt += bytes([0x00, 0x0E])                        # protocol 14, big-endian
    pkt += bytes([sequence & 0xFF, 0])                # sequence, physical
    pkt += bytes([universe & 0xFF, (universe >> 8) & 0xFF])   # universe LE
    pkt += bytes([(len(data) >> 8) & 0xFF, len(data) & 0xFF])  # length BE
    pkt += data
    return bytes(pkt)


def build_e131(universe: int, sequence: int, data: bytes) -> bytes:
    total = 126 + len(data)
    pkt = bytearray(126)
    pkt[0:2] = (0x0010).to_bytes(2, "big")            # preamble size
    pkt[4:16] = b"ASC-E1.17\0\0\0"
    pkt[16:18] = (0x7000 | (total - 16)).to_bytes(2, "big")
    pkt[21] = 0x04                                    # root vector
    pkt[22:38] = b"run_network_live"                  # CID (any stable 16 bytes)
    pkt[38:40] = (0x7000 | (total - 38)).to_bytes(2, "big")
    pkt[43] = 0x02                                    # framing vector
    pkt[44:53] = b"MoonLight"                         # source name (NUL-padded)
    pkt[108] = 100                                    # priority
    pkt[111] = sequence & 0xFF
    pkt[113:115] = universe.to_bytes(2, "big")
    pkt[115:117] = (0x7000 | (total - 115)).to_bytes(2, "big")
    pkt[117] = 0x02                                   # DMP vector
    pkt[118] = 0xA1
    pkt[122] = 0x01                                   # address increment
    pkt[123:125] = (1 + len(data)).to_bytes(2, "big")  # property count
    return bytes(pkt) + data


def build_ddp(offset: int, push: bool, data: bytes) -> bytes:
    pkt = bytearray(10)
    pkt[0] = 0x40 | (0x01 if push else 0x00)
    pkt[2] = 0x01                                     # RGB
    pkt[3] = 0x01                                     # default display
    pkt[4:8] = offset.to_bytes(4, "big")
    pkt[8:10] = len(data).to_bytes(2, "big")
    return bytes(pkt) + data


def load_selected_devices():
    """The devices the user CHECKED in MoonDeck's Live tab (the `selected` flag,
    persisted to moondeck.json), of the active network, re-probed for
    reachability (a stale flag must not hang the run on a 15 s mutating-call
    timeout). The checkbox is the run set — it's what the UI implies and what
    app.js filters Live-tab runs by. A device with no `selected` key (older
    state) counts as selected, so a freshly-discovered list still runs."""
    if not MOONDECK_STATE.exists():
        print(f"FAIL  {MOONDECK_STATE} not found — run MoonDeck once to discover devices")
        sys.exit(2)
    state = json.loads(MOONDECK_STATE.read_text())
    nets = state.get("networks", [])
    net = next((n for n in nets if n.get("name") == state.get("active_network")),
               nets[0] if nets else None)
    if not net:
        print("FAIL  moondeck.json has no networks")
        sys.exit(2)
    devices = []
    for dev in net.get("devices", []):
        if not dev.get("selected", True):   # unchecked → skip; missing key → run
            continue
        try:  # quick reachability probe, far shorter than Client's mutate timeout
            with urllib.request.urlopen(f"http://{dev['ip']}/api/state", timeout=3):
                devices.append(dev)
        except OSError:
            print(f"  WARN  {dev.get('deviceName', dev['ip'])} checked but unreachable — skipped")
    return devices

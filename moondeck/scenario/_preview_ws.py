"""Minimal RFC 6455 WebSocket client for the device's /wsp preview stream.

Stdlib-only (socket/base64/os/time) — the live test scripts must run anywhere
uv runs, with no third-party deps. Sibling-private helper like _observed.py.

The device serves TWO WebSocket paths: `/ws` (control plane, JSON state) and `/wsp`
(the lossy preview channel, binary only). This reader takes `/wsp`, so it sees
PreviewDriver binary frames — 0x03 coordinate tables and 0x02 RGB frames
(`[0x02][count u32 LE][stride u16 LE][epoch u8][drops u8][rgb x count]`, a 9-byte header, see
src/light/drivers/PreviewDriver.h). This reader skips everything except 0x02.

Two simplifications the firmware guarantees (HttpServerModule.cpp):
- frames are never fragmented (broadcastBinary always sends FIN frames), so a
  frame is fully described by its header;
- payloads stay far below 64 KiB, so the 127 (u64) length form never occurs.
We never send a data frame (server pushes unprompted), so no client-side
masking is needed; closing the TCP socket is how a vanishing browser behaves
and the server reaps it the same way.
"""

import base64
import os
import socket
import time


def handshake(host: str, path: str, timeout_s: float = 5.0):
    """Open `path` on `host` ("ip[:port]") as a WebSocket: the socket and the bytes read past the 101 headers, which are already the
    first frame(s). Status 101 is proof enough; skipping the Sec-WebSocket-Accept check saves the SHA-1 dance against our own firmware."""
    h, _, p = host.partition(":")
    sock = socket.create_connection((h, int(p or 80)), timeout=timeout_s)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(
        (f"GET {path} HTTP/1.1\r\nHost: {host}\r\n"
         f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
         f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    raw = b""
    while b"\r\n\r\n" not in raw:
        chunk = sock.recv(1024)
        if not chunk:
            raise ConnectionError("WS handshake: connection closed")
        raw += chunk
    head, _, rest = raw.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0]
    if b" 101" not in status:
        sock.close()
        raise ConnectionError("WS handshake refused: the device serves at most 4 preview clients; close open preview browser tabs "
                              "and retry. Got: " + status.decode(errors="replace"))
    return sock, rest


def client_frame(opcode: int, payload: bytes) -> bytes:
    """One masked client frame (RFC 6455 requires the mask), for a payload under 126 bytes: the preview's requests and the keepalive."""
    mask = os.urandom(4)
    return bytes([0x80 | opcode, 0x80 | len(payload)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def request_preview(sock) -> None:
    """Post the standing frame request [0x51][stride][fps] on an open /wsp socket: the pull model streams to a client only after its request."""
    sock.sendall(client_frame(0x2, bytes([0x51, 1, 25])))


class PreviewSocket:
    """One /wsp connection. `host` is the device's HTTP address ("ip[:port]")."""

    def __init__(self, host: str, timeout_s: float = 5.0):
        self.sock, rest = handshake(host, "/wsp", timeout_s)
        self._buf = rest
        request_preview(self.sock)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _read_exact(self, n: int, deadline: float) -> bytes:
        while len(self._buf) < n:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("WS stream closed by device")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def read_frame(self, deadline: float):
        """Return (opcode, payload) for the next frame. Raises on close/timeout."""
        b0, b1 = self._read_exact(2, deadline)
        opcode = b0 & 0x0F
        length = b1 & 0x7F  # server→client frames are unmasked (mask bit 0)
        if length == 126:
            ext = self._read_exact(2, deadline)
            length = (ext[0] << 8) | ext[1]
        payload = self._read_exact(length, deadline)
        if opcode == 0x8:
            raise ConnectionError("WS close frame from device")
        return opcode, payload


def wait_for_solid(host: str, rgb, tolerance: int = 0, min_match_pct: float = 100.0,
                   timeout_s: float = 10.0):
    """Wait for a 0x02 preview frame where ≥min_match_pct of the streamed points
    equal `rgb` (per-channel ±tolerance). Returns (ok, best_pct, points, detail);
    on failure `detail` is one sample mismatching point for the error message."""
    deadline = time.monotonic() + timeout_s
    best_pct, points, detail = 0.0, 0, ""
    ws = PreviewSocket(host)
    try:
        while time.monotonic() < deadline:
            try:
                opcode, payload = ws.read_frame(deadline)
            except TimeoutError:    # socket.timeout is an alias since Python 3.10
                break
            if opcode != 0x2 or not payload or payload[0] != 0x02:
                continue  # text/state frame or 0x03 coordinate table
            count = payload[1] | (payload[2] << 8) | (payload[3] << 16) | (payload[4] << 24)
            triples = payload[9:9 + count * 3]
            if count == 0 or len(triples) < count * 3:
                continue
            matched = 0
            sample = ""
            for i in range(count):
                p = triples[i * 3:i * 3 + 3]
                if all(abs(p[c] - rgb[c]) <= tolerance for c in range(3)):
                    matched += 1
                elif not sample:
                    sample = f"point {i} = ({p[0]},{p[1]},{p[2]})"
            pct = matched * 100.0 / count
            if pct > best_pct:
                best_pct, points, detail = pct, count, sample
            if pct >= min_match_pct:
                return True, pct, count, ""
        return False, best_pct, points, detail
    finally:
        ws.close()

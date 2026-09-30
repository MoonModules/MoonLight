#!/usr/bin/env python3
"""Serve a built firmware binary over HTTP so a board can pull it via OTA.

MoonBase and the app's updater both take a URL and stream it with
`esp_https_ota`, which is the fast path for putting a local build on a board
that is not on USB. What it needs from the other end is HTTP/1.1.

WHY THIS SCRIPT EXISTS rather than `python -m http.server`: that serves
HTTP/1.0, whose default is no keep-alive and a body that ends when the
connection closes rather than at Content-Length. MoonBase asks for keep-alive
and a 32 KB receive buffer, and against a 1.0 server the transfer crawls and
then fails with `0xffffffff` (ESP_FAIL) partway through. That cost two failed
OTA attempts on a bench board before the `HTTP/1.0` in the response line was
spotted, while the identical URL from GitHub (HTTP/1.1) worked first time.

Serves exactly one file, read fresh per request so a rebuild is picked up
without a restart.
"""

import argparse
import http.server
import socket
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_esp32 import APP_BIN  # noqa: E402


def _lan_ip() -> str:
    """The address a board on the LAN can reach, not 127.0.0.1.

    No packet is sent: connect() on a UDP socket only picks the route, which is
    what names the interface the board would arrive on. That needs a route to
    exist, though, and a bench network with no way out has none, so a failure
    here falls back to the hostname and finally to a placeholder. The address is
    only PRINTED, so getting it wrong must not stop the server binding: an
    isolated network is exactly where this script is most useful.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "<this machine>"
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help="path to the .bin, or a firmware name under build/esp32-<name>/")
    ap.add_argument("--port", type=int, default=8099)
    args = ap.parse_args()

    binary = Path(args.firmware)
    if not binary.is_file():
        binary = ROOT / "build" / f"esp32-{args.firmware}" / APP_BIN
    if not binary.is_file():
        print(f"no firmware at {binary}", file=sys.stderr)
        return 1

    class Handler(http.server.BaseHTTPRequestHandler):
        # THE POINT OF THIS SCRIPT. Content-Length framing and keep-alive, both
        # of which esp_https_ota relies on.
        protocol_version = "HTTP/1.1"

        def _send(self, body: bool) -> None:
            data = binary.read_bytes()   # re-read per request: a rebuild needs no restart
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if body:
                self.wfile.write(data)

        def do_HEAD(self) -> None:
            self._send(body=False)

        def do_GET(self) -> None:
            self._send(body=True)

        def log_message(self, fmt: str, *a) -> None:
            print(f"  {self.address_string()} {fmt % a}", flush=True)

    url = f"http://{_lan_ip()}:{args.port}/{binary.name}"
    print(f"serving {binary} ({binary.stat().st_size} bytes) as HTTP/1.1")
    print(f"  {url}")
    print("  paste that into MoonBase's 'From a URL', or POST it to /api/firmware/url")
    print("  ctrl-c to stop")
    with http.server.ThreadingHTTPServer(("", args.port), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())

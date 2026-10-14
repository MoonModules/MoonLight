#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""End-to-end Improv smoke test against a USB-connected ESP32.

Three sequential checks; PASS only when all three pass within timeout:

  1. **Probe** — device responds to GET_DEVICE_INFO + GET_CURRENT_STATE
     Improv RPCs. Confirms the device-side listener is alive and parsing
     frames. (Same checks improv_probe.py does standalone.)

  2. **Provision** — send WIFI_SETTINGS with a known SSID + password.
     Confirms the device accepts credentials and transitions to
     PROVISIONED. (Same flow improv_provision.py drives standalone.)

  3. **Reachable** (optional, --no-network to skip) — after PROVISIONED,
     HTTP GET the device's reported URL. Confirms the device actually
     joined the LAN and is serving its API.

The script exists because the browser-side Improv flow (ESP Web Tools'
modal) is awkward to automate and harder to reproduce on demand: it
needs Chrome, Web Serial, and a click-through. This script exercises
the **device-side** Improv implementation — which is the part we own
and the part most likely to break across firmware changes. The browser
side is upstream-maintained and stable.

Recommended developer test before any commit touching:
  - src/core/util/ImprovFrame.h
  - src/platform/esp32/platform_esp32_improv.cpp
  - mooninstaller/index.html
  - src/ui/install-picker.js
  - moondeck/build/improv_*.py

Usage:
  uv run moondeck/build/improv_smoke_test.py --port /dev/tty.usbserial-X

Exit codes: 0 = all three checks passed, 1 = device-side failure
(probe or provision), 2 = environment failure (couldn't open port, or
provision succeeded but device unreachable on LAN — distinct from
device failure so CI can decide whether to retry).
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
PROBE_SCRIPT = SCRIPT_DIR / "improv_probe.py"
PROVISION_SCRIPT = SCRIPT_DIR / "improv_provision.py"


def _run(script: Path, args: list[str], timeout: float) -> tuple[int, str, str]:
    """Run a sibling script via the current interpreter, capture stdout+stderr.

    Both improv_probe.py and improv_provision.py carry an inline `# /// script`
    dependency block, so direct invocation via `sys.executable` only works
    when pyserial is on this interpreter's path. We're already running under
    `uv run`, so pyserial is available; no need to re-launch under uv.
    """
    try:
        r = subprocess.run(
            [sys.executable, str(script)] + args,
            capture_output=True, text=True, timeout=timeout,
        )
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {timeout}s"


def _device_url(out: str) -> str | None:
    """The device URL in a step's output: `==> provisioned: http://...` from improv_provision.py, or `url: http://...` from the probe's current state."""
    for line in out.splitlines():
        line = line.strip()
        if line.startswith(("==> provisioned:", "url:")):
            return line.split(":", 1)[1].strip() or None
    return None


def _check_reachable(url: str, timeout: float) -> tuple[bool, str]:
    """HTTP GET / on the device URL — does the LAN-joined device respond?"""
    import urllib.request
    import urllib.error
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "improv-smoke-test"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status == 200, f"HTTP {resp.status}"
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}"
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        return False, str(e)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True,
                    help="Serial device path, e.g. /dev/tty.usbserial-X")
    ap.add_argument("--ssid", default=None,
                    help="WiFi SSID (default: improv_provision.py picks from "
                         "host_wifi.py → moondeck.json active network → host OS)")
    ap.add_argument("--password", default=None,
                    help="WiFi password (default: same source as --ssid)")
    ap.add_argument("--no-network", action="store_true",
                    help="Skip the post-provision HTTP reachability check. "
                         "Use when the device is on an isolated provisioning "
                         "network this host can't route to.")
    ap.add_argument("--probe-timeout", type=float, default=10.0,
                    help="Seconds to wait for the probe step (default: 10)")
    ap.add_argument("--provision-timeout", type=float, default=60.0,
                    help="Seconds to wait for the provision step including "
                         "WiFi join (default: 60 — slow APs / DHCP can take a while)")
    ap.add_argument("--network-timeout", type=float, default=10.0,
                    help="Seconds to wait for the post-provision HTTP GET "
                         "(default: 10)")
    ap.add_argument("--json", action="store_true",
                    help="Emit a single-line JSON summary (suitable for CI parsing)")
    args = ap.parse_args()

    started = time.monotonic()
    summary: dict = {
        "port": args.port,
        "probe": None,           # bool: did the device answer the probe?
        "provision": None,       # bool: did the device reach PROVISIONED?
        "reachable": None,       # bool|None: did HTTP succeed (None=skipped)?
        "device_url": None,
        "elapsed_s": None,
        "failure": None,
    }

    def finish(exit_code: int) -> int:
        summary["elapsed_s"] = round(time.monotonic() - started, 1)
        if args.json:
            print(json.dumps(summary))
        else:
            ok = exit_code == 0
            tag = "PASS" if ok else "FAIL"
            parts = []
            if summary["probe"]: parts.append("probe")
            if summary["provision"]: parts.append("provision")
            if summary["reachable"]: parts.append("reachable")
            if summary["reachable"] is None and exit_code == 0:
                parts.append("network-skipped")
            print(f"\n{tag} improv smoke test: {' + '.join(parts) or '(no checks completed)'} "
                  f"(took {summary['elapsed_s']}s)")
            if summary["failure"]:
                print(f"     reason: {summary['failure']}")
            if summary["device_url"]:
                print(f"     device: {summary['device_url']}")
        return exit_code

    # ----- Step 1: probe ---------------------------------------------------
    print(f"==> [1/3] probe   (timeout {args.probe_timeout}s)")
    rc, out, err = _run(PROBE_SCRIPT,
                        ["--port", args.port, "--timeout", str(args.probe_timeout)],
                        timeout=args.probe_timeout + 5)
    sys.stdout.write(_indent(out))
    probe_url = _device_url(out)   # a provisioned board reports it, so the network step runs when provisioning keeps its credentials
    if err.strip():
        sys.stderr.write(_indent(err))
    if rc != 0:
        summary["probe"] = False
        summary["failure"] = f"probe exited {rc} (port {args.port} — Improv not answering?)"
        return finish(1)
    summary["probe"] = True

    # ----- Step 2: provision ----------------------------------------------
    print(f"==> [2/3] provision   (timeout {args.provision_timeout}s)")
    prov_args = ["--port", args.port, "--timeout", str(args.provision_timeout)]
    if args.ssid is not None: prov_args.extend(["--ssid", args.ssid])
    if args.password is not None: prov_args.extend(["--password", args.password])
    rc, out, err = _run(PROVISION_SCRIPT, prov_args,
                        timeout=args.provision_timeout + 5)
    sys.stdout.write(_indent(out))
    if err.strip():
        sys.stderr.write(_indent(err))
    if rc != 0:
        summary["provision"] = False
        summary["failure"] = f"provision exited {rc} (see output above)"
        return finish(1)
    summary["provision"] = True

    url = _device_url(out) or probe_url
    summary["device_url"] = url

    # ----- Step 3: reachable (optional) -----------------------------------
    if args.no_network:
        print("==> [3/3] network   skipped (--no-network)")
        return finish(0)
    if not url:
        # Neither step reported a URL; the device is provisioned, so the reachability check is skipped with the reason.
        print("==> [3/3] network   skipped (no device URL in the probe or provision output)")
        summary["failure"] = "provision succeeded but device URL not parseable from output"
        return finish(0)

    print(f"==> [3/3] network   GET {url}   (timeout {args.network_timeout}s)")
    ok, msg = _check_reachable(url, args.network_timeout)
    if not ok:
        summary["reachable"] = False
        summary["failure"] = f"device unreachable at {url}: {msg}"
        # Exit 2 (distinct from device-side failure): provision worked, the
        # gap is between LAN routing / mDNS / firewall and this host.
        return finish(2)
    summary["reachable"] = True
    print(f"     OK ({msg})")
    return finish(0)


def _indent(s: str) -> str:
    """Prefix each non-empty line with two spaces so child output reads
    as a nested step under the [1/3] / [2/3] / [3/3] markers above."""
    return "".join(("  " + ln if ln.strip() else ln) for ln in s.splitlines(keepends=True))


if __name__ == "__main__":
    sys.exit(main())

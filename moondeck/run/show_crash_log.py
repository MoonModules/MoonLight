#!/usr/bin/env python3
"""Print the most recent projectMM crash report from the OS diagnostic store.

macOS writes .ips crash reports to ~/Library/Logs/DiagnosticReports/.
Windows records faults in the Application event log instead, as "Application
Error" entries naming the faulting module and offset; there is no file to find
unless WER LocalDumps is switched on, which it is not by default.
This script finds the newest projectMM-*.ips, extracts the key fields
(exception type, faulting thread, call stack), and prints them so they
appear in MoonDeck's log stream next to projectMM.log.

If no crash report exists it prints the last 40 lines of projectMM.log
so the run log is always visible from one place.
"""

import json
import platform
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def _build_dir() -> Path:
    return ROOT / "build" / {
        "Darwin": "macos",
        "Linux": "linux",
        "Windows": "windows",
    }.get(platform.system(), platform.system().lower())


def _crash_reports_dir() -> Path:
    return Path.home() / "Library" / "Logs" / "DiagnosticReports"


def _find_latest_ips() -> Path | None:
    d = _crash_reports_dir()
    if not d.exists():
        return None
    candidates = sorted(d.glob("projectMM-*.ips"), key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0] if candidates else None


def _print_ips(path: Path) -> None:
    print(f"=== macOS crash report: {path.name} ===")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"(could not parse: {e})")
        print(path.read_bytes().decode("utf-8", errors="replace")[:2000])
        return

    exc = data.get("exception", {})
    print(f"Type    : {exc.get('type','?')} — {exc.get('signal','?')}")
    print(f"Subtype : {exc.get('subtype','?')}")
    print(f"PID     : {data.get('pid','?')}  uptime: {data.get('uptime','?')} ms")
    print(f"Captured: {data.get('captureTime','?')}")

    threads = data.get("threads", [])
    for t in threads:
        if not t.get("triggered"):
            continue
        print(f"\nFaulting thread {t.get('id','?')} ({t.get('queue','')}):")
        for i, frame in enumerate(t.get("frames", [])[:20]):
            sym = frame.get("symbol", "?")
            off = frame.get("symbolLocation", "")
            print(f"  #{i:<2} {sym}  +{off}")
        break


def _print_windows_faults() -> tuple[bool, bool]:
    """Print recent Application Error entries naming one of this repository's binaries.

    Returns (found, read_ok): the caller needs them apart, because "the log holds nothing"
    and "the log could not be read" want different things said to the reader.

    Windows keeps no crash FILE by default, but the Application event log always
    records the fault with the faulting module and offset, which is the half that
    says where it died. Read through PowerShell rather than a dependency: Get-WinEvent
    ships with the OS and needs no elevation for the Application log.
    """
    # `-ErrorAction Stop` plus an explicit catch, rather than SilentlyContinue: an empty result and
    # a refused read are BOTH non-zero exits with empty stdout under SilentlyContinue, so the exit
    # status alone cannot tell "this machine has never crashed" from "the log could not be read",
    # and Ignore behaves the same. Only the terminating error carries the distinction, in
    # NoMatchingEventsFound, so the query separates them here and the exit status then means what
    # the caller reads it to mean.
    ps = (
        "try {"
        " Get-WinEvent -FilterHashtable @{LogName='Application';"
        "ProviderName='Application Error'} -MaxEvents 200 -ErrorAction Stop |"
        " Where-Object { $_.Message -match 'projectMM|MoonLight' } |"
        " Select-Object -First 3 |"
        " ForEach-Object { $_.TimeCreated.ToString('s') + ' :: ' +"
        " ($_.Message -replace \"`r`n\", ' ') }"
        " } catch {"
        " if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') { exit 0 }"
        " Write-Error $_; exit 1 }"
    )
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", timeout=60)
    except Exception as e:
        print(f"(could not read the Application event log: {e})")
        return False, False
    # The exit status is the only signal a failed query gives. `-ErrorAction SilentlyContinue`
    # is there so an empty log is not an error, but it also swallows a real one, so a refused
    # read reaches here looking exactly like a clean log: no output, no stderr. Without this,
    # "could not read" would be reported to the reader as "nothing crashed".
    if out.returncode != 0:
        err = (out.stderr or "").strip().splitlines()
        detail = f": {err[0][:200]}" if err else ""
        print(f"(could not read the Application event log, powershell exited "
              f"{out.returncode}{detail})")
        return False, False
    lines = [line.strip() for line in (out.stdout or "").splitlines() if line.strip()]
    if not lines:
        return False, True
    print("=== Windows Application Error entries ===")
    for line in lines:
        print(line[:400])
    return True, True


def _print_run_log() -> None:
    log = _build_dir() / "projectMM.log"
    if not log.exists():
        print("No projectMM.log found.")
        return
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    print(f"=== Last {min(40, len(lines))} lines of projectMM.log ===")
    for line in lines[-40:]:
        print(line)


def main() -> None:
    if platform.system() == "Windows":
        found, read_ok = _print_windows_faults()
        if read_ok and not found:
            print("No Application Error entries for this repository's binaries in the event log.")
        print()
    elif platform.system() == "Darwin":
        ips = _find_latest_ips()
        if ips:
            _print_ips(ips)
        elif _crash_reports_dir().exists():
            print("No projectMM crash reports found in DiagnosticReports.")
        else:
            print("DiagnosticReports directory not found (sandboxed?).")
        print()
    else:
        # Linux keeps no per-application crash store this tool can read: coredumps land wherever
        # core_pattern points, which is systemd-coredump on most distributions. Say so rather than
        # naming an Apple directory at a reader who can never have one.
        print("No crash-report reader for this platform; the run log follows.")
        print()

    _print_run_log()


if __name__ == "__main__":
    main()

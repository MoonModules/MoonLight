#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "mkdocs-material>=9.5",
#     "pymdown-extensions>=10",
# ]
# ///
"""Build (or serve) the MoonLight docs site with MkDocs Material.

Renders the docs/ tree as a navigable site, config in mkdocs.yml. Dependencies are
declared inline (PEP 723) so `uv run` provisions them — same pattern as the other
moondeck/docs/ tools and the uv-everywhere project rule; no requirements file.

Strict by default in CI: a broken link or a dead anchor fails the build. Out-of-tree source
links (../src/*.h, ../CLAUDE.md) are rewritten to GitHub URLs by mkdocs_hooks.py before
MkDocs sees them, so they raise no warning. A restructure once moved 170 files and left 153
dangling anchors that only a strict build surfaced; that is the failure this flag exists for.

The docs preview serves on :8422 (mkdocs' own default is :8000; we override it).
The three local dev servers use adjacent ports — MoonDeck :8420, installer
preview :8421, docs preview :8422 — so all three run at once without a port
clash. Override with --port.

Usage:
    uv run moondeck/docs/build_docs.py            # build to site/
    uv run moondeck/docs/build_docs.py --serve     # live-preview at :8422
    uv run moondeck/docs/build_docs.py --serve --port 9000   # serve on a custom port
    uv run moondeck/docs/build_docs.py --strict   # fail on any warning (the CI gate)
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CONFIG = ROOT / "mkdocs.yml"
# :8422 (mkdocs' own default is :8000) — adjacent to MoonDeck :8420 and the
# installer preview :8421, so all three local servers run at once (see docstring).
SERVE_PORT = 8422


def _kill_stray_serve() -> None:
    """Kill any already-running `mkdocs serve` before starting a new one.

    `mkdocs serve` is a long-running server bound to a fixed port; a leftover
    instance (from an earlier run left open, or a crashed session) keeps the
    port and makes a fresh serve fail or, worse, silently serve stale content.
    Self-clean on start so `--serve` is idempotent however it's launched
    (MoonDeck button or terminal). Only serve leaks a process — a plain
    `mkdocs build` exits — so this guards the serve path only. Windows uses
    taskkill; POSIX uses pkill, matching MoonDeck's _kill_process_by_name."""
    if sys.platform == "win32":
        # No cmdline match on Windows taskkill; skip (serve-on-Windows is rare
        # in this dev flow, and a bound port surfaces a clear "address in use").
        return
    # -f matches the full command line so it hits `mkdocs serve …` specifically,
    # not a one-shot `mkdocs build` (which has already exited anyway).
    subprocess.run(["pkill", "-f", "mkdocs serve"], capture_output=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the MoonLight docs site.")
    ap.add_argument("--strict", action="store_true",
                    help=("Promote EVERY mkdocs warning to fatal — a local anchor audit, NOT the "
                          "CI gate. The plain build is what CI runs; it already fails on a missing "
                          "page or broken nav. This additionally fails on the out-of-tree source "
                          "links and stale cross-doc anchors that mkdocs.yml deliberately sets to "
                          "`warn`, so it aborts and writes NO site until those are swept."))
    ap.add_argument("--serve", action="store_true",
                    help="Live-preview the site locally instead of building.")
    ap.add_argument("--port", type=int, default=SERVE_PORT,
                    help=f"Port for --serve (default: {SERVE_PORT}; installer preview owns 8421, MoonDeck 8420).")
    ap.add_argument("--site-dir", default=None,
                    help="Output directory for the built site (default: site/).")
    args = ap.parse_args()

    if args.serve:
        _kill_stray_serve()

    cmd = ["mkdocs", "serve" if args.serve else "build", "-f", str(CONFIG)]
    if args.serve:
        # Bind :8422 (overriding mkdocs' :8000 default) so the docs preview and
        # the installer preview (:8421) can run side by side.
        cmd += ["--dev-addr", f"localhost:{args.port}"]
    if args.strict:
        cmd.append("--strict")
    if args.site_dir and not args.serve:
        cmd += ["--site-dir", args.site_dir]

    # Tell the mkdocs hook to stage the installer into the site (local-preview
    # single-origin mirror). Serve only — in CI/plain build, release.yml owns
    # install/ staging (with the firmware binaries), so the hook must skip it.
    env = dict(os.environ)
    if args.serve:
        env["MM_DOCS_SERVE"] = "1"

    print(f"$ {' '.join(cmd)}")
    rc = subprocess.run(cmd, cwd=ROOT, env=env).returncode

    # Closing summary so the URL / next step is the last thing in the log,
    # not scrolled off. `serve` runs until interrupted, so this only prints
    # after a build (rc reached) or once a serve is stopped.
    if rc == 0 and not args.serve:
        site = args.site_dir or "site/"
        print()
        print(f"==> docs built to {site}")
        print(f"    preview locally: uv run moondeck/docs/build_docs.py --serve"
              f"  → http://localhost:{SERVE_PORT}/projectMM/")
        print("    deployed (after merge to main): https://moonmodules.org/projectMM/")
    elif args.serve:
        # A serve that just exited: remind where it was.
        print()
        print(f"==> docs preview was at http://localhost:{args.port}/projectMM/")
    return rc


if __name__ == "__main__":
    sys.exit(main())

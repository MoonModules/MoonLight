#!/usr/bin/env python3
"""Bundle and minify the web UI into the files the firmware serves.

`app.js` and the modules it imports become one minified `app.js`, and `style.css` is minified;
`index.html`, the vendored `prism.js` and the logo are copied as they are. Comments are most of
the source, so the embedded UI shrinks to about a third, and a page load sends a third of the
bytes through the render loop. The desktop running from a checkout still serves `src/ui` itself.

esbuild does the work: a real JavaScript parser, so minifying never changes what the code does.
The firmware builds need only Python, so this fetches esbuild's own platform binary from the npm
registry, the package esbuild's npm install itself downloads, pinned to one version and checked
against the registry's integrity hash, and keeps it under build/tools.

    python moondeck/build/bundle_ui.py <src/ui> <stage-dir>

Stdlib only, since the ESP32 build runs it with ESP-IDF's Python.
"""

import base64
import hashlib
import io
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
VERSION = "0.28.2"
# Each host's package and the integrity the registry publishes for it (`dist.integrity`).
PACKAGES = {
    ("Darwin", "arm64"): ("darwin-arm64", "sha512-n4KqkOQrraxHJcgjM1RvwbigfQKIKJVpM7xp+KsxiyUSrRdIXnt73VhrPAx0fV44hgfmIVKjxMN9J1t5jySVkw=="),
    ("Darwin", "x86_64"): ("darwin-x64", "sha512-uq6suIWYP37qzGddBKPw5QEQPi6HiLGsO7UmkpfyaYNQ3D+rN6w6WfwH+nuqcGXWvawGwxOEroO4YGnFh95azw=="),
    ("Linux", "x86_64"): ("linux-x64", "sha512-4xTZr1FUmSoQW4XIWmit3tzQrUTZM+N3P0XV8xROKYF50XfI7xeO90+1bZvNwxIufQ9hDQVRJH5YhgPVF8A/HQ=="),
    ("Linux", "aarch64"): ("linux-arm64", "sha512-pW4AC0P3it8c7do9MVM4p51FzHzdM/TZrerurgRcHJ2WTa1VQ1CIq18xncfpBJw4ojkiZZrKW2yIBWBP92j6Ug=="),
    ("Linux", "arm64"): ("linux-arm64", "sha512-pW4AC0P3it8c7do9MVM4p51FzHzdM/TZrerurgRcHJ2WTa1VQ1CIq18xncfpBJw4ojkiZZrKW2yIBWBP92j6Ug=="),
    ("Windows", "AMD64"): ("win32-x64", "sha512-5ebpxr3nWMzrL/rnUI755Jkuee0bHL/Gq0WTF9lvcpv73wAp5eu8MfBUgWK9bhWvZjj7yX8etf/8tI8Ney695g=="),
    ("Windows", "ARM64"): ("win32-arm64", "sha512-PIhhEkE9uPBleRBrQEJpUn7MBnibZzbGzYWPmY3x+YoVg/95zbjB4CxPPOQ8l5tYYM4mMaCthF8/1DIfBQQyWQ=="),
}
# What the firmware serves besides the bundle, copied as they are: the vendored file is minified upstream, the PNG compressed.
COPIED = ["index.html", "vendor/prism.js", "momo.png"]


def esbuild() -> Path:
    """The pinned esbuild for this host, downloaded and verified on first use."""
    key = (platform.system(), platform.machine())
    if key not in PACKAGES:
        sys.exit(f"bundle_ui: no esbuild package for {key[0]} {key[1]}; add it to PACKAGES")
    name, integrity = PACKAGES[key]
    exe = "esbuild.exe" if key[0] == "Windows" else "esbuild"
    target = ROOT / "build" / "tools" / f"esbuild-{VERSION}-{name}" / exe
    if target.exists():
        return target
    url = f"https://registry.npmjs.org/@esbuild/{name}/-/{name}-{VERSION}.tgz"
    with urllib.request.urlopen(url, timeout=60) as r:
        data = r.read()
    algo, expected = integrity.split("-", 1)
    if base64.b64encode(hashlib.new(algo, data).digest()).decode() != expected:
        sys.exit(f"bundle_ui: {url} does not match its pinned integrity")
    member = f"package/{exe}" if key[0] == "Windows" else "package/bin/esbuild"
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        binary = tar.extractfile(member).read()
    target.parent.mkdir(parents=True, exist_ok=True)
    # Written beside and renamed, so two builds fetching at once never run a half-written binary.
    part = target.with_suffix(".part")
    part.write_bytes(binary)
    part.chmod(0o755)
    os.replace(part, target)
    return target


def main() -> int:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ui, stage = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    stage.mkdir(parents=True, exist_ok=True)
    tool = str(esbuild())
    common = ["--minify", "--legal-comments=none", "--log-level=warning"]
    subprocess.run([tool, str(ui / "app.js"), "--bundle", "--format=esm", f"--outfile={stage / 'app.js'}", *common], check=True)
    subprocess.run([tool, str(ui / "style.css"), f"--outfile={stage / 'style.css'}", *common], check=True)
    for f in COPIED:
        shutil.copyfile(ui / f, stage / Path(f).name)
    return 0


if __name__ == "__main__":
    sys.exit(main())

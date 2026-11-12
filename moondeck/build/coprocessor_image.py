#!/usr/bin/env python3
"""Build the radio chip's firmware a variant with a WiFi co-processor carries: ESP-Hosted's `slave` project, for the chip the variant names.

build_esp32.py calls it after the app build, which fetched the `espressif/esp_hosted` component: the image is built from that component's own `slave/` project, so it matches the host side the app links on every version.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

import sdkconfig_fragments   # sibling: a changed fragment regenerates a build dir's sdkconfig

ROOT = Path(__file__).resolve().parent.parent.parent
COMPONENT = ROOT / "esp32" / "managed_components" / "espressif__esp_hosted"
# The slave project's output name, its `project()` in slave/CMakeLists.txt.
SLAVE_BIN = "network_adapter.bin"


def hosted_version(component: Path = COMPONENT) -> str:
    """The ESP-Hosted version the app links, from the fetched component's manifest; empty before an app build fetched it."""
    manifest = component / "idf_component.yml"
    if not manifest.exists():
        return ""
    m = re.search(r"^version:\s*['\"]?([0-9][0-9.]*)", manifest.read_text(), re.MULTILINE)
    return m.group(1) if m else ""


def asset_name(chip: str, version: str) -> str:
    """The release asset's name, `c6-slave-2.12.13.bin` for an esp32c6: the version is what a device asks for."""
    return f"{chip.removeprefix('esp32')}-slave-{version}.bin"


def build_coprocessor(cmd: list[str], env: dict, chip: str) -> Path | None:
    """Build the slave image for `chip` into build/coprocessor-<chip>/, named for its release asset; None when no app build fetched the component."""
    version = hosted_version()
    if not version:
        print("No esp_hosted component fetched: build a variant with a WiFi co-processor first.")
        return None
    build_dir = ROOT / "build" / f"coprocessor-{chip}"
    # A copy, since building in place writes the project's lock file and managed components into a fetched component, which the component manager then reports as modified.
    src = build_dir / "src"
    stamp = build_dir / "src.version"
    if not src.exists() or not stamp.exists() or stamp.read_text() != version:
        shutil.rmtree(build_dir, ignore_errors=True)
        shutil.copytree(COMPONENT / "slave", src)
        build_dir.mkdir(parents=True, exist_ok=True)
        stamp.write_text(version)
    out = build_dir / "idf"
    b_arg = ["-B", str(out), f"-DSDKCONFIG={out}/sdkconfig"]
    fragments = [f for f in (src / "sdkconfig.defaults", src / f"sdkconfig.defaults.{chip}") if f.exists()]
    digest = sdkconfig_fragments.refresh(cmd + b_arg, out, fragments, src, env)
    if not out.exists():
        print(f"Setting the radio chip's target to {chip}...")
        r = subprocess.run(cmd + b_arg + ["set-target", chip], cwd=src, env=env)
        if r.returncode != 0:
            sys.exit(r.returncode)
    print(f"Building the {chip} radio chip firmware, ESP-Hosted {version}...")
    r = subprocess.run(cmd + b_arg + ["build"], cwd=src, env=env)
    if r.returncode != 0:
        sys.exit(r.returncode)
    sdkconfig_fragments.record(out, digest)
    image = build_dir / asset_name(chip, version)
    shutil.copy(out / SLAVE_BIN, image)
    print(f"Radio chip image: {image.name}, {image.stat().st_size / 1024:.0f} KB")
    return image

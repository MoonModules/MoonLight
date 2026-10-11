#!/usr/bin/env python3
# /// script
# dependencies = ["truststore"]
# ///
"""Install the Teensy 4.1 SDK at its pinned versions into ~/teensy (TEENSY_SDK overrides).

Four parts, each from its publisher at a pinned version:
PJRC's Teensy 4 core, LittleFS and SPI, and QNEthernet, from GitHub,
Arm's GNU toolchain for bare-metal Arm (checksum-verified),
and teensy_loader_cli, compiled from its tagged source on macOS and Linux.

Idempotent: a part already at its pin is left alone, so a re-run after a pin bump fetches only what changed.
build_teensy.py reads the pins and paths from here, so every version has this one home.
"""

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

# PJRC's sources: the core at Teensyduino's release tag, the libraries (untagged) at the commit that release ships.
TEENSYDUINO = "1.62"
REPOS = {
    "cores":    ("https://github.com/PaulStoffregen/cores.git", "a664effb008d1ac8d8f00f3f19b47c0d1ea46e3b"),
    "LittleFS": ("https://github.com/PaulStoffregen/LittleFS.git", "8c2badab74a01795931ea14adc85ec6da3db235f"),
    "SPI":      ("https://github.com/PaulStoffregen/SPI.git", "7c83d0726746b652af37319e70cd3932c253ecae"),
    # lwIP and the W5500 driver (AGPL-3.0-or-later), at release v0.37.0.
    "QNEthernet": ("https://github.com/ssilverman/QNEthernet.git", "9b6018d81d91a86c6f04fa7814fb8884ed8ef128"),
}

GCC_VERSION = "15.3.rel2"
GCC_URL = ("https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/"
           f"gnu-toolchain/{GCC_VERSION}/")
# Arm publishes these four hosts; the checksums are Arm's own .sha256asc files.
GCC_HOSTS = {
    ("Darwin", "arm64"):   ("darwin-arm64", "tar.xz", "b6ccf805aa4fb8c50406256f99cf802385894b86c0cb6c0185dc0e2b5f8288b9"),
    ("Linux", "x86_64"):   ("x86_64", "tar.xz", "0662b2f01e0cd8b951fd110911d339c35b05998c68a12b3530bbbb4bf6c7874e"),
    ("Linux", "aarch64"):  ("aarch64", "tar.xz", "9fc9e6baedf2b6779bd39ce4cc458d3529e200bc92bebe28be18f8942a456ad8"),
    ("Windows", "AMD64"):  ("mingw-w64-x86_64", "zip", "062888d4f8a213f930e367189804910e5215c4e623e5063e64494c302f94b889"),
}

LOADER_VERSION = "2.3"
LOADER_REPO = ("https://github.com/PaulStoffregen/teensy_loader_cli.git", "03fca4156c244c7ad36bd368cf6e24531dbd566a")
# One C file; its Makefile assumes a long-gone macOS SDK path, so it is compiled directly.
LOADER_BUILD = {
    "Darwin": ["-DUSE_APPLE_IOKIT", "-framework", "IOKit", "-framework", "CoreFoundation"],
    "Linux":  ["-DUSE_LIBUSB", "-lusb"],
}


def sdk_root() -> Path:
    return Path(os.environ.get("TEENSY_SDK", Path.home() / "teensy"))


def teensyduino_define() -> int:
    """The TEENSYDUINO value the core's sources expect, 162 for 1.62."""
    major, minor = TEENSYDUINO.split(".")
    return int(major) * 100 + int(minor)


def gcc_host():
    host = GCC_HOSTS.get((platform.system(), platform.machine()))
    if not host:
        sys.exit(f"Arm publishes no {GCC_VERSION} toolchain for {platform.system()} {platform.machine()}")
    return host


def gcc_bin() -> Path:
    return sdk_root() / f"arm-gnu-toolchain-{GCC_VERSION}-{gcc_host()[0]}-arm-none-eabi" / "bin"


def loader_path() -> Path:
    return sdk_root() / "teensy_loader_cli" / "teensy_loader_cli"


def head(repo: Path) -> str:
    r = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def missing() -> list:
    """The parts not installed at their pin, empty when the SDK is ready to build and flash."""
    out = [name for name, (_, pin) in REPOS.items() if head(sdk_root() / name) != pin]
    out += [p.name for name in REPOS for p in patches_for(name) if not patch_applied(sdk_root() / name, p)]
    if not (gcc_bin() / "arm-none-eabi-g++").exists() and not (gcc_bin() / "arm-none-eabi-g++.exe").exists():
        out.append(f"arm-gnu-toolchain {GCC_VERSION}")
    if platform.system() in LOADER_BUILD and not loader_path().exists():
        out.append("teensy_loader_cli")
    return out


def git_at(url: str, ref: str, dest: Path):
    if not (dest / ".git").exists():
        subprocess.run(["git", "clone", "--filter=blob:none", url, str(dest)], check=True)
    if head(dest) == ref:
        return
    subprocess.run(["git", "-C", str(dest), "fetch", "--tags", "origin"], check=True)
    # Forced, since the tree carries our patches, which apply_patches puts back on the new pin.
    subprocess.run(["git", "-C", str(dest), "checkout", "--force", "--detach", ref], check=True)


def install_gcc():
    if (gcc_bin() / "arm-none-eabi-g++").exists() or (gcc_bin() / "arm-none-eabi-g++.exe").exists():
        return
    host, ext, sha = gcc_host()
    name = f"arm-gnu-toolchain-{GCC_VERSION}-{host}-arm-none-eabi.{ext}"
    archive = sdk_root() / name
    # The system's certificate store, as curl and git use, so a network with its own root certificate downloads too.
    import truststore
    truststore.inject_into_ssl()
    print(f"Downloading {name} ...")
    with urllib.request.urlopen(GCC_URL + name) as resp, open(archive, "wb") as f:
        shutil.copyfileobj(resp, f)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != sha:
        archive.unlink()
        sys.exit(f"{name}: checksum {digest} does not match Arm's {sha}")
    print("Unpacking ...")
    if ext == "zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(sdk_root())
    else:
        with tarfile.open(archive) as t:
            t.extractall(sdk_root(), filter="tar")
    archive.unlink()


# Our changes to an upstream part, one file per change under teensy/patches/, named after the part they apply to.
PATCH_DIR = Path(__file__).resolve().parent.parent.parent / "teensy" / "patches"


def patches_for(part: str) -> list:
    return sorted(PATCH_DIR.glob(f"{part}-*.patch"))


def patch_applied(repo: Path, patch: Path) -> bool:
    """Whether the patch is already in the working tree: its reverse applies cleanly."""
    return subprocess.run(["git", "-C", str(repo), "apply", "--reverse", "--check", str(patch)],
                          capture_output=True).returncode == 0


def apply_patches(part: str, repo: Path) -> bool:
    """Apply the part's patches when its tree lacks any; True when they were applied."""
    patches = patches_for(part)
    if all(patch_applied(repo, patch) for patch in patches):
        return False
    # An edited patch meets the tree its earlier version changed, so every patch starts from the pinned tree.
    subprocess.run(["git", "-C", str(repo), "checkout", "--force", "."], check=True)
    for patch in patches:
        if subprocess.run(["git", "-C", str(repo), "apply", str(patch)]).returncode != 0:
            sys.exit(f"{patch.name} does not apply to {part} at its pin; update the patch with the pin")
        print(f"Applied {patch.name}")
    return True


def install_loader():
    flags = LOADER_BUILD.get(platform.system())
    if not flags:
        print("teensy_loader_cli: no source build for this host; flash with PJRC's Teensy app instead.")
        return
    src = sdk_root() / "teensy_loader_cli"
    git_at(LOADER_REPO[0], LOADER_REPO[1], src)
    patched = apply_patches("teensy_loader_cli", src)
    if loader_path().exists() and not patched:
        return
    r = subprocess.run(["cc", "-O2", "-Wall", "teensy_loader_cli.c", "-o", "teensy_loader_cli", *flags], cwd=src)
    if r.returncode != 0:
        hint = " (install libusb-dev, the libusb 0.1 headers)" if platform.system() == "Linux" else ""
        sys.exit(f"teensy_loader_cli did not compile{hint}")


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    sdk_root().mkdir(parents=True, exist_ok=True)
    for name, (url, pin) in REPOS.items():
        git_at(url, pin, sdk_root() / name)
        apply_patches(name, sdk_root() / name)
    install_gcc()
    install_loader()
    left = missing()
    if left:
        sys.exit(f"Still missing: {', '.join(left)}")
    print(f"Teensy SDK ready in {sdk_root()}: Teensyduino {TEENSYDUINO} sources, Arm GCC {GCC_VERSION}, teensy_loader_cli {LOADER_VERSION}")


if __name__ == "__main__":
    main()

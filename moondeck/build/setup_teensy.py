#!/usr/bin/env python3
# /// script
# dependencies = ["truststore"]
# ///
"""Install the Teensy 4.1 SDK at its pinned versions into ~/teensy (TEENSY_SDK overrides).

Four parts, each from its publisher at a pinned version:
PJRC's Teensy 4 core, LittleFS, SPI, SD and SdFat, and QNEthernet, from GitHub,
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
import tempfile
import urllib.request
import zipfile
from pathlib import Path

# PJRC's sources: the core at Teensyduino's release tag, the libraries (untagged) at the commit that release ships.
TEENSYDUINO = "1.62"
REPOS = {
    "cores":    ("https://github.com/PaulStoffregen/cores.git", "a664effb008d1ac8d8f00f3f19b47c0d1ea46e3b"),
    "LittleFS": ("https://github.com/PaulStoffregen/LittleFS.git", "8c2badab74a01795931ea14adc85ec6da3db235f"),
    "SPI":      ("https://github.com/PaulStoffregen/SPI.git", "7c83d0726746b652af37319e70cd3932c253ecae"),
    # The built-in microSD socket: the SD library over PJRC's SdFat, whose SDIO driver runs the 4.1's own slot.
    "SD":       ("https://github.com/PaulStoffregen/SD.git", "27a6aabceed533ea7169137e625ff35a8de91803"),
    "SdFat":    ("https://github.com/PaulStoffregen/SdFat.git", "1e8c3320c293e34865dcdae1a9e6eb0b406c6193"),
    # lwIP and the W5500 driver (AGPL-3.0-or-later), at release v0.37.0.
    "QNEthernet": ("https://github.com/ssilverman/QNEthernet.git", "9b6018d81d91a86c6f04fa7814fb8884ed8ef128"),
}

GCC_VERSION = "15.3.rel2"
GCC_URL = ("https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/"
           f"gnu-toolchain/{GCC_VERSION}/")
# Arm publishes these four hosts; the checksums are Arm's own .sha256asc files, as Arm re-published the packages on 2026-10-12.
GCC_HOSTS = {
    ("Darwin", "arm64"):   ("darwin-arm64", "tar.xz", "f011e4832046155f06408dda6b252b21b139a8159c8529802227371f06656c60"),
    ("Linux", "x86_64"):   ("x86_64", "tar.xz", "463df9665c246fa13bab2102115bbf83440661b6eb70b44f3dfd4712ca30037a"),
    ("Linux", "aarch64"):  ("aarch64", "tar.xz", "a0dc18893171d39dec82fd8d57eeab3da836fd3df4b61978db3eccbf4a6bb948"),
    ("Windows", "AMD64"):  ("mingw-w64-x86_64", "zip", "0daff4141d4f62ad1e9794ca8bd430ef730f07fd78f6d71d2ffdffc21db8cb9e"),
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


def missing(loader: bool = True) -> list:
    """The parts not installed at their pin, empty when the SDK is ready to build, and to flash when `loader` is set."""
    out = [name for name, (_, pin) in REPOS.items() if head(sdk_root() / name) != pin]
    out += [f"{name} patches" for name in REPOS if not patches_applied(sdk_root() / name, patches_for(name))]
    if not (gcc_bin() / "arm-none-eabi-g++").exists() and not (gcc_bin() / "arm-none-eabi-g++.exe").exists():
        out.append(f"arm-gnu-toolchain {GCC_VERSION}")
    if loader and platform.system() in LOADER_BUILD and not loader_path().exists():
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
        # A mismatch that repeats is Arm re-publishing the package: its .sha256asc beside the archive says which checksum to pin.
        sys.exit(f"{name}: checksum {digest} does not match the pinned {sha}; compare Arm's {GCC_URL}{name}.sha256asc")
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


def patches_applied(repo: Path, patches: list) -> bool:
    """Whether the working tree is its pin with the patches applied in order, built in a scratch index so two patches touching neighboring lines check as one stack. A part we do not patch counts as applied, whatever is edited in it."""
    if not patches:
        return True
    with tempfile.TemporaryDirectory() as tmp:
        env = {**os.environ, "GIT_INDEX_FILE": str(Path(tmp) / "index")}

        def git(*args) -> bool:
            return subprocess.run(["git", "-C", str(repo), *args], env=env, capture_output=True).returncode == 0

        return git("read-tree", "HEAD") and all(git("apply", "--cached", str(p)) for p in patches) and git("diff", "--quiet")


def apply_patches(part: str, repo: Path) -> bool:
    """Put the part's tree back at its pin with its patches applied in order, when it differs from that; True when they were applied."""
    patches = patches_for(part)
    if patches_applied(repo, patches):
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-loader", action="store_true", help="skip teensy_loader_cli, for a machine that builds but never flashes (CI)")
    args = parser.parse_args()
    sdk_root().mkdir(parents=True, exist_ok=True)
    for name, (url, pin) in REPOS.items():
        git_at(url, pin, sdk_root() / name)
        apply_patches(name, sdk_root() / name)
    install_gcc()
    if not args.no_loader:
        install_loader()
    left = missing(loader=not args.no_loader)
    if left:
        sys.exit(f"Still missing: {', '.join(left)}")
    print(f"Teensy SDK ready in {sdk_root()}: Teensyduino {TEENSYDUINO} sources, Arm GCC {GCC_VERSION}, teensy_loader_cli {LOADER_VERSION}")


if __name__ == "__main__":
    main()

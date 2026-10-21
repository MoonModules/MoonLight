# Running MoonLight on a Linux machine

MoonLight runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as on a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether a server, a Raspberry Pi or a NanoPi.

Deploying is covered here. Building and developing on Linux are [MoonWalk's contributor processes](../moonwalk.md#desktop-build-test).

> Windows, with screenshots: [Install MoonLight](../moonwalk.md#install). Flashing a board: the same process with ESP32 picked.

## Which route applies to your machine

Check the CPU first:

```sh
uname -m
```

| `uname -m` says | Machine | Route |
|---|---|---|
| `x86_64` | Intel or AMD PC, server or VM | Install the package |
| `aarch64` | arm64 board: Pi, NanoPi, most SBCs | Install the package |

Both architectures get a released binary, so the route below is the same one and only the filename differs. Building from source is still there for a distribution the package does not suit, and for developing. A Pi 4 or 5 has ample headroom. A NanoPi R28S has two Gigabit ports, so it can sit between the house network and the lighting network, and 1 GB of RAM, which runs MoonLight comfortably. Everything here assumes a Debian-based system (Debian, Ubuntu, Raspberry Pi OS, Armbian); on another distribution, translate the package names.

> `x64` and `amd64` are two names for the same thing. `arm64` is different machine code.

## Install the package

Install the `.deb` as [Install MoonLight](../moonwalk.md#install) describes with Linux picked: its step holds the two lines that fetch the right file over ssh on a headless board. The package needs glibc 2.35, so Raspberry Pi OS Bookworm, Debian 12 and 13, and Ubuntu 22.04 and later. apt refuses a package for the wrong architecture by name rather than installing something that cannot run. Then open `http://<machine>:8080`. On something older than that floor, build from source below.

## Build from source

For a distribution the package does not suit, an older glibc, or to develop on the board. Steps: [Prepare a Linux board for MoonLight](../moonwalk.md#prepare-linux-board), which ends in building and running it.

## Keeping it running after a reboot

For the package or a source build. A container does this with `--restart unless-stopped` instead, see [Docker](#docker).

Steps: [Keep MoonLight running on a Linux board](../moonwalk.md#run-linux-board), which holds the systemd unit.

## Shutting down

Shut down cleanly, since an SD card interrupted mid-write can corrupt the filesystem: [Keep MoonLight running on a Linux board](../moonwalk.md#run-linux-board).

MoonLight writes to disk only when settings change, so the card is a fine home for it. The risk is the operating system's own writes.

## Docker

The same program, installed as a container rather than a package: start it as [Install MoonLight](../moonwalk.md#install) describes with Docker picked. One published image carries amd64 and arm64, so a board and a server pull the same tag and each gets native instructions. `--restart unless-stopped` brings it back after a reboot, so Docker replaces the systemd unit above rather than needing one of its own.

**Use `--network host`.** The web UI and unicast fixture output work through ordinary port mapping (`-p 8080:8080`). mDNS discovery and Art-Net's broadcast mode are multicast, and neither crosses a bridge network. Host networking is what lets a container find boards and be found by them. Verified on a NanoPi R28S driving a ColorLight card.

**The volume is the configuration.** `/data` holds everything you set up; without it, every `docker rm` starts you over.

`:latest` follows the rolling prerelease, the same build the installer offers; a version tag such as `:6.0.0` pins one. The release workflow builds each image from the `.deb` that release ships, so the image and the package are the same build.

| | |
|---|---|
| **Config** | `/data/MoonLight/.config/` in the volume (`XDG_DATA_HOME=/data`) |
| **Identity** | `/data/MoonLight/.config/identity`, written on the first run |
| **Logs** | standard output, so `docker logs` |
| **Output** | Art-Net UDP 6454, DDP 4048, E1.31 5568, all outbound |
| **Capabilities** | none: it binds its port as an ordinary process |

**Several instances** run side by side: each container has its own ports and writes its own identity, so they are told apart on the network. CPU is the limit rather than memory, about one core each, since the desktop build renders as fast as it may; cap it with `cpus:` when running a fleet.

### From a source checkout, with Compose

The repository's `docker-compose.yml` builds the image from the checkout and publishes the interface on port 8081, so it never meets a native install on 8080. Steps: [Install MoonLight](../moonwalk.md#install) with Docker picked, under running it from a source checkout.

### When the container exits at once

**`failed to mount ... fstype: overlay ... invalid argument`** means the machine's root filesystem is itself an overlay, which several board and NAS systems use, and Docker's default `overlay2` driver cannot stack one on another. Check with `findmnt -no FSTYPE /`; if it says `overlay`, switch drivers:

```sh
sudo mkdir -p /etc/docker
# Add the key to whatever is already there. A plain `tee` would discard existing settings, and a
# NAS or a board image often ships some.
sudo sh -c 'f=/etc/docker/daemon.json; [ -s "$f" ] || echo "{}" > "$f"; \
  tmp=$(mktemp); python3 -c "import json,sys;d=json.load(open(sys.argv[1]));d[\"storage-driver\"]=\"vfs\";json.dump(d,open(sys.argv[2],\"w\"),indent=2)" "$f" "$tmp" && mv "$tmp" "$f"'
sudo systemctl restart docker
```

`vfs` copies layers instead of sharing them, so it uses more disk and pulls are slower. `fuse-overlayfs` is the faster alternative where the package exists.

## Package or container: which to pick

| | Package (`.deb`) | Docker |
|---|---|---|
| Install | `apt install ./moonlight_*.deb` | one `docker run` |
| Updates | download the new `.deb` | `docker pull` and recreate |
| Survives a reboot | needs a systemd unit | `--restart unless-stopped` |
| Runs on | Debian family, glibc 2.35+ | any Linux with Docker |
| Disk | the binary plus libraries the system already has | an image carrying its own libraries |
| Isolation | none: an ordinary program | its own filesystem and process space |
| Upgrades cleanly | apt handles dependencies | the image carries its own |
| Debugging | logs in the terminal, files on disk | `docker logs`, and no shell inside the image |

**Take the package** on a board you own and administer: it is smaller, it starts faster, and its files are where you expect them.

**Take Docker** where you would rather not install anything permanent, where the machine already runs containers, or where the distribution is too old for the package's glibc floor. A container carries its own libraries, so the host's age stops mattering.

## Where else this runs

Docker widens the target from "a Debian-family board" to "anything that runs containers", which is a much larger set:

- **Raspberry Pi** (3, 4, 5, and Zero 2 W): arm64, so the same image and the same commands as a NanoPi. Nothing here is NanoPi-specific.
- **A NAS**: Synology (Container Manager), QNAP (Container Station), Unraid, TrueNAS. Give it host networking, or mDNS and Art-Net broadcast will not leave the box.
- **A mini PC or home server**: amd64, the largest and least surprising target.
- **A router or firewall appliance** running OpenWrt or OPNsense with Docker: plausible where the device has the RAM, and the overlay-root note above is likely to apply.
- **Kubernetes**: one pod, one volume. Multicast needs an L2 CNI rather than the default bridge.

The realistic limits are architecture and memory, not the kind of device. It needs 64-bit, arm64 or amd64, because no 32-bit image is published. Memory is rarely the binding constraint: a container rendering a 16x16 grid measures 4.5 MB on a NanoPi. A large installation grows that by its buffers rather than by a fixed overhead. A coffee machine is not out of the question if it meets both and runs Docker, though the fixtures would have to come to it.

## Where to go next

- [Install MoonLight](../moonwalk.md#install): the same program on an ESP32.
- [How MoonLight works](../explanation/how-moonlight-works.md): layouts, layers, effects and drivers.
- [Build, run and test on the desktop](../moonwalk.md#desktop-build-test): building, testing and packaging from source.

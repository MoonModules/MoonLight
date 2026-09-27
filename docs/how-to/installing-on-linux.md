# Running MoonLight on a Linux machine

MoonLight runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as on a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether a server, a Raspberry Pi or a NanoPi.

Deploying is covered here. Building and developing on Linux is in [building.md](../how-to/building.md).

> Windows, with screenshots: [Installing MoonLight on a desktop](installing-to-desktop.md). Flashing a board: [Install & first light](../gettingstarted.md).

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

The [releases page](https://github.com/MoonModules/projectMM/releases/latest) carries one `.deb` per architecture: `_amd64.deb` for `x86_64`, `_arm64.deb` for `aarch64`.

On a machine with a browser, download it and install:

```sh
sudo apt install ./projectmm_X.Y.Z_arm64.deb
MoonLight
```

On a headless board, fetch it over ssh instead. This picks the right file for the architecture it runs on, so the same two lines work on a Pi, a NanoPi and a server:

```sh
arch=$(dpkg --print-architecture)
url=$(curl -fsSL https://api.github.com/repos/MoonModules/projectMM/releases/tags/latest \
      | grep -o "https://[^\"]*_${arch}\.deb" | head -1)
curl -fsSL -o projectmm.deb "$url" && sudo apt install -y ./projectmm.deb
MoonLight
```

`latest` is the rolling build from `main`, which is what the web installer offers too. For the newest tagged release, replace `tags/latest` with `latest` in that URL.

Open `http://<machine>:8080`. A `.tar.gz` to unpack anywhere is on the same page.

**A package built for the wrong architecture refuses to install**, which is the failure you want: `apt` rejects it by name rather than installing something that cannot run.

The arm64 build targets glibc 2.35, so it installs on Raspberry Pi OS Bookworm, Debian 12 and 13, Ubuntu 22.04 and later. On something older, build from source below.

## Build from source

For a distribution the package does not suit, an older glibc, or to develop on the board. Allow an hour the first time, most of it waiting.

### 1. Write an OS image to the SD card

Take a Debian-based image. For a Raspberry Pi, [Raspberry Pi OS Lite](https://www.raspberrypi.com/software/): the desktop build leaves less memory for the compile. For a NanoPi, the Debian image from the board's [FriendlyELEC wiki page](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R28S#Flashing_the_OS_to_the_microSD_card), under `01_Official images/01_SD card images`. Skip FriendlyWrt: it is router firmware with a different package manager. [Armbian](https://www.armbian.com/download/) covers many boards from one project, if it lists yours.

Use an 8 GB card or larger. Write it with [Raspberry Pi Imager](https://www.raspberrypi.com/software/) or [balenaEtcher](https://etcher.balena.io/); both take the compressed download directly.

On a Raspberry Pi, open Imager's settings before writing: set the username, hostname and WiFi, and enable SSH. Raspberry Pi OS ships with SSH off and no default user, so a card written without those boots to a machine you cannot reach.

### 2. First boot

Insert the card, connect the network cable, power on. Give it 10 to 20 minutes: the first boot resizes the filesystem and may reboot itself.

Find it on the network:

```sh
ping raspberrypi.local          # or NanoPi-R28S.local
arp -a                          # everything the network has seen
```

Your router's client list is the fallback when mDNS does not resolve. A name that never resolves means the board has no mDNS responder: Raspberry Pi OS ships one, a minimal Debian or FriendlyELEC image often does not, so `.local` fails while the IP answers. Log in by IP and fix it below.

### 3. Log in

```sh
ssh <you>@<hostname>.local      # Raspberry Pi OS: the user you set in Imager
ssh pi@NanoPi-R28S              # FriendlyELEC Debian: user pi, password pi
ssh pi@192.168.1.156            # by address, when neither name resolves
```

**A wall of `setlocale: LC_CTYPE: cannot change locale (UTF-8)` warnings on login is harmless.** macOS sends its own `LC_CTYPE` to the board, which has no locale by that name, and bash repeats the warning per startup file. It fires before anything you run. Silence it on the board:

```sh
sudo apt install -y locales && sudo locale-gen en_US.UTF-8
```

Change a default password at once:

```sh
passwd
```

**If `.local` did not resolve**, install the mDNS responder now and the name works from the next boot:

```sh
sudo apt install -y avahi-daemon
```

Without network access, attach a keyboard and monitor and configure it there: `sudo nmtui` for WiFi and addresses, `ip ad` to see what the board has. On a NanoPi, `sudo nmtui` also configures the second port.

### 4. Update the system

```sh
sudo apt update
sudo apt upgrade -y
```

### 5. Install the prerequisites

```sh
sudo apt install -y python3-pip cmake build-essential git
pip install uv --break-system-packages
```

`--break-system-packages` is routine on Debian 12 and later, where the system Python is marked externally managed; it affects pip's own environment only.

If `uv` is then not found, add its directory to your path:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

### 6. Build and run

```sh
git clone https://github.com/MoonModules/projectMM.git
cd MoonLight
uv run moondeck/build/build_desktop.py
uv run moondeck/run/run_desktop.py
```

The build takes a few minutes on a Pi 4 or a NanoPi, longer on older boards. `run_desktop.py` detaches, so the program outlives the ssh session. Open `http://<board>:8080`.

That is a running system. Everything below is optional.

> A board with 1 GB of RAM or less can run out of memory while compiling; the compiler is killed rather than reporting an error. Add swap: edit `/etc/dphys-swapfile` to raise `CONF_SWAPSIZE`, then `sudo dphys-swapfile swapoff && sudo dphys-swapfile setup && sudo dphys-swapfile swapon`. Editing alone changes nothing; `setup` regenerates the file.

## Keeping it running after a reboot

For the package or a source build. A container does this with `--restart unless-stopped` instead, see [Docker](#docker).

Give it a systemd unit at `/etc/systemd/system/projectmm.service`:

```ini
[Unit]
Description=MoonLight
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/home/pi/MoonLight/build/MoonLight
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now projectmm
systemctl status projectmm
```

`Restart=always` covers a crash as well as a reboot. Adjust `User` and the path: `/usr/bin/MoonLight` for the package, or where you built for a source build.

## Shutting down

Shut down cleanly; an SD card interrupted mid-write can corrupt the filesystem:

```sh
sudo shutdown now     # or: sudo reboot
```

MoonLight writes to disk only when settings change, so the card is a fine home for it. The risk is the operating system's own writes.

## Docker

The same program, installed as a container rather than a package. One published image carries amd64 and arm64, so a board and a server pull the same tag and each gets native instructions.

```sh
docker run -d --name projectmm --network host \
  -v projectmm-data:/data --restart unless-stopped \
  ghcr.io/moonmodules/projectmm:latest
```

Open `http://<machine>:8080`. `--restart unless-stopped` is what brings it back after a reboot, so Docker replaces the systemd unit above rather than needing one of its own.

**Use `--network host`.** The web UI and unicast fixture output work through ordinary port mapping (`-p 8080:8080`), but mDNS discovery is multicast and Art-Net's broadcast mode is too, and neither crosses a bridge network. Host networking is what lets a container find boards and be found by them. Verified on a NanoPi R28S driving a ColorLight card.

**The volume is the configuration.** `/data` holds everything you set up; without it, every `docker rm` starts you over.

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
| Install | `apt install ./projectmm_*.deb` | one `docker run` |
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

The realistic limits are architecture and memory, not the kind of device. It needs 64-bit, arm64 or amd64, because no 32-bit image is published. Memory is rarely the binding constraint: a container rendering a 16x16 grid measures 4.5 MB on a NanoPi, and a large installation grows that by its buffers rather than by a fixed overhead. A coffee machine is not out of the question if it meets both and runs Docker, though the fixtures would have to come to it.

## Where to go next

- [Install & first light](../gettingstarted.md): the same program on an ESP32.
- [How MoonLight works](../tutorials/how-projectmm-works.md): layouts, layers, effects and drivers.
- [building.md](../how-to/building.md): building, testing and packaging in depth.

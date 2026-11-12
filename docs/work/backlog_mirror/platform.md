# Backlog: Platform

Generated from [the Project](https://github.com/users/MoonModules/projects/5) by `moondeck/report/project_mirror.py`: edit the cards, not this file.

## Across modules

### SIMD inner loops for noise and the bilinear lerp on the S3

Kind: Feature · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287083794)

The noise inner loop and the bilinear resampler are the per-pixel cost of every field and transport effect. PIE variants behind `src/platform/`, bit-identical to the scalar forms by test, and the FPU `curl16` where `hasFpu`, measured through the KPI gate.

### SIMD inner loops for noise and the bilinear lerp on the P4

Kind: Feature · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287083858)

The same PIE variants and FPU `curl16` as on the S3, measured on the P4 through the KPI gate. The P4's PPA fits `upscale16`, a scaled blit, and nothing in advection, which displaces every pixel by its own velocity.

### Network Receive on a P4 past 3,000 packets a second (2026-10-26)

Kind: Feature · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287083932)

Below about 3,000 packets a second a P4 receives every frame; past it packets go, more for Art-Net than for DDP, as [the table and where a packet's time goes](../../reference/performance.md#network-receive) show.
Network Receive's task spends tens of microseconds per socket call, a `select` hundreds.
Both network tasks may run on either core, yet stay on core 1 with core 0 idle.
Two levers, each its own change: the network tasks on core 0, where pinning them by `ETH_MAC_FLAG_PIN_TO_CORE` and lwIP's task affinity kept Ethernet from coming up, so that needs its own look first; and lwIP's raw `udp_recv` callback writing into staging from lwIP's task, with no socket, no `select` and no receive task.

### The `.deb` ships the systemd unit, raw Ethernet included (2026-10-22, from robwomp on Discord)

Kind: Feature · Platform: Pi · Stage: Install · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287083996)

The package holds only the binary, so a Pi or NanoPi user writes `/etc/systemd/system/moonlight.service` by hand from MoonWalk's "Run at boot" step. That unit grants no `CAP_NET_RAW`, so the panel card driver's `AF_PACKET` socket fails unless MoonLight runs as root, and the `setcap` alternative is lost whenever apt replaces `/usr/bin/MoonLight`. `CAP_NET_RAW` is all MoonLight needs, since it sets no interface flags. The fix: `package_desktop.py` installs the unit with `AmbientCapabilities=CAP_NET_RAW` and a dedicated `moonlight` user, so installing is `apt install` plus `systemctl enable --now moonlight`. MoonWalk's unit step shrinks to that enable, and the `setcap` step stays for a binary run by hand.

### An HTTPS request on a board without PSRAM (2026-10-24, from the JSON plan)

Kind: Investigation · Platform: classic · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084099)

A board without PSRAM never fetches over HTTPS: `tlsRoom` asks for the 100 KB internal floor plus a 45 KB TLS session, and a classic ESP32 has 55 KB free at idle. `CONFIG_MBEDTLS_DYNAMIC_BUFFER` allocates mbedTLS's buffers as a connection needs them rather than the worst case up front, which may fit one request on an idle board. To measure on `MM-Olimex` (.207): the peak a session takes with it, what a smaller floor costs WiFi and the HTTP server, and whether a feed then fetches with lights running. Until then such a board gets its data pushed from one with PSRAM.

### A StadBeest eye reset once by the task watchdog (2026-10-24)

Kind: Investigation · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084185)

On 2026-10-23 eye2 (`MM-Eye2`, an ESP32-S3-Zero) reset once by the task watchdog, shortly after its OSC service was added, with no USB attached to read the panic. It has not recurred: both eyes ran the crossfading autopilot without a reset afterwards, and on 2026-10-24 they report `POWERON` after the beast was switched on. Recorded so a recurrence is recognized: `bootReason` on the System card is the one reading the network gives, since the board keeps no crash record; the panic itself needs the eye on USB with `monitor_esp32.py`.

### What the WiFi buffer copy costs (2026-10-24)

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084278)

`CONFIG_LWIP_L2_TO_L3_COPY` copies every received frame so a socket the blocked loop stops draining cannot hold the 32 WiFi receive buffers; without it, every firmware upload stalled at 150 to 860 KB. Its cost is unmeasured: the frame rate on a busy DDP stream with and without it, same scene. If it costs much, firmware uploads move onto the transfer slots so nothing blocks the loop, and the copy goes.

### An update's connection that would not open until a reset (2026-10-22, from the internal RAM plan)

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084374)

On 3.0.0 a board could not open the update's connection (`ESP_ERR_HTTP_CONNECT`) until it was reset, with 80 KB of internal RAM free. TLS has lived in PSRAM since then (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), which may be what cured it; it has not recurred. Recorded so a recurrence is recognized: the free internal RAM and largest block at that moment are the first readings to take.

### Move more devices to MoonLight over WiFi (2026-10-09)

Kind: Feature · Stage: Install · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084435)

The migration image (`moonbase/migrate/`) moves a board running WLED to MoonLight through WLED's own update page, with no cable: verified on WLED 0.15.0 on an ESP32-C3, on a bench board with a power cut at every step and on an Athom bulb.
An image is built for one chip and carries one variant's partition table, so each further reach is its own piece:
- **Every MoonBase variant:** `migrate: True` on `esp32`, `esp32-16mb`, `esp32-wrover`, `esp32-pico` and `esp32s3-zero`, each rehearsed on a bench board of that chip and shipped as a release asset. The variants without MoonBase (the larger S3s, the P4) need another design, the image writing the app itself.
- **Other WLED versions:** 0.13 and 0.14 keep the same files and layout, untested. 0.16 checks an upload's release name, so the user ticks WLED's option to skip it, untested. A WLED build with one app slot cannot move the image aside.
- **Tasmota:** its layout already has a recovery app in the factory slot and one app slot, so the image arrives running where MoonBase goes with nowhere to move aside; it would write over the recovery app instead. Its WiFi lives in a binary settings block in NVS, which needs its own reader.
- **ESPHome:** its update accepts the image, but its WiFi is compiled into the firmware, so MoonBase opens its access point.
- **A generic WiFi source:** ESP-IDF's WiFi driver can keep the last network in NVS under a namespace every framework shares; tried when no WLED files are found, it may carry the network from the others. Bench first, since whether it is there depends on how each firmware starts WiFi.

### Back to WLED over WiFi (2026-10-09)

Kind: Feature · Stage: Install · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084501)

The migration image moves a WLED device to MoonLight without a cable, but the way back needs one: MoonBase installs MoonLight images only, so a bulb going back to WLED is opened for USB.
Owners and makers of these devices care most about this: a firmware change that cannot be undone without a cable is a bigger step than one that can.
It is ours to build rather than WLED's, since the layout it undoes is MoonLight's, about one to two sessions:
- **A small image that downloads WLED itself** from WLED's GitHub releases while it runs, so MoonLight never redistributes WLED's binary and the image stays far from what it writes.
- **WLED goes into WLED's second app slot**, past the image's own position, so nothing moves aside; then WLED's table, and the boot pointed at that slot. WLED starts, formats its filesystem and opens its own access point.
- **MoonBase installs it** only once the image is stamped as one of MoonLight's, a deliberate change to the one rule that refuses other images (`src/core/util/FirmwareImage.h`).
- **A "Back to WLED" button** in the Firmware card, a release asset per chip, and the bench rehearsal with a power cut at every step that the way in had.
- The WiFi stays behind: WLED opens its access point, where the user sets it.

### The desktop build advertises no mDNS (2026-09-06)

Kind: Feature · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084562)

`mdnsInit` on the desktop platform is a stub returning false, so a desktop instance is invisible to
discovery: it neither announces itself nor answers `<name>.local`. On a board mDNS is what makes a
device findable without knowing its address.

Nice-to-have rather than blocking, because instances address each other by IP and that is what the
network drivers take (`NetworkSendDriver`'s `ips` list). What it costs today is convenience: a
container on a NAS has to be looked up rather than named, and a fleet of them cannot be enumerated
the way a fleet of boards can.

**What it costs:** a platform implementation per OS, which is the awkward part. macOS has Bonjour
built in, Linux would want Avahi (a dependency the container deliberately does not have), and
Windows has its own. A small self-contained responder is the alternative, since the announce side is
a handful of multicast records and we do not need the resolver half.

Worth revisiting if instance-to-instance sync grows: discovery is the difference between configuring
a fleet by address and having it assemble itself.

### The arm64 `.deb` is untested on Raspberry Pi OS bookworm (2026-09-13)

Kind: Investigation · Platform: Pi · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084649)

One piece of the arm64 verification was never done, because the bench has no such board. The job runs on
`ubuntu-22.04-arm` **deliberately**, for glibc 2.35: Raspberry Pi OS bookworm ships 2.36, and a
build on `ubuntu-24.04-arm` (glibc 2.39) would install there and then die at startup with
`GLIBC_2.38 not found`, the same failure the Dockerfile records hitting on debian12. The floor is
what makes bookworm work, and **bookworm is the one target nobody has run it on**.

Two halves are untested together: that the binary starts (the symbol check says the highest
requirement is `GLIBC_2.35`, which is the proxy), and that `Depends: libcurl4 | libcurl4t64`
resolves there. Trixie provides only `libcurl4t64`, bookworm only `libcurl4`, and only the first
half has been exercised. A wrong alternation fails visibly at `apt install` with an unsatisfiable
dependency rather than installing something broken, which is the good failure mode.

**A user with a Pi settles it in one command.** Closing this needs a report, not a code change.

### A flashable SD image with MoonLight already on it (robwomp, 2026-09-08)

Kind: Feature · Platform: Pi · Stage: Install · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084753)

Suggested on Discord while bringing up a NanoPi R28S: most Pi users want to write an `.img` to a
card and be running, not to install a toolchain. Armbian's own build tooling supports exactly this
(`armbian/os` carries per-application "extensions", OMV being a small worked example), so the image
is a spin of a maintained distribution plus our package rather than a distribution to maintain.

Wants the arm64 `.deb` above first: with it the extension is roughly "install this package, enable
this service", which is the shape those extensions already have. Without it, the image would have to
carry a source build, which is the thing it exists to avoid.

### MoonBase follow-ups

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084833)

MoonBase v1 ([MoonBase](../../explanation/architecture/moonbase.md))
ships exactly one action: install firmware (upload + URL). The name is deliberately broader than
"recovery", these are the candidate next actions, each solving something only a separate boot
image can solve, and each must earn its bytes in the slot's headroom (a few KB of code is fine; a
new IDF component is the expensive kind).

- **Boot with config disabled**: one-shot flag the app reads at startup to skip loading
  `/.config`: diagnose "is it my config or the firmware?" without erasing anything.
- **WiFi re-provisioning**: edit the stored credentials from MoonBase's page (today it only
  *reads* them; the AP fallback plus the app's provisioning already covers most of this).
- **Firmware downgrade guard**: MoonBase installs whatever image it is given; a version display
  (read from the incoming image's app descriptor) before flashing would make an accidental
  downgrade visible.
- **Hardware diagnostics**: chip/flash/PSRAM identification and a minimal pin tester, for
  triaging a board that misbehaves under the full app.
- **MoonBase Ethernet beyond classic RMII**: the P4's IP101/managed-component PHY and the S3's SPI W5500,
  which matter only if MoonBase ever goes beyond the 4 MB classics.
- **MoonBase as the only update mechanism, all boards** (PO, 2026-08-26): would delete the app's
  whole in-place OTA path (a real subtraction) and grow every app slot, with the stronger
  power-fail story everywhere. Three deciding factors first: MoonBase Ethernet beyond classic RMII (the P4 has no
  WiFi of its own and uses a different PHY; the S3 uses SPI W5500), a migration that does not lose config ([config backup and restore](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287075118)), and accepting
  ~45 s of visible downtime per update where dual-OTA installs in the background. Trigger:
  MoonBase proven in the field on the 4 MB boards.

### LEDs on the Teensy flash white, then the board resets (2026-10-13)

Kind: Bug · Platform: Teensy · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084894)

Five times in one session, the 64-light strip on the Teensy 4.1 bench board went full white, and the board restarted with reset reason `UNKNOWN`, no fault mark and no watchdog: a supply dip, which a full-white strip on USB power explains.
Each white flash came while the device built a large state document: switching SdCard's board with the old 46 KB full state, and subscribing to Drivers or the whole tree with the new one; none came with the LEDs disconnected.
A WS2812 strip reads a data line held high as all ones, so suspect the GPIO-DMA output stalling high, or its buffer moving under the DMA when the Layer reduces its buffer under memory pressure ("buffer reduced" was showing).
Reproduce with a logic analyser on the data pin while subscribing to Drivers, before changing code.

### A Teensy sometimes drops a message the page sends (2026-10-13)

Kind: Bug · Platform: Teensy · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287084990)

Choosing Custom on SdCardService's board select reached the Teensy once in three tries in one session, and every time in later ones; the same clicks always reach the Dig-Octa.
A lost write leaves the page showing a choice the device never made, and nothing tells the user.
Suspects, in order: the `/wsp` preview stream filling the send side while a frame from the page waits, a long render tick delaying the poll that reads the socket, and power dips while the LEDs show full white.
Reproduce with the preview open and closed and the LEDs dark and lit, logging each frame the device receives, before changing any code.

### SD card writes over SPI stop the card on the Dig-Octa (2026-10-12)

Kind: Bug · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085076)

On the QuinLED Dig-Octa's SPI slot (SCK 14, MOSI 15, MISO 36, CS 16), a write past about 50 to 200 KB fails mid-transfer, and the card then answers no command until its power is cut.
Reads hold: a 5 MB file reads back byte-exact at 184 KB/s.
The same no-brand 32 GB card writes 5 MB cleanly on the native SD bus of the P4 and of the Olimex ESP32-Gateway.
Ruled out: the clock (20, 10 and 4 MHz fail alike), 30 ms between writes, and the wiring, which Quindor's schematic shows with 10 kOhm pull-ups, its own LDO and decoupling. Turning the LED driver off moved the limit a little without removing it.
Next: the same 5 MB test with an A-brand card, since SPI mode is the part of the spec newer cards treat as secondary. ESPixelStick, the one other firmware driving this slot, throttles its writes into 7 KB blocks with a 30 ms gap and pauses LED output during an upload (forkineye/ESPixelStick issue 802 and `src/FileMgr.cpp`).
Until it is solved, a card mounted over SPI shows a warning status.

### ESP32 UDP receive is bounded by PACKET COUNT, not bytes

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085150)

Reported from other projects working the same ground: the ESP32 family's incoming UDP limit behaves
as a **mailbox of packets** rather than a memory budget, with a hard numerical ceiling, and the P4
is no better. The observed shape is perfect reception up to that count and then progressively worse
loss as universes climb, rather than a clean cliff. Raising it is tunable but bounded, since the raw
packet buffers are full-frame sized whatever the payload.

Two consequences worth having in mind:

- **Pixels-per-packet is the lever, not bandwidth.** Art-Net is DMX512 on the wire, so it is capped
  at 512 values per packet whatever the frame size; DDP fills close to a whole MTU. For the same
  pixel count DDP therefore needs far fewer packets, which is the resource that runs out first.
  This matches our own measurement that ArtNet is where the WiFi limit bites.
- **A P4-specific escape exists.** Have the packet handler DMA the payload to PSRAM and release the
  hardware buffer immediately, then parse from another task, so the mailbox drains at memory speed
  rather than at parse speed. That is real work and speculative, but it is the shape of a fix rather
  than a tuning knob.

Unverified on our own bench: this is other people's measurement, recorded so the next receive-path
investigation starts from it rather than rediscovering it. Confirm before acting on it.

### WiFi Art-Net performance

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085235)

128×128 WiFi ArtNet measurements exist (see [performance.md](../../reference/performance.md) "ArtNet over WiFi" and "Build-variant WiFi comparison"). Remaining matrix:

- WiFi STA 64×64 (4K LEDs, 24 universes)
- WiFi STA 32×32 (1K LEDs, 6 universes)

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **prefer wired Ethernet for any ArtNet workload on classic ESP32** — the default `esp32` build carries both stacks, so Ethernet is available even when the original measurements were taken on the old `esp32-eth-wifi` variant.

### Memory ceiling on non-PSRAM ESP32 with WiFi and Ethernet

Kind: Investigation · Platform: classic · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085323)

On the `esp32` firmware, which carries both stacks, default 128×128 grid, measured when the two were a separate variant, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on a classic board without PSRAM (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [lessons.md](../../contributing/lessons.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Boot-time buffer degradation on non-PSRAM at 128×128

Kind: Investigation · Platform: classic · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085395)

On the Olimex (no-PSRAM) at 128×128 with a modifier, the Layer sometimes comes up **degraded** at boot — status `"buffer reduced — not enough memory"`, with a visibly wrong render (the reduced render buffer overflows what the LUT/extrude expects). **Toggling any layout control (forcing a fresh `onBuildState`) fixes it** — the rebuild allocates the full buffer and the display is correct. So this is a boot-time allocation *race*, not a code bug: the same rebuild path that fails at boot succeeds moments later.

Measured: the full pipeline needs two ~49 KB contiguous buffers (Layer render buffer + Drivers output buffer, both 128×128×3), plus the LUT. At boot the largest contiguous block is only ~14–20 KB while the network stack / mDNS / HTTP-server buffers are still settling into the heap — so the Layer can't claim a contiguous 49 KB and degrades (halves its dimensions). A rebuild after the heap settles wins the contiguous block and allocates full. The device is at the fragmentation edge either way (~42 KB free / ~14 KB largest block at 128×128 with the full pipeline up).

The annoyance is purely that the device boots degraded and needs a poke to recover — it should come up working. Fix options in increasing scope:
- **Allocation order** — claim the big Layer/Drivers buffers *before* the network/mDNS/HTTP buffers fragment DRAM (i.e. wire the light pipeline's `onBuildState` ahead of network bring-up in `main.cpp`). Cheapest if the ordering is safe.
- **Boot retry** — if `onBuildState` degrades, schedule one more `buildState()` after boot settles (a one-shot, e.g. after the first `loop1s` or once the network reports up). Self-healing without reordering init.
- **Cap the default grid** on no-PSRAM to a size whose two buffers fit the post-boot largest block (same lever as the memory ceiling above).
- **PSRAM for the buffers** on PSRAM-equipped variants: sidesteps DRAM fragmentation entirely (related: the [Memory ceiling](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085323) PSRAM notes).

Related: this is the render/output-buffer face of the same non-PSRAM fragmentation cliff the paged `MappingLUT` already addressed for the *LUT*. The buffers themselves still allocate as single contiguous blocks.

### WiFi runtime disable

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085471)

WiFi initializes only when a station or the access point starts, and `wifiRadioDown` frees it when both stop, so a board that Ethernet carries holds no WiFi memory. Turning WiFi off entirely, the access point fallback included, is undesigned: whether the gate keys off detected hardware presence, an explicit control, or a deviceModel-catalog field is not decided until a concrete case asks for it.

### Trim the TLS certificate bundle to the common roots

Kind: Feature · Trigger: flash runs tight on a board · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085540)

`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` keeps the common roots only, an estimated 40 KB less flash; `_NONE` saves about 50 KB but breaks TLS.

### Platform API: `std::span` migration

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085617)

Several `platform.h` APIs still use `(buf, len)` pairs where `std::span` would catch length/pointer mismatches at compile time. Concrete sites: `http_fetch_to_ota`, `improvProvisioningInit`, and friends. ~2 h including ripple updates to callers. Do alongside the next platform-API expansion (Windows socket port or POST /api/firmware streaming).

### Ethernet on the QuinLED Dig-Next-2 (W5500 over SPI)

Kind: Investigation · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085683)

The DN2 is in the catalog with no Ethernet block, so it comes up WiFi-only. Adding it is a `deviceModels.json` entry and nothing else: the preset work landed the seam, and W5500 already applies live.

**RMII is impossible on this board, so the W5500 is the only route.** The DN2 carries an ESP32-PICO-V3-02, whose package leaves GPIO 16, 17, 18 and 23 unbonded, and those are the RMII management and clock pins. This is the same package fact behind the flash-cache wedge recorded in [lessons](../../contributing/lessons.md). QuinLED's own LAN8720 board stacks onto the QuinLED-ESP32 only, not onto a Dig board.

**The wiring, from [Quindor's firmware source](https://github.com/intermittech/WLED-Dig-Next-2_W5500)**, using the QEXT header for the bus and the Stemma QT header for the two control lines. Fitting Ethernet therefore costs the I2C port:

| W5500 | connector | GPIO |
|---|---|---|
| SCLK | QEXT green | 25 |
| MOSI | QEXT blue | 32 |
| MISO | QEXT yellow | 0 |
| CS | QEXT white | 33 |
| INT | Stemma QT SDA | 15 |
| RST | Stemma QT SCL | 14 |

MISO on GPIO 0 is deliberate and sound: GPIO 0 is a strapping pin, and a W5500 leaves MISO high-Z while CS is inactive, which is what Espressif's rule about not driving strapping pins at reset asks for.

**It stays `Custom`, and that is the finding.** Across the three W5500 boards now known, LightCrafter 16, SE 16 V1 and this one, no two share a single pin on any of the six lines. SPI Ethernet is a module wired to whatever a board left free, so there is no W5500 pattern to make a preset from, and inventing one would be a preset that fits nothing.

**What is unverified.** The pin table is trustworthy as what that firmware does, because it was read out of the code rather than a description. What rests on one source is that it works: a single-author repository created 2026-09-19, self-labelled experimental, tested on one unit, with LED output, buttons, relays and mic explicitly not exercised. No independent build was reported when this was written. So the open question is not the pins but whether SPI Ethernet and the LED output coexist on this board under load, which is a bench answer.

### Link-time optimization on the ESP32 builds

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085768)

Splitting `HttpServerModule` into three files cost 7,360 bytes on the classic ESP32 (2,129,216 to 2,136,576), the compiler no longer inlining across the files. Link-time optimisation (`-flto`) optimises the whole program at the link and wins that back, and likely more across the firmware. ESP-IDF's current docs offer it as `CONFIG_COMPILER_LTO_LINKTIME`; v6.1, which MoonLight builds with, has no such option. Try it once the ESP-IDF release MoonLight moves to offers it, per board: the image size, the IRAM placement in the map file (an interrupt handler must not inline code from flash), a run on the RMT and parallel drivers, and the build time, which LTO makes two to three times longer.

### Adopting the v6.x ecosystem changes

Kind: Investigation · Trigger: each v6 item's own trigger fires · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085832)

The as-is state of each item is the table in [build-system.md § The v6 ecosystem](../../reference/build-system.md#the-v6-ecosystem); this entry carries the plan and the triggers.

**Per item, how and when:**

- **EIM** — adopt any time. It shipped *in* v6.0 so it clears the v6.0-floor rule, and it has a headless CI mode. Add EIM as the **preferred** path in `setup_esp_idf.py` (`eim install`), keep `install.sh` as a documented fallback for one release. Keep the exact-commit pin: EIM's multi-version management helps reproducibility, it does not replace the pin.
- **PSA Crypto** — nothing to migrate while we stay on high-level components. Watch only: if a feature needs hashing or signing directly (signed-OTA verification, a device identity), write it against the **PSA API** from the start, not legacy mbedTLS. Trigger: first direct crypto use.
- **`network_provisioning`** — adopt to close the phone-app + SoftAP gap. It is in v6.0 (clears the floor) and is the IDF-native standard. Add it as a **sibling provisioning module** beside ImprovProvisioning (both wired-by-code System modules; a device offers whichever transports its chip supports), reusing the same WiFi-credential plumbing. Not a replacement for Improv: they cover different front-ends (browser vs phone-app) and a product can want either. Weigh the BLE-stack cost per chip. Spec before code.
- **CMake Build System v2** — watch until GA, not while it is a preview: adopting a preview build system is the opposite of common-patterns-first. Trigger: v2 ships as the default. Then dry-run a build under v2, fix any `idf_component_register()` / Kconfig fallout, switch. Low risk given how little custom CMake we have.
- **Built-in MCP server** — evaluate, do not default to it. The risk is a *second control path* bypassing the script policy, and it is ESP32-only (no desktop), so it cannot be the uniform path. If adopted, wrap it behind a script (`moondeck/run/idf_mcp.py`) so the policy layer still applies. Trigger: a concrete debug workflow the scripts cannot cover.

**Sequence.** Close the v6.0 gaps one at a time as normal feature commits:

1. **EIM installer** — rework `setup_esp_idf.py` to prefer `eim install`, keeping `install.sh` as a one-release fallback. Smallest and lowest-risk (build-path only, no firmware change, no hardware re-test), and EIM's multi-version management is what cleanly supports the v6.0-floor / v6.1-fallback juggling, so it sequences first as an enabler.
2. **`network_provisioning`** — the headline capability: the phone-app + SoftAP onboarding flow. Its own plan.
3. The rest (PSA-native crypto, CMake v2, MCP) as their triggers fire.

**The pin's drift, shown in MoonDeck.** The pin and its policy live in [build-system.md § ESP-IDF version](../../reference/build-system.md#esp-idf-version); its drift warning shows only while `setup_esp_idf.py` runs, so a MoonDeck banner or status dot surfacing the same drift is still to do.

**Guardrails.** Platform-generic stays intact: these are ESP32-specific gains and none may regress Teensy or the desktop paths, which do not use ESP-IDF at all. An IDF feature is adopted *inside* the ESP32 platform layer / build tooling, never by leaking an IDF assumption into shared `src/` or the desktop build. And the v6.0 floor holds: adopt only what is in v6.0, so the v6.0 fallback keeps working.

### ESP32-P4 Parlio: a real long WS2812 strip

Kind: Investigation · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086025)

A real *long* WS2812 strip, not just the bench panel, is the remaining hardware proof of the Parlio driver. The loopback self-test caps its frame to `ParallelLedDriver::kLoopbackTestLights`, which its declaration explains.

### ESP32-P4 DHCP hostname not shown by the router (recheck later)

Kind: Investigation · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086097)

the device sets its DHCP hostname (option 12 = `deviceName`, default `MM-XXXX`) in the `ETHERNET_EVENT_CONNECTED` handler, verified working on two boards: the S3 over WiFi (router shows `MM-70BC`) and the Olimex over RMII Ethernet (`MM-BD3C`) — the *same* `ethEventHandler` code path the P4 uses. Yet the bench P4 (Waveshare P4-NANO, RMII) still shows as blank/"Unknown" in the GL.iNet client list, while serial confirms `set_hostname` succeeds with no error. Two unconfirmed suspects, neither our logic: (1) the router holds a **sticky lease** for the P4's MAC and won't relearn the hostname until it fully expires (the per-client "forget" isn't exposed in this GL.iNet UI, and a plain reboot didn't clear it); (2) a P4-specific IDF netif quirk serializing option 12 differently on the newer P4 Ethernet path. Since the shared code path is proven on two other boards, this is not treated as a code bug. Recheck after the P4's lease naturally expires, or on a different router, before spending more on it.

### ESP32-P4 400 MHz on rev-3+ silicon

Kind: Feature · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086159)

**Found:** bench, 2026-07-25 (Quindor Discord question about the P4's 400 MHz).

The P4 build runs at **360 MHz** because IDF's `Kconfig.cpu` caps a `SELECTS_REV_LESS_V3` build (which we set, so a stock binary boots on the v0.x/v1.x P4 chips in the field) at 360; 400 MHz is IDF's default only for rev ≥ 3. Forcing 400 on our bench P4 (a **rev v1.3** chip) was tried and **bootloops** — `assert failed: esp_clk_init clk.c:105` — so 400 is a genuine hardware limit on pre-rev-3 silicon, not marginal stability. The ~11% compute headroom is left on the table for rev-3+ owners.

The rev-3 image exists: `esp32/sdkconfig.defaults.esp32p4rev3` sets `CONFIG_ESP32P4_REV_MIN_300`, so the bootloader refuses a pre-rev-3 chip. What remains is `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400` in that fragment, which waits until a rev-3 P4 on the bench proves 400 runs clean: no untested clock config, per the same rule the S31/320 and this P4/400 bootloop both taught.

### LED output from a Raspberry Pi's own pins (robwomp, 2026-10-11)

Kind: Feature · Platform: Pi · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086230)

Suggested on Discord after the Teensy port: a Pi runs MoonLight today as the Linux desktop build, so its LEDs are reached only through the network drivers.
Driving a strip from the Pi's own header needs the platform backend that does not exist yet (`src/platform/rpi/`, the same prerequisite as the sensor item below), and a `LedPeripheral` behind it.
The routes differ by model: on a Pi 4 and earlier, DMA through the PWM, PCM or SPI peripheral, the way rpi_ws281x does; on a Pi 5 the header sits behind the RP1 chip, whose PIO blocks are the route.
SPI is the one path every model shares, one lane per SPI bus with each WS2812 bit encoded in SPI bits, and the place to start.
Portable GPIO libraries (WiringPi, libgpiod) are too slow for LED timing, so they serve buttons and relays, not this.
On a Pi 5, own PIO programs reach the hardware through the kernel's PIO driver, which piolib shows; Adafruit's PioMatter proves HUB75 that way, and HUB75 on a Pi is a second backend once this one exists.
The research, with sources: [the LED-driver analysis](../../design/leddriver.md).

### LightCrafter 16: questions for the maker (2026-10-07)

Kind: Investigation · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086342)

The [LightCrafter 16 page](../../reference/hardware/lightcrafter-16.md) holds what the catalog and limpkin's pull requests say.
These are open, to ask limpkin:

- The module part, the level shifter, the output voltage and the connectors.
- The power input: the connector, the voltage range, the fusing and the reverse-polarity protection.
- Which USB-C connector is the S3's own USB and which the serial bridge, and which chip the bridge is.
- Whether the S3's own port supplies 5 V to a plugged-in device, such as a MIDI desk.
- The RS-485 transceiver and the Hall current sensor parts.
- A schematic and a pin-layout image.

The device model's use of GPIO 19 and 20 as the LCD driver's unused signal lines is unverified on hardware.

### Sensor input on Raspberry Pi 5: microphone, IMU, line-in

Kind: Feature · Platform: Pi · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086408)

Audio-reactive lighting (and motion-reactive) is core to what WLED-MM / MoonLight are known for. The Pi 5 is the right host for it: it has the CPU and RAM for real FFT-based audio analysis that the Xtensa ESP32 struggles with, and a full Linux audio + I²C stack. The audio side exists: `AudioService` on the desktop backend captures any PulseAudio or PipeWire device on a Pi, and the effects read its frame. What is missing is a `src/platform/rpi/` backend for I²S and I²C, since the Pi runs the **desktop** backend, which has no GPIO access. So this is a domain expansion built on a real platform-backend prerequisite, not a small add.

**Target sensors and their Pi 5 interfaces:**

- **Microphone** — I²S MEMS mic, or a USB audio device read via ALSA. The high-value one: FFT → frequency bands + beat detection drive audio-reactive effects.
- **Line-in** — the Pi 5 has no native analog input, so this is a USB audio interface / DAC HAT feeding the same audio pipeline as the mic; only the source differs.
- **IMU / gyro** — an I²C device (MPU-6050 / 9250-class) on the Pi's I²C bus; tilt / motion → effect parameters.

**Increments:** a real `src/platform/rpi/` hardware backend (GPIO/I²C/I²S/ALSA) first, the prerequisite; until it lands, the Pi runs the desktop backend with no sensors. The IMU and line-in then slot into the same source-module + platform-API shape `AudioService` has.

### Bench-verified classic parallel setups, not yet in the catalog

Kind: Feature · Platform: classic · Trigger: a classic board ships parallel output as its default · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086478)

- **Bench-verified classic parallel setups, not yet in the catalog** (2026-09-06). Every classic
  board in `deviceModels.json` ships `RmtLedDriver`, so none carries a `ParallelLedDriver` entry,
  and the driver's defaults (`clockPin` unset, `dcPin` 33) work on a classic board without one.
  Two setups are proven on hardware and worth recording before they are lost: the **Olimex
  ESP32-Gateway** drives 64 lights on GPIO 16 with `clockPin` 32 / `dcPin` 4, and its free pins are
  scarce enough that a catalog entry should pin them explicitly rather than inherit a default (its
  Ethernet PHY holds 18/23 as MDIO/MDC and 5 as reset, all of which the old defaults collided
  with); the **QuinLED Dig-Next-2** drives 256 lights on GPIO 2 with `clockPin` unset / `dcPin` 33.
  Add them when a classic board actually ships parallel output as its default, rather than
  speculatively across the other 16 classic boards, none of which has been tested this way.

### Classic-board memory: the wins are system-level, not per-module (2026-09-09)

Kind: Investigation · Platform: classic · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086575)

The Dig-Octa on a clean boot with RmtLed has **88,284 bytes free internal, 86,016 largest block**.
Under load and after driver swaps that largest block fell to 24,576, so fragmentation matters as
much as the total.

What the modules hold is NOT where the RAM went. The whole tree reports **9,342 bytes** of
`dynamicBytes` (Layer 3,586, Preview 2,560, RmtLed 1,536, Drivers 1,536). So per-module trimming has
almost nothing left to win, and the remaining ~230 KB of internal RAM in use is WiFi, lwIP, FreeRTOS
task stacks and the binary's static data.

Where to look, in order of likely return:

- **lwIP pool sizes** (`CONFIG_LWIP_*`): TCP PCBs, pbuf counts, and the send/receive windows are all
  sdkconfig knobs, sized for a general-purpose device rather than a controller with a handful of
  connections.
- **WiFi buffer counts** (`CONFIG_ESP32_WIFI_*_BUFFER_NUM`): the static RX/TX buffer pools are the
  single largest tunable block on a classic board.
- **Fragmentation, not just totals.** A 24 KB largest block with 50 KB free is a placement problem;
  allocating the long-lived buffers early and together is what fixes that.

Each is an sdkconfig change measurable on the bench in minutes (`freeInternal` and `maxBlock` on a
clean boot), and each risks a different failure: too few WiFi buffers drops packets under load, too
small a stack overflows under a rare path. So measure one at a time on a board that is actually
serving the UI, not idle. The payoff is real: at 88 KB free a classic board is one large allocation
away from trouble, which is what drove both driver decisions on 2026-09-09.

### A serial UART for sensors and scripts (2026-10-29)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287640906)

A UART that reads and writes a byte stream, as a platform seam with MoonLive builtins, so a serial device is a compiled module or a script written from its datasheet. The platform has none today: the only UART use is the Improv provisioning listener.

**Who needs it**
- The HLK-LD2450 24 GHz radar, up to three targets with x and y, at 256000 baud. Step 7 of [Audio reactive live performance](../Plan-20261013%20-%20Audio%20reactive%20live%20performance%20(0%20of%207).md) tracks performers with it. Two kit B units are on the bench.
- Wired DMX-512 output, the same UART in RS-485 half-duplex mode: [RS-485 and DMX-512 wired output](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287080691).
- A DIN MIDI port, which is a UART at 31250 baud. MIDI reaches the board over USB, the network and the browser today.
- GPS modules, sending NMEA text lines at 9600 baud.
- BLE-to-serial modules (Nordic UART Service, HM-10): BLE without a BLE stack beside WiFi on the ESP32.
- A radar as a presence trigger where a PIR gives false positives and active infrared shows in night-vision gear, such as a trail camera.

**The shape**, following the [input-mapping plan](../Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20(7%20of%2012).md)'s rule that a seam ships with its builtin:
- The driver fills a receive ring buffer from its interrupt, so a read never blocks the render path. Pins are claimed through the Pins module. UART0 is the console; a classic ESP32 and an S3 have two more, a C3 one, a P4 four.
- Builtins with the Arduino names: `serialBegin(rx, tx, baud)`, `serialAvailable()`, `serialRead()`, `serialWrite(b)`, plus a bulk read into an array, since at 256000 baud up to about 500 bytes arrive per 20 ms tick.
- The seam is device-neutral: a device's protocol lives in its module or its script.

**Open**
- Delivery beyond a polled buffer: a callback per byte, line or frame; a line or frame mode for NMEA and radar packets; passing the stream through to the network.
- Whether the input-mapping plan takes it as a step, with the LD2450 as its bench sensor. The plan's sensor table has no UART row yet; [the input-mapping design study](../../design/input-mapping.md) names the radar's need for it.

### Video in: an HDMI source on the lights through V4L2 capture (2026-11-07, from Discord)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=296929750)

A USB HDMI grabber (UVC) and an RK3588's HDMI input both appear on Linux as V4L2 capture devices. Platform: open the device, stream frames by mmap, YUYV first, since cheap grabbers' MJPEG at 1080p needs a JPEG decoder. Light domain: a Video In effect scaling the picture onto the layer, an HDMI source or a screen on the lights. Later: macOS (AVFoundation) and Windows (Media Foundation).

Test with a USB grabber on the NanoPi R3S-LTS; an RK3588 board with HDMI in is in the community. Check whether the RK3588's HDMI input needs Rockchip's kernel or works on mainline Linux before promising it.

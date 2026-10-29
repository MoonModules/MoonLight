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

### ESP32-P4 WiFi through the C6 co-processor

Kind: Bug · Platform: P4 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287085924)

Rounds 1 (board and Ethernet-only) and 2 (Parlio LED driver) have landed; WiFi through the C6 is what remains.

Issue #146 (a P4 `eth-wifi` boot loop) was fixed without anyone seeing WiFi work: the bench P4-NANO's C6 never answers on SDIO (`ESP-Hosted link not yet up`, no `Card init success`), with or without the fix. So every P4 WiFi question waits on a reporter's board. The textbook cause of a silent C6 is a host/slave mismatch: our host side is ESP-Hosted 2.12.13, built into the firmware, while the C6 runs whatever slave firmware the board maker flashed. **Make our own P4-NANO's link come up, then keep it as the WiFi reference.**

- **The slave image**: `network_adapter.bin` built from esp-hosted-mcu's `slave` example at the host's version (2.12.13), for the C6.
- **Flashing it, with the link down** (our case): directly over the C6's own UART with its boot pin held, through an ESP-Prog or a USB-serial adapter. The P4-NANO's schematic says where the C6's UART and boot pads are; ESPHome's board page names only the SDIO pins (`clk` 18, `cmd` 19, `d0`-`d3` 14-17) and the C6 reset (`GPIO54`).
- **Flashing it, with the link up**: ESP-Hosted's transport OTA, its `host_performs_slave_ota` example, which ESPHome wraps as the `esp32_hosted` update platform. This is the path for users once one board works, and a candidate for MoonLight itself: carry the slave image and offer the update on the WiFi card, as ESPHome does.
- **Say which failure `wifiCoproc` shows**: `no version reply` covers two cases, a link that never came up and a C6 too old to answer the version query (or answering 0.0.0), and only the first stops WiFi. `coprocessorWifi()` in `platform_esp32.cpp` checks the transport state first and reports `link down` or `C6 too old to report`, so a reporter's System card answers what today needs a serial log.
- **Then**: the round-3 regression (associates on rc1, not on the v6.1 pin) gets measured on a board with a known-good link, and the two defects of the cascade below get a repro.

What #146 taught, for any P4 change: before the scheduler starts, the P4 has 148 KB of internal RAM (retention, RTC and SPM; the 384 KB L2 region still holds the startup stacks), and every global constructor allocates from it. ESP-Hosted's took 123 KB until `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` moved its buffers to PSRAM. A boot that asserts in `vApplicationGetIdleTaskMemory` is that budget exhausted; a `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` report from a constructor of priority 101 and an `ESP_SYSTEM_INIT_FN` at stage 999 brackets it.

#### Round 3: association and the slow link

- **Round 3 regression: WiFi associates intermittently on IDF v6.1 (2026-10-12; the PO sees it associate on some boots and not on others, 2026-10-16).** The variant keeps shipping so users can try it. It associated and served traffic on IDF v6.1-rc1 (below). The bench P4-NANO (v1.3, port `usbmodem5ABA0767291`, registry .139) flashed with `esp32p4rev1-eth-wifi` on the v6.1 pin, Ethernet unplugged: Improv delivered the bench SSID over USB serial, the board answered "unable to connect", and it never appeared on the network. Its log does not reach `monitor_esp32` on that USB-JTAG port, so the cause is unread. First step: read the P4's console (UART pins with an adapter) for the esp_hosted / C6 lines, then compare against the rc1 build that worked: the IDF move from rc1 to v6.1 and the esp_hosted version it pulls are the two candidates.
- **Round 3, WiFi via the C6 co-processor: the link is slow (2026-08-19).** Boots and associates on IDF v6.1-rc1; the remaining defect is throughput, bench-bisected below. How the variant is built: [lessons](../../contributing/lessons.md#esp32-p4-round-3-wifi-via-the-c6-the-abstraction-the-earlier-round-feared-wasnt-needed).

  **Open issues before this is done:**

  0. **The esp_hosted build is slower on HTTP — BISECTED to the hosted link, not to us (2026-08-19), and much improved but NOT gone (re-measured 2026-08-21).**

     **Original (2026-08-19):** Same board, same commit, same application code, measured over **Ethernet on both builds** so the radio is not in the path:

     | build | per-request (`/api/system`) | throughput (73 KB `app.js`) |
     |---|---|---|
     | `esp32p4rev1-eth-wifi` | alternating 0.434 / 0.788 / 0.446 / 0.813 s | 42 KB/s |
     | `esp32p4rev1-eth` | 13 ms flat, 12 consecutive fetches | 708 KB/s |

     Render is healthy in both (121-134 fps), so this is not frame-loop contention. Because the two images share every line of application code, the penalty is **esp_hosted being compiled in and its SDIO link serviced**, degrading traffic on an interface it is not even carrying. The cost is **per-request, not per-byte** (a 1 KB fetch cost nearly as much as a 36 KB fetch), which points at a periodic blocker a request must wait out rather than a slow pipe.

     Already excluded on the bench: SDIO width/clock (already 4-bit/40 MHz, the maximum), WiFi buffer counts (match the S3's), ICMP latency (6.6 ms). An earlier `SystemModule` fix (`coprocessorWifi()` retried an unanswered version query every tick: 1,012,344 us → 257 us) was real and is kept, but it was **not** this: the stutter outlived it and vanished only when the C6 path left the image.

     **Per-module tick timing, both builds on the SAME Ethernet interface (2026-08-19).** The measurement that says what this is NOT:

     | module | eth-only | eth-wifi | factor |
     |---|---|---|---|
     | HttpServer | 113 us | 3441 us | **30x** |
     | File Manager | 279 us | 4686 us | 17x |
     | Services | 242 us | 1388 us | 5.7x |
     | Audio | 239 us | 1376 us | 5.7x |
     | Effects | 172 us | 397 us | 2.3x |
     | **Network** | 10 us | **92 us** | (negligible in absolute terms) |
     | Drivers | 6601 us | 4215 us | **0.64x (FASTER)** |
     | ParallelLed | 7186 us | 6569 us | 0.91x (faster) |

     **Two theories are refused by this table.** It is not a blocking WiFi call in `NetworkModule` (`Network` costs 92 us, and `wifiStaRssi`/`wifiTxPower` are state-gated off entirely in `ConnectedEth`); and it is not periodic WiFi scanning, since no module carries anything like a 0.37 s cost. Instead **modules that touch no network at all** (Audio, Noise, Effects, Layer) slow by the same kind of factor as the HTTP path: a broad, roughly proportional slowdown of ordinary code.

     **Working theory: L2 cache contention.** Both builds are byte-identical in every memory setting (`CACHE_L2_CACHE_128KB`, `SPIRAM_SPEED_200M`, `FLASHFREQ_40M`, `SPIRAM_MODE_HEX` — a full sdkconfig diff shows no cache/PSRAM/flash/CPU-freq difference), and code executes from flash through that shared 128 KB L2 in both. Adding esp_hosted's tasks, ISRs and DMA buffers evicts application code that was previously resident, so ordinary work starts missing to flash/PSRAM. This also explains the otherwise odd inversion: the DMA-bandwidth-bound `Drivers`/`ParallelLed` got *faster*, which is what happens when the CPU competes less for the same bus. **Consistent with the data, not yet proven** — separating cache contention from plain CPU stealing needs a cache-hit-rate counter or a per-tick histogram, neither of which we expose today.

     **The cache theory does NOT explain the visible LED hiccup, and there are TWO effects here (PO observation, 2026-08-19).** Uniform cache contention predicts a smooth frame-rate drop (133 → 96 → ~73 fps), which is what the averages show. It does not predict a *stall*, yet a once-per-second hiccup is plainly visible on the fixture. So the throughput loss and the hiccup are separate problems and must be chased separately.

     **Our instrumentation structurally cannot see the hiccup.** `MoonModule::tickTimeUs_` is a MEAN (`accumUs_ / frameCount`, [MoonModule.h:656](../../../src/core/module/MoonModule.h)) and `Scheduler::fps()` is derived from it, so a single 300 ms frame among 70 good ones shifts the average ~4 ms and vanishes. Every per-module number in the table above is an average and none of them can confirm or refute a stall. External evidence of the stall is therefore weak but non-zero: sampling `fps` once per second for 30 s gives a steady 73 with periodic dips to 69-70 (~55 ms lost in those seconds), and the dips are NOT on a clean 1 s period. Ruled out along the way: the degradation is not caused by our own HTTP polling (fps is identical at 76 after 25 s of zero traffic and under continuous polling), and the early 96 → 73 decay is warm-up, not load.

     **Per-TASK CPU, both builds, `--task-cpu-stats` (2026-08-19) — this is the decisive measurement.** Same profiling overhead on both, so the comparison is clean:

     | task | prio | eth-only | eth-wifi | |
     |---|---|---|---|---|
     | `main` (render) | 1 | **35.4%** | **93.3%** | the same work costs 2.6x the CPU |
     | `IDLE0` | 0 | 63.3% | 5.1% | core 0 headroom is gone |
     | `ipc1` | 24 | 7.8% | 12.7% | present in BOTH, so not the cause |
     | `mmEncode` | 5 | 3.4% | 6.0% | |
     | `sdio_read` / `sdio_write` / `sdio_process_rx` / `rpc_rx` / `rpc_tx` | 23 | absent | **0.0-0.1%** | the hosted tasks are IDLE |

     **What this refutes.** Not a busy WiFi task: every SDIO/RPC task sits at 0.0-0.1%. Not periodic WiFi scanning, and not a blocking call in our render path either, since `main` is not *waiting* — it is *running*, and burning 2.6x the cycles for identical work. Not `ipc1` (priority 24, preempts everything), which is present on the eth-only build too. There is no WiFi *activity* to stop, so a "compile it in but don't run it" variant would likely change nothing; the cost is already there with the link idle.

     **What this supports.** The same instruction stream executing 2.6x slower with no extra runnable work is the signature of **memory contention**, consistent with the L2-cache theory above: esp_hosted's footprint evicts application code from the shared 128 KB L2, so ordinary code stalls on flash/PSRAM fetches. Cycles are spent *inside* `main`, which is why every module slowed proportionally and why the DMA-bound drivers (bandwidth-bound, not cache-resident) got faster.

     **Three more theories died on the bench (2026-09-29, IDF v6.1 final).** Each was tested against 30 consecutive `/api/system` fetches over Ethernet, so the radio carried nothing in any of them.
     The FreeRTOS tick warning esp_hosted prints at boot (`CONFIG_FREERTOS_HZ is 100, ESP-Hosted recommended 1000`) is an unconditional nag rather than a measurement, and the arithmetic runs the wrong way: IDF's `usleep` busy-waits sub-tick intervals exactly, so the first eight TX retries cost 14.4 ms at 100 Hz against 17.2 ms at 1000 Hz.
     Priority inversion is refuted by its own null result: raising our task from 1 to 10 changed nothing measurable, and IDF's own `esp_http_server` runs at priority 5, which would be equally starved if six tasks at 23 were the mechanism.
     Nagle with delayed ACK looked compelling, since responses are written as a header then a body with no `TCP_NODELAY` and the median penalty is close to a 40 ms delayed-ACK timer, but setting it moved neither build: eth-wifi stayed at ~55 ms and eth-only was already 8-44 ms.
     `CONFIG_LWIP_TCPIP_TASK_PRIO=23` is kept, on the grounds that level-with rather than below is defensible, NOT because it is proven: one run lost its 230-270 ms tail spikes and a later run saw them return.

     **What the penalty actually costs a user, measured per interface (2026-09-29, both fixes in, 30 fetches of `/api/system` each).**

     | build and link | median | spikes |
     |---|---|---|
     | eth-only, Ethernet | **14 ms** | none |
     | eth-wifi, Ethernet | 51 ms | 3 of 30 at ~200 ms |
     | eth-wifi, WiFi (RSSI -44) | 57 ms | 4 of 30 at 280-870 ms |

     Over Ethernet the eth-wifi build is DEGRADED rather than unstable, and an earlier note here calling it unstable on every interface was wrong.
     A click lands in 51 ms where a person notices around 100, so the interface feels fine and the 3.6x is invisible for single actions.
     It surfaces in two places: the ~200 ms outlier roughly once in ten requests, which a page load of eight parallel requests will usually hit, and any client that issues hundreds of requests, which is why the live scenario sweep failed 4 of 6 on this build over Ethernet while a human clicking around noticed nothing.
     Over WiFi the median barely moves but the tail doubles and reaches 870 ms, and under scenario load it reaches multi-second timeouts.
     So the honest split is: usable on a wired link, unreliable on a wireless one, and the eth-only build is the one to run when a P4 has to be dependable.

     **The interface is noticeably quicker than it was (PO observation, 2026-09-29).** Changing an effect over WiFi went from a visible hang to something that responds, which a fetch loop cannot measure: that gesture is a POST plus a tree rebuild plus a state resync, so it pays the tail several times over where a single GET pays it once.
     THREE things changed together and no single one is credited: `TCP_NODELAY` on accepted connections, `CONFIG_LWIP_TCPIP_TASK_PRIO=23`, and a board no longer carrying a failed scenario's leftover driver.
     That last one also taints the earlier "20-second timeouts on WiFi" figure recorded here, which was measured on a board in that state; the 4x penalty over Ethernet is clean and reproducible, the WiFi instability was not measured as carefully.

     **Why the co-processor board and not the others.** A native-radio chip carries its WiFi stack in the image the cache was sized around; the P4 has no radio, so WiFi arrives as an additional resident subsystem (six tasks, an SDIO driver, RPC machinery, DMA buffers) on a chip whose 128 KB L2 is shared by everything. The difference is not silicon quality but whether WiFi is extra code competing for that cache.

     **Still unexplained: the visible ~1 s LED hiccup.** Cache contention predicts the steady 2.6x, not a stall. Per-task CPU is cumulative-since-boot and cannot show a spike either. So the hiccup remains unmeasured, and the worst-case instrumentation below is still the next step for it specifically.

     **Instrumentation gap to close first — worst-case tick tracking.** Add a per-module and per-scheduler *max* (and ideally a coarse histogram or a "frames over 2x mean" counter) alongside the existing mean, and expose it in `/api/system`. Cheap (one comparison per tick), and it converts "the PO can see a hiccup that no number shows" into a measurable quantity. Without it, every theory below is unfalsifiable from the host side. Reading the P4 console needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` or a UART adapter on GPIO 37/38 (see the dev-loop note in round 4), so serial is not the fast path.

     Then, cheapest first: (a) read the new max/histogram to see whether the cost is uniform or spiky (uniform favors cache contention, spiky favors a blocking call, and the two coexisting is now the leading reading); (b) the P4 cache performance counters for a direct miss-rate read; (c) moving the hot render path to IRAM/internal RAM and re-measuring, which is also a candidate fix rather than only a diagnostic.
  1. **Runtime SDIO re-init of the C6 fails: CONFIRMED a C6 slave-firmware problem (not a guess).** SystemModule now exposes a `wifiCoproc` read-only control (via `platform::coprocessorWifi()` → `esp_hosted_get_coprocessor_fwversion()`), and on the bench it read **`not detected`** at the time (the control now reports **`no version reply`**, and only after a bounded number of attempts): the C6 returns no valid firmware version. **What that means was overstated here.** It was read as the signature of absent / incompatible slave firmware, but the same board later associated and served traffic over that very link while this RPC still went unanswered, so an unanswered version query says the QUERY failed, not that the C6 is absent. The round-3 conclusion below rests on the `sdmmc_card_init failed` re-init evidence, not on this control. Likely a version mismatch on top of that: The host pulled esp_hosted **2.12.9**; Espressif's P4-Function-EV-Board ships its C6 pre-flashed with esp_hosted slave **v0.0.6**, and the **Waveshare NANO is a different board that may carry a different / absent C6 slave image**. The symptom fits: boot inits the host SDIO master fine, but resetting the C6 (GPIO 54) and re-enumerating it as a slave fails (`sdmmc_card_init failed`) because the C6 has no compatible slave firmware responding. **Primary next step: build + flash the version-matched esp_hosted slave firmware onto the NANO's C6.** The slave project is already vendored at `esp32/managed_components/espressif__esp_hosted/slave/` (`sdkconfig.defaults.esp32c6`, `partitions.esp32c6.csv`); `idf.py create-project-from-example "espressif/esp_hosted:slave"` → `set-target esp32c6` → flash. **Caveat / needs PO + bench hardware:** flashing the C6 on the EV board uses an **ESP-Prog wired to the `PROG_C6` header** with the P4 held in bootloader mode (esp_hosted `docs/esp32_p4_function_ev_board.md` §5.2): the NANO's C6-flash path must be confirmed (separate USB? equivalent header? ESP-Prog?), and an ESP-Prog may be needed. An OTA slave-update path exists but needs a *working* link first (chicken-and-egg here). This is a hardware-provisioning task, not application code. Secondary fallbacks if firmware-match doesn't fix it: an esp_hosted option to skip the reconfigure/slave-reset when the transport is already up at boot; a slower SDIO freq or 1-bit mode; verify GPIO 54 reset polarity/timing for the NANO. **(Note: EIM, the build-system v6 ecosystem item, does NOT help here; it's a host-machine installer, unrelated to device-side C6 firmware.)**

  **User lead (2026-07-09): avoid the WiFi teardown/re-init on hosted targets entirely; it may sidestep the slave-reset failure.** A user hit the mirror symptom on a *different* codebase (ESP32-Sveltekit / WLED-MM, not MoonLight — its `lib/framework/WiFiSettingsService.cpp` doesn't exist here): after a clean C6-flash the P4-NANO WiFi worked, but their app's boot-time `WiFi.disconnect(true)` fully tore down the WiFi stack, and the later AP bring-up then failed with `esp_hosted_transport_config: Transport already initialized` / `esp_hosted_init failed!` / `AP enable failed!`. Their working fix: on `CONFIG_ESP_WIFI_REMOTE_ENABLED` (= hosted) targets, **don't do a full stack reset** — keep STA enabled, reconnect *without* a full teardown/re-init, and never call `WiFi.disconnect(true)` in the disconnect callback. **Why this is relevant to us even though the file differs:** our own comment at `platform_esp32.cpp:796` already documents that the esp_hosted transport is set up **once at boot** and is fragile to re-init (`connect_to_slave` = a transport reconfigure that resets the slave + re-inits SDIO and fails on a live link). Our disconnect *callback* is already safe (`wifiEventHandler` on `STA_DISCONNECTED` only sets a flag — no teardown), BUT our **failover path is not**: `wifiStaStop()` (`platform_esp32.cpp:915`) calls `esp_wifi_deinit()`, and the STA-retry / AP-fallback then re-runs `ensureWifiInit()` → `esp_wifi_init()` — the exact deinit→reinit cycle that on a hosted target triggers the GPIO-54 slave reset (round-3 open issue 1's `sdmmc_card_init failed`). So the round-3 failure ("runtime SDIO re-init of the C6 fails") and this user's report may be **the same root cause**: the re-init shouldn't happen at all on a hosted target. **Concrete next step to try on the bench:** guard the teardown/re-init on `platform::hasWifiCoprocessor` (already defined = `isEsp32P4 && hasWiFi`) — on hosted targets, do NOT `esp_wifi_deinit()` in `wifiStaStop()` and do NOT re-`esp_wifi_init()` in `ensureWifiInit()` once the boot-time init is up; instead just `esp_wifi_disconnect()` + `esp_wifi_set_config()` + `esp_wifi_connect()` (STA retry) or `esp_wifi_set_mode(APSTA)` for the fallback, reusing the live transport. This is cheaper than the C6 reflash and independent of #18759, so it's worth trying first once the board boots. If it works, it also removes the slave-reset from the normal failover, not just the AP case. Blocked behind the #18759 boot crash like everything else P4-WiFi, but this is the first thing to try when the board boots again.

#### The cascade into a dead co-processor link aborts the board (2026-09-08)

Bench, MHC-WLED ESP32-P4 shield on `esp32p4rev1-eth-wifi`, no Ethernet cable: the NetworkModule
cascades to WiFi, and the board reboots every ~22 s with `task_wdt: main (CPU 0)` while `IDLE0`
runs. The main task is not spinning, it is BLOCKED: on the P4 every `esp_wifi_*` call is forwarded
over SDIO to the ESP32-C6 by `esp_wifi_remote`, and this shield's C6 never completes the ESP-Hosted
handshake (`E H_API: ESP-Hosted link not yet up` at boot), so `esp_wifi_init` and the calls after it
each wait out their timeout, on the render thread, past the 5 s watchdog. The wait is
esp_hosted's `transport_drv.c` slave-ready loop: 200 ms polls, a slave reset every 50 of them (the
`Reset slave using GPIO[54]` lines at 10 s and 21 s), up to MAX_RETRY_TRANSPORT_ACTIVE = 100
polls, so worst case ~20.0 s in the caller's task, which is ours. Not the HWLOOP/FFT
erratum: audio was ruled out by the watchdog text itself (a blocked main task with idle running,
not a spinning core). Control experiment, same image on the bench P4 (.139, same shield model,
esp_hosted host 2.12.13): its SDIO card init succeeds (`Card init success, TRANSPORT_RX_ACTIVE`),
it never associates either, but the main task keeps ticking, it falls back to its own AP, and it
rejoins Ethernet when the cable returns. The new shield never prints a card-init success: its C6
does not answer the bus (no slave firmware, or one that does not match the 2.12.x host).

Two defects are ours, whatever the C6 carries:

1. **The cascade does not consult the link.** `coprocessorWifi()` in `platform_esp32.cpp` already
   asks the C6 for its firmware version, bounded to two attempts, exactly to detect an absent or
   incompatible slave. The WiFi cascade never asks; it walks straight into `esp_wifi_init`. Gate the
   cascade on that answer (or on `esp_hosted` reporting the link up) and degrade to "no network,
   C6 not answering" as a Network status.
2. **A failing cascade must not abort.** Robustness says degrade visibly, never crash. Even with the
   gate, a link that dies later would hit the same watchdog: the forwarded calls need to run off the
   render thread, or with a timeout shorter than the watchdog, so the worst case is a status line.

Practical today: the shield runs the eth-only image without WiFi, or its C6 gets the ESP-Hosted slave
firmware flashed (the `ships: false` note on the variant in `build_esp32.py` says why that is not
yet reproducible). The Improv script's eth-only rule and the catalog's two firmwares for the shield
are already in place.

**Everything tried on 2026-09-08 was reverted; both P4s now run `esp32p4rev1-eth` and are stable.**
The tree is back to its pre-attempt state except for the catalog, which now lists BOTH P4 firmwares
for the MHC-WLED shield so the installer offers the WiFi variant at all (it listed only the eth one,
which is why the WiFi image could not be picked). What the day established, so the next attempt does
not repeat it:

- **The C6 link is INTERMITTENT on this shield, not simply dead.** After the vendor C6-update tool
  ran (it never completed an OTA: it looped on "Not able to connect with ESP-Hosted slave device"),
  the link came up and the board reached a DHCP lease on WiFi, first attempt, no retries. A power
  cycle later it was back to `sdmmc_send_cmd returned 0x107` and a reboot loop. Any future fix has
  to survive a cold boot, not one lucky session.
- **A patient STA retry (MoonLight's 5 s cadence, ~2 min budget) was implemented and reverted.** It
  never fired on either board: both connected on the first attempt when the link worked, and when it
  did not the board died before the render loop. Keeping untested robustness was not worth the
  surface. The reasoning still holds and is worth redoing WITH a repro: MoonLight retries forever
  and never tears the radio down, and its own comment warns that toggling WiFi on a co-processor
  board forces costly esp_hosted reinit cycles, which is exactly what our AP fallback does.
- **Espressif does not endorse retrying into readiness.** Their esp-hosted troubleshooting puts
  "not able to connect with slave" and SDIO 0x107 down to wiring, pull-ups, signal integrity, or a
  host/slave VERSION MISMATCH: "use the same version for master and slave". Our host is 2.12.13; the
  shield's C6 is on the factory build the vendor tool calls 0.0.6 (target 2.0.17). Updating the C6
  needs Method 2 (direct USB/UART to the C6), since Method 1 needs the very link that is broken.

**A fail-fast gate was tried on the bench (2026-09-08) and REVERTED.** The idea was right and both
signals were wrong. `platform::wifiHardwareReady()` gated the STA and AP init, and it STOPPED the
reboot loop dead: the shield ran 80 s with 0 reboots and 59 ticks, logging "WiFi co-processor link
not up" instead of aborting. But on the bench P4 (.139), whose link demonstrably works, the same
predicate read FALSE at its 26 s cascade and refused WiFi on a healthy board. Two signals were
tried, neither is usable as a readiness test:

- **`ESP_HOSTED_EVENT_TRANSPORT_UP`**, subscribed on the default event loop (both lazily and from
  `ensureNetifInit`, i.e. before esp_hosted's task posts it). Never observed arriving. esp_hosted
  posts through its own `g_h.funcs->_h_event_post` indirection; where that lands was not chased.
- **`esp_hosted_get_coprocessor_fwversion` returning a non-zero version.** Also false on .139, which
  matches what `coprocessorWifi()` already records: on a live link this RPC times out rather than
  answering, which is why that function is bounded to two attempts.

**Not a regression: the RELEASED v4.0.0 image fails the same way on this shield** (bench, same day).
Flashed from the web installer after an erase, it logs `App version: v4.0.0` and then:

```
W (13438) H_SDIO_DRV: Reset slave using GPIO[54]
W (13438) gpio: conflict found for GPIO[54]
E (14988) sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107   (ESP_ERR_TIMEOUT)
E (14988) H_SDIO_DRV: failed to read registers
```

and reboots without ever reaching the render loop. The same v4.0.0 image serves WiFi on the bench
P4 (.139) with no SDIO error and no GPIO 54 conflict, and no MoonLight config on any P4 entry
references GPIO 54 (esp_hosted drives it as the slave reset). The two boards differ in silicon
revision: this shield is chip rev **v1.0**, .139 is **v1.3**. So the C6 side of this shield does not
come up, which is a board/slave-firmware matter rather than anything in our WiFi path, and the
firmware's job is only to degrade rather than reboot.

**The product owner reports this same shield ran WiFi under MoonLight on IDF 5.5**, which makes a
dead C6 unlikely and points at host-side SDIO configuration on 6.1. What was checked (2026-09-08):

- The SDIO data pins are NOT board-preset dependent: they come from the SLOT choice
  (`ESP_HOSTED_SDIO_SLOT_1`, fixed silicon pins), so swapping `ESP_HOSTED_P4_DEV_BOARD_*` presets
  would not move them. The preset mostly moves SPI pins, which we do not use.
- `ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` defaults to **54 on any P4** regardless of preset, so the
  `gpio: conflict found for GPIO[54]` line is esp_hosted resetting the slave twice, not a wrong pin
  from our config. No MoonLight P4 entry references 54.

**Our SDIO configuration is not the difference.** Diffed against a known-good local reference
(`ewowi/FlowFields/sdkconfig.esp32-p4`, an IDF 5.5-era P4 build), every hosted setting is IDENTICAL:
pins (CLK 18, CMD 19, D0-D3 14-17), `GPIO_RESET_SLAVE` 54, `RESET_ACTIVE_HIGH=y`, `CLOCK_FREQ_KHZ`
40000, `SLOT_1`, `4_BIT_BUS`, `RESET_DELAY_MS` 1500, `RX_STREAMING_MODE`. So reset polarity, reset
pin and SDIO clock are all ruled out as differences, and the identical released v4.0.0 image works
on .139 and not on this shield. Same firmware, same config, two boards, two outcomes: what remains
is on the board side (C6 slave firmware, power/strapping, or the SDIO traces on this revision), and
the only MoonLight work left is the degrade-instead-of-reboot fix above.

What esp_hosted's private `is_transport_tx_ready()` reports is the signal the slave-ready loop
itself polls, but it lives in a PRIVATE include dir (`host/drivers/transport`, not in the
component's `pub_include`), so reaching it means either adding that dir to our include path or
asking upstream to export a readiness getter. That is the next thing to try, and it needs the
.139-still-associates control run alongside the shield-stops-rebooting one: the fix is only right
when BOTH hold.

#### Reading the P4's runtime log over USB

The P4-NANO's primary console is **UART on GPIO 37/38** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`), not the USB port, so `ESP_LOGI` / `mm_net` lines are *not* visible over `/dev/cu.usbmodem*` by default — only the ROM boot banner and `std::printf`-to-stdout (which routes to the **secondary** USB-Serial-JTAG console) come through. Two workarounds when you need the runtime log over USB: (a) temporarily set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (note the JTAG endpoint re-enumerates when the app starts, so a reader must reconnect across the drop — `idf.py monitor` handles it; a plain fixed `pyserial` handle dies); or (b) hang a USB-UART adapter on GPIO 37/38. This cost real time during the P4 no-DHCP hunt; the fastest signal there turned out to be a `printf` of the runtime struct (stdout → secondary JTAG console) plus a `git worktree` bisect (build an old commit, flash, check LAN reachability) to prove code-vs-hardware without needing the log at all.

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

### SE16 / LightCrafter power monitoring: sense pins free, module still to build (2026-09-02)

Kind: Feature · Platform: S3 · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086286)

Both boards carry voltage and current sensors. MoonLight records the SE 16 V1's pins
(`MoonBase/Modules/ModuleIO.h`, board presets): **voltage GPIO 8, current GPIO 9**; the LightCrafter's are on [its page](../../reference/hardware/lightcrafter-16.md#power-sensing).

**Still open**, per board: `voltagePin`/`currentPin` in the two device definitions, "Power
monitoring" moving from `planned` to `supported`, the SE16's scale factors (the divider ratio and
shunt value, which are NOT in MoonLight's preset and need the schematic), and a reading checked
against a meter.

Worth doing because it also gives the ADC seam a real bench rig: an on-board analog signal beats a
hand-wired potentiometer. NOTE the pin move is unverified on hardware, both boards being offline
when it was made: confirm LED output still works after the change before trusting it.

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

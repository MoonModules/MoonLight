# Backlog — core

Forward-looking to-build items for the **core / infrastructure** domain (`src/core/`, `src/platform/`, build, CI, network, persistence, UI). The light-domain counterpart is [backlog-light.md](backlog-light.md); items that genuinely span both are in [backlog-mixed.md](backlog-mixed.md). Index + overview: [README.md](index.md). Completed items are removed.

### The update overlay's Cancel button does nothing on a plain OTA (2026-09-14)

`showUpdateOverlay` shows a Cancel button on every firmware install and wires it to
`POST /api/firmware/cancel` (`src/ui/app.js:1479`). Only MoonBase implements that route
(`moonbase/main/moonbase_main.cpp:532`); the application's dispatch table has none, so the request
404s and the `.catch(() => {})` swallows it. The button therefore works during the MoonBase phase of
an install and is inert for a plain app OTA on a device without MoonBase.

A file upload still cancels there, but by a different mechanism: `uploadCtl.abort()` drops the
connection on the line above. So the two install sources behave differently behind one button, and a
URL install on a non-MoonBase device is the case where pressing Cancel does nothing at all.

Two ways out, and the choice is the question. Implement the route in the application, which means an
abort flag the OTA task polls between chunks, the shape MoonBase already uses. Or hide the button
where it cannot act, which is honest but leaves a long download uninterruptible. The first is the
better system and the larger change; the second is a few lines. Found while writing
[Updating firmware](../../how-to/updating-firmware.md), which deliberately does not mention Cancel:
documenting a button that half-works is worse than leaving it undocumented until it is one thing or
the other.

### The audio-sync test waits on the wall clock (2026-09-06)

`test/unit/core/unit_AudioService_sync.cpp` drives the quiet-packet case with `platform::delayMs(1)`
inside a 100-iteration polling loop, so the assertion that `level` reaches zero depends on real
elapsed time and on loopback UDP delivering within that window. It passes today and has not been
seen to flake, but it is the shape that produces a rare CI failure nobody can reproduce, and it
spends real milliseconds in a suite that otherwise runs on a test clock.

The fix is the deterministic UDP seam plus `platform::setTestNowMs`, the same pattern every other
timing test here uses: feed the packet, advance the clock explicitly, assert. Raised by CodeRabbit
on PR #96 and deliberately not taken in that pass: it is pre-existing rather than part of that
diff, and swapping a test's transport is a change that wants its own verification.

## The desktop build advertises no mDNS (2026-09-06)

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

## The desktop build renders as fast as the machine allows (2026-09-06)

Nothing paces the desktop render loop. On a board the LED refresh does; on desktop the loop runs
flat out, measured at 83,000 fps in a container and ~110% of a core per instance. Memory is not the
constraint (about 5 MB), so the practical ceiling on running several instances is one per core.

That matters now the desktop build ships as a container: an array of instances is a plausible
deployment (a fleet under test, several zones of a show), and today each one costs a whole core for
frames nobody consumes. Nothing downstream benefits from more than the output rate: Art-Net, DDP and
E1.31 all clock at their own wire rate, and the preview socket is throttled separately.

**What it costs:** a target-fps control on the desktop platform, defaulting to something like 120,
with the loop sleeping the remainder of each frame. The tick path already measures its own duration
(`tickTimeUs`), so the sleep is a subtraction rather than new measurement. Roughly the shape of the
`--port` flag: a desktop-only concern, invisible on a board.

**Also worth knowing:** it would cut power draw on a single instance too, which matters for anyone
running MoonLight permanently on a NAS or a Pi.


## Distribution

### The arm64 `.deb` is untested on Raspberry Pi OS bookworm (2026-09-13)

**Shipped** (`a229be6d`): `build-linux-arm64` publishes `projectmm_X.Y.Z_arm64.deb` and a tarball,
the container image is a multi-arch manifest, and the SBC route is `apt install`. Verified on a
NanoPi R28S (Debian 13 trixie): installed, ran, and reported to MoonStats as `linux-arm64`.

One piece of that verification was never done, because the bench has no such board. The job runs on
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

Suggested on Discord while bringing up a NanoPi R28S: most Pi users want to write an `.img` to a
card and be running, not to install a toolchain. Armbian's own build tooling supports exactly this
(`armbian/os` carries per-application "extensions", OMV being a small worked example), so the image
is a spin of a maintained distribution plus our package rather than a distribution to maintain.

Wants the arm64 `.deb` above first: with it the extension is roughly "install this package, enable
this service", which is the shape those extensions already have. Without it, the image would have to
carry a source build, which is the thing it exists to avoid.

### OTA upload refuses a normal client: the body must arrive within ~50 ms (2026-09-02)

`POST /api/firmware/upload` answers `400 {"error":"incomplete request body"}` to an ordinary
`curl --data-binary @firmware.bin`, in 37 ms, before reading the image at all.

The streaming branch sets `bodyNeeded` to the whole prefix buffer and then polls for it with a
50-iteration, 1 ms budget (`HttpServerModule.cpp`, the read loop). A client that writes its headers
and pauses before the body, which curl does, trips that timeout and is rejected. The browser path
works because `fetch` hands the whole body to the socket at once.

Worked around by writing headers plus the first 8 KB in a single `sendall` from a small Python
client, after which a 1.97 MB image uploaded in 6.3 s and the device rebooted correctly. So the
transfer is fine; the acceptance test is what is wrong.

Worth fixing because OTA is the only route to a board whose USB port is unavailable, which is
exactly when a firmware update matters most. The fix is to wait for the CONTENT-LENGTH the client
declared rather than for a buffer to fill, and to time out on stall rather than on total elapsed.

### Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64 + Windows x64. Still to add:

- **ESP32-P4** firmware variant — **`esp32p4rev1-eth` (Ethernet-only) shipped**: in `build_esp32.py`'s `FIRMWARES`, the `deviceModels.json` catalog (Waveshare P4-NANO), and CI builds + publishes it to the web installer + releases. **`esp32p4rev1-eth-wifi` now ships too** (2026-08-19): it boots and associates on IDF v6.1-rc1, so it is out of the experimental set in the installer and carries a normal description. Its open defect is throughput, not shipping — see § ESP32-P4 round 3, open issue 0.
- **ESP32-P4 v3.x silicon variant (backlog)** — `esp32p4rev1-eth` is built for pre-v3 P4 (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` + `REV_MIN_0`), because the v6.1 IDF default (v3.1) refused to boot on the bench/field v1.x P4 and rev <3.0 vs >=3.0 are "huge hardware difference" (one binary can't cover both). **DONE (2026-08-19): `esp32p4rev3-eth` and `esp32p4rev3-eth-wifi` ship** (`REV_MIN_300`, which covers v3.0-v3.99), reusing the rev1 board fragment so the partition table and EMAC config stay in one place. **Still open: neither has ever been booted** — both bench boards are v1.3 engineering samples, so the rev3 images are flagged experimental in the installer and need a v3 board to verify. Espressif does not recommend v0.x/v1.x for new designs, so a board bought today is v3.x and needs these.
- **Teensy 4.1** — toolchain-file build, `.hex` for Teensy Loader.
- **Raspberry Pi** — ARM64, cross-built or native.
- **macOS code-signing (Developer ID)** — the release `.dmg` is now ad-hoc signed, which turns Gatekeeper's outright refusal into the "unidentified developer" prompt a user can accept via right-click Open. A paid Developer ID certificate plus notarization would drop that prompt too.
- **Windows code-signing** — drops the SmartScreen warning on first run of `MoonLight.exe`. Same shape as macOS signing; needs an EV / OV code-signing certificate (Microsoft Trusted Signing is the cheapest current option). Until then, the README notes the SmartScreen prompt.
- **Live RMII Ethernet reconfigure** — runtime PHY/pin config shipped (`ethType` + pin controls in NetworkModule, per-board defaults in `deviceModels.json`, `platform::setEthConfig`/`ethInit` dispatch). W5500 (SPI) on S3 applies **live** — `ethStop()` tears down the SPI bus and `ethInit()` re-runs on the next `loop1s()` with no reboot. RMII (classic/P4 internal EMAC) still saves config and asks for a restart to apply, because the EMAC bring-up is fiddlier to hot-cycle cleanly. Make RMII live too: a hot `esp_eth_stop` + EMAC/netif teardown + re-init on config change, matching the W5500 path, so every interface honours the no-reboot principle.
- **GCC below 16 needs four warnings demoted, and nothing exercises those versions** - `-Wnull-dereference`, `-Wrestrict`, `-Wstringop-overflow` and `-Wformat-truncation` fire on provably correct code from GCC 12 through 15 (five of the twelve inside libstdc++ and glibc headers, unreachable from our source), so CMakeLists demotes them to non-fatal there and keeps them fatal on 16+. That unblocks CI and from-source builds on Debian and Raspberry Pi OS alike, but it is a suppression, not an understanding: nobody routinely compiles with 12-15, so a REAL instance of one of these on those versions is now a warning nobody reads. Revisit when the runner's default GCC reaches 16, at which point the whole block can be deleted.
- **Installer UX polish** — clear "Pre-release (beta)" warning on RC/latest picks, yank-by-asset-tag instead of yank-by-release-deletion.
- **Offer MoonLight/MoonLight as a library** — a downstream sketch where another firmware/app consumes the light pipeline (or a subset) as an embeddable dependency rather than running the whole binary. `library.json` is already a PlatformIO *library* manifest, so the seed exists. When this is designed, give it a small public **identity surface**: one runtime constant the consumer reads (a `kProjectName`, likely a `ProjectInfo` bundle of name + version + url) that the network wire-strings (ArtNet/E1.31 source-name + CID), the UI banner, and any "About" string all *derive from* — the one place a consumer queries "what am I embedding." This is the genuine home for the name-centralisation that the rename ([the MoonLight plan](../present/Plan-20260922%20-%20MoonLight,%20from%20v5.0.0%20to%20the%20rename.md)) deliberately *didn't* do: the rename is a one-time sweep (a constant would just split it), but a library consumer references the identity ongoing and widely, which is the test a constant must pass. Build it *then*, against the real library API, not speculatively now.
- **HTTP: a request whose headers or body arrive a few ms late is dropped, intermittently
  (2026-08-20).** `handleConnection` runs SYNCHRONOUSLY inside `tick20ms`, so its waits are kept
  short to protect the render loop: a freshly accepted connection gets **~5 ms** for its request
  headers (`HttpServerModule.cpp`, the `empties > 5` bail) and **~50 ms** for a body
  (`empties > 50`). A client that misses either budget gets no response at all (the header case
  closes the socket: *"Remote end closed connection without response"*, seen client-side at 28 ms)
  or a `400 {"error":"incomplete request body"}`.

  **Observed:** the MoonLive live scenarios fail roughly **1 run in 3** on the S3, always as a
  failed `POST /api/file` that cascades (`module not found` for every step depending on that
  script). A 40-write burst failed 4/40. Not reproduced on the classic (3/3 clean), so it may be
  S3-specific or simply load/timing-dependent.

  **NOT render load — that theory is disproved.** Disabling the heavy driver took the tick from
  2889 us to 472 us (fps 346 → 2118) and made it WORSE: 14/40 failures instead of 4/40. So a
  faster tick means more accept batches per second and more chances to hit the window, which
  points at the budget itself rather than at contention.

  **Why it matters:** any client on a busy or distant network can lose a write with no useful
  error, and the UI's own file saves ride the same route. The fix is not simply a longer wait —
  that would stall the render loop, which is what the budgets exist to prevent. It wants the
  connection handling moved off the render tick, or a state machine that parks a partial request
  and resumes it next tick instead of dropping it.

- **Desktop backend: a control-arena write is not seen by a second `run()` of an already-compiled
  program (2026-08-20).** The live-edit path is "move a slider, the next frame reads the new arena
  byte, no recompile". On the host JIT the first render is correct, the write lands (the arena
  reads back `232,3` = 1000 at the control's own `offset`), and a second `run()` of the same
  compiled program still renders the OLD value.

  **Ruled out — do not re-investigate these:**
  - Not `LoadCtrl16` missing: it is implemented, lowers to `a.load16`, and `load16` exists in all
    three assemblers including `platform/desktop/moonlive_asm_host.cpp` (`ldrh`, immediate scaled).
  - Not uint16-specific: a `uint8_t` member behaves identically.
  - Not `defineControls()` or the control sink: a class with an unrelated second function fails the
    same way, and one with no members at all (a literal in `tick`) does too.
  - Not the arena address or offset: `controlSlot(dc[0].offset)` is the byte the write reaches.

  **A large part of the original symptom was a TEST BUG, not an engine bug.** `run()` without an
  entry name starts at the BLOCK START — the first function compiled — which in a class with
  `defineControls()` is `defineControls`, not `tick`. So the script rendered nothing and looked
  like a broken control read. Naming `kEntryTick` makes the initial render correct in every
  variant tried (with/without `defineControls`, with an unrelated second function, `tick` first or
  second, uint8 and uint16). Only the re-read after a slot write is still wrong.

  **Reproduce** (in `unit_moonlive_fill.cpp`, which has `kCtrlTable`/`kSys` to hand): compile
  `class T { int big = 5; defineControls() { addControl("big", big, 0, 1000); } tick() {
  setRGB(0, big, 0, 0); } }`, `run(..., kEntryTick)` → 5, write the slot to 7, run again → still 5.

  **Impact: desktop only, nothing ships broken.** A wide control is hardware-verified on both ISAs
  (S3 Xtensa and S31 RISC-V drive ember's `cycle` to 2000 and back). What is missing is DESKTOP
  coverage of the live-edit loop — the path users touch most — so no host test can pin it and the
  next regression there would surface only on a board. Add the runtime assertion together with the
  fix, not before: asserting the current behavior would encode the bug.

- **ESP32-P4 panics with `Cache error` every few minutes, pre-existing** (2026-08-19): the bench
  P4 (Waveshare P4-NANO, `esp32p4rev1-eth`) reboots roughly every four minutes while IDLE, with
  `Guru Meditation Error: Core 0 panic'ed (Cache error)`, sometimes followed by an
  `Illegal instruction` and a `CHIP_LP_WDT_RESET` on the way down.

  **What the device reports about its own restarts:** `bootReason` alternates between `PANIC` and
  the watchdog. The serial shows why: the `Cache error` panic sometimes completes its dump and
  reboots cleanly (`PANIC`), and sometimes the panic HANDLER itself then dies with an
  `Illegal instruction` before it finishes, leaving the low-power watchdog to reset the chip
  (`rst:0x10 CHIP_LP_WDT_RESET`, `W boot.esp32p4: CPU has been reset by WDT`). So a WDT boot reason
  here is a SYMPTOM of the same fault, not a second one: nothing is hanging a task. Worth checking
  `bootReason` over several restarts rather than one, because either value can appear.

  **Not caused by MoonLive, and not a regression.** Established by two independent checks: the board
  runs the DEFAULT module tree (GridLayout + NoiseEffect, no MoonLive module at all, so none of that
  code executes), and a firmware built from a clean `main` crashes identically. The filesystem is
  healthy throughout: LittleFS mounts, `/.config` lists, and writes succeed.

  What is known about the fault site: `MEPC` resolves to `pxPortGetCoprocArea`
  (`freertos/.../portable/riscv/port.c`) reached from `rtos_int_enter` (`portasm.S`), which is FreeRTOS's
  RISC-V coprocessor-context save on INTERRUPT ENTRY. That is a symptom of something faulting inside
  an ISR context rather than a bug in the kernel itself, and the P4 is the only RISC-V target with a
  coprocessor, which is why no other board shows it. The prior art at
  `Plan-20260718 - MoonI80 lapping-v2 clock-oracle ring` (in the plans archive)
  is a DIFFERENT cause with the same panic name (an ISR reading PSRAM while a flash write disabled
  the cache, fixed with a `spi_flash_cache_enabled()` defer guard) and is worth re-reading first:
  the same shape on another ISR would present exactly like this.

  Next step is a decoded backtrace from the full panic dump rather than the register line, then
  bisecting which ISR is live (audio, the LED driver, ethernet) by disabling each. Two hypotheses
  were tested and falsified during the session that found it, so start from evidence.

  A SEPARATE P4 boot loop, also found that session, WAS a real regression and is fixed: the MoonLive
  engine had grown to 1440 bytes held by value in every scripted module, and `registerType`'s `T
  probe` constructs each module on the main task's stack at boot. Re-indexing its seeded-member table
  by member rather than by arena byte took it to 784 bytes and the board boots clean.

- **ESP32-P4 DHCP hostname not shown by the router (recheck later)** — the device sets its DHCP hostname (option 12 = `deviceName`, default `MM-XXXX`) in the `ETHERNET_EVENT_CONNECTED` handler, verified working on two boards: the S3 over WiFi (router shows `MM-70BC`) and the Olimex over RMII Ethernet (`MM-BD3C`) — the *same* `ethEventHandler` code path the P4 uses. Yet the bench P4 (Waveshare P4-NANO, RMII) still shows as blank/"Unknown" in the GL.iNet client list, while serial confirms `set_hostname` succeeds with no error. Two unconfirmed suspects, neither our logic: (1) the router holds a **sticky lease** for the P4's MAC and won't relearn the hostname until it fully expires (the per-client "forget" isn't exposed in this GL.iNet UI, and a plain reboot didn't clear it); (2) a P4-specific IDF netif quirk serializing option 12 differently on the newer P4 Ethernet path. Since the shared code path is proven on two other boards, this is not treated as a code bug. Recheck after the P4's lease naturally expires, or on a different router, before spending more on it.

### DevicesModule — interop plugins + the command half (discovery shipped)

DevicesModule discovers via **passive UDP presence** (UDP 65506) feeding a [`DevicePlugin`](../../src/core/util/DevicePlugin.h) seam (shipped: MoonLight + WLED plugins). mDNS is advertise-only so MoonLight appears in the native WLED apps + Home Assistant; the WLED-app interop (list + live color + brightness control) is shipped too. What remains is *growth on the seam*, each piece additive (one plugin file, no core change):

- **More discovery plugins** — ESPHome, Tasmota, Hue (*hub-shaped*: a bridge whose Zigbee bulbs are children behind it, with link-button auth). Each is a new `DevicePlugin` declaring its `discoveryPort()` + classifying the datagram (or, for a system that only does mDNS, a re-introduced advertise-side browse scoped to *foreign* services only — never the ones we advertise). Hue is the canonical "more than a flat device" case the seam is shaped for. (Note: Hue *control* already ships as an **output driver**, see [HueDriver](../../moonmodules/light/moxygen/HueDriver.md) — bulbs as effect pixels; the driver also *lists* its bridge in DevicesModule with the color-light count. Two complementary follow-ups remain: (a) auto-fill the driver's bridge IP from discovery so the user doesn't type it (the mDNS-browse plugin above); (b) **pair once, not per driver** — pairing + the app key currently live on each HueDriver, so two drivers on one bridge pair twice. The clean end-state moves the bridge identity (IP + key + Pair button + light list) into DevicesModule and makes HueDriver a pure output that reads the paired bridge by IP — do this together with the discovery plugin, since both hinge on DevicesModule owning the bridge.)
- **The command half** — `DevicePlugin::command()` (+ per-plugin capability/auth), so MoonLight can *control* a discovered foreign device, not just list it: set WLED brightness via its JSON API, a Hue resource via the bridge's authenticated CLIP API, a Tasmota via `cmnd`. Built when a control consumer exists; the discovery seam is already shaped to accept it (incl. hub plugins). This is the **multi-ecosystem selling point** — one UI controlling WLED + ESPHome + Hue. Commands split by need (the rule, not "all REST"): must-arrive config over REST; latency-critical sync over UDP (~0.5–1 ms vs REST's 10–50 ms — REST would visibly de-sync).
- **Live peer state** — a discovered peer's brightness / on-off shown in our list, refreshed by polling its REST `/json` after discovery gives the IP (discovery = UDP/mDNS, state = REST). The read-side complement to the command half.
- **Non-IP transports (board-gated, far future)** — Tasmota-MQTT / zigbee2mqtt need an MQTT client; **direct Zigbee/Thread** (S31/C6/H2 802.15.4 radio) makes MoonLight the *hub itself*, driving bulbs over the mesh with no gateway — the standout differentiator, the biggest lift. Same plugin philosophy, a transport addition + board gate.

Full design + the reasoned transport split: `Plan-20260629 - UDP device discovery + mDNS advertise-only` (in the plans archive).

## MoonBase follow-ups

MoonBase v1 ([MoonBase](../../explanation/architecture/moonbase.md))
ships exactly one action: install firmware (upload + URL). The name is deliberately broader than
"recovery", these are the candidate next actions, each solving something only a separate boot
image can solve. The budget rule from the partition table applies to all of them: the 896 KB slot
has ~150 KB headroom, sized for one new *component*, so each action must earn its bytes (a few KB
of code is fine; a new IDF component is the expensive kind).

- **Factory reset**: erase the filesystem (and optionally NVS) from MoonBase's page: recovers a
  device whose config crashes the app on boot, without a USB cable.
- **Boot with config disabled**: one-shot flag the app reads at startup to skip loading
  `/.config`: diagnose "is it my config or the firmware?" without erasing anything.
- **WiFi re-provisioning**: edit the stored credentials from MoonBase's page (today it only
  *reads* them; the AP fallback plus the app's provisioning already covers most of this).
- **Publish SHA-256 sums with release assets**: the Defender false-positive guidance can only
  say "re-download over HTTPS" today; a checksums file per release lets a user verify a
  quarantined installer before restoring it. One sha256sum step in the release staging.
- **Streamed HTTP responses drain across ticks**: serveFileContents/serveHlsFile block the
  render tick for the whole transfer (a slow player fetching a ~1 MB HLS segment every second
  can hold it for hundreds of ms), and fsReadAt reopens the file per 1 KB chunk. The named fix
  is the writeSome/drain pattern preview already uses, plus a larger chunk.
- **Re-entrant network bring-up**: NetworkModule::setup() (netif create, driver install, the
  eth→WiFi→AP cascade) runs once at boot and crashes if re-run live, so it opts out of the
  live config apply (`appliesConfigLive() = false`) and a restored NetworkModule.json waits
  for the next boot. The real fix is bring-up that reconciles instead of re-creates; until
  then the restore dialog offers the restart.
- **Config backup / restore, tiers 2+3**: tier 1 (browser-side bundle over the file API, with
  the rename map and restore report) ships in the File Manager. Remaining: tier 2, a
  single-archive device endpoint (one request instead of a walk); tier 3, restore hosted on
  MoonBase's page, the migration answer for future partition-table moves.
- **Restore clones a device's IDENTITY along with its config** (found answering
  [#76](https://github.com/MoonModules/projectMM/issues/76), 2026-09-04). Backup bundles every
  file including hidden `.config`, and restore writes them all back unfiltered, so restoring one
  device's bundle onto another copies four things that must differ per device:

  | field | why it must differ |
  |---|---|
  | `deviceName` | the single network identity: mDNS hostname, SoftAP SSID and DHCP hostname all derive from it (`SystemModule.h`). Twelve clones all answer to `<name>.local`, resolution goes non-deterministic, MoonDeck's device list collapses to one row |
  | WiFi `password` | travels in the bundle (the UI button warns), so a shared or attached backup leaks it |
  | a static IP | if set, every clone claims one address |
  | `universeStart` and the Art-Net/DDP window | exactly what must differ per device in the light-pole case #76 describes: cloned, every pole shows the same thing |

  This turns "clone this pole to the other eleven" from the feature the issue wants into a trap.
  **The fix is small and is a prerequisite for the recipe idea rather than a separate job:** restore
  treats identity as per-device, either skipping those fields or prompting once with the target's
  current values prefilled. Worth deciding whether the bundle should carry the secrets at all, or
  keep an identity section the restoring device is expected to supply.
- **A backup is a file bundle, not a recipe** (#76 step 2): nothing binds it to a `deviceModel`, so
  restoring a Dig-Quad bundle onto an S3 writes pin maps that do not fit the board. The 28 profiles
  in `deviceModels.json` are the missing half.
- **Firmware downgrade guard**: MoonBase installs whatever image it is given; a version display
  (read from the incoming image's app descriptor) before flashing would make an accidental
  downgrade visible.
- **Hardware diagnostics**: chip/flash/PSRAM identification and a minimal pin tester, for
  triaging a board that misbehaves under the full app.
- **Ethernet: shipped for classic RMII (2026-08-26)**. MoonBase reads the eth wiring from the
  same config file as the credentials (ethType gates it) and runs ONE interface at a time in
  the app's own preference order (eth, else WiFi, else AP), so the browser keeps the address
  the app had. Still open here: the P4's IP101/managed-component PHY and the S3's SPI W5500,
  which matter only if MoonBase ever goes beyond the 4 MB classics.
- **Static IP for MoonBase**: MoonBase always uses DHCP; a venue network without a DHCP server
  (fixed-address rigs exist) would reach the app (static `addressing`) but not MoonBase. Read
  the addressing block from the same config scrape when a venue actually asks for it (the app
  itself applies static addressing to eth already).
- **MoonBase as the only update mechanism, all boards** (PO, 2026-08-26): would delete the app's
  whole in-place OTA path (a real subtraction) and grow every app slot, with the stronger
  power-fail story everywhere. Three deciding factors first: MoonBase Ethernet beyond classic RMII (the P4 has no
  WiFi of its own and uses a different PHY; the S3 uses SPI W5500), a migration that does not lose config (backup/restore above), and accepting
  ~45 s of visible downtime per update where dual-OTA installs in the background. Trigger:
  MoonBase proven in the field on the 4 MB boards.

## Provisioning and live-reconfig gaps (bench, 2026-08-26)

Both surfaced while bringing up MoonBase Ethernet on the migrated Olimex; neither is MoonBase's.

- **A migrated device is not fully provisioned until the deviceModel catalog push is re-applied.**
  After the MoonBase table migration the board ran for days with `deviceModel` empty and
  `ethType` 0: Improv/AP provisioning restores WiFi credentials only, and nothing tells the user
  the catalog half (eth wiring, per-board settings) is missing. Candidates: MoonDeck re-pushes
  the catalog on discovering a device whose deviceModel is empty, or the UI badges the state.
- **`ethType` does not apply live.** Setting it to 1 over the API (cable in, pins valid) left
  Ethernet down; the same config brought it up at the next boot. Every setting applies live is
  a core principle (architecture.md, Live reconfiguration), so the eth init path is missing from
  the control's apply/build-state sweep.

## ESP32 performance and memory

### Size estimates for unbuilt features (reference)

Estimates, not measurements, so they live here rather than in [performance.md](../../reference/performance.md) which carries measured numbers only. (The 4 MB flash-budget investigation these once fed is resolved: MoonBase's single-app-slot layout grew the classic app slot to 2496 KB, see architecture.md § MoonBase.)

| Feature | Est. | Rationale |
|---|---|---|
| Mozilla cert bundle trimmed | −40 KB | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` keeps common roots only. `_NONE` saves ~50 KB but breaks TLS. |
| Static IPv6 | +20 KB | lwIP IPv6 component (off by default). Only if a deployment needs it. |
| WebSocket TLS (`wss://`) | ~0 KB | Reuses linked mbedTLS; certificate handling adds <5 KB. |

### E1.31 multicast receive (IGMP join)

NetworkReceiveEffect accepts E1.31 via unicast only — the same scope MoonLight ships. Multicast senders address the per-universe group `239.255.{universe_hi}.{universe_lo}`, which a receiver must join via IGMP. **The platform half of this now exists**: `UdpSocket::joinMulticast()` shipped with the WLED audio-sync work (2026-08-29) and is used in anger there, on both desktop and ESP32. What is left for E1.31 is the join-per-accepted-universe bookkeeping, not the socket support. Add when a multicast-only sender actually shows up on a bench; until then the spec documents "point sACN senders at the device's IP".

**The SEND half is the more interesting one, and it's the honest scale answer.** sACN puts the universe number *in the group address*, so with **IGMP snooping** the switch filters per-universe in hardware — each node's NIC sees only the universes it joined. That is broadcast's send-once efficiency *plus* unicast's selectivity, and it's the one addressing mode that beats per-node unicast when many nodes want overlapping universes. `NetworkSendDriver` already knows its universe range, so the group address is a pure function of `universe_start` — a small increment, not a redesign. **The catch that keeps it off the default path:** without IGMP snooping the switch floods multicast exactly like broadcast (and on WiFi it goes out at the lowest basic rate to every station), so it degrades straight back into the starvation regime — and firmware cannot detect whether the switch snoops. So: unicast stays the portable default; multicast is the opt-in optimization for a network the user controls. Do the receive join and the send group together when it lands.

### A scripted module's DIMENSION chip still comes from its type, not its script

`writeModuleJson` emits an instance's `tags()` (HttpServerModule.cpp), so a MoonLive module shows
the emoji its loaded script declares. Its DIMENSION does not follow the same path: `/api/types`
carries `dim` per TYPE, captured at boot from a probe with no script loaded, and the module state
carries no `dim` at all. So a script declaring `int dimensions() { return 3; }` renders as 🟦 on the
card while behaving as D3 through `Layer::extrude`: the behavior is right and the chip lies.

The fix is not a line in the serializer. `MoonModule` deliberately has no `dimensions()`:
`ModuleFactory::registerType` detects one with `if constexpr (requires ...)` on the CONCRETE type
precisely so the light-domain `Dim` enum stays out of core (ModuleFactory.h). Core holds a
`MoonModule*` when it writes state, so emitting a per-instance dim means giving `MoonModule` a
virtual that returns a byte, which puts a light-domain concept on the domain-neutral base for the
sake of a chip.

Options, cheapest first: a `uint8_t dimByte()` on MoonModule defaulting to 0 (the enum stays in the
light domain, only the number crosses, mirroring what the probe already does); or leave it and
accept that a scripted module's dimension chip reflects its type. Worth doing when someone is
annoyed by the wrong chip, not before.

### British spellings and em-dashes predate the prose gate (319 files)

`check_prose.py` reports on ADDED lines only, so the American-spelling rule has been enforced from
the day it landed forward, and everything written before it was never swept. 118 files still carry
`colour`, `centre`, `behaviour`, `recognise`, `initialise` and friends, in comments and in a few
identifiers.

**Em-dashes are the same story and the larger half**: 10,326 of them across 319 files under `docs/`,
against the same rule and missed for the same reason. They are worse than the spellings to sweep
mechanically, because the right replacement depends on the sentence (a colon where it explains, a
comma for an aside, a full stop between two clauses), so a blind substitution produces prose nobody
proofread. Count them per file and do the biggest offenders by hand.

The gate keeps it from growing, so this is a one-time sweep rather than a leak. It is deliberately
NOT folded into a feature branch: a whole-repo rename touches more files than any review can read,
and mixing it with real changes is how a review gets declined for size. Do it as its own commit,
mechanically, with the gate run over the whole tree afterwards rather than over a diff.

Identifiers first and separately: a rename changes an API, where a comment does not. `centre()` in
the shipped `crosshair.mle` was one and is already fixed, since a shipped teaching script is the
highest-value case.

**Now caught going forward for scripts too**: `.mle`/`.mll`/`.mlm` joined the checker's SUFFIXES
(they were unchecked, which is how `colour` reached ten shipped scripts), so the library cannot
drift again.

### Multicast discovery has no fallback when the group never arrives

`DevicesModule` announces presence on the multicast group and every device always joins it, so peers
find each other whatever each has set `wledCompatible` to. The docstring states the worst case as
"without IGMP snooping a switch floods multicast exactly like broadcast", i.e. it degrades to the
thing it was avoiding. **Field reports from other projects say the real worst case is stronger:
multicast sometimes does not arrive at all**, most often when the path bridges physical media
(a WiFi client talking to a wired one), where consumer access points and switches handle group
membership least well. Bursty delivery is reported too, packets arriving in clumps rather than at
the send cadence.

That failure is silent here: peers simply never appear, and the card shows an empty list that looks
exactly like a healthy single-device setup.

**A periodic broadcast probe is the obvious fix and it is the wrong one.** The trigger would be
"no peer seen for N seconds", which is the PERMANENT state of every device that has no company,
and most installs are a single device. Every one of them would broadcast forever, which is the
chatter multicast was chosen to avoid, and worst on exactly the WiFi networks already struggling.
A device cannot tell "the group is broken" from "I am alone" by listening: both are silence.

So the fallback needs a trigger that is not silence.

**The shape that works: try broadcast because it is cheap, rather than waiting for silence to mean
something.** Announce on multicast, listen on BOTH, and let evidence decide. What is detectable is
not "I heard nothing" but an ASYMMETRY: a peer heard over broadcast that never arrived over
multicast proves the group is broken, where silence proves nothing. A device that sees that adds
the broadcast copy to its own announcements and keeps it.

That needs a bootstrap, because two devices both waiting for evidence never produce any: each is
quiet on broadcast, so neither gives the other the packet that would settle it. **Announce on both
for a bounded window after boot, then settle to multicast alone unless broadcast proved necessary.**
The chatter is one-time and bounded rather than permanent, which is what makes it affordable on the
WiFi networks this exists for.

The control that follows is a mode rather than a compatibility flag: `multicast` (quiet, today's
default), `multicast + broadcast` (what `wledCompatible = true` does now, and what WLED apps need),
and `auto` (the rule above). Unicast is deliberately absent: discovery is one-to-many, and there is
no address to unicast to before anything has been discovered. Document broadcast as the
WLED-compatible mode rather than naming the flag after WLED.

**It stays on DevicesModule rather than moving up to NetworkModule.** Three places in the codebase
send to a group, and only one of them is ours to choose: discovery uses MoonLight's own
`239.255.x.x`, audio sync uses WLED's `239.0.0.1`, and sACN send uses the universe-derived
`239.255.{hi}.{lo}` that E1.31 mandates. A network-level "prefer broadcast" switch could not move
the latter two without breaking the protocols they speak, so a control there would imply an
authority it does not have.

**Can a device self-test by hearing its own multicast? Mostly no, and the reason is worth writing
down.** `IP_MULTICAST_LOOP` is never set anywhere in the platform layer, so it sits at the stack
default, which is ON for both lwIP and BSD sockets. A device therefore hears its own multicast
delivered internally, before the packet ever reaches the wire, so the test passes on a network where
multicast is entirely broken. Turning loopback off makes hearing yourself meaningful, but then the
test demands that the switch or AP reflect group traffic back to the sending port, which plenty
deliberately do not do, so healthy networks would fail it.

What the self-test IS good for is the negative case: with loopback on, NOT hearing your own
multicast means the local join or socket is broken, which is a real and actionable fault. It
diagnoses the device, not the network. The network half still needs a peer, because "did my packet
cross this switch" cannot be answered with nothing on the other side.

**Unverified:** the lwIP loopback default above is read from the socket semantics and our own code,
not measured on a board. Confirm on an ESP32 before building on it.

**Land the diagnosis whatever else happens.** Reporting "joined the group, no peers seen" on the
card costs almost nothing and turns a silent failure into a legible state, and it is useful even if
the auto mode is never built.

### ESP32 UDP receive is bounded by PACKET COUNT, not bytes

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

### WiFi ArtNet performance (pending investigation)

128×128 WiFi ArtNet measurements exist (see [performance.md](../../reference/performance.md) "ArtNet over WiFi" and "Build-variant WiFi comparison"). Remaining matrix:

- WiFi STA 64×64 (4K LEDs, 24 universes)
- WiFi STA 32×32 (1K LEDs, 6 universes)

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **prefer wired Ethernet for any ArtNet workload on classic ESP32** — the default `esp32` build carries both stacks, so Ethernet is available even when the original measurements were taken on the old `esp32-eth-wifi` variant.

### Network round-trip test — drop/reorder measurement (deferred)

`moondeck/scenario/run_network_roundtrip.py` measures desktop→device→desktop **latency and jitter** per protocol (ArtNet/E1.31/DDP) by timing how long a sent color takes to appear in the device's preview stream. It deliberately does **not** measure per-frame **drops or reorder**, because the path can't track individual frames cleanly: `NetworkSendDriver` re-clocks at its own fps (decoupled from receive) and `NetworkReceiveEffect` holds-last-frame, so frames don't pass through 1:1 — a sequence number embedded in frame N may be re-sent 0, 1, or several times downstream. The min/median/max spread the test already reports *is* the jitter signal (it surfaced multi-second outliers on the classic ESP32). To measure true drop/reorder, the firmware would need a sequence-faithful echo path (e.g. a `NetworkReceiveEffect` echo mode that re-emits each received frame 1:1 back to the sender, bypassing the fps re-clock), then the desktop could match sent↔received sequence numbers. The test's docstring lists this under "extend later" alongside per-frame sequence matching and the device→device chain.

### Async ArtNet send — decouple the wire from the render tick (PSRAM-only)

The ArtNet send is synchronous: `NetworkSendDriver::loop()` blasts ~97 universes (a 48 KB frame at 128×128) inline, and the per-universe `send()` blocks on lwIP TX backpressure — the netif/EMAC (or WiFi) drivers throttle to wire throughput. Measured on hardware: **~35 ms over Ethernet, ~90 ms over WiFi**, charged straight to the render tick, so ArtNet alone caps the Olimex at ~15 FPS and the S3 (WiFi) at ~7 FPS at 128×128. This is a transport throughput limit, **not** something a non-blocking socket can shed — verified that neither `O_NONBLOCK` nor `MSG_DONTWAIT` makes lwIP return early for UDP (the block is below the socket API; both flags drop zero packets and cost the same ~35 ms). The earlier "non-blocking recovered it to ~2 ms" reading was a transient external condition (the receiver/switch draining the burst freely in one window), unreproducible under steady load with the exact firmware.

The real fix is a **dedicated send task**: `loop()` snapshots the corrected frame into a handoff buffer and signals the task; the send task drains it to the wire at its own pace while the render task continues. The tick stops paying the ~35–90 ms — render runs at its own rate (~30 FPS on the Olimex), ArtNet streams independently at whatever the link sustains.

**Gate this on `platform::hasPsram`. It does not fit non-PSRAM boards at the grid sizes where it would help** — the math is hard:
- The handoff buffer must hold one full frame: **48 KB at 128×128** (16384 × 3), plus a ~4 KB task stack.
- The Olimex (no-PSRAM) at 128×128 runs at **~46 KB free heap, ~18 KB largest contiguous block** (measured). The 48 KB buffer exceeds *both* the total free heap and (by far) the largest block — it can't allocate at all, the same fragmentation cliff the paged MappingLUT just had to work around.
- A double-buffer (so the task reads frame N while render writes N+1) doubles it to ~96 KB — even more out of reach.
- At 64×64 the frame is only 12 KB and *might* fit, but at 64×64 the synchronous send is already fast enough that ArtNet isn't the bottleneck — so the task buys nothing where it's affordable on no-PSRAM.

So the PSRAM gate isn't conservative; it's a hard requirement. PSRAM boards (S3/S2, Olimex-with-PSRAM variants) have megabytes for the handoff buffer via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`; non-PSRAM boards keep the synchronous send and the documented "use Ethernet / smaller grid for high FPS at large grids" guidance ([NetworkSendDriver.md](../../moonmodules/light/moxygen/NetworkSendDriver.md)).

Acceptance criteria: `if constexpr (platform::hasPsram)` (or a runtime `hasPsram()` check) selects the async path; the buffer lives in PSRAM; the send task pins to the core opposite the render task (the same pattern as the shipped render↔encode split's worker). Non-PSRAM keeps `loop()`'s inline send unchanged. The single handoff buffer needs an explicit ownership contract, exactly like the shipped render↔encode split's `outputBuffer_`: the render path may write frame N+1 only after the send task signals it has finished reading frame N (a binary semaphore/notification is the handoff), so the two never touch the buffer at once. When the sender still owns the buffer at the next render tick, the render path drops (or coalesces onto) that frame rather than overwriting a live read — a dropped frame is acceptable, a torn one is not. That single-buffer contract is the minimal shape; a second buffer (double-buffering) or a ring of frames is only warranted if measurement shows the drop rate hurts, or a second consumer appears.

### `esp32-eth` slow Ethernet bring-up vs `esp32-eth-wifi` (investigation)

On Olimex ESP32-Gateway flashed with `esp32-eth`, Ethernet sometimes takes **a minute or more** to acquire a DHCP lease at boot. The same hardware flashed with `esp32-eth-wifi` brings Ethernet up in seconds. The B1 Idle-recovery fix in `src/core/system/NetworkModule.h` masks the symptom (status correctly transitions to "Eth: <ip>" once the lease arrives), but the underlying slow bring-up is a real performance regression on the eth-only build.

What we know:
- `build/esp32-esp32-eth/sdkconfig` and `build/esp32-esp32-eth-wifi/sdkconfig` are **byte-identical** (3,617 lines each, `cmp -s` confirms). So lwIP buffer pools, DHCP timeouts, and Ethernet driver settings are the same.
- Same hardware (Olimex ESP32-Gateway Rev G), same RMII pin/clock config (`EMAC_CLK_OUT` on GPIO17), same `ethInit()` code in `src/platform/esp32/platform_esp32.cpp`.
- The only difference at link time: `esp32-eth` passes `EXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_coex` to ESP-IDF (see `moondeck/build/build_esp32.py:31`).
- `esp_coex` (WiFi/Bluetooth coexistence) was an early hypothesis: even though Ethernet doesn't share the radio, `esp_coex`'s init might warm a shared clock path that helps Ethernet auto-negotiation, and the eth-only build excludes it. **Disproven — see below.**

**Firmware is ruled out (the evidence is contradictory across reboots with the *same* build, which by itself proves build-independence).** Over one session, on the same Olimex + cable:
- `esp32-eth-wifi` (keeps `esp_coex`) → flapped: `Ethernet link up` → `link down` repeating, never reached DHCP.
- `esp32-eth` (excludes it) → on one flash **came up immediately and worked**; on a later flash **flapped the same way**.

So the *same* eth-only build both works and flaps at different times, and the eth-wifi build flaps too. The instability does **not** track the firmware build — it's intermittent. That kills the `esp_coex` theory and any WiFi-interference theory. It also confirms our code isn't the cause: `mm_net: Ethernet link up/down` is logged straight from the ESP-IDF `ETHERNET_EVENT_CONNECTED/DISCONNECTED` events (`platform_esp32.cpp:238-243`) — the **PHY hardware reports the drops**; `NetworkModule` only reacts, never stops/restarts the link. Memory is ruled out (boot heap 286 KB, steady 133 KB free / 110 KB block — abundant).

**Signature = physical layer.** When flapping, the link holds for ~2 s, drops, comes back ~10 s later — a repeating cycle, not random. That fingerprints the PHY auto-negotiating, holding briefly, then losing sync: marginal **PHY power, clock, cable, or connector**, not firmware.

**Correlates with board reset.** The flapping tends to *start* right after a flash/soft-reset (this session: a reset preceded each flapping window; a clean power-up tended to link cleanly). Fits the documented slow/flaky PHY re-link on the Olimex after a reset — on some resets the PHY settles, on others it cycles for a long time before (or instead of) holding.

What we still don't know (all **physical** tests — no code change is warranted):
- Does a **clean power-cycle** (vs soft reset) reliably link? (Tests the reset-relink correlation.)
- Does **barrel-jack / stronger 5V** power stop it? (Tests PHY brown-out under USB-only supply, a known Olimex-Gateway weakness.)
- Does **swapping cable / switch port** stop it? (Rules out cable/connector.)

Bottom line: intermittent, build-independent, reset-correlated → a hardware/PHY issue, not a firmware bug. The earlier "slow DHCP at boot" is likely the same root cause (the PHY cycling many times before one window holds long enough to complete DHCP). Pick this up with the physical tests above before touching any code.

### MoonDeck doc-asset endpoint hardening (backlog)

`moondeck/moondeck.py::_serve_doc_asset` accepts any ROOT-relative path and serves the file. Path traversal *is* blocked (`asset_path.relative_to(ROOT.resolve())`), but inside the repo any file is served — including local-only artefacts like `moondeck/build/wifi_credentials.json` if present. MoonDeck binds to all interfaces by design (the existing comment in `main()` explicitly enables LAN reach), so anyone on the LAN can hit the endpoint.

Two improvements when this matters:
- **Subdirectory whitelist** — only serve under `docs/` (and image asset paths the markdown renderer needs). Reject `moondeck/build/wifi_credentials.json` etc. with 403.
- **Extension whitelist** — only image / CSS / JS mime types via a small allowlist.
- **Optional bind-to-localhost flag** — `--bind 127.0.0.1` for users who don't want LAN reachability. Default stays "" (all interfaces) since the LAN-reach is the documented design.

Not blocking — MoonDeck is a developer tool, not a production server. Pick this up when MoonDeck is in scope for hardening.

### A tagged release does not reach the web installer until the next main deploy (bug)

`deploy-pages` in `.github/workflows/release.yml` is gated `if: github.ref == 'refs/heads/main'`, because the `github-pages` environment's protection rule only allows main. The installer's release list is **staged into the Pages site at deploy time** (`install.js` self-hosts the last 5 stable + 5 prerelease releases; the release-asset URLs redirect to a host that sends no CORS header, so the browser cannot read them from the Pages origin). Together those mean **pushing a `vX.Y.Z` tag publishes the release but never updates the installer** — the new version reaches the picker only when something later pushes to main.

Hit on v4.0.0 (2026-08-24): the release published at 16:15:59, a main deploy ran at 16:16 and enumerated releases *before* it existed, and the installer offered v3.0.0 as newest for hours. Re-running the workflow with the tag fixed it, and that manual re-run is the current workaround.

Note the device's own OTA picker is unaffected — it reads `api.github.com` live ([app.js](../../src/ui/app.js) `RELEASES_API`), which is why a device could offer v4.0.0 while the installer could not. Two independent paths to the same release list.

`restage-pages-for-tag` in the same workflow already covers this: on a `v*` tag it re-invokes the workflow on `main` with `tag=<the tag>`, so `deploy-pages` runs under the allowed ref and the manifests land in `releases/<tag>/`. The open item is narrower than the symptom suggested — whether the dispatched run can still lose the race, since it is a *second* run whose release enumeration happens later, and whether relaxing the `github-pages` environment's branch policy (needs repo-admin) would remove the indirection entirely.

### CI: pin GitHub Actions to commit SHAs (supply-chain hardening)

`.github/workflows/release.yml` references all 9 action types by mutable `@vN` tag (`actions/checkout@v4`, `astral-sh/setup-uv@v3`, `softprops/action-gh-release@v2`, `espressif/esp-idf-ci-action@v1`, …). A mutable tag can be force-moved to malicious code by a compromised publisher; pinning each `uses:` to a full commit SHA (with a `# vN` trailing comment) removes that vector. **Done already (cheaper half):** `persist-credentials: false` on every checkout that doesn't push, so the `GITHUB_TOKEN` isn't left in `.git/config` for later steps to read (the `release` job keeps it — it force-pushes the `latest` tag). **Not done (this item):** SHA-pinning, because it carries an ongoing cost — pinned SHAs go stale and miss security patches, so it only pays for itself **alongside Dependabot** (or a Renovate config) to auto-bump them. Pick this up as a deliberate "CI hardening + Dependabot" pass, not piecemeal. Low risk today: every action pinned is a first-party `actions/*` or a well-known publisher (astral, espressif, softprops), not an obscure third-party action.

### Static IP on WiFi STA — wire the existing fields to the network (backlog)

NetworkModule exposes `addressing` (DHCP / Static) plus `ip` / `gateway` / `subnet` / `dns` fields, and they persist — but they are **not applied to the WiFi STA interface**. `wifiStaInit(ssid, password)` takes only credentials; the STA always runs DHCP (there is no `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` — that pattern exists only for the AP). So selecting Static and entering an IP currently does nothing: the device keeps its DHCP lease. The fields are display-only scaffolding ahead of the functionality.

Implementing it needs to answer three UX/safety questions (these *are* the spec):

- **When is it applied?** NOT per-keystroke — editing the fields must only update the stored values, never reconfigure the live interface mid-entry (a valid `ip` with a still-zero `gateway` would otherwise be applied and break routing). Apply on an explicit commit — safest is **on next connect / reboot**, not a live switch, because changing the STA IP drops the very connection the browser UI is talking to.
- **Validation before apply.** Require all of ip/gateway/subnet present and self-consistent; reject `0.0.0.0` gateway/ip. If invalid, stay on DHCP rather than half-apply.
- **Warn before a live change.** If applied live (not reboot-deferred), the UI must confirm ("about to change this device's IP to X — you'll need to reconnect at the new address") and surface the new URL, since the current socket dies the instant the IP changes.

Platform work: extend `wifiStaInit` (or add `wifiStaSetStatic`) to take optional ip/gateway/subnet/dns and call `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` when addressing is Static and the config validates. Needs careful hardware testing — a wrong static config locks the device off-network (recovery is the AP-fallback path or a flash erase). Until landed, consider hiding the Static option so it doesn't read as functional.

### Memory ceiling on non-PSRAM ESP32 with eth-wifi (backlog)

On `esp32-eth-wifi`, default 128×128 grid, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on `esp32-eth-wifi` (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [lessons.md](../../work/past/lessons.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Boot-time buffer degradation on non-PSRAM at 128×128 (investigation)

On the Olimex (no-PSRAM) at 128×128 with a modifier, the Layer sometimes comes up **degraded** at boot — status `"buffer reduced — not enough memory"`, with a visibly wrong render (the reduced render buffer overflows what the LUT/extrude expects). **Toggling any layout control (forcing a fresh `onBuildState`) fixes it** — the rebuild allocates the full buffer and the display is correct. So this is a boot-time allocation *race*, not a code bug: the same rebuild path that fails at boot succeeds moments later.

Measured: the full pipeline needs two ~49 KB contiguous buffers (Layer render buffer + Drivers output buffer, both 128×128×3), plus the LUT. At boot the largest contiguous block is only ~14–20 KB while the network stack / mDNS / HTTP-server buffers are still settling into the heap — so the Layer can't claim a contiguous 49 KB and degrades (halves its dimensions). A rebuild after the heap settles wins the contiguous block and allocates full. The device is at the fragmentation edge either way (~42 KB free / ~14 KB largest block at 128×128 with the full pipeline up).

The annoyance is purely that the device boots degraded and needs a poke to recover — it should come up working. Fix options in increasing scope:
- **Allocation order** — claim the big Layer/Drivers buffers *before* the network/mDNS/HTTP buffers fragment DRAM (i.e. wire the light pipeline's `onBuildState` ahead of network bring-up in `main.cpp`). Cheapest if the ordering is safe.
- **Boot retry** — if `onBuildState` degrades, schedule one more `buildState()` after boot settles (a one-shot, e.g. after the first `loop1s` or once the network reports up). Self-healing without reordering init.
- **Cap the default grid** on no-PSRAM to a size whose two buffers fit the post-boot largest block (same lever as the eth-wifi memory ceiling above).
- **PSRAM for the buffers** on PSRAM-equipped variants — sidesteps DRAM fragmentation entirely (related: the Async ArtNet and [Memory ceiling](#memory-ceiling-on-non-psram-esp32-with-eth-wifi-backlog) PSRAM notes).

Related: this is the render/output-buffer face of the same non-PSRAM fragmentation cliff the paged `MappingLUT` already addressed for the *LUT*. The buffers themselves still allocate as single contiguous blocks.

## Architecture

### Group src/core into folders, the way src/light already is (2026-09-17)

`src/light` keeps nothing at its root: every header sits in `drivers`, `effects`, `layers`, `layouts`, `modifiers` or `moonlive`. `src/core` is the opposite, 61 files flat, with `moonlive` the one folder that exists. The documentation pages already name the grouping, so the folders would only make the tree say what the docs say: `system.md` fronts 13 modules, `services.md` 7, `supporting.md` 4, and `control.md` one.

**The proposed shape.** `system/` takes the 13 modules on system.md plus `HttpServerModule`, which is a real module with its own `.cpp` and no page today. `supporting/` takes `MoonModule`, `Scheduler`, `Control` and `FilesystemModule`. `services/` takes the 7 service modules. That leaves 36 shared utilities, which want a `util/` of their own so the root ends up empty of headers like the light domain's: the packet and frame types, the maths and noise helpers, `JsonSink`, `JsonUtil`, `ModuleFactory`, `ScratchBuffer`, `Sort` and the rest.

**What it costs.** 172 include lines across 96 files, 52 in `src` and 44 in `test`, plus the explicit `.cpp` list in CMakeLists and a handful of scripts and architecture pages that name `src/core/` paths. Mechanical, and a compiler error names every line that was missed, but it touches nearly every file in the tree.

**Why it waits.** A pure rename of this size is unreadable as part of anything else, and it wants its own branch so the diff is one thing. It is also the kind of change that should follow the documentation sweep rather than ride inside it, since the sweep is what settled which page owns which header. The light domain has its own version of this: a `src/light/supporting` for the headers its own supporting page fronts.

### Filesystem-change notification (live preset refresh) — undesigned

ControlModule rebuilds its preset list by rescanning `/.config/presets`, and that rescan runs at startup and after every save, rename, delete and reorder. So a preset file **uploaded or deleted through the File Manager** appears only once the module next rescans (a reboot, or any preset action on the surface), not the instant the file lands. Documented as the actual behaviour in [the Control card](../../moonmodules/core/system.md#presets).

The fix is a **core-neutral filesystem-change notification**: FileManagerModule (or the `platform::fs*` write paths) signals "this path changed", and a module with a folder it cares about re-reads. Deliberately not built yet — it is a new core seam serving one caller today, which is the shape [architecture.md § Core primitives, not one-offs](../../explanation/architecture/index.md#core-and-light-domain) warns about. **Build trigger**: a second consumer appears (a scripted-effect folder for MoonLive is the likely one, since live scripts uploaded as files have exactly the same staleness), or the manual-refresh step proves annoying in real use.

Whatever the design, it stays domain-neutral (a path + a change kind, no preset/light vocabulary in core) and off the hot path — the notification marks a flag, the rescan happens on the owning module's next tick, never inside the writer. (CodeRabbit flagged the staleness; deferred here rather than growing the seam for one caller.)

### WiFi runtime disable — open design question (undesigned)

Today the eth-only build profile compiles WiFi out (`MM_NO_WIFI`). Turning WiFi off *at runtime* instead is undesigned: whether the gate should key off detected hardware presence, an explicit control, or a deviceModel-catalog field isn't decided. The eth-only build covers the need until a concrete case forces the choice. (Moved from architecture.md § What we leave undesigned; it's a deferred design decision, not a settled 🚧 one.)

### Consolidate the two module-by-name tree-walkers (backlog)

`HttpServerModule::findModuleByName` (`findInTree` recursion) and `Scheduler::firstByName` (`firstInTree`) are two implementations of the same "find the first module in tree-walk order with this name" operation. The duplication predates the `setControl`-to-Scheduler extraction, but that extraction made `Scheduler::firstByName` public *and* added `Scheduler::instance()`, so HttpServer no longer needs its own copy: its ~9 remaining call sites (identify, addModule, removeModule, clearChildren, the WLED/system shims) can call `scheduler_->firstByName()` and the private `findModuleByName`/`findInTree` pair deletes. Purely a *No duplication* cleanup — no behaviour change — worth doing so the walk order/semantics live in exactly one place. (Reviewer note, IrService/setControl branch.)


### Pin-uniqueness check across modules (prevents conflicts; replaces a singleton hack)

**Problem it solves.** Two modules must not drive the same physical GPIO. Today nothing stops it: add two `RmtLedDriver`s with `pins="18"`, or two `AudioService`s with the same `wsPin/sdPin/sckPin`, and they fight over the pin — at best garbage output, at worst (for I2S) endless `i2s_new_channel` driver-error spam every tick. This surfaced when a repeated catalog inject stacked duplicate AudioModules and the device spammed I2S failures (a clean install is fine; the duplicates were the artifact).

**Why pin-uniqueness, not a per-type singleton.** The first instinct was "make AudioService single-instance" — but that's a crude proxy. The *real* invariant is pin non-overlap: a board legitimately can have **two LED drivers on different GPIOs** (multi-output rigs do exactly this), or even two mics on distinct pin sets. "One mic" isn't fundamentally true; "no two modules on the same pin" is. So check pin conflicts, which both prevents the breakage **and** allows legitimate multi-instance setups. (A per-type singleInstance flag was prototyped and rejected in favour of this.)

**The clean mechanism — reuse `ControlType::Pin`.** Pins are already their own control type (the `addPin` work). So the check is domain-neutral and needs no per-module declaration: enumerate every `Pin`-typed control's value across the whole tree; a value of `-1` is "unused" (ignored); any other value appearing on two controls is a conflict. Handle the list case: `RmtLedDriver.pins` is a comma list (`"18,19,20"`), so the enumerator expands list-of-pins controls too.

**Where it runs.** Two sites, because a pin can be introduced at add *or* edit:
- `POST /api/modules` (add): if the new module's catalog/default pins collide, reject.
- `POST /api/control` (pin write): if setting a `Pin` control to a value already used elsewhere, reject (or soft-flag — see below).

**Open decision (UX).** Conflict on add → reject with a clear message (`"GPIO 18 already used by RmtLed"`). Conflict on a live pin edit → reject is safest but blocks mid-reassignment (you can't swap two drivers' pins without a free intermediate); a **soft-flag** (accept, set a status warning) is friendlier for live editing. Leaning: reject on add, soft-flag on live edit. Product-owner call.

**Hardware-limit tail (not covered by the pin check).** Pin-uniqueness rejects the common case but not the controller-count limit: the S3 has **2 I2S controllers** regardless of pins, so a 3rd mic on distinct pins passes the pin check yet fails `i2s_new_channel` at runtime. That tail is already handled — the platform I2S init returns false on failure (no panic, module stays `inited_=false`); verified live (4 pinned AudioModules → error spam, no crash). So scope = pin-uniqueness check + the existing graceful-degrade; don't try to make the pin check also model controller counts.

**Related:** the shipped "disabling releases resources" work (see [past plans](../past/plans/index.md)) — a disabled module freeing its pins is what lets the same GPIO be reassigned live without a conflict-reject.

### PinsModule — strict reject-on-add mode (the one remaining increment)

[PinsModule](../../moonmodules/core/system.md) is shipped: the read-only ownership map, reserved/strap severity grading, the conflict **soft-flag** (a GPIO claimed by two controls shows both owners, flagged red, never rejected), live-state dir/level/drive, and the disable-frees-pins cascade all landed — see [pins-analysis-top-down.md](pins-analysis-top-down.md) for the shipped design record. The soft-flag choice obviated the reassignment broker (a live pin swap already works: set A→B's pin, then B→A's, the transient conflict clears).

The one still-open item is an **optional strict reject-on-add mode** for the installer/catalog path: a "clean tree" that *refuses* a module add whose pins collide, rather than accepting it soft-flagged. This is add-path *validation* (a different feature than the live soft-flag reassignment), wanted only if a stricter installer UX is desired. Spec + `/plan` if picked; the soft-flag default stays for live editing.

### Runtime board presets (multi-commit, partially landed)

The firmware-vs-board separation is now in place across the codebase (see [MoonInstaller, firmware vs deviceModel vs board](../../explanation/architecture/mooninstaller.md#the-three-words)). `build_esp32.py --firmware <variant>` picks the compiled binary; MoonDeck deduces the physical board where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ `olimex-esp32-gateway-rev-g`) and lets the user pick from a short hardcoded list otherwise. Firmware variants stay separate — `esp32-eth` saves ~670 KB flash + ~30 KB DRAM vs the default `esp32` (WiFi+Ethernet, measured); merging would erase that win.

What still needs separation: the eth variants hardcode Olimex Gateway RMII pins in `src/platform/esp32/platform_esp32.cpp::ethInit()`, so they only work on that one PCB. As we add boards with different pins (LOLIN D32 tested 2026-06-02, QuinLED variants planned), runtime pin configuration becomes the next step.

Pin config moves to runtime (next, separate commit):
- Drop hardcoded `GPIO_NUM_17` from `ethInit()`. NetworkModule reads `Network.eth_rmii_clock_gpio` (new control) and similar pin values, defaulting to current Olimex hardcodes so behaviour is unchanged.
- Same for any other hardware-pin literal in the firmware.

Board preset catalog + upload (later, when the runtime config has real consumers):
- Add structured per-board files (location TBD — not `docs/` since they're config not docs; `boards/` at repo root is the strong candidate, matches the PlatformIO convention contributors will recognise).
- Each file declares chip, flash, PSRAM, Ethernet PHY + pins, default module config.
- New `/api/board-preset` endpoint accepts the JSON; device persists to LittleFS; bootstrap applies pins + defaults on next boot.
- MoonDeck "Set board" picker reads the catalog to populate the dropdown.
- Pin reassignment requires reboot (ESP-IDF can't hot-reconfigure EMAC pins after `esp_eth_driver_install`); document the constraint.
- A first attempt at this catalog landed and was rolled back during the firmware-vs-board separation work — the catalog only earns its keep once the device reads it, otherwise it's a docs-shaped file in the wrong place.

**Prior art — MoonLight's per-board pin database** ([ModuleIO.h](https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Modules/ModuleIO.h)). MoonLight (our own project) already models exactly this for ~25 boards across ESP32-D0 / S3 / P4: a `pins[]` array of `{GPIO, usage, index}` plus board-level `maxPower`, `ethernetType`, `ethPhyAddr`, `ethClkMode`. Don't copy the file or paste its tables here — read it when building the catalog and write our own. Its `usage` enum enumerates the hardware functionalities a projectMM board preset *could* drive once the device-side consumers exist (each needs its own module/control before the corresponding `deviceModels.json` / catalog field earns its keep — none exist today beyond `System.deviceModel` + `Network.txPowerSetting`):

- **LED output pins** — per-strip data GPIOs (1–16 outputs/board); the first real consumer (a Driver pin control) unblocks multi-output boards (QuinLED Dig-Quad/Octa, SE16, LightCrafter). **This consumer now exists** (the `pins` control on every LED driver; e.g. QuinLED Dig-Quad ships `"pins": "16,3,1,4"` in `deviceModels.json`), so the field earns its keep — but only up to **8 lanes** today (`kMaxLanes = 8` / `kMaxPins = 8`). The parallel drivers are moving to **16 lanes (choose 1..16)**; when they do, this becomes a real gap with two halves:
  - **Per-model usable-GPIO map (the data).** Identify **which up to 16 GPIOs each device model actually exposes for LED output** — not the chip's full pin count, but the pins broken out to a usable header/connector AND safe to drive (exclude strapping, flash/PSRAM, input-only, and pins already owned by eth-RMII / I²C / the onboard LED). The codebase knows the chip *ceiling* (`MM_MAX_GPIO` from `CONFIG_SOC_GPIO_PIN_COUNT`, [Control.h:13](../../src/core/module/Control.h)) and the live *ownership/reserved* grading ([PinsModule](../../moonmodules/core/system.md)), but NOT the per-board *exposed-and-safe* set — that is board knowledge (schematic/pinout per model). The authoritative source is the **annotated pinout image per model under `docs/assets/deviceModels/`** (e.g. [`esp32-s3-n16r8-dev.png`](../../assets/deviceModels/esp32-s3-n16r8-dev.png)) (and MoonLight's `ModuleIO.h` `pins[]` as prior art — read it, write our own against `deviceModels.json`). **Worked example — ESP32-S3-N16R8 dev board** (from `esp32-s3-n16r8-dev.png`): the 16 safe LED-output GPIOs are **`4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21`**, derived by excluding — octal flash+PSRAM (the R8/N16 part reserves `26–37`, the `SPIIO4-7/SPIDQS/SPICLK` pins), USB (`19,20`), UART0 console (`43,44`), the onboard RGB LED (`38`), and the strapping pins (`0,3,45,46`). That leaves exactly 16 clean I/O — enough for the full 16-lane target. Do the same read per model.
  - **Encode as per-model defaults + coverage (the catalog).** The 16 usable lane GPIOs become the model's default `pins` string, so a fresh flash of e.g. "Serg UniShield V5" comes up with the *right* lane pins pre-filled, not blank/generic. Coverage today is uneven: of 25 models, **7 carry no LED-pin config** (Olimex Gateway, LOLIN D32, Generic ESP32 Dev, ESP32-S3 N16R8 Dev, LightCrafter 16, SE 16 V1, and any bare dev board) — those default to nothing and force the user to guess. Fill every model's LED-capable pin default, and **document the per-model map** (the annotated-pin images the § below already reserves are the natural home). **The per-board usable set is wider than the exposed header:** a pin the board commits to a peripheral it *doesn't mount* is fair game (the Olimex Gateway leaves 6 clean LED pins only if you count the **unmounted micro-SD** pins 4/13/14 — see [gpio-usage § Usable LED-output GPIOs](../../reference/hardware/gpio-usage.md) and the bench note in memory), so the per-model map must record *which non-exposed/repurposable pins are safe on this board*, not just the header breakout. Scope: **16 pins max** — do not over-generalize past the peripheral lane ceilings ([measured lane ceilings](../../reference/performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid): Parlio 65535 bytes/lane single-shot (897 RGB lights at 8 lanes, ~448 at 16), LCD **8 or 16 lanes** on S3 (the widening shipped), RMT up to 8 TX channels). **Note the i80 gotcha:** an `LcdLedDriver` model should pick `clockPin`/`dcPin` clear of its data lanes — an overlap is a *warning*, not a blocker (the driver still runs; that one lane just carries the clock/DC waveform, which is fine for an unused parked lane but garbles an active strand), so a catalog default that overlaps an *active* lane would ship a subtly-broken board.
- **Ethernet PHY config** — LAN8720/RMII (MDC/MDIO/CLK/power-pin/PHY-addr/clock-mode) vs W5500/SPI (MISO/MOSI/SCK/CS/IRQ); the consumer is the runtime `Network.eth_*` controls listed above, replacing the hardcoded Olimex pins.
- **Power budget** — `maxPower` (Watts) per board, for a future current-limit / brightness-cap control.
- **Audio / I2S** — SD/WS/SCK/MCLK pins, the input side of audio-reactive effects (Pi-5 sensor note is the desktop counterpart).
- **Buttons & inputs** — push/toggle/lights-on, PIR, digital-input; needs an input-event concept the firmware doesn't have yet.
- **Relays & power control** — relay / lights-on / high-low pins.
- **Infrared** — IR receive pin (remote control).
- **RS485 / DMX** — TX/RX/DE pins (wired DMX-512 output beyond the current Art-Net path). The driver that consumes this is the [RS-485 / DMX-512 wired output item](backlog-light.md#rs-485-dmx-512-wired-output-future-the-physical-dmx-driver) in the light backlog.
- **Sensing** — voltage / current / battery / temperature ADC pins.
- **Onboard LED / key, exposed / reserved pins** — board-housekeeping and conflict-avoidance metadata.

Sequencing rule (unchanged): each functionality lands a device-side control first, then its preset field; the catalog grows one earned consumer at a time, never as a speculative pin dump.

**Module variant + PSRAM within the classic-ESP32 family.** `getChipDescription()` and MoonLight's `ModuleIO.h` both report only the *core* family ("ESP32"), not the *module* (WROOM / WROVER / PICO) — so neither distinguishes whether a classic-ESP32 board has PSRAM. This matters for MoonLight (whose large-LED story leans on PSRAM) in a way it doesn't for MoonLight: e.g. the **QuinLED Dig-Next-2 is built on an ESP32-PICO with 2 MB PSRAM**, but MoonLight's `esp32` build has no `CONFIG_SPIRAM` (see the `#ifdef CONFIG_SPIRAM` gate in `platform_esp32.cpp::psramAlloc`), so it flashes and runs as a no-PSRAM device and hits the non-PSRAM fragmentation ceiling at large grids that the 2 MB would otherwise relieve. A PSRAM-enabled classic-ESP32 firmware variant (e.g. `esp32-psram`) would unlock it; `deviceModels.json` could then carry a `psram` hint per board to steer the picker — but only once that variant exists (no consumer today). `deviceModels.json` currently maps every classic board to the WiFi-only `esp32` variant, which is correct-but-unoptimised for PSRAM-bearing PICO boards.

### Per-layout coordinate offset for independent placement (backlog)

`Layouts` stitches multiple child layouts into one physical light space, but only their *indices* are stitched (offset sequentially in `placeLights`) — their *coordinates* are not translated. Two layouts therefore overlap in the same coordinate box: two 64×64 grids both occupy x,y ∈ 0..63, so the Layer's dense bounding-box buffer is 64×64 (4096 voxels) even though the container reports 8192 lights, and the second layout's lights land on the first's positions. `scenario_Layouts_mutation` documents this (its steps assert pipeline liveness, not buffer-size arithmetic).

When picked up: add `offsetX/Y/Z` (lengthType) controls to `LayoutBase`; `Layouts::placeLights` translates each child's emitted coords by its offset so layouts occupy disjoint regions of the physical extent (a 64-wide grid at offsetX=64 sits beside another at offsetX=0 → a 128×64 combined extent). `Layer::onBuildState` already derives physical dims from the max emitted coordinate, so it would pick up the wider extent automatically. Until then, "multiple layouts" means "multiple layouts sharing a coordinate box", which is only useful when they genuinely overlap (e.g. a sphere inscribed in a grid).

### Improv as a child of NetworkModule (deferred — needs scheduler work first)

Architecturally the right shape; attempted in plan-21, reverted. Blocker: `Scheduler::tick()` only walks top-level modules for `loop20ms`/`loop1s` — children silently miss those callbacks. See [lessons.md](../../work/past/lessons.md) "Trying to add a child module to NetworkModule".

Minimum-scope fix before the move:
1. `MoonModule::loop20ms`/`loop1s` propagate to children (or Scheduler walks them) — pick whichever costs less at runtime.
2. Audit every existing override of `setup`/`loop1s`/`loop20ms`/`onBuildControls`/`teardown` in NetworkModule, SystemModule, FilesystemModule, HttpServerModule to confirm base-chaining.
3. Then the actual move is a one-line `networkModule->addChild(improvModule)` swap. Estimate ~2 h total.

### Platform API: `std::span` migration (backlog)

Several `platform.h` APIs still use `(buf, len)` pairs where `std::span` would catch length/pointer mismatches at compile time. Concrete sites: `http_fetch_to_ota`, `improvProvisioningInit`, and friends. ~2 h including ripple updates to callers. Do alongside the next platform-API expansion (Windows socket port or POST /api/firmware streaming).

### Improv-as-REST follow-ups

Device-model injection over Improv shipped as **"Improv = REST over serial"** (the `APPLY_OP` vendor RPC pushes the whole `deviceModels.json` entry over serial during install; the device runs the same apply-core the HTTP REST API does, on WiFi *and* eth-only firmware). That subsumed the earlier multi-step "board injection + Improv as a general data injector" plan — the general injector *is* APPLY_OP. What remains:

**Open follow-up: closed-loop APPLY_OP pacing (read-back ack + retry).** The installer paces APPLY_OP frames open-loop (`sendApplyOpFrame` waits a fixed ~120 ms between ops) rather than reading the device's ack back, because a Web Serial duplex read while the writer lock is held is awkward. The delay covers the worst-case single-buffer consume window with headroom, and each op is idempotent (a lost op re-applies cleanly on a re-flash), so this is robust today. The closed-loop upgrade — read the RPC response, retry once on error `0x82` (buffer busy) — removes the fixed delay (faster install) and makes op-loss impossible rather than improbable. Worth doing if a real install is ever observed dropping an op, or when the config push grows large enough that the cumulative fixed delay is noticeable. Touches only `install-orchestrator.js`.

**Open follow-up: shared JS helpers across device-UI and mooninstaller.** `safeLocalGet` / `safeLocalSet` (3-line hostile-storage guards) are duplicated in `src/ui/install-picker.js` (device firmware, embedded as a C string via `embed_ui.cmake`) and `mooninstaller/devices.js` (web installer page, served from Pages). The two live in different build contexts so the shared extract isn't trivial — it'd need a new `src/ui/safe-storage.js` plus updates to: `embed_ui.cmake` (embed the new file), `ui_embedded.h` generator (new C array), HTTP server file routing (new path served), `release.yml` workflow staging, `preview_installer.py` staging. Five files for one 3-line helper is too much pre-merge. Worth doing when the next shared helper arrives — `relativeTime` and `formatBytes` are candidates. Two helpers earn the build-glue cost; one doesn't.

**Open follow-up: P4 Improv scan on a cold WiFi link (bench check).** `improvHandleScan` in `src/platform/esp32/platform_esp32_improv.cpp` calls `esp_wifi_scan_start`, which needs the WiFi driver started. On native ESP32/S3 the driver is up by the time a user provisions; on the P4 the radio lives on the C6 and comes up only after the esp_hosted prelude in `ensureWifiInit()` (triggered by `wifiApInit` / `wifiStaInit`). A scan requested on a P4 that has not initialised WiFi returns an error cleanly rather than scanning a cold link, so nothing crashes. The check: bench-verify whether a P4 provisioned from cold needs the link brought up first, and if so route the scan through the public `wifiAp`/`wifiSta` path.

### Live scripting — author effects/layouts/modifiers/drivers/sensor logic on-device (multi-commit, design phase)

Run user-authored scripts on a running device — a scripted effect, layout, modifier, driver, or core sensor rule, pushed as text and live on the next tick with no reflash/reboot — the leap WLED took with ARTI-FX and the heart of the PixelBlaze product. A scripted module **is** a MoonModule (controls, `loop()`, role, generic UI). The engine lives in core (domain-neutral: also "transform sensor data") and serves the light domain specifically. Targets in order: ESP32 classic + S3 first, then P4/other ESP32, then Teensy, then desktop. Must be blazingly fast (runs in the render hot path at 16K+ lights × 50 FPS), memory-smart (IRAM/PSRAM via `platform::alloc`, compile-once), and synced (Scheduler tick, tick-atomic hot-swap, live reconfig).

The **bottom-up landscape survey** is done — [livescripts-analysis-bottom-up.md](livescripts-analysis-bottom-up.md): deep-reads the [ESPLiveScript fork](https://github.com/ewowi/ESPLiveScript/tree/fix-warnings) (a from-scratch C-like JIT that emits **native Xtensa** machine code — blazingly fast but **Xtensa-only**, so it covers classic+S3 and *not* P4/Teensy/desktop), surveys the field (PixelBlaze bytecode VM + web editor, WLED ARTI-FX AST-walking interpreter, embedded VMs / WASM / lightweight multi-ISA JITs), and extracts the load-bearing decisions (execution strategy, the IR seam ESPLiveScript lacks, the MoonModule binding, the per-pixel contract, memory placement, sync, sandboxing). Its thesis to validate: a **portable bytecode-VM baseline that runs on every target on day one + an optional native back-end for the hot ISAs behind a shared IR**. **Next: the top-down redesign** — the prompt that generates `livescripts-analysis-top-down.md` is at the bottom of the bottom-up doc; it produces the reference architecture + staged spike plan. Implementation is multi-commit, spike-ordered, after the top-down lands. Credits: [friend-repos/hpwit-ESPLiveScript.md](../../friend-repos/hpwit-ESPLiveScript.md).

### Duplicate module names are reachable, and silent (backlog)

Two modules in the tree may hold the SAME name. Found on the bench: a classic ESP32 had a
`MoonLiveLayout` and a `MoonLiveEffect` both called `MoonLive`, one under `Layouts` and one under a
`Layer`. Nothing reported it. The UI keys a card's controls by module name, so both cards resolved to
the same entry and the effect's `bpm`/`zoom` sliders rendered under the LAYOUT's heading, where its
own `petals`/`radius` should have been. The server data was correct throughout; only the display was
wrong, which is what makes it hard to recognise.

`Scheduler::ensureUniqueName` exists and is called on `/api/modules` creation and after a persistence
load, so the tree normally cannot reach this state. The bench pair predates that pass or arrived
through a path that skipped it, which is exactly the case a check would catch. **The gap is that
nothing NOTICES:** a name collision is tolerated silently rather than reported, and the first symptom
is a UI showing another module's controls.

Fix: assert uniqueness after the persistence load and report a collision in the module status, so a
device that reaches this state says so instead of rendering the wrong card. Renaming a module from
the UI would also give a user a way out; there is no `name` control today.

### Deleting a module by name removes the FIRST match (backlog)

`DELETE /api/modules/<name>` resolves through `findModuleByName`, which returns the first match in
tree order. With a duplicate name (above) that is not necessarily the module the caller meant: on the
bench, deleting the effect by name would have removed the layout, because the layout came first.

Noticed while repairing that device, and avoided only by reading the handler before running the
request. It is latent rather than dangerous today, because duplicates are supposed to be impossible,
but the two issues compound: the state that makes a delete ambiguous is the same state nothing warns
about. Fix alongside the check above, either by refusing an ambiguous delete or by addressing a
module by a path rather than a bare name.

### HTTP file serving blocks the render tick (backlog)

`HttpServerModule::handleConnection()` serves large embedded files (`app.js`, `style.css`) with the blocking `TcpConnection::write` — a page load can briefly stall `loop20ms`. One-shot per load (lower priority than the per-tick preview issue, which is fixed). Fix: serve large HTTP responses through a resumable per-client cursor drained on tick20ms (the shape the preview and full-state sends use).

### Generic control + state topics over MQTT — the automation escape hatch (backlog)

The MQTT bridge ships a **semantic** topic surface (`<prefix>/on/set`, `brightness/set`, `hsv/set` → the `Drivers` controls) sized for Homebridge/HomeKit, which need fixed, typed topics per accessory. That leaves everything else the web UI can reach — effect choice, layout, modifier params, palette-by-index — unreachable from a home hub. The ask that surfaced this: *"expose the whole REST API over MQTT."*

**It's compatible with Homebridge/HA, because it's a second topic namespace, not a replacement.** Homebridge only ever subscribes to the semantic topics it's configured for; a generic topic family sits beside them and a broker carries both. The two surfaces already share one apply-core — `Scheduler::setControl(module, control, json)`, the same seam `/api/control` and IR funnel through — so a generic MQTT surface is that core under a different topic shape, honouring the module's own "MQTT is a transport, not new control logic" contract.

But "the whole REST API" over-scopes: of the routes (`/api/control`, `/api/state`, `/api/system`, `/api/modules[/…]`, `/api/dir`, `/api/file`, `/api/firmware/*`, `/api/reboot`, `/api/types`, the `/json/*` shim, `/ws`), only two are genuinely pub/sub-shaped. The rest are request/response or bulk transfer and belong on REST:

- **`<prefix>/api/control/set`** ← `{"module","control","value"}` (the exact JSON `/api/control` takes) → `applySetControl`. The real win: any control on any module — the effect/layout/modifier/palette reach HomeKit can't express. The apply-core already validates (range / read-only / module-not-found via `SetControlResult`), so safety is free.
- **`<prefix>/api/state/get`** → publishes the `/api/state` JSON (retained). For dashboards (fps / heap / tick / current effect across a fleet), offline detection, and alerting — the monitoring half.

Explicitly **out** (no practical MQTT case, and wrong for the transport):
- **`/api/file`, `/api/dir`, `/api/firmware/upload`** — kilobytes-to-megabytes of binary; MQTT is a small-message bus, REST already streams these correctly.
- **`/api/modules` add/delete/move/replace** — stateful, order-sensitive pipeline surgery a human does once in the UI; an automation reshaping the pipeline is an anti-pattern (the *Robust to any input* guarantee makes it *safe*, not *advisable*).
- **`/api/reboot`** — one narrow action; a single fleet-reboot topic is a mild nice-to-have, not "the API."
- **`/api/types`, `/json/*`, `/`, `/ws`, `/api/firmware/url`** — discovery metadata, the WLED shim, the UI itself, the socket: static, already-served-elsewhere, or a different protocol.

**Shape:** ~15 lines routing into the existing `applySetControl` + a state publish; gate behind a `generic API` bool defaulting **off**, so a home user's broker isn't a remote-config backdoor and the curated HomeKit surface stays the clean default. **Don't** auto-generate a semantic topic per control — HomeKit/HA need stable typed topics; a generic `palette/set` whose meaning shifts per firmware breaks their discovery. Keep semantic topics hand-curated; let the generic pair be the escape hatch.

**HA MQTT Discovery — SHIPPED.** The device announces a retained JSON-schema light config to
`homeassistant/light/<id>/config` (the Tasmota/ESPHome/Zigbee2MQTT pattern), so HA auto-creates a
wired entity — gated on the `haDiscovery` control, with a Last-Will availability topic. This covered
the "HA can't reach the device over MQTT" incidents. The *generic-topics escape hatch* above (the
whole REST API over MQTT, for power-user scripting) is the remaining unbuilt piece; keep it coherent
with the [DevicesModule command half](#devicesmodule-interop-plugins-the-command-half-discovery-shipped)
(Tasmota-MQTT / zigbee2mqtt as *outbound* control — the mirror of this *inbound* surface) and the
[LightsControl integration point](backlog-mixed.md).

**Open question — Homebridge example maps both `setBrightness` and `setHSV`.** In `homebridge-mqttthing`'s `lightbulb`, HSV's *value* (V) already carries HomeKit's Brightness characteristic, so mapping both topic pairs may double-drive brightness (mqttthing's docs lean toward using one or the other). The [MQTT § Homebridge example](../../moonmodules/core/system.md#mqtt) currently lists both, to show the device's full topic surface. Resolve on hardware: flash a board, run Homebridge + mqttthing with that config, and check whether the Home-app brightness slider misbehaves — if it does, drop the two `brightness` topics from the example; if not, it's a non-issue. (Flagged by CodeRabbit; parked here rather than changing the doc on an untested hunch.)

### HA update entity via MQTT discovery — release check (open follow-up)

**Discovery config + install command wiring — shipped.** [`MqttModule`](../../src/core/system/MqttModule.cpp) now publishes a second HA-discovery component at `homeassistant/update/MoonLight_<mac6>/config` (same haDiscovery gate, same MAC-stable id, same device card as the light), state on `<prefix>/update/state`, and subscribes to `<prefix>/update/set`. HA renders a *"Firmware: <version>"* card in the diagnostic section of the device panel. An install command routes to `platform::http_fetch_to_ota` with the release-artifact URL built from the payload version + `kFirmwareName` (`https://github.com/MoonModules/projectMM/releases/download/v<version>/firmware-<key>-v<version>.bin`), reusing the same OTA task the `/api/firmware/url` route drives.

**Still to build — device-side release check.** `installed_version` and `latest_version` are equal today (both `MM_VERSION`), so HA renders the entity as up-to-date and the Install button is disabled. What's missing is a periodic poll of `https://api.github.com/repos/MoonModules/projectMM/releases/latest`: at boot (~30 s post-network-up) and every 24 h thereafter, fetch the release JSON, extract `tag_name`, and if newer than `MM_VERSION` call `MqttModule::publishUpdateState()` with the updated `latest_version` (the publish path is already there — only the caller is missing). ~2 KB per check.

Two blockers, both platform-layer:
- **`platform::http_fetch_to_ota` is OTA-shaped** — it writes bytes into the next OTA partition; there's no general "HTTPS GET into a buffer" seam. A small `platform::http_fetch_to_buffer(url, buf, cap, statusBuf, size_out)` next to it — ~30 lines wrapping `esp_http_client` on ESP32, a libcurl / URLSession stub on desktop — is the right home for this.
- **JSON parsing on-device** — the existing `mm::json` scalar helpers are flat and key-order-independent; a `releases/latest` payload uses a nested `assets[]` array which the current helpers don't walk. Either extend them (~30 lines to walk one level of array), or scan for the `tag_name` string directly with `strstr` for this one caller.

Once both land, add a `ReleaseCheckModule` (or a small extension inside NetworkModule / FirmwareUpdateModule — decide when building) that owns the poll timer + last-seen version cache and calls into `MqttModule::publishUpdateState()`. ~50 additional lines.

## Testing

### Additional test coverage (pending)

- **Memory degradation cascade** — the output-buffer *allocation* decision (no buffer for a lone identity layer; a buffer for ≥2 layers or any LUT layer) is unit-pinned (`unit_Layers_container` "Drivers allocates the output buffer only when…"), and LUT-vs-identity is pinned by `unit_Layer_sparse_mapping`. What's **not** pinned is the *low-heap* half of [architecture.md § Degradation cascade](../../reference/hardware/firmware-variants.md#degradation-cascade): under heap pressure the LUT + driver buffer are skipped *together* (`lutSkipped()` true, forced 1:1), and below that the layer buffer *reduces dimensions* (halving to a 8×8 floor) rather than failing. The hook exists — `unit_BlendMap` already uses `platform::setTestMaxAllocBlock` to force allocation failure for the paging test — so a test could cap the block size and assert: (1) LUT+output buffer both skip and `lutSkipped()` flips, (2) the layer buffer shrinks to fit and never goes null. Pre-existing gap (predates multi-layer); the *happy-path* allocation contract is covered, only the OOM-degrade branch isn't.
- **Per-step assertions in scenarios (a framework gap, not a scenario gap).** A scenario step can
  assert TIMING and HEAP (`bounds`, `contract`) and the run asserts the final buffer, but it cannot
  say "after this step the fixture has 24 lights", "this module's status reports a compile error",
  or "the rendered output changed". So a migrated script step proves the pipeline still ticks, not
  that the edit did what it claims — the MoonLive scenarios walk break-and-recover cycles whose
  most interesting states go unasserted. Wants a small vocabulary (`expect_lights`,
  `expect_status`, maybe `expect_pixel`) added to **both** runners together: the desktop/live split
  is exactly what let `write_file` exist on one and silently no-op on the other. Raised by
  CodeRabbit against scenario_MoonLive_pipeline and scenario_MoonLiveEffect_controls; the sites are
  listed there, but the vocabulary has to exist first.
- **UI page load time** — scenario step measuring HTTP response time for `/`, `/api/state`, `/api/system` via the live runner. Verifies acceptable load time on ESP32.
- **Module teardown memory** — scenario that tears down all modules and verifies heap returns to pre-setup baseline. Confirms no lifecycle leaks.
- **JavaScript test harness** — `vitest` + `jsdom` for the browser UI: pure helpers in `install-picker.js` (`isCompatible`, `parseFirmwaresFromAssets`, `relativeTime`) **and `app.js`'s conditional-control DOM logic** (`syncVisibleControls` — reconciles which control rows are rendered when a `hidden` flag flips). The C++/backend half of conditional controls IS unit-tested (`conditional_controls.h` + per-module tests pin the binding + `hidden` flag), but the **UI re-render half is not** — `syncVisibleControls` was the source of a real re-render-loop freeze (Network static-IP toggle) caught only on hardware. A `jsdom` test that builds a card, flips a control's `hidden`, runs the reconcile, and asserts the right rows appear/disappear (and that it converges — the unchanged→no-op fast path) would have caught it. **Attempted and reverted (2026-06-17):** stood up vitest + 13 passing tests for the install-picker pure helpers, but the high-value half (`syncVisibleControls`) needs either an `app.js` module seam or extracting its reconcile logic into a separate served `.js` (6 embed/route wiring edits for a firmware-served file). Judged not worth adding a whole Node/npm toolchain to a C++/Python repo to test ~3 small pure functions; the toolchain earns its place only once the `syncVisibleControls` DOM test (and a real body of JS logic) lands with it. **Do it as its own focused branch**, deciding the app.js seam first (it's already `type="module"`, so extracting `reconcileControlRows` into a served file — wired through `embed_ui.cmake` + the two HttpServerModule routes like the other UI .js — is the clean shape). Pure-helper `_test` exports + the reconcile extraction are the two pieces; both were prototyped in that reverted attempt.
- **Browser-level Improv automation** (deferred) — `moondeck/build/improv_smoke_test.py` (added 2026-06-03) exercises the device-side Improv listener over plain serial; what's missing is the browser-side equivalent — Playwright driving Chrome's Web Serial, clicking through ESP Web Tools' install modal, filling the WiFi creds form, asserting `PROVISIONED`. Catches "ESP Web Tools changed its Improv handling in a way that broke our manifest format" failures the serial-only smoke test can't see. Hard to set up reliably (headless Chrome with Web Serial is finicky, needs a wired ESP32 in CI). Pick this up if a regression in the browser flow ever escapes the manual dev-environment test (preview_installer flash-ready mode at <http://localhost:8000/>).

### Live full-suite run leaks state between scenarios (test infra)

`run_live_scenario.py --module all` runs scenarios in sequence against one device, and they share the live tree. Two scenario styles don't compose:
- **Canvas-preparing scenarios** (`scenario_modifier_swap`, `scenario_perf_light`, `scenario_perf_full`) `clear_children` the containers and rebuild, then their cleanup leaves the tree **bare**.
- **Canonical-tree-assuming scenarios** (`scenario_GridLayout_resize`, `scenario_MoonModule_control_change`, `scenario_NetworkModule_mdns_toggle`) are `mutate` scenarios that expect the boot tree (Grid / Noise / Multiply) to already exist and only tweak it.

Run a bare-leaving scenario before a tree-assuming one and the latter fails pre-flight ("references ids neither on the device nor added by an earlier step"). Each passes **in-process** (fresh tree per scenario — the authoritative gate) and **live individually** (after a clean boot); only the chained live run trips. Not a product bug — a consequence of the "scenarios own their state, no restore" model the canvas-preparing scenarios follow, which the older ones predate.

Fix options: (a) make every live mutate scenario clear+rebuild its own canvas (consistent with the newer ones) so order never matters; or (b) have the live runner reboot / restore the canonical tree between scenarios. (a) is the cleaner long-term shape. Until then, the in-process suite is the gate; live full-suite runs need a clean boot per scenario, or run scenarios individually.

## An explicit `Custom` Ethernet preset is reseeded to a named one

`ethPresetIsUnset()` returns true for any editable row, and `Custom` is the only editable row, so it cannot tell "nothing has chosen a preset" from "the user chose Custom". A board saved as `Custom` whose pins happen to match a named preset is therefore reseeded to that preset on the next rebuild, and its pin rows are hidden: a hand-wired board silently adopts a board map it was never given.

The seam is what makes this awkward. `defineControls` runs before the config is restored, so the seed cannot simply latch a flag on the first render: that is the legacy path the seed exists for, a board whose file predates `ethBoard` and carries only pins. Distinguishing the two needs to know whether the restore supplied an `ethBoard` value at all, which is not visible where the seed runs. An attempt that latched on the first render broke the legacy path and was reverted.

**What it costs today**: nothing on a board the catalog configures, since those name their preset. It bites a user who deliberately picks `Custom` on a board whose pins match a preset, which is the hand-wired case `Custom` exists for.

**Where to look**: `FilesystemModule::applyNode` overlays values, calls `rebuildControls`, then overlays again. A control that wants to know whether IT was restored needs a signal from that pass, which no control has today. That is the general shape worth solving rather than a special case for one select.

## Ethernet on the QuinLED Dig-Next-2 (W5500 over SPI)

The DN2 is in the catalog with no Ethernet block, so it comes up WiFi-only. Adding it is a `deviceModels.json` entry and nothing else: the preset work landed the seam, and W5500 already applies live.

**RMII is impossible on this board, so the W5500 is the only route.** The DN2 carries an ESP32-PICO-V3-02, whose package leaves GPIO 16, 17, 18 and 23 unbonded, and those are the RMII management and clock pins. This is the same package fact behind the flash-cache wedge recorded in [lessons](../past/lessons.md). QuinLED's own LAN8720 board stacks onto the QuinLED-ESP32 only, not onto a Dig board.

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

## Housekeeping

### Promote the one-line `//` cap to an error, once the 604 `.cpp` blocks are homed (2026-09-21)

`MAX_CODE_COMMENT = 1` is already the rule in both kinds of file, and already an ERROR in a header. It stays a warning in a `.cpp` only because `_blocks()` stages the sweep by file kind, and that function names its own removal: a rule leaves the staging as the tree meets it, which no-hard-wrap already did.

**The shape of what is left.** 604 blocks across 92 files, 2081 comment lines. By area: 239 in `test/`, 210 in `src/`, 155 under `src/platform/`. By length: 81 are two lines, 321 are three, 124 are four, so 526 of 604 are three lines or fewer and the tail is one 15-line block, two of ten and a handful between.

It concentrates, which is what makes it approachable. `HttpServerModule.cpp` alone holds 100 and the top ten files hold 312, over half; the other 82 files average under four each.

**Where the depth goes is the blocker.** Of the 92 files only 19 already carry an `@moreinfo` appendix. 68 have a `///` lead with no appendix and 5 have neither, so most files need the appendix written before a single block can move.

**The fix is not to split them.** Chopping one comment into two shorter lines leaves the text identical and satisfies nothing, which is why the cap counts a block rather than a line. Each one is depth that belongs in the file lead's appendix with an `@xref` back, or prose that says less.

**Suggested order.** `HttpServerModule.cpp` first: 100 blocks, 444 comment lines, and it already has the appendix. Then the other nine heavy files for 212 more. Then `test/` as its own sweep, because the argument that a test comment explains why a test EXISTS is strongest there and may deserve a different budget rather than an appendix. Promote once at zero.

**Cost.** Not a scripted pass. Each block is a judgement between depth that belongs in an appendix, an explanation that should be shorter, and history git already holds. About 35 done by hand in one session is the measured rate, so the first file is a session of its own and the whole sweep is several days.

### Hot path: move blocking work off the render callbacks (architecture)

`-Wfunction-effects` proves the render path really does block — these are not annotation gaps,
they are synchronous I/O, allocation and lifecycle work reachable from `tick()`. Each needs the
work moved to a worker or made resumable, so each is a design change rather than a lint fix.
Confirmed by external review (CodeRabbit, PR #56).

**`tick()` — every frame, the sharp ones:**
- `PreviewDriver::tick` — the socket half is FIXED (the pull-model transport only ARMS a message;
  every socket byte moves on the transport tick). What remains: `buildCoordTable()` resizes
  `keptIdx_` and the staging buffer on a rebuild/adopt tick. Suppressed at the site with the reason.
- `ParallelLedDriver::tick` — `tickSync()`/`tickRing()` reach `busWaitIfBusy()`, which spins for
  the DMA peripheral. Deliberate (the driver owns the bus for the frame) but blocking. Suppressed.
- `Drivers::tick` — joins/stops the render-split worker synchronously on the timed-out recovery
  path. Wants asynchronous handling that still preserves buffer ownership.
- `HueDriver::tick` / `tick1s` — synchronous HTTP (`pushOneChangedLight`, `pollPairing`,
  `fetchLights`, `fetchGroups`) plus allocation. Wants a queue to a worker.
- `Layer::tick` — calls `applyState()` on a modifier rebuild, which runs prepare/release and
  resizes ScratchBuffers. Wants the rebuild deferred to the scheduler's non-render prepare path,
  keeping today's coalescing of `consumeNeedsRebuild()`.
- `DemoReelEffect::tick` — `advance()`/`swapTo()` create, delete and `applyState()` child effects
  from the render callback. Wants a pending-switch flag consumed off-tick.
- `NetworkSendDriver::tick` — sends inline; wants to enqueue.
- `RmtLedDriver::tick` — waits on hardware completion and reset; wants polling or offload.
- `AudioService::tick` — UDP `sendTo`/`recvFrom` every frame (`syncSend`, `syncReceive`,
  `syncEnsureSocket`).

**`tick20ms()` / `tick1s()` — milder, same shape:**
- `HttpServerModule::tick20ms` — inline `handleConnection` transport work, so the 100 ms budget
  cannot actually bound it. Wants resumable connection handling. `tick1s` writes WLED state
  frames inline; wants the existing resumable sender.
- `MqttModule::tick1s` — synchronous DNS + discovery-buffer allocation.
- `NetworkModule::tick1s` — WiFi/AP lifecycle transitions and tree rebuilds.
- `FilesystemModule::tick1s` — the debounced `flush()` does filesystem work; the poll itself is
  fine, the save wants a worker.
- `FileManagerModule::tick1s` — `platform::filesystemUsed()`; wants a cached value.
- `SystemModule::tick1s` — the P4 `coprocessorWifi()` path calls
  `esp_hosted_get_coprocessor_fwversion()` synchronously; wants a cached snapshot.
- `ImprovProvisioningModule::tick` — runs the queued APPLY_OP inline; wants a cold task to
  execute and publish only the result.
- `Scheduler::tick` — dispatches all of the above, so its own contract is only as good as theirs.

**Explicitly NOT in scope:** the float-math findings (`BouncingBallsEffect`, `RipplesEffect`,
`SphereMoveEffect`). Review suggested fixed-point, but the float trajectory IS the ported
MoonLight behaviour and fidelity is deliberate. If the FPU-less Xtensa cost is real, that is a
profiling question first, not a lint fix.

`-Wfunction-effects` reports these but never fails a build, and
[hotpath-baseline.txt](../../reference/metrics/hotpath-baseline.txt) freezes the known set so a
NEW blocking call stands out in the report. The pre-commit gate runs the same check incrementally.

### check_nonblocking: drop the baseline, the way check_docgen did (decision)

The baseline has inverted: it now manufactures false alarms instead of suppressing noise. It holds
**107 entries against 240 live sites**, and was last written in an unrelated docs-restructure
commit, so whole files it never covered report every site as NEW. `OscModule.h` has seven findings
and zero baseline entries; `PanelCardDriver.h` has eight and zero. A comment-only cleanup batch
reported 82 NEW and none of them were new code.

That is the docgen failure one step further along: docgen's baseline hid findings, this one invents
them, and both train a reader to distrust the number. `docgen_baseline.txt` was deleted for exactly
this reason and the check now states absolute limits.

The shape is settled, the scope is not. Deleting the file and the `--baseline` flag is
straightforward. The open question is whether `check_nonblocking` becomes a **gate**: today it is
explicitly not one (every exit path returns 0, and `-Wno-error=function-effects` keeps the build
green on purpose), because each finding is a judgment — move the work, annotate the callee, or
accept it with a scoped reason — and 240 sites is architecture work, not a sweep. An absolute check
that fails red for months is one nobody reads.

So it splits in two, and the first half stands alone:
- **Remove the baseline**, keep the check a non-gate. Buys an accurate report with no false NEW,
  costs nothing, and can happen today.
- **Make it a gate**, once the section above has driven the count down far enough that red means
  something. That decision belongs with the work, not before it.

Re-running `--baseline` is the one thing to avoid: it would absorb 133 unreviewed sites into the
known-good set and destroy the signal the file exists to carry.

### ESP32 clang/LLVM toolchain — extend the clang checks to src/platform/esp32/

Espressif ships an xtensa LLVM (their fork), but the installed `esp-clangd` package contains
**only `clangd`** — no `clang++` driver — so a clang analysis pass over ESP32 sources needs their
full LLVM installed separately.

What it would buy: `-Wfunction-effects` (and clang-tidy, clang-query) over `src/platform/esp32/`
— 20 translation units, the only code the desktop build excludes. Everything else, including the
LED drivers, already compiles on desktop and is already checked.

What it costs: a second ~1 GB toolchain, maintained purely for analysis — the firmware would
still be built by GCC, so the analysing compiler is not the shipping compiler. That is a real
"one rule, one owner" tension, and the reason this is a decision rather than an obvious yes.

Worth revisiting when either the coverage gap bites (a hot-path bug traced to the platform layer
that the desktop check could not see) or Espressif's LLVM becomes the default toolchain.

### lizard: 94 functions are measured under a mis-parsed name (baseline can't pin them)

lizard's C++ parser loses the function name on certain bodies and falls back to the first keyword
or cast it meets inside, so `mm::SolidEffect::tick` is reported as `mm::SolidEffect::static_cast<lengthType>`
and `mm::NetworkModule::tick1s` as `mm::NetworkModule::switch`. Measured across `src/`: **94 of
2404** functions carry such a name (`if` 47, `for` 41, `static_cast` 5, `switch` 1), of which **10**
are over threshold and therefore reach the report.

The consequence is the part that matters. `whitelizard.txt` matches by NAME, so those entries pin
nothing: **35 of 162** baseline lines name a function lizard no longer produces. Each is a function
whose complexity is now unmeasured against the baseline — real growth in it would either surface as
a spurious "NEW violation" under the fallback name, or not surface at all. The check currently
reports `FAIL — 9 NEW` on an unmodified tree for exactly this reason, which trains the reader to
ignore the number.

It is not a threshold problem and not fixable by re-baselining: re-running `--baseline` just freezes
today's fallback names, which shift again the moment a line moves inside the body. Real options, in
order of preference: key the baseline on `file:startline`-anchored identity or lizard's `long_name`
instead of `name`; pre-process so the parser keeps the name; or replace lizard's C++ front end with
a clang-AST-based complexity pass (`check_clang_query.py` already has the AST machinery, so the
metric could move there and drop the dependency entirely). Sizeable enough for its own `/plan`.

### clang-tidy: triage the remaining 30 findings

`.clang-tidy` runs `*` minus a documented disable list and reaches zero on everything except the
path-sensitive `clang-analyzer-*` family: **30 findings**, led by
`bugprone-unchecked-string-to-number-conversion` (9), `security.insecureAPI.strcpy` (5),
`core.NonNullParamChecker` (4), `deadcode.DeadStores` (3) and `unix.cstring.NullArg` (3). Roughly
half sit in test code (`strcpy` into fixed local buffers, a deliberate divide-by-zero probe).

The nine `unchecked-string-to-number-conversion` sites were read individually and are all safe:
HueDriver guards on `if (id > 0)`, JsonUtil documents 0-means-absent, MqttModule uses a `v = -1`
sentinel and clamps. They stay reported rather than suppressed — a report shows the real situation,
and a `NOLINT` would hide a genuine class of bug for the next person who writes one of these.

These surfaced late for an instructive reason: the report parser's check-name pattern rejected the
`,-warnings-as-errors` suffix clang-tidy appends when `WarningsAsErrors` is set, so **every finding
was silently dropped** the moment the ratchet was switched on. Fixed; recorded here because it is
the sixth silent-zero this tooling has produced, and each looked like a clean tree.

`WarningsAsErrors` stays empty: clang-tidy reports, it does not gate
([testing.md § Static analysis](../../reference/testing.md#static-analysis)).


### Heap-allocate the `registerType<T>` boot probe (lift a per-module lesson into core)

`ModuleFactory::registerType<T>` stack-constructs a `T probe` at boot to capture `sizeof(T)`. On the main task's ~8 KB stack, a module with large inline members can overflow it and bootloop — a lesson the code records *per module* as a comment rather than fixing once in core: GameOfLifeEffect (`an inline array here caused a P4 stack-overflow bootloop`), HueDriver (`the lightsBuf_ stack-probe lesson`), and AudioService still carries ~5 KB of inline scratch while being factory-registered. This is the [*Complexity lives in core*](../../CLAUDE.md#principles) "lift the rule into core, don't paste it per module" clause: heap-allocate the probe (`MoonModule::operator new` already routes to PSRAM), or capture `sizeof` without constructing at all, so no module author ever has to remember the stack budget. Flagged by the 👾 Reviewer on PR #43. Small, core-only, its own `/plan`.

### Migrate HueDriver's name buffers to `ScratchBuffer` (finish the ScratchBuffer sweep)

`HueDriver::ensureNameBuffers()` / `freeNameBuffers()` is the alloc-in-build / free-in-release / null-guard bundle that `ScratchBuffer` absorbs — and it does **no** `dynamicBytes` accounting, so the module's memory readout under-reports its Hue-light name buffers. It was skipped in the ScratchBuffer migration (its buffers are `char[]` name tables, not the grid-sized state the sweep targeted). Now that `DriverBase::release()` chains to `MoonModule::release()` (fixed on PR #43), a `ScratchBuffer` member here would free correctly on disable and self-report its bytes. Mechanical, one-file, behaviour-preserving. Flagged by the 👾 Reviewer on PR #43.

### Socket-pair fixture for HttpServerModule WS-send tests (test infra)

`HttpServerModule`'s resumable preview send (`sendBufferedFrame` / `drainPreviewSend` / `cancelBufferedSend`, the newest-wins drop, the per-client cursor over `[hdr ++ body]`, the memory-adaptive chunk) has no direct unit test because driving it needs real `TcpConnection` clients whose `writeSome` returns partial / WouldBlock under control — and there's no socket-pair test fixture today. The send *contract* is covered indirectly: `unit_PreviewDriver` drives a `CaptureBroadcaster` mock for route-to-buffered / gate-on-idle / cancel-on-rebuild, and the live device sweep exercises the real drain across ticks. A loopback `socketpair()` fixture on the desktop platform (a `TcpConnection` pair where the test reads the bytes the server pushed, and can simulate a stalled receiver by not draining) would let the drain/drop/cancel/over-push paths be pinned host-side. Build it when the next core transport change lands (it'd also serve future WS tests).

### Adopting the v6.x ecosystem changes (plan)

The as-is state of each item is the table in [building.md § Adopting the v6.x ecosystem changes](../../how-to/building.md#adopting-the-v6x-ecosystem-changes); this entry carries the plan and the triggers.

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

**Guardrails.** Platform-generic stays intact: these are ESP32-specific gains and none may regress Teensy or the desktop paths, which do not use ESP-IDF at all. An IDF feature is adopted *inside* the ESP32 platform layer / build tooling, never by leaking an IDF assumption into shared `src/` or the desktop build. And the v6.0 floor holds: adopt only what is in v6.0, so the v6.0 fallback keeps working.

### ESP-IDF version pinning (pending)

The build IDF is `v6.1-dev-399-gd1b91b79b5`, a dev-branch snapshot (2025-11-05) ahead of the v6.0 stable but on the unreleased v6.1 line. The version facts (what v6.0 vs v6.1 changed, the release schedule, the 30-month support policy, how to check for a newer tag) live in [building.md § ESP-IDF version](../../how-to/building.md#esp-idf-version); this entry tracks only the **open decisions** the doc doesn't make. Being on a dev branch already cost us once — the missing `ESP_ROM_ELF_DIR` in the post-build gdbinit step (fixed in `build_esp32.py`). **Partly landed:** `setup_esp_idf.py` carries `PINNED_IDF_COMMIT`/`PINNED_IDF_VERSION` and **warns on drift** (installed HEAD vs pinned) — it can't `checkout` for you (it doesn't own the clone), but a silent `git pull` or a stray shallow clone is now visible. **Still to do:** (a) a MoonDeck UI banner / status dot surfacing the same drift (the CLI warning only shows during Setup), and (b) the migrate-or-stay call — stay on the pinned commit (chosen for now: it's what all targets incl. P4 were validated against), or move to `v6.1` stable (skipping v6.0, since v6.1 is close); migration is a full re-validation pass across classic/S3/P4, a deliberate task, not a pull. Until then: don't `git pull` the IDF. **Schedule note:** the v6.1-stable target of 2026-07-31 is unlikely to hold — v6.0 slipped ~1 month (planned 2026-02-27, shipped late March), and Espressif minors historically slip 2-6 weeks on the *final* even when betas land on time. So migrate **to the event** (v6.1 stable actually tagging on the releases page), not to the calendar date. `v6.0` stable is the lower-risk fallback if the dev-branch warts (`ESP_ROM_ELF_DIR`, API-churn risk) get worse before v6.1 lands.

### Clock sync — a shared monotonic clock across devices (committed design, unwired)

The second half of the core [multi-device runtime](../../explanation/architecture/mooncore.md#multi-device-runtime); discovery ships, this does not. The design: one leader broadcasts its elapsed time (millis); followers compute their offset, targeting sub-millisecond accuracy. A shared monotonic clock is the foundation any cross-device coordination builds on, which is why it is core rather than light-domain.

The light-domain payoff is a wall of controllers animating in lockstep: effects already animate off elapsed time, so feeding them the leader's synced clock instead of each device's local one is the whole change on the render side. Device-to-device light *distribution* is a separate topology question and rides the existing ArtNet / E1.31 / DDP standards rather than a bespoke protocol.

### Config provenance: firmware/MCU → deviceModel (catalog follow-ups)

The model itself is now a shipped design; see architecture.md § Config provenance (deviceModel is the one provenance level, the `txPowerSetting` example, and "default only where the hardware actually fixes it"). The catalog that carries it is `mooninstaller/deviceModels.json` (schema). The remaining forward-looking pieces — a `devices.json`/MCU-layer split and annotated-pin images — stay gated by the sequencing rule (no catalog field ahead of a consumer).

### Persistence overlay: partial-save / schema-change audit (backlog)

The absent-key fix (`json::hasKey` guard in `applyControlValue`, so a saved file omitting a key no longer zeroes the control's default — the P4 `ethType` no-DHCP root cause) closed the acute hole. A broader audit would harden the overlay against the rest of the schema-drift surface, now that controls carry meaningful non-zero defaults:
- **Type-change safety (not migration, see MIGRATING.md).** `ethType` changed `int16` → `uint8` (Select). A persisted file written under the old type still loads (the flat parser reads the number either way, and Clamp snaps an out-of-range value to the new bounds). The task is to *confirm* each type transition degrades gracefully through the robust loader (absent → default, stale → clamp, unknown → ignore) — especially Text/Password/IPv4 buffer-size changes — NOT to add migration code. A transition that would lose data silently is documented as a break in the documented break, per the settled policy.
- **Test coverage.** `unit_Control_apply_absent_key` pins the absent-key contract; extend with a type-change round-trip (save int16, load into uint8 Select) and a narrowed-range clamp case so a future schema change can't regress silently.
This is hardening, not a known bug — the shipped fix is correct for the cases that occur today.

### ESP32-P4 support — rounds 3-4 (in progress)

Rounds 1 (board + Ethernet-only) and 2 (Parlio LED driver) have landed. Remaining rounds, each its own plan + commit:
- **Round 3 — WiFi via the C6 co-processor. WORKING, BUT THE LINK IS SLOW (2026-08-19).** Boots and associates on IDF v6.1-rc1 (see round 4); the remaining defect is throughput, bench-bisected below. The P4 has no native radio (`SOC_WIFI_SUPPORTED` absent); WiFi comes from the on-board ESP32-C6 over SDIO via `esp_wifi_remote` / esp_hosted. Landed as the `esp32p4rev1-eth-wifi` firmware variant: components pulled P4-only (`rules:` gate in `idf_component.yml`), and `ensureWifiInit()` needs no hosted bring-up of its own: esp_hosted self-initialises at boot via a constructor (`ESP_SYSTEM_INIT_FN`), which sets up the SDIO transport, RPC and wifi-remote channels before `app_main`. Calling `esp_hosted_init`/`connect_to_slave` there would be worse than redundant — `connect_to_slave` is a transport *reconfigure* that resets the slave and re-inits SDIO, which fails on a live link. The rest of the WiFi seam is unchanged because `esp_wifi_remote` is API-compatible. A deliberate, documented [v6.0-floor exception](../../how-to/building.md#esp-idf-version); C6 config via `CONFIG_SLAVE_IDF_TARGET_ESP32C6` + `CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6` + the `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD` SDIO-pin preset.

  **Hardware results (bench, P4-NANO, 2026-06-12):**
  - ✅ **esp_hosted / C6 SDIO comes up at boot.** `host_init: ESP Hosted`, `H_API: ESP-Hosted starting`, `add_esp_wifi_remote_channels`, `H_SDIO_DRV: sdio_data_to_rx_buf_task started`. No NVS error / assert / panic / hang. Device boots fully (~57-60 FPS), `hasWiFi` true, WiFi controls present. esp_hosted **self-initialises at boot via a constructor** (`ESP_SYSTEM_INIT_FN` → `esp_hosted_init`), so no bring-up code is needed in our platform layer — an earlier explicit `esp_hosted_init` + `esp_hosted_connect_to_slave` prelude was *removed*: init was a redundant no-op and `connect_to_slave` is actually a transport *reconfigure* (slave GPIO-54 reset + SDIO re-init). SDIO config confirmed correct on the wire: `CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]`, 4-bit 40 MHz.
  - ❌ **WiFi STA connect fails on the SDIO re-init.** The cascade DOES fire correctly (`Ethernet no link, cascading` → STA path), but `esp_wifi_init()` (forwarded to esp_wifi_remote) internally triggers `esp_hosted_reconfigure` → `Reset slave using GPIO[54]` → **`sdmmc_card_init failed` (×15) → `card init failed` → `esp_wifi_init failed: ESP_FAIL`**. So: the boot-time SDIO init succeeds, but a **runtime slave reset can't re-establish the SDIO link**. The C6 doesn't come back after the GPIO-54 reset during operation. This is an esp_hosted/SDIO/C6-slave-firmware level issue (reset timing or slave image), below our application code — the pins and Kconfig are correct.

  **Open issues before this is done:**

  0. **The esp_hosted build is slower on HTTP — BISECTED to the hosted link, not to us (2026-08-19), and much improved but NOT gone (re-measured 2026-08-21).**

     **Re-measurement on IDF v6.1-rc1**, same board, same commit, same Ethernet interface, same method:

     | build | per-request (`/api/system`) | throughput (76 KB `app.js`) |
     |---|---|---|
     | `esp32p4rev1-eth` | 10 ms flat | 1,973 KB/s |
     | `esp32p4rev1-eth-wifi` | 40 ms typical, one 280 ms outlier in 12 | ~980 KB/s |

     So the penalty is now **4x per-request and 2x throughput**, against 33-60x and 17x when this was written, and **the alternating 0.4/0.8 s pattern is gone** (one outlier in twelve, not every other request). Render is healthy in both (359 fps on the WiFi build). Something between the two IDF versions fixed most of it; what remains is the same shape (per-request, not per-byte) and still worth closing. The original measurement follows.

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

     **Our instrumentation structurally cannot see the hiccup.** `MoonModule::tickTimeUs_` is a MEAN (`accumUs_ / frameCount`, [MoonModule.h:656](../../src/core/module/MoonModule.h)) and `Scheduler::fps()` is derived from it, so a single 300 ms frame among 70 good ones shifts the average ~4 ms and vanishes. Every per-module number in the table above is an average and none of them can confirm or refute a stall. External evidence of the stall is therefore weak but non-zero: sampling `fps` once per second for 30 s gives a steady 73 with periodic dips to 69-70 (~55 ms lost in those seconds), and the dips are NOT on a clean 1 s period. Ruled out along the way: the degradation is not caused by our own HTTP polling (fps is identical at 76 after 25 s of zero traffic and under continuous polling), and the early 96 → 73 decay is warm-up, not load.

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

     **Still unexplained: the visible ~1 s LED hiccup.** Cache contention predicts the steady 2.6x, not a stall. Per-task CPU is cumulative-since-boot and cannot show a spike either. So the hiccup remains unmeasured, and the worst-case instrumentation below is still the next step for it specifically.

     **Instrumentation gap to close first — worst-case tick tracking.** Add a per-module and per-scheduler *max* (and ideally a coarse histogram or a "frames over 2x mean" counter) alongside the existing mean, and expose it in `/api/system`. Cheap (one comparison per tick), and it converts "the PO can see a hiccup that no number shows" into a measurable quantity. Without it, every theory below is unfalsifiable from the host side. Reading the P4 console needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` or a UART adapter on GPIO 37/38 (see the dev-loop note in round 4), so serial is not the fast path.

     Then, cheapest first: (a) read the new max/histogram to see whether the cost is uniform or spiky (uniform favors cache contention, spiky favors a blocking call, and the two coexisting is now the leading reading); (b) the P4 cache performance counters for a direct miss-rate read; (c) moving the hot render path to IRAM/internal RAM and re-measuring, which is also a candidate fix rather than only a diagnostic.
  1. **Runtime SDIO re-init of the C6 fails — CONFIRMED a C6 slave-firmware problem (not a guess).** SystemModule now exposes a `wifiCoproc` read-only control (via `platform::coprocessorWifi()` → `esp_hosted_get_coprocessor_fwversion()`), and on the bench it read **`not detected`** at the time (the control now reports **`no version reply`**, and only after a bounded number of attempts): the C6 returns no valid firmware version. **What that means was overstated here.** It was read as the signature of absent / incompatible slave firmware, but the same board later associated and served traffic over that very link while this RPC still went unanswered — so an unanswered version query says the QUERY failed, not that the C6 is absent. The round-3 conclusion below rests on the `sdmmc_card_init failed` re-init evidence, not on this control. Likely a version mismatch on top of that: The host pulled esp_hosted **2.12.9**; Espressif's P4-Function-EV-Board ships its C6 pre-flashed with esp_hosted slave **v0.0.6**, and the **Waveshare NANO is a different board that may carry a different / absent C6 slave image**. The symptom fits: boot inits the host SDIO master fine, but resetting the C6 (GPIO 54) and re-enumerating it as a slave fails (`sdmmc_card_init failed`) because the C6 has no compatible slave firmware responding. **Primary next step: build + flash the version-matched esp_hosted slave firmware onto the NANO's C6.** The slave project is already vendored at `esp32/managed_components/espressif__esp_hosted/slave/` (`sdkconfig.defaults.esp32c6`, `partitions.esp32c6.csv`); `idf.py create-project-from-example "espressif/esp_hosted:slave"` → `set-target esp32c6` → flash. **Caveat / needs PO + bench hardware:** flashing the C6 on the EV board uses an **ESP-Prog wired to the `PROG_C6` header** with the P4 held in bootloader mode (esp_hosted `docs/esp32_p4_function_ev_board.md` §5.2) — the NANO's C6-flash path must be confirmed (separate USB? equivalent header? ESP-Prog?), and an ESP-Prog may be needed. An OTA slave-update path exists but needs a *working* link first (chicken-and-egg here). This is a hardware-provisioning task, not application code. Secondary fallbacks if firmware-match doesn't fix it: an esp_hosted option to skip the reconfigure/slave-reset when the transport is already up at boot; a slower SDIO freq or 1-bit mode; verify GPIO 54 reset polarity/timing for the NANO. **(Note: EIM — the building.md v6.0-adoption item — does NOT help here; it's a host-machine installer, unrelated to device-side C6 firmware.)**

  **User lead (2026-07-09) — avoid the WiFi teardown/re-init on hosted targets entirely; it may sidestep the slave-reset failure.** A user hit the mirror symptom on a *different* codebase (ESP32-Sveltekit / WLED-MM, not MoonLight — its `lib/framework/WiFiSettingsService.cpp` doesn't exist here): after a clean C6-flash the P4-NANO WiFi worked, but their app's boot-time `WiFi.disconnect(true)` fully tore down the WiFi stack, and the later AP bring-up then failed with `esp_hosted_transport_config: Transport already initialized` / `esp_hosted_init failed!` / `AP enable failed!`. Their working fix: on `CONFIG_ESP_WIFI_REMOTE_ENABLED` (= hosted) targets, **don't do a full stack reset** — keep STA enabled, reconnect *without* a full teardown/re-init, and never call `WiFi.disconnect(true)` in the disconnect callback. **Why this is relevant to us even though the file differs:** our own comment at `platform_esp32.cpp:796` already documents that the esp_hosted transport is set up **once at boot** and is fragile to re-init (`connect_to_slave` = a transport reconfigure that resets the slave + re-inits SDIO and fails on a live link). Our disconnect *callback* is already safe (`wifiEventHandler` on `STA_DISCONNECTED` only sets a flag — no teardown), BUT our **failover path is not**: `wifiStaStop()` (`platform_esp32.cpp:915`) calls `esp_wifi_deinit()`, and the STA-retry / AP-fallback then re-runs `ensureWifiInit()` → `esp_wifi_init()` — the exact deinit→reinit cycle that on a hosted target triggers the GPIO-54 slave reset (round-3 open-issue #2's `sdmmc_card_init failed`). So the round-3 failure ("runtime SDIO re-init of the C6 fails") and this user's report may be **the same root cause**: the re-init shouldn't happen at all on a hosted target. **Concrete next step to try on the bench:** guard the teardown/re-init on `platform::hasWifiCoprocessor` (already defined = `isEsp32P4 && hasWiFi`) — on hosted targets, do NOT `esp_wifi_deinit()` in `wifiStaStop()` and do NOT re-`esp_wifi_init()` in `ensureWifiInit()` once the boot-time init is up; instead just `esp_wifi_disconnect()` + `esp_wifi_set_config()` + `esp_wifi_connect()` (STA retry) or `esp_wifi_set_mode(APSTA)` for the fallback, reusing the live transport. This is cheaper than the C6 reflash and independent of #18759, so it's worth trying first once the board boots. If it works, it also removes the slave-reset from the normal failover, not just the AP case. Blocked behind the #18759 boot crash like everything else P4-WiFi, but this is the first thing to try when the board boots again.
  2. **Co-processor components no longer compile into `esp32p4rev1-eth` — FIXED.** The gate is now `rules: if "$CONFIG{MM_P4_WIFI} == True"` (the `$CONFIG{NAME}` form, no `CONFIG_` prefix inside the braces — the bare form silently skipped the dependency, see round 4 below) (a Kconfig option declared in `esp32/main/Kconfig.projbuild`, set only by `sdkconfig.defaults.esp32p4rev1-eth-wifi`), so `esp_hosted` / `esp_wifi_remote` are pulled **only** by the WiFi build, never by eth-only. The old `target == esp32p4` gate pulled them into `esp32p4rev1-eth` too; that wasn't merely build-time waste — esp_hosted self-inits its SDIO master at boot, which on the eth-only build interfered with the EMAC bring-up (a red herring chased during the P4 no-DHCP hunt). The eth-only image dropped 1.36→1.12 MB once gated out. The `wifiCoproc` read-out stays compile-gated on `platform::hasWifiCoprocessor` (`isEsp32P4 && hasWiFi`).
  3. **Build reproducibility.** `build_esp32.py` does not yet build this variant reliably: the C6 slave-target Kconfig `default ... if IDF_TARGET_ESP32P4` only fires on `set-target`, and the reconfigure a plain `build` triggers drops it back to ESP32-H2 (no WiFi) → fails on missing `CONFIG_WIFI_RMT_*`. A clean manual sequence works (`rm -rf <build dir>` → `set-target esp32p4` → `build`, all with the same `-DSDKCONFIG`/`-DSDKCONFIG_DEFAULTS`); the wrapper needs a fix so the auto-default sticks across reconfigures (see the KNOWN ISSUE comment in `build_esp32.py`).

  **Round 4 — the IDF-update regression (2026-07-03).** After the IDF bump to `v6.1-dev-5215-g0d928780081`, the variant stopped building entirely, then stopped booting. Two distinct causes, both IDF/component-manager side (all our config was correct):
  - ❌→✅ **Build: `esp_hosted.h` not found — the manifest `if` syntax changed.** The component manager (now 3.0.3) silently skipped `esp_hosted`/`esp_wifi_remote` (`NOTICE: Skipping optional dependency`) because our `idf_component.yml` rule used the bare `CONFIG_MM_P4_WIFI == True`. The current manager only recognises a Kconfig variable in the **`$CONFIG{...}`** form (its `KCONFIG_VAR_REGEX = \$CONFIG\{([^}]+)}`); the bare `CONFIG_X` falls through to plain string-eval → false → skipped. **Fixed:** `if: "$CONFIG{MM_P4_WIFI} == True"` (note: NO `CONFIG_` prefix inside the braces). Confirmed by [idf-component-manager #104](https://github.com/espressif/idf-component-manager/issues/104) + the official manifest docs. This also supersedes open-issue #3 above — with the correct syntax the pull is reliable (no `set-target` dance needed for the *dependency*).
  - ❌ **Boot: `sleep_clock_icg_startup_init` aborts with `ESP_ERR_NO_MEM` (0x101) → reboot loop.** A KNOWN, OPEN ESP-IDF bug: **[esp-idf #18759 (IDFGH-17859)](https://github.com/espressif/esp-idf/issues/18759)** — on ESP32-P4 + PSRAM, this sleep-clock retention init runs unconditionally at a SECONDARY boot phase (before `app_main`, NOT gated by `CONFIG_PM_ENABLE`) and fails to allocate its REGDMA retention links when early internal DRAM is tight, which it is once esp_hosted's SDIO stack is pulled in (WiFi build only; the eth-only P4 has DRAM to spare). Espressif's guidance on the issue: reduce early internal-DRAM static usage. Bench findings (2026-07-03): `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` does NOT help (those buffers allocate after the boot init); `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y` drops the ICG file but only moves the failure to the next retention alloc (PCR / int_wdt) then a `sleep_retention.c:914` assert; `.bss`/`.noinit` → PSRAM (`CONFIG_SPIRAM_ALLOW_{BSS,NOINIT}_SEG_EXTERNAL_MEMORY`) still fails — because `MALLOC_CAP_RETENTION` is a *specific reserved memory region*, not general DRAM, so freeing general DRAM doesn't reach it. MoonLight ran the same board on IDF 5.5 without it, so it's a 6.1-era regression. **RESOLVED for us 2026-08-19 by resolution path 1 below** (`CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n` on IDF v6.1-rc1) — a workaround, since the allocation failure is untouched and #18759 remains open upstream.

    **Resolution paths (each independent):**
    1. **Upstream fix — SHIPPED on master, needs a v6.1 back-port or a cherry-pick (rechecked 2026-07-27).** #18759 was marked **Done** on 2026-07-27 and has since been **reopened — it is OPEN upstream** (rechecked 2026-08-20; see the status note at the end of this item). The fix is exactly the knob we wanted: commit `7d31b82d27d` ("change(esp_pm): add kconfig option for REGDMA sleep clock ICG", 2026-07-06) adds **`CONFIG_PM_SLEEP_CLK_ICG_ENABLE`** (`bool`, default `y`) — set it **`n`** and the crashing `sleep_clock_icg_startup_init` is not built/run. Since a 236-FPS LED controller never light-sleeps, ICG retention is dead weight for us, so `=n` costs nothing. **But it is on `origin/master` only — NOT back-ported to `release/v6.1`** (our pinned IDF `14f663f`/dev-5880 and the current `origin/release/v6.1` are the same commit; neither has it). So bumping the pinned v6.1 IDF does *not* get it yet. **Decision (2026-07-27): wait for the next v6.1 beta** that carries the back-port, rather than cherry-pick `7d31b82d27d` onto the pinned IDF — a manual local IDF patch is bespoke, drifts, and complicates the single-pinned-IDF story for a fix that is a clean one-line sdkconfig change once it's in the branch. **DONE 2026-08-19 — that plan executed exactly as written.** v6.1-rc1 published carrying the option as `4b8e1e87106` (verified present in `components/esp_pm/Kconfig` at the pinned commit `44f0c59f7c8`); the IDF pin was bumped, `CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n` added to `sdkconfig.defaults.esp32p4rev1-eth-wifi`, and the P4-WiFi boot bench-tested: it boots, associates (RSSI -52) and serves the UI. The variant is out of the installer's experimental set and ships as a normal firmware. Reported back on the upstream issue ([comment](https://github.com/espressif/esp-idf/issues/18759#issuecomment-5345660136)), which also told the original reporter the option had landed — nobody had announced it in the thread. **#18759 stays OPEN upstream** and we did not ask for it to be closed: the retention allocation still fails under early-DRAM pressure, the option merely means nothing requests that memory. Round 3 is now unblocked, and its remaining defect is throughput, not boot (see open issue 0 above).
    2. **IDF 5.5 fallback — investigated 2026-07-03, NOT a cheap escape.** MoonLight ran this exact board on IDF **5.5** without the crash, so a 5.5 build of just this variant (everything else on 6.1) looked like a timeline-independent path. A bench attempt against IDF 5.5.4 found `src/platform/esp32/` has drifted to genuinely require IDF **6.x**: four distinct 5.5↔6.1 API breaks in `platform_esp32.cpp` / `platform_config.h` — (a) `RMT_LL_TX_CANDIDATES_PER_INST` renamed (5.5: `SOC_RMT_TX_CANDIDATES_PER_GROUP`); (b) `esp_eth_phy_ip101.h` moved (5.5: ctor lives in core `esp_eth_phy.h`, no standalone header); (c) `CHIP_ESP32S31` enum is 6.1-only; (d) `ETH_ESP32_EMAC_DEFAULT_CONFIG()` in 5.5 has out-of-declaration-order designated initializers — a C++ **hard error no compiler flag suppresses** (`-fpermissive` doesn't touch it). So a working 5.5 binary needs permanent `#if`-IDF-version compat branches in the platform layer (an EMAC-init back-port + a second IDF 5.5.4 in the CI matrix) — a real feature, not a throwaway. Only worth it if #18759 stalls long enough that a shippable P4-WiFi is needed sooner.
    3. **Version-matched C6 slave reflash** (see round-3 item 1) — may change the boot memory picture, but is blocked behind the boot crash (host must boot to test the C6 handshake), so it only matters once 1 or 2 gets us to `app_main` stably.
- **Round 4 — Parlio loopback self-test FIXED (2026-07-09), real long strip still to prove.** The Parlio loopback self-test now passes on P4 hardware at any grid size (verified on MM-P4, jumper GPIO 32↔33, at both 8×8 and 128×128). A real *long* WS2812 strip (not just the bench panel) is the remaining hardware proof.

  **The bug it exposed (fixed):** on IDF `v6.1-beta1` the self-test failed `bad bit 0/0` at large grids — it worked at 8×8 but not 128×128. Two stacked hardware limits, both hit only because the test built a frame sized to the *whole operational grid*: (1) the P4 Parlio has no DMA-EOF, so one non-loop transfer is hard-capped at `PARLIO_LL_TX_MAX_BITS_PER_FRAME` (~0.5 Mbit) — a 128×128 grid (~1.2 Mbit) is rejected outright with `ESP_ERR_INVALID_ARG`, so nothing transmits; and (2) even once the transfer fits, the shared RMT-RX capture can't hold a large symbol count (a big frame → `rx captured 0 symbols`). Both surfaced as the same misleading `bad bit 0/0`. **A wrong first guess** (an IDF `sample_edge`→`shift_edge` clock-edge rename) was tried on hardware and rejected — both edge values fail, ruling the edge out; the serial log (`loopback: N bytes in …`, `rx captured 0 symbols`) is what pinned the real cause. **The fix:** the self-test now caps its frame to a small fixed `ParallelLedDriver::kLoopbackTestLights` (256) instead of the operational grid — the test only needs to prove the peripheral emits correct WS2812 bits, which a few hundred lights exercise fully (encode → fused correct+transpose → single-shot DMA → latch pad), and 256 fits under both the transfer cap and the capture buffer on every parallel driver, so it runs identically at any grid size. **Lesson:** an on-device self-test must size its own probe to the peripheral's real limits, not to the user's configured output size. (This whole diagnosis is a good "verify against the working reference before concluding" case — the PO's memory that it *had* worked at fd6c1d8e is what redirected the hunt from "capability gap" to "regression".)

  **Loopback on the MHC-WLED P4 shield — RESOLVED: the header is not pure GPIO (shield builder confirmed, 2026-07-09).** The shield's 4x-In/Out header (pins **46, 47, 2, 48**; 46 & 2 are P4 boot straps per gpio-usage) never passed the continuity precheck: **46↔47** captured partial/corrupt (206 of 1536 symbols → `bad bit 0/0`); **47↔48** read `jumper not detected` even with the jumper seated. The `mm_loopback continuity …: hi=%d lo=%d` diagnostic read `hi=0 lo=0` (no direct signal path). **Shield builder (Wladi) confirmed the cause:** the header has **no pure GPIOs** — outputs go through a **level shifter**, inputs through **diode protection + a ~16 kHz low-pass filter** (by design, for robust button-style inputs), so the raw drive-and-read continuity probe can't bridge it. Not a firmware bug and not fixable in software; the self-test needs a genuinely direct GPIO pair. **Workaround for a real loopback test on the shield (Wladi's suggestion):** drive the LED signal out one **RS485 output**, configure a 4th channel as an **RS485 input**, and jumper the two with cables — the signal is then readable on the respective GPIO. The frame-size fix itself is **proven independent of this** (MM-P4 NANO, direct 32↔33, PASSes at every grid). The `loopbackJumperOk` continuity read is kept as a permanent bench HAL diagnostic (it's what pinned this) — a `hi=0 lo=0` result now has a documented meaning: a buffered/level-shifted header, not a direct GPIO.

  **Dev-loop note — reading the P4's runtime log over USB.** The P4-NANO's primary console is **UART on GPIO 37/38** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`), not the USB port, so `ESP_LOGI` / `mm_net` lines are *not* visible over `/dev/cu.usbmodem*` by default — only the ROM boot banner and `std::printf`-to-stdout (which routes to the **secondary** USB-Serial-JTAG console) come through. Two workarounds when you need the runtime log over USB: (a) temporarily set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (note the JTAG endpoint re-enumerates when the app starts, so a reader must reconnect across the drop — `idf.py monitor` handles it; a plain fixed `pyserial` handle dies); or (b) hang a USB-UART adapter on GPIO 37/38. This cost real time during the P4 no-DHCP hunt; the fastest signal there turned out to be a `printf` of the runtime struct (stdout → secondary JTAG console) plus a `git worktree` bisect (build an old commit, flash, check LAN reachability) to prove code-vs-hardware without needing the log at all.

### WiFi runtime disable (backlog)

Compile-time answer already ships: `--firmware esp32-eth` excludes the WiFi stack. The default `esp32` already *cascades* — `ethInit()` runs first, WiFi only comes up if no PHY responds — so a wired board never associates over WiFi. What's still missing is reclaiming WiFi's **heap**: even when Ethernet wins the cascade, `esp_wifi_init`'s RX buffers stay allocated. This item skips that init entirely once Ethernet is up, freeing ~16 KB. Defer until the heap saving is worth the teardown-ordering risk.


## UI

Forward-looking companion to the shipped UI spec, [moonmodules/core/services.md](../../moonmodules/core/services.md). The live spec describes the UI as shipped; this file holds what is **not** in it yet: deferred items, open design questions for 1.0, and the gap analysis against MoonLight v1.

### Deferred to 1.x

- Side nav with drag-reorder of root modules (root order is fixed in `main.cpp` today; not painful — and arguably correct, see the gap-analysis note below)
- Health panel (`<details>` + `GET /api/test`)
- Log panel (`<details>` + WS `{t:"log",m:"…"}`)
- Core affinity badge (C0/C1) — only meaningful when core pinning lands
- Module `category()` field — taxonomy beyond `role()` for the picker (decision: derive from `role()` for now)

### File Manager follow-ups

The shipped File Manager (see [system.md](../../moonmodules/core/system.md#file-manager)) is a lazy expand/collapse tree over `/api/dir` + a size-capped text editor over `/api/file`, with drag-drop upload (tier 1) + per-file download + a filesystem-usage bar. Deferred capabilities, each self-contained:

- **Upload — recursive folder tier.** Single-file upload of **any size** streams to the file (`fsWriteStream`, binary-safe, `kUploadMax` + free-space guarded) and downloads stream back (`fsReadAt`), so text/config/binary all work. Remaining: **multi-file / recursive folder drops** — client-side `mkdir` + walk via `dataTransfer.items` `webkitGetAsEntry()`.
- **Download — folder as `.zip`.** Per-file download streams (`<a download>` on `/api/file`). A folder download needs the browser to walk `/api/dir` recursively, fetch each file, and build a `.zip` client-side — which means bundling a zip library into `app.js`. That's a permanent app.js size bump, and app.js is embedded in firmware, so it weighs on the flash budget (see the flash-budget item above). Gate on real demand; symmetric with the folder-upload tier.
- **Last-modified dates.** Needs a time source (NTP) + LittleFS mtime storage; the tree is name + size until both land. Backlogged with the NTP work.
- **`.ml` syntax highlighting in the editor.** MoonLive source wants *highlighting* (a color layer over the textarea), not the JSON-style reformat — a bigger editor change (a highlight overlay or a small tokenizer), added when MoonLive `.ml` files land on disk. The editor already has an extension seam (`fmPrettify`) for the reformat case; highlighting is the separate, larger tier.
- **Keyboard + screen-reader access for the tree.** The `fm-row` tree entries are mouse-only today (click to select/expand/open). Making them keyboard-operable (focusable, Enter/Space activation) with ARIA tree semantics (`role="treeitem"`, `aria-expanded` from `isOpen`, `aria-level`) is a real accessibility win but adds JS that lands in the firmware-embedded `app.js` (flash cost). Do it when a11y is a stated goal, together across the whole generic UI, not just this panel.

### Open design questions

These don't block the shipped baseline but should be answered before 1.0:

- **Multi-layer UI** — [the architecture](../../explanation/architecture/index.md) plans for N layers blended into one Drivers. The current card layout shows one Layer. Likely needs a tab/accordion to switch layers, or a per-layer column.
- **Modifier chain visualization** — show the modifier order visually. They're a flat list today, but the `children[]` order **is** the apply order now (modifiers compose as a chain, M₁∘M₂∘…), so a visual that conveys the stacking (and that order matters) would help users reason about a multi-modifier layer.
- **Presets** — save/load named bundles of control values. Persistence already stores them; needs a UI surface.
- **Canvas/node-graph view** — v2 attempted this. Powerful for complex setups but doubles the UI surface. A reasonable v3 follow-up gated on user demand.

### Gap analysis — v1 features not yet in v3

Inventory of v1 frontend behaviours v3 lacks, with a recommendation each. Items already shipped (control types, dragTs, two-timescale inputs, type picker, theme, scroll-shrink preview, status bar, reset-to-default, fps/ms toggle, drag reorder, side nav + drawer + footer) are not repeated.

Legend: **Adopt-1.0** (small, high value) · **Defer-1.x** (needs engine work or a feature we lack) · **Drop** (not needed).

### Per-card features

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Header: setup-dot before name | name only | **Defer-1.x** — needs `setupOk()` + `health()` on MoonModule with a real failure mode. Today both would always be `true` / `""`. |
| Module ID shown separately from name | name only | **Defer-1.x** — add when instances need disambiguating (e.g. two effects of the same type under one Layer). |
| Category emoji badge on the card header | role emoji in the picker, not on the card | **Defer-1.x** — `ROLE_EMOJI` already exists in `app.js`; showing it per-card is a small step if card scannability needs it. |
| Core affinity badge (C0/C1) | core pinning not implemented | **Drop** until core pinning is a real engine feature. |
| Memory split heap vs PSRAM | `static+dynamic` shown on the card | **Defer-1.x** — splitting `dynamicBytes` further needs `platform::isPsramPointer(p)` or per-alloc tracking, neither exists yet. |

### WebSocket / panels

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Drag-to-reorder *root* modules (`POST /api/modules/reorder`) | not supported | **Drop** — root order is fixed in `main.cpp` and that's correct: Layouts/Layers/Drivers + system modules are mandatory and ordered. Children reorder via drag already. |
| Log channel `{t:"log",m:"…"}` pushed by server | no server log push | **Defer-1.x** — needs an engine-side log producer. Gate: when boot/network/persistence logs become interesting to non-developers. |
| Schema channel `{t:"schema",modules:[…]}` for tree-shape changes | full `/api/state` push every update | **Drop** — keep the full-tree push; re-evaluate only if WS bandwidth becomes a problem with large trees. |
| System health panel (polls `GET /api/test`, pass/fail table) | none | **Defer-1.x** — needs a runtime `/api/test` that runs the doctest suite; `ctest` covers this for now. |
| Log panel (ring buffer, severity coloring, stick-to-bottom, `GET /api/log` backfill) | none | **Defer-1.x** — pairs with the log WS channel; both arrive together. |

### Cost / decision table

| Cost class | Items |
|---|---|
| Tiny (< 30 lines, no backend) | category emoji badge on the card header |
| Medium (minor backend change) | help-link mapping (needs docs site); richer `category()` than role()-derived |
| Large (separate plan) | health panel + `/api/test`; log panel + WS log channel; OTA + GitHub-update badge; full multi-layer UI; presets UI |

### GCC Release-mode truncation warnings (17, non-blocking)

`uv run moondeck/build/build_desktop.py --gcc` reproduces CI's compiler (CI builds **Debug**, and that is clean). Building the same tree **Release** with GCC surfaces **17 further `-Wformat-truncation` / `-Wstringop-truncation` / `-Wstringop-overflow` warnings** in `Control.cpp`, `HttpServerModule.cpp`, `JsonUtil.h`, `MqttModule.cpp` — Release inlines more, so GCC can prove more about buffer sizes and gets stricter.

They are **not** CI failures (CI is Debug) and each one inspected so far is a false positive of the same shape: a bounds check exists, but GCC cannot prove the degenerate case (e.g. `HttpServerModule.cpp:2400` warns "writing 1 byte into a region of size 0" on a line *guarded* by `if (wsLen + headerLen > sizeof(hdr)) return false;`). Still worth clearing: each is either a genuinely unprovable bound (fix the code) or a bound the code knows but does not state (make it explicit). Doing it in one pass beats letting them mask a real one later.

Not done with the multi-destination/tab-UI merge because 17 warnings across four core files is its own change, not a tail on someone else's.


## MoonLive core/platform layering + JIT sdkconfig scoping (CodeRabbit #29, 3 findings left)

Four 🟠 Major boundary findings from the PR #29 review are real but each is its own scoped change, not a tail on the ring branch. The Critical sibling (a `cpl<3` overflow guard in the MoonLive effect's `tick`) landed with the branch it was found on.

**One of the four is CLOSED (2026-09-07):** the scenario now uses `PreviewDriver`, the in-process
sink, instead of `NetworkSendDriver`. Its `tick_us` half was reviewed and DISMISSED rather than
fixed: a `measure` step asserts nothing, it records, and that recording is what feeds repo-health's
per-commit performance trend. A reviewer reading the file could not see that; deleting the
baselines would have blinded the trend to fix nothing. Recorded here so it is not re-raised.

The three that remain:

- **Core includes platform, compiled core in `mm_core`.** `src/core/moonlive/MoonLive.cpp` `#include`s `platform/platform.h` and calls the exec-memory API directly, and the root `CMakeLists.txt` compiles `MoonLive.cpp`/`MoonLiveCompiler.cpp` into `mm_core` and links `mm_core → mm_platform` — violating the header-only-core / no-platform-includes contract both files declare. The runtime exec-memory placement layer wants a core-neutral injected interface (or to move out of `src/core`), so the compiled/platform-dependent surface sits behind `mm_platform` and `mm_core` stays INTERFACE-only. These two are one change (same boundary).
- **W^X disabled in the board default.** `esp32/sdkconfig.defaults.esp32s3-n16r8` turns off `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` and enables `CONFIG_HEAP_HAS_EXEC_HEAP` for *every* build on that board, even with no MoonLive effect installed. The JIT genuinely needs a writable-then-executable heap, but that belongs in a dedicated MoonLive/JIT opt-in overlay or an explicit build profile, not the board default — so a stock build keeps memory protection on.
## MoonI80 prime-only ring: no stall backstop (sibling-path gap)

**Found:** 👾 Reviewer, pre-commit on the whole-frame stall fix (2026-07-22).

The MoonI80 wait-timeout backstop (`moonI80Ws2812Wait`, `platform_esp32_moon_i80.cpp`) now recovers a lost/coalesced EOF on **two** of the driver's three completion paths: the lapping ring (`nSlices > ringBufs`, oracle-gated) and the whole-frame path (`!isRing`). The **prime-only ring** (`isRing && nSlices <= ringBufs`) falls through both conditions, so a lost terminator-EOF there leaves `busy` stuck true with no recovery — and every prime/arm/transmit-ring path refuses under `busy`, the same permanent-wedge class the whole-frame fix just closed.

Mitigated in practice: prime-only fires exactly one EOF per frame (no intra-frame coalescing), so the lost-EOF trigger is far rarer than on the whole-frame or lapping paths. But it is the same defect, and per the CLAUDE.md sibling-path rule (a cross-cutting recovery that core owns for one path should cover the sibling path, not be re-implemented per case) the backstop should extend to it rather than leave a third path uncovered.

**Fix:** widen the backstop's condition so a stuck prime-only ring finalizes too — likely a single "any ring frame whose wire time has elapsed with `busy` still set" oracle that subsumes both ring branches, calling the shared `finalizeStalledTransfer`. Verify on the expander wall (prime-only = the small-strand ring config), since the ring recovery is not desktop-testable.

**Related (same file, same wall dependency):** the whole-frame backstop stops the hardware before draining the FIFO and the EOF ISR drops a firing against an empty FIFO, which rejects a late EOF that arrives after recovery. A tighter guarantee — an ISR that has *already passed* the empty-FIFO guard cannot then clear a freshly-recovered `busy` or give `wireFree` for the next transfer — would need explicit serialization (disable the EOF interrupt across finalize+rearm, or an explicit recovery/rearm state the ISR checks). The window is extremely narrow (an ISR in-flight at the instant of finalize) and stopping GDMA+LCD first already disables the source; the hardened version is an ISR-concurrency change that must be proven on the wall, not landed blind.

## ESP32-P4 400 MHz on rev-3+ silicon

**Found:** bench, 2026-07-25 (Quindor Discord question about the P4's 400 MHz).

The P4 build runs at **360 MHz** because IDF's `Kconfig.cpu` caps a `SELECTS_REV_LESS_V3` build (which we set, so a stock binary boots on the v0.x/v1.x P4 chips in the field) at 360; 400 MHz is IDF's default only for rev ≥ 3. Forcing 400 on our bench P4 (a **rev v1.3** chip) was tried and **bootloops** — `assert failed: esp_clk_init clk.c:105` — so 400 is a genuine hardware limit on pre-rev-3 silicon, not marginal stability. The ~11% compute headroom is left on the table for rev-3+ owners.

**Two ways to reach it, both deferred until rev-3 P4 hardware is on the bench to validate:**
- **A separate `esp32p4-400` variant** — `sdkconfig` with `ESP32P4_REV_MIN` = rev 3 + `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400`, so IDF's normal boot-time clock init sets 400 and the bootloader refuses to run on a pre-rev-3 chip (safe). The web installer offers it only to rev-3+ boards. This is the MoonLight-idiomatic per-variant path.
- **A runtime bump** — keep the 360 build (boots everywhere) and, at `app_main`, read `esp_chip_info().revision` and if ≥ 300 call `rtc_clk_cpu_freq_mhz_to_config(400)` + `rtc_clk_cpu_freq_set_config`. One binary, self-selecting. Riskier: a runtime CPU-PLL change on the P4 couples to the flash/PSRAM clock trees configured for 360, so it needs validation on real rev-3 hardware before shipping.

Neither ships until a rev-3 P4 can prove 400 runs clean — no untested clock config, per the same rule the S31/320 and this P4/400 bootloop both taught.

## ESP32-P4: the esp-dsp assembly FFT faults once a second task runs (workaround shipped)

**Found:** bench, 2026-08-28, bringing up HLS on the P4.

**Symptom:** a crash loop the moment the HLS encode task exists alongside the render loop —
`Guru Meditation Error: ... (Illegal instruction)` or `(Load access fault)`, the type varying
between boots, always faulting on the twiddle-table load in `dsps_fft2r_fc32_arp4.S:52`
(`flw fa0, 0(t3)`) reached from `platform::audioFft`. The LED output freezes after a few frames
and the board reboots. Present only with audio analysis running AND a second task; either alone
is stable, which is why it never surfaced before HLS.

**Cause:** the P4's hardware-loop unit has a documented silicon erratum, declared by Espressif's
own `soc_caps.h`: `SOC_CPU_HAS_HWLOOP_STATE_BUG` — "HWLOOP state doesn't go to DIRTY after
executing the last instruction of a loop". FreeRTOS saves the HWLP registers lazily and keys that
save on the DIRTY flag, so a context switch out of the FFT concludes there is nothing to save and
the loop state is lost; the resumed kernel then reads its table through a stale register. The
esp-dsp `_arp4` kernel uses `esp.lp.setup` (the hardware-loop instruction), which is why the FFT
is where it lands. IDF v6.1-rc1 carries workarounds for this erratum at two sites in
`portasm.S` (lines 217 and 795, gated `ESP32P4_REV_MIN_FULL <= 1`, which our `REV_MIN_FULL=0`
build satisfies) — **both on the RESTORE side**. The **save** site (~line 676) reads
`CSR_HWLP_STATE_REG`, skips saving unless it equals `HWLP_DIRTY_STATE`, and carries no erratum
guard at all. That is precisely what the erratum breaks: a task switched out after a loop's last
instruction reports non-DIRTY, so its hardware-loop registers are never saved, and the resumed
FFT reads its twiddle table through a stale register. Restore is patched; save is not.

**Workaround shipped:** `CONFIG_DSP_ANSI=y` in `sdkconfig.defaults.esp32p4rev1-eth`, which swaps
esp-dsp's hand-written assembly kernels for portable C. Measured cost on the bench P4: the Audio
module's tick goes from ~615 us to ~658 us, about 40 us (7%) against 3.3 ms of tick headroom.
Stability confirmed over a soak with HLS streaming: zero crashes, zero corrupt packets.

**Reported upstream:** [esp-idf#19025](https://github.com/espressif/esp-idf/issues/19025)

**Root cause found upstream (2026-09-09), and it is not the one above.** Espressif reproduced it
deterministically and found two separate problems. The "Cause" paragraph above, which names the
unguarded HWLP save path, was our reading of `portasm.S` and it was wrong; it stays as the record of
what we thought. What is actually happening:

1. **esp-dsp violates the P4's hardware-loop constraints.** The loop start (`esp.lp.setup`) and the
   last loop instruction must be 4-byte aligned, the body needs enough 32-bit instructions, and the
   last instruction must not be a coprocessor (FPU/PIE) op. Some kernels are misaligned, and an
   interrupt landing exactly on that boundary corrupts the loop counter. That is why it is
   timing-dependent (it hid for a 20-minute soak on 2026-09-08) and why `CONFIG_DSP_ANSI` cured it:
   the C kernels never enter the misaligned assembly. Fix: an esp-dsp release with `.balign 4`.
2. **The IDF erratum workaround was gated on a misnamed macro** (`ESP32P4_REV_MIN_FULL` without
   the `CONFIG_` prefix), so it was always on for every revision. Being corrected to rev < v3.0 only,
   with a positive test (preemption inside a live hardware loop) and negative tests (interrupt at a
   misaligned boundary).

**To do when the esp-dsp release lands:** bump the managed component, drop the `CONFIG_DSP_ANSI`
line for good, and re-run the workload that found it (continuous FFT + the H.264/HLS encoder as a
second task, sustained HTTP load) on the rev v1 board. We have the reproducing workload and told
them we would report back; that soak is the report.
(2026-08-28), which names the unguarded save path. Espressif already had the symptom on file from
another reporter: [esp-dsp#119](https://github.com/espressif/esp-dsp/issues/119) hits the same
fault at the same instruction on IDF v5.5-beta1 and settles on the same `CONFIG_DSP_ANSI=y`
workaround at the same ~7% cost, and [esp-dsp#102](https://github.com/espressif/esp-dsp/issues/102)
tracks P4 hardware loops in general. So the bug is real, reproducible by others, and spans at
least v5.5-beta1 to v6.1-rc1 - but it is unfixed, and the workaround stays until it is answered.

**What is NOT proven:** the save-path gap is read from the source and matches every symptom, but
it has not been confirmed by instrumenting the switch itself (logging `CSR_HWLP_STATE_REG` on a
switch out of the FFT), nor reduced to a minimal project. Offered upstream if triage wants it. Ruled out on the bench, so nobody re-treads them: worker stack size (8K -> 16K),
core placement (worker on core 1 vs core 0), heap corruption (comprehensive heap poisoning reports
nothing), an unpinned task migrating with coprocessor state (our main task is pinned to core 0),
an FFT size mismatch (512 <= 1024 <= 4096), and a cross-task race on the single `audioFft` call
site. **To report upstream** (esp-dsp and/or esp-idf) with the repro above; the workaround stands
until it is answered, and reverting it needs a bench soak with audio + HLS together.

## Flaky unit tests: the AudioService sync suite contends on a fixed UDP port

**Found:** 2026-07-27, caught by `premerge.py`; pinned to the exact cases by looping the suite and keeping the failing logs. Fails roughly **1 run in 10**.

Two cases in `test/unit/core/unit_AudioService_sync.cpp` fail intermittently, both around the socket rather than the audio logic:

- `:125` *"…drives frame_, then holds it and reports listening"* — `CHECK(strcmp(status(a), "listening") == 0)`. The receive path **binds** the fixed port `kTestSyncPort` (21988); when a previous run's socket is still in `TIME_WAIT` the bind fails and the service never reaches "listening".
- `:95` *"Local+send: lazy-opens once and reports sending"* — `CHECK(strcmp(status(a), "sending") == 0)` plus `syncOpenForTest()`. This one **connects** to a broadcast address rather than binding, so it is a *different* failure mode on the same suite.

**An attempted fix that did not hold (recorded so it is not retried blind).** Deriving the port per run (from the steady clock) instead of the fixed constant cut the receive-path failures but left the send-path case failing ~3 in 20 — because that case never binds the test port, so the port was never its problem. The real fix has to address the send path's socket open on its own terms, not just port choice.

**Why it matters:** the gate scripts run the suite on every commit and merge, so a 1-in-10 flake interrupts gate lists often enough to train the reader to re-run instead of investigate — the habit that hides a real regression later.

**Fix (do not paper over it):** treat the two cases separately. For the receive path, bind an **ephemeral** port (bind 0, read the assigned port back) so two runs can never contend. For the send path, first determine *why* the lazy open fails under repetition — whether the broadcast connect is refused or the latch is left set by an earlier case in the same process. A retry wrapper or a sleep is explicitly the wrong fix: it hides the next genuine failure too. `unit_NetworkReceiveEffect*` and `unit_SineEffect` share the fixed-port shape and deserve the same treatment while the fix is fresh.

## ModuleFactory: registration can fail silently (uint8_t ceiling, unchecked bool)

**Found:** 2026-07-27, while adding the effects grid sweep; flagged by the 👾 Reviewer.

`ModuleFactory::registerType` returns `bool` and **appends unconditionally** — it does not dedupe by name, so the same type registered from several places takes several slots (in the test binary `Layer` holds 9 and `RainbowEffect` 8). `count_` is a `uint8_t`, and at the 255 ceiling `grow()` returns false, registration fails, and the return value is discarded at every call site — including the static-init registrars, which cannot check it anyway. The failure is silent: the type is simply absent from `/api/types` and `create()` answers `nullptr` for it, far from the cause.

Production is not near the ceiling today (~90 types), so this is not urgent. It became visible because a test binary that registers repeatedly can reach it, which is what made the grid sweep construct effects directly instead of registering them.

**Options, cheapest first:** (a) make `registerType` idempotent — ignore a duplicate name and return true, which fixes the multiplier and is arguably the correct semantic anyway; (b) widen `count_`/`capacity_` to `uint16_t` (a handful of bytes, removes the ceiling as a practical concern); (c) make a failed registration loud in a debug build (assert or a boot-time log) so it can never be silent again. (a) plus (c) is probably the right pair.

## Network-receive throughput starves HTTP (issue #69)

**Found:** 2026-08-20, reproducing issue #69 on MM-testbench-S3 (ESP32-S3, 240 MHz, v6.1-rc1).

Reported as "DDP gets very stuttery once the led count gets over a few thousand, even with no led drivers, and the same stream to WLED plays smoothly". Reproduced, and the threshold matches: HTTP availability while a DDP source runs, sampled 6 requests per point.

| lights | pkt/s | KB/s | HTTP |
|---:|---:|---:|---:|
| 480 | 30 | 44 | 6/6 |
| 1440 | 90 | 131 | 6/6 |
| 2880 | 180 | 262 | 6/6 |
| 4800 | 300 | 437 | 3/6 |
| 8160 | 510 | 743 | 0/6 |
| 12288 | 780 | 1136 | 0/6 |

Fully reversible: 8/8 before, 1/8 during, 8/8 after. Art-Net degrades the same way and, per light, sooner — it carries 170 lights/packet against DDP's 480.

**The limit is throughput, not packet rate.** Art-Net survived 510 pkt/s where DDP failed at 300, but both broke at ~440-480 KB/s. A matched-throughput control (~740 KB/s) gave 3/6 for DDP at 510 pkt/s and 2/6 for Art-Net at 1350 pkt/s — 2.6x the packet rate, same result. So a deeper `LWIP_UDP_RECVMBOX_SIZE` (6 today) is not the lever it looks like; the bytes have to be moved regardless of how they are grouped.

**Not the render loop either.** MM-S31 at 4096 lights held 6/6 all the way to 1966 KB/s while ticking at 12 fps (82 ms), against the S3's 8.3 ms tick. If the once-per-tick drain in `NetworkReceiveEffect::tick()` were the constraint, the board with the 10x slower tick would fail first. It is the fastest board that fails, which points at contention between the receive path and the HTTP task rather than at drain cadence.

**Partly fixed 2026-08-20** (`NetworkReceiveEffect`, this branch). The staging→layer `memcpy` ran every tick whether or not a packet had arrived; the Layer does not clear the buffer between frames, so that copy was writing identical bytes. Guarding it on a `dirty_` flag cut the effect's idle tick from **3522 us to ~240 us** at 12288 lights (93%), measured on MM-testbench-S3. HTTP availability under load, same board, before → after:

| lights | pkt/s | DDP before | DDP after |
|---:|---:|---:|---:|
| 4800 | 300 | 3/6 | 6/6 |
| 8160 | 510 | 0/6 | 5/6 |
| 12288 | 780 | 0/6 | 2/6 |

Art-Net gains the same way (4800 lights 3/6 → 6/6; 1350 pkt/s 2/6 → 4/6) — the fix is on the shared staging path, not per protocol. The reported threshold ("a few thousand") is now clean; 12288 lights at 780 pkt/s still degrades.

**Remaining work — measurement, not a fix:** find where the rest of the bytes cost. Candidates are the `recvfrom` copy out of lwIP plus the staging→layer `memcpy` (both O(bytes/s), both on the render thread), and core-0 contention between the lwIP task and HTTP — the LC16 Ethernet starvation entry is the same shape. `handleConnection` running synchronously in `tick20ms` with ~5 ms/~50 ms budgets is the likely victim.

Related: WLED is smooth on the same stream because it receives via `AsyncUDP` — packets are consumed in a callback from the lwIP task the instant they arrive, rather than polled once per render tick. Moving to that model is the structural fix, and it needs `staging_` synchronized against the render thread.

## MIDI as a control surface, and the OpenLamp convention (2026-09-22)

[ControlSurface](../../../src/core/util/ControlSurface.h) was written for MIDI hardware and has no MIDI transport. Its own documentation cites the APC40 mk2's ring-style CCs at 0x18/0x38, the X-Touch MINI's CC 1-8, and per-vendor SysEx for RGB pads, and its four verbs (`sendValue`, `sendRing`, `sendColor`, `sendLabel`) exist because MIDI hardware needs exactly those. OSC is the only transport that implements it. A MIDI transport is therefore a gap the architecture already anticipated rather than a new concept, and it is the obvious second implementation that proves the abstraction holds.

**What OpenLamp offers.** [openlamp-spec-midi](https://github.com/openlamp/openlamp-spec-midi) is an MIT-licensed convention for driving WLED over MIDI: notes 59-68 for hues plus black and white, notes 48-56 for off, on, toggle and blackout, CC 1 for brightness, CC 3-4 for hue and saturation, CC 5-8 for effect, speed, intensity and palette, Program Change for presets, MIDI channel for targeting, and MIDI clock for beat sync. The [organisation](https://github.com/openlamp) also has an Ableton Link and MIDI-clock tempo library, and a CC0 asset set of 72 palette illustrations and 216 effect previews in eight languages.

**Why the convention matters more than the code.** Its engine is Python and cloud-free by design, so nothing there ports to a device. The value is in agreeing what a note and a CC *mean*, since a MIDI transport has to answer that whatever we do, and answering it the same way as a project already aimed at WLED costs nothing and buys a user their existing mappings. The palette and effect artwork is CC0 and separately interesting for the catalog, which today has one screenshot per module and no palette illustrations at all.

**Judge it against principle 2** before adopting: the standard construct beats a bespoke one, and this is a candidate standard. The caution is that the spec says plainly it is a draft, "likely to change, and early on to change quickly", and the organisation has low single-digit stars. So the sequence is a MIDI transport shaped by our own `ControlSurface` first, with the OpenLamp note and CC numbers as the default mapping where they fit, rather than a port of their model. Their beat-sync library is worth reading against [the audio work](backlog-light.md), since MIDI clock is a tempo source we do not have.

**What it is not.** Not a replacement for OSC, which carries labels and arbitrary addresses that MIDI cannot. Not a lighting-control protocol in the DMX or Art-Net sense. This is about a musician's controller driving the show.

Build trigger: someone with a MIDI controller who wants it to drive a device, or the DMX work reaching a point where a tempo source is the missing piece.

### Notes as an effect input, which is the other half

Routing a keyboard through a device so **each note triggers something inside an effect** is a different feature from the surface above, and the difference is worth stating before either is built. A control surface *sets controls*: a fader moves `brightness`, a pad selects a preset, and the effect never knows a surface exists. Notes are **performance data**: an effect reads them the way it reads audio, so what a note does is the effect's decision rather than a mapping table's.

The architecture already has the shape this needs, which is why it belongs here rather than as a new concept. [AudioService](../../../src/core/services/AudioService.h) publishes an [AudioFrame](../../../src/core/util/AudioFrame.h) each block, effects read it through `latestFrame()`, and MoonLive scripts reach the same values through builtins such as `level` and `bands`. `AudioFrame::onset` is already "a hit happened this block", which is exactly a note-on without a pitch. A `MidiService` publishing a `MidiFrame` alongside it would need no new plumbing: the same publish-and-read rule, one more producer.

What a `MidiFrame` plausibly carries: which notes are held and how hard, which arrived this tick and which left, the pitch bend and modulation positions, and the clock's beat phase. Held notes are the interesting part, because that is what audio cannot give an effect. An effect can then light a pixel per key, place a particle at a pitch, colour by velocity, or hold a shape while a chord is held and release it on note-off, none of which is expressible as "a control moved".

The open design questions, all cheap to answer badly and expensive to redo: whether a note is a level that decays or an event that fires once, how polyphony maps onto a grid without the effect hardcoding a keyboard's range, whether an effect opts in (as audio effects do, where `hasAudio` gates them) or every effect sees the frame, and what happens on a device with no MIDI attached. The audio path answered all four already, so the honest first step is to read how it did and follow it rather than invent a parallel set of answers.

**The relationship to OpenLamp:** their spec maps notes to *fixed meanings* (note 59 is a hue, note 48 is off), which is the control-surface half. Notes as performance data is the opposite: the note has no meaning until an effect gives it one. Both can share one MIDI transport, and they want different things from it, so the transport should deliver raw messages and let each consumer interpret, rather than translating to lamp actions on the way in.

Build trigger: the same MIDI transport as above, since both halves need it and neither is worth building it alone.

### The rest of the band: drums, guitar, voice

A keyboard is the easy instrument, because MIDI hands us the notes already decided. Every other instrument in a live setting arrives as **audio**, and the work is deciding what happened in it. That splits into three paths with very different costs, and conflating them is how this becomes a project with no end.

**Path one: the instrument already speaks MIDI, or can.** Electronic drums are MIDI over a cable, so each pad is a note and the work is zero beyond the transport above. A guitar reaches MIDI through a hex pickup or a converter pedal, a voice through a pitch-to-MIDI box. In every case the hard problem sits in a device the musician already owns and has already paid for accuracy on. **This is the path that reaches the most instruments for the least code**, and it argues for building the MIDI transport before anything else here.

**Path two: one instrument on its own channel.** A live rig already splits the band across a mixer's channels, so a direct out or an aux send gives one input carrying one instrument. That is a source-seam question, and [the audio roadmap](audio-dsp-roadmap.md) already covers widening the seam with line-in, codecs and multi-channel front-ends. What it does not cover is the analysis, which is the real work:

- **Drums**: our single `onset` fires on any hit anywhere in the spectrum, so a kick and a snare are indistinguishable. Per-band onsets separate them cheaply, since a kick lives in the low bands and a snare in the mids and highs, and the bands are already computed. This is the smallest useful step and it needs no new hardware.
- **Guitar and bass**: `peakHz` gives a dominant frequency, which tracks a single note and falls apart on a chord. Real polyphonic transcription is a research problem and out of scope; monophonic pitch tracking on a bass or a lead line is not, and the existing peak is most of it.
- **Voice**: a pitch track plus a loudness envelope is achievable and genuinely useful for lighting a vocal line. Recognising words is not, and nothing in a live show needs it.

**Path three: the whole mix through one microphone.** What the device does today. Source separation to pull a drummer out of a mixed stereo field is a machine-learning problem that does not fit a microcontroller, and pretending otherwise would be the kind of speculative build principle 1 rejects. The honest ceiling here is what the band structure already gives: bass energy, spectral flux, a tempo estimate.

**The one unifying idea worth keeping.** Whatever the source, the useful output is the same shape: *this instrument did this thing, this hard, just now*. A MIDI note-on is that. A per-band onset is that. A pitch track is that with a pitch attached. So the frames stay separate at the source (`AudioFrame` and a `MidiFrame`) and an effect reads whichever it wants, rather than one merged everything-frame that forces every producer to pretend it is the others. That keeps each producer honest about what it actually knows.

**The order this argues for**, cheapest and most certain first: the MIDI transport (which reaches electronic drums, MIDI guitars and pitch-to-MIDI voice with no analysis at all), then per-band onsets (drums from a channel, and a better beat for everything), then monophonic pitch (bass and vocal lines), and only then anything about separating a mixed signal.

Build trigger: a live rig to test against. Every step is measurable in an evening with an instrument in the room, and none of it is worth guessing at without one.

### Everything as MIDI, and where the conversion should live

The appealing shape is one where every instrument on stage arrives as MIDI: the drums, the guitar and the voice each converted, so the device reads one kind of event and an effect never asks where a hit came from. It is the right instinct, and the question it turns into is **where the conversion runs**, because that decides whether this is a weekend or a research project.

**Converted off the device, it is available today.** Drum triggers, hex pickups and pitch-to-MIDI pedals all exist, are well made, and put the hard analysis in hardware the musician already trusts. A mixer's direct outs feed those boxes, their MIDI merges onto one cable, and the device reads notes. **Nothing here needs writing beyond the MIDI transport.** That is the version worth having first, and it is genuinely what a working stage rig already looks like.

**Converted on the device, each instrument is a different problem.** Calling them all "X to MIDI" hides that the three have nothing in common technically. A drum channel is an onset detector, which we nearly have. A bass line is monophonic pitch tracking, which is a known algorithm and real work. A voice is pitch plus envelope, doable. A strummed guitar chord is polyphonic transcription, which is a research problem that does not fit a microcontroller and should be named as out of scope rather than left as an implied maybe.

**The cost that bounds it.** One mono audio input measures about **8 ms per tick on a P4**, already the second-largest consumer on the board after the effect itself. Four instruments analysed on-device is four FFTs and four detectors, which is most of a frame budget before a single light is drawn. So the ceiling is not the algorithms, it is the arithmetic: **one or two analysed channels on a device, not a band.** A mixer feeding eight direct outs into one ESP32 is not a design this hardware supports, and saying so early is cheaper than discovering it late.

**Where a mixer does fit.** A direct out or aux send giving **one clean instrument** is exactly the input that makes path two work, and it is worth more than any algorithm: a kick drum alone on a channel needs a threshold rather than a separator. So the mixer plug-in is valuable for a *small* number of channels, which is also the number the frame budget allows. The seam for that is the multi-channel front-end [the audio roadmap](audio-dsp-roadmap.md) already anticipates.

**The architectural conclusion.** Rather than converting everything to MIDI on the way in, keep the frames separate and let an effect read what each producer honestly knows: a `MidiFrame` for what arrived as notes, an `AudioFrame` per analysed channel for what arrived as sound. Forcing a per-band onset to pretend it is a note-on adds a fake pitch and a fake velocity, and an effect that reads them cannot tell which are real. Same rule as above, applied to the tempting case.

Build trigger: a stage with more than one instrument to point at it. The first honest experiment is one drum channel through a direct out, because it answers the cost question and the usefulness question at the same time.

## OSC pads and the Open Stage Control session's labels (2026-08-30)

Two gaps found wiring a real control surface to the [OSC module](../../moonmodules/core/services.md).

**`/mm/pad/N` has no handler.** The [OSC plan](../past/plans/Plan-20260829%20-%20OSC%20control%20ingest%20(shipped).md)
lists it (`i 1 -> apply preset in slot 12`), and `OscModule::handle` routes `/mm/fader/`,
`/mm/encoder/`, `/mm/switch/` and `/mm/control/` but not pads. So a surface can drive every
continuous control and every switch, but cannot fire a preset, which is the one thing a pad grid
exists for. The route is small; what needs deciding is what a pad press means when the slot is
empty, and whether a nonzero value is a press or a press-and-hold.

**The shipped Open Stage Control session renders no labels, and its pad matrix draws nothing.**
[`docs/reference/examples/open-stage-control.json`](../../reference/examples/open-stage-control.json)
works for every fader, encoder and switch, but the widget names never appear and the `matrix` of
pads is an empty box. Five attempts at the label property failed (`@{}`, `JS{}`, `#{}` with `unit`,
an explicit string, omitting it so `"auto"` applies), and notably a session SAVED by Open Stage
Control itself carries no `label` key on a fader at all, so the name is drawn from something else.
The session was written by reading a minified app's bundled HTML docs; the reliable fix is to build
one widget of each kind by hand in its editor, save, and copy the shape it produces. Cosmetic: the
control path works, only the labels and the pad grid are missing.

## POST /api/file reports success while writing an empty file (no Content-Length)

**Found:** 2026-08-20, on MM-testbench-S3, while uploading an edited MoonLive script. The upload
answered `{"ok":true}` twice while the board kept the previous file, and a compile result was read
from the stale script before the mismatch was noticed.

`contentLen` defaults to **0** when the request carries no `Content-Length`
(`HttpServerModule.cpp:152`, "declared body length (0 if no Content-Length)"). `handleWriteFile`
then clamps the already-buffered body against it:

```cpp
const size_t initial = initialLen < contentLen ? initialLen : contentLen;   // → 0
```

so `fsWriteStream` streams nothing, succeeds, and the handler answers 200. The body on the socket is
discarded. Reproduced deliberately: an 11-byte file written with `Content-Length: 11` lands as 11
bytes; the identical request sent `Transfer-Encoding: chunked` answers `{"ok":true}` and leaves a
**0-byte** file, destroying what was there. Any HTTP client that streams without declaring a length
hits this — `curl --data-binary` on a pipe, and chunked uploads generally.

This is data loss reported as success, which is the part that matters: a caller has no way to know
the write did not happen, and the previous contents are already gone.

**Options:** (a) reject a body-bearing POST with no `Content-Length` (411 Length Required) — smallest,
honest, and standard; (b) support `Transfer-Encoding: chunked` in the upload source, which is more
work and only worth it if a real client needs it; (c) at minimum, never report 200 for a write whose
byte count does not match what was declared. (a) plus (c) is the pair worth doing. The UI's own
File Manager always sends a length, so this does not affect it — an API caller or a script does.

Pin with a test that a length-less upload does not report success and does not truncate the target.

## repo-health compares numbers from different machines (2026-08-22)

`repo-health.json` records one value per metric with no note of which host produced it, so running the KPI gate on a second machine rewrites the baseline with figures that were never comparable. Measured on the same commit: desktop flash reads 1,060 KB on a Windows/MSVC bench against 1,165 KB recorded on macOS/clang, printed as "−105 KB ✓"; desktop tick reads 368 µs against 179 µs, printed as "+189 µs ⚠". Neither is a change in the code. The firmware rows are now guarded by a freshness rule, which stops a stale binary being re-measured, but freshness cannot detect a different compiler or a different CPU.

The file's own docstring states the property this breaks: "two machines agree and a number never moves for a reason nobody can explain". Two ways out, and it is a design call rather than a bug fix: key the host-dependent metrics by platform (`flash.desktop.windows`, `perf.desktop.macos`) so each machine tracks its own trend, or declare one canonical machine (CI) the only writer and have every other run print the delta without saving it. The second is less data and less honest about a Windows contributor's numbers; the first grows the file, which its "never grows" design resists. Until then, read a cross-host delta as noise.

## MoonDeck scripts crash on Windows when run BY HAND (2026-08-22)

62 of the ~64 scripts print `→ ✓ ⚠ —` or box-drawing characters. Run from a Windows terminal their stdout takes `locale.getpreferredencoding()` — cp1252 — and the first such character raises UnicodeEncodeError, *after* the real work has succeeded: `collect_kpi.py` measures everything, writes the metrics, then dies printing the summary arrow. Every path is now exposed: the gate runner that handed children `PYTHONIOENCODING=utf-8` is gone, so a Windows agent run hits it too, not only the human path MoonDeck exists for.

Per-script `sys.stdout.reconfigure()` is the wrong shape at 62 files: every new script would have to remember, and the one that forgets fails in the field. It wants ONE home — the candidates are a `PYTHONUTF8=1` in whatever env MoonDeck's front ends already establish, or a shared `moondeck/_stdio.py` imported by the handful of scripts that are entry points. Pick when someone next runs a check by hand and it dies on a tick mark.

## `disasm.py` cannot run on Windows (2026-08-21)

The tool shells out to `c++` and `objdump` to build the per-ISA emitter and disassemble its bytes, and a Windows machine carrying only MSVC has neither. That holds for every ISA, not just the new `x86_64` one — but x86-64 is where it bites, because the host it disassembles is now a Windows desktop and the tool is what turns "the script did nothing" into an answer. The x86-64 backend was brought up without it, using byte-pinning tests plus WinDbg on the emitted image; that worked, and was slower than reading the instructions would have been.

Closing it is a compiler/disassembler pair behind the two `subprocess.run` calls: `cl.exe` for the build, and for the disassembly either LLVM's `llvm-objdump` (which understands `-b binary` the way the tool already expects, and ships with the VS "C++ Clang tools" component) or a `.obj` wrapper around `dumpbin /disasm`, which does not. Prefer the former — the flags are already right, so it is a lookup rather than a second code path.

## A non-ASCII Windows profile path defeats the desktop settings directory (2026-08-23)

`std::getenv("LOCALAPPDATA")` returns the ANSI form of the path, so a Windows user whose profile name carries characters outside the system codepage (CJK and Cyrillic on a Western machine; most accented Latin survives cp1252) gets `?` where those characters were. `?` is not legal in a Windows filename, so `create_directories` fails, `fsMount` returns false, and the driver reports `cannot use ..., persistence disabled` naming the mangled path. It degrades visibly rather than corrupting anything, which is the standard Principle 5 asks for, but that user has no working persistence.

Reading the variable wide is only half of it. `toFsPath` composes a `std::filesystem::path`, which stores wide on Windows, but every open in the layer goes back through `.string()` to reach `std::fopen` (fsRead, fsReadAt, fsWriteAtomic, fsWriteStream, and the mount probe), a narrowing the code comments on deliberately at `fsRead`. So a wide `LOCALAPPDATA` alone would produce a correct path that still cannot be opened: the fix is one `_wfopen`-on-Windows helper shared by all five sites, plus a test with a non-ASCII root.

Not done with the per-user data directory because it reverses a documented decision across the whole desktop filesystem layer, four of whose five call sites predate that change. What the change did do is make the root capable of containing a username, where it was previously the literal `build`. Worth closing the next time this file is opened for other reasons.

## Opt-in usage reporting: know which hardware and configurations are actually out there (2026-09-01)

We build for hardware we cannot see. Which chips people actually run, how big their walls are, which drivers and features they enable, and how many are still on an old version are all questions currently answered by whoever happens to speak up on Discord. That is a biased sample: it hears from people with problems and from people who post, and it is silent about the majority who install something and it simply works. Effort therefore goes where the loudest reports are rather than where the users are, and a decision to drop support for something rests on a guess.

**What a report would carry.** Enough to answer "what should we build and what may we stop supporting", and nothing else:

- **The machine.** Chip family and variant for a device (classic ESP32, S3, P4, S31 and so on), flash size, whether PSRAM is present and how much. For a desktop, the OS and architecture instead, which is a question we have no way to answer at all today.
- **The version.** Firmware or application version, release channel, and on an upgrade the version it came from. Whether this was a fresh install or an upgrade.
- **The light setup, in shape rather than content.** Total light count, which layouts are in use and their dimensions, which drivers are active, and how many outputs or buses. Not what the wall shows, only how big it is and how it is wired.
- **Which features are switched on.** The modules present and enabled: audio, MoonLive, MQTT, Art-Net or E1.31 send and receive, panel cards, Ethernet versus WiFi. This is the half that answers "is anyone actually using this", which is what decides whether a feature earns its maintenance.
- **A country**, derived at the server from the address the request arrives from, so the map is by region rather than by installation.

**What it must never carry**, and this list is a constraint on the design rather than a note on it: device name, IP or MAC address, WiFi credentials, MQTT passwords, any free-text field a user typed, and the contents of a layout or a script.

**The identifier: a random value per install, dropped after seven days** (settled 2026-09-08). Not derived from the MAC or from anything else about the hardware, so it cannot be reversed to a device even by whoever holds it. It exists to answer one question the aggregates cannot: how many DEVICES, rather than how many reports, and therefore how many installs are still on an old version. The server keeps per-report rows for seven days, de-duplicates, then keeps only the aggregate and drops the id.

Two options were weighed and rejected. **No identifier at all** was the original text here: simplest to promise, but it makes every per-device question unanswerable, and "how many are still on the old version" is one of the questions this whole entry exists to answer. **A hashed MAC** is what WLED shipped first (`sha1("WLEDUSAGE" + MAC)`) and it buys exactly one thing more than a random id, given reports are one-time: linking one device's reports across months. That is the tracking capability itself, it survives a factory reset and a reflash, an open-source salt over a MAC space that size is rainbow-tablable, and a stable pseudonymous identifier is personal data under GDPR rather than anonymous. WLED's own shipped version moved off it to an anonymous id, which is the same conclusion from people who had it running.

**Consent.** Opt-in, from a prompt shown after a fresh install or an upgrade, with a decline that is as easy to click as the accept and is remembered. One report per install or upgrade, never a heartbeat. A user who declines transmits nothing at all, rather than transmitting a "declined" record.

**The privacy policy already commits to this shape** ([docs/legal/privacy-policy.md](../../legal/privacy-policy.md)), including the honest wording about the IP address a server unavoidably sees. Whatever is built has to match what is promised there, and the policy has to be updated with the specifics BEFORE the feature ships, not alongside it.

**Open questions for whoever picks this up.** Where the server runs and who administers it, since it is the first piece of infrastructure this project would own rather than borrow from GitHub. Whether the dashboard is public, which we would want it to be for the same reason the source is. Whether the desktop reports at all or only devices, given the desktop is the half we currently know nothing about. And what happens to a report from a version whose fields have since changed, because a schema that cannot be read a year later answers nothing.

**WLED ships this, and the dashboard is public**: [usage.wled.me](https://usage.wled.me). Worth reading before building anything, because it answers two of the questions above by example and disagrees with this entry on a third.

What it shows: distributions by version, chip, matrix, flash size, PSRAM, release, LED count and filesystem usage; upgrade versus install events over six months, split by chip; which LED features, peripherals, integrations, usermods and bus types are in use; and device count by country. That list is close to what is proposed above, which is reassuring about the shape.

How it is collected ([PR #5116](https://github.com/wled/WLED/pull/5116), merged 2025-11-27, superseding an earlier [#4342](https://github.com/wled/WLED/pull/4342)): a one-time POST to `usage.wled.me/api/usage/upgrade` when a persisted version file shows the firmware changed, behind a startup prompt offering Yes / Not Now / Never Ask, and suppressed entirely in AP mode. The server is open source ([netmindz/WLED_usage](https://github.com/netmindz/WLED_usage)), which is how they answer "is the deployed code the published code".

**Where they differ from this entry, and it is the interesting part.** WLED sends a device id: the first design hashed the MAC (`sha1("WLEDUSAGE" + MAC)`, "unique but not reversible"), the shipped one an anonymous `deviceId`. Either way the point is de-duplication, and it is what lets their dashboard say how many DEVICES rather than how many reports. This entry deliberately refuses that, and pays for it in accuracy. Their earlier PR also discussed a retention split (per-device data for 7 days, aggregates longer), which is a middle position between the two: keep an identifier only long enough to de-duplicate, then drop it. Worth considering rather than assuming the strictest option is automatically right.

### The smallest honest version

Sketched 2026-09-08. Not started; the blocker is the server, not the firmware.

**The firmware side is small, because the payload already exists.** Every field is a control or a
module name the device publishes today: `chip`, `cpu`, `flash`, `psram`, `sdk`, `firmware` and
`deviceModel` are SystemModule controls; `version` and the previous version are FirmwareUpdateModule
controls; the enabled modules are the state tree's own top level; the light setup is the Layouts
grid and the Drivers children. So this is a serializer over facts already in memory, not new
instrumentation. Budget it as one module of a few hundred lines, plus the consent prompt.

**When it sends.** Once, when a persisted version file shows the firmware changed, which is the
shape WLED converged on after starting from a UDP spray every second. Never a heartbeat. Suppressed
in AP mode, where there is no internet and a device is usually mid-provisioning.

**Consent.** A prompt on the first boot after an install or upgrade: Yes / Not Now / Never, with
Never remembered. A decline sends nothing at all, not even a "declined" record. The prompt says what
is in the report in one sentence, and links the privacy policy.

**Ask WLED to host it, before building a server at all.** Investigated 2026-09-08, and their server
is closer to a fit than expected.

[netmindz/WLED_usage](https://github.com/netmindz/WLED_usage) is Kotlin on Spring Boot with a MySQL
schema under Flyway migrations, a Docker compose deploy, and one endpoint:
`POST /api/usage/upgrade`, no auth, with the country derived server-side from an `X-Country-Code`
header. The dashboard is a single static `index.html` against a `/api/stats` controller.

**It is already multi-project.** The `device` table carries a `repo` column (added 2025-12), there
is a `RepoHistory` entity, and EVERY stats query is written `(:repo IS NULL OR repo = :repo)`. So a
second project reporting under its own repo name is a supported case rather than a change they would
have to make, and our figures would not be mixed into theirs.

**We adopt their schema unchanged, and drop the one field that does not fit** (settled 2026-09-08).
chip, version, previousVersion, releaseName, ledCount, isMatrix, flashSize, psramSize/Present,
fsUsed/Total, busCount and busTypes mean the same thing in both projects, so those figures are
directly comparable and MoonLight can sit in a pooled overview rather than being a special case.
Our own vocabulary rides the free-form lists: ledFeatures, peripherals, integrations, and `usermods`
for the enabled-module names. That works because migration V2026040301 (2026-04) replaced twenty-odd
fixed boolean feature columns with three comma-separated lists, aggregated by counting whatever
values appear: adding MoonLight's names costs their server nothing.

The layout dimensions this entry originally wanted (width x height x depth, since we are 2D and 3D)
have no equivalent there, and are DROPPED rather than added. `ledCount` plus `isMatrix` answers most
of what they were for, and being identical to a schema someone else maintains is worth more than one
field. If a real question later needs the third dimension, a list value is the place for it.

**Their payload is close to what this entry wants.** `UpgradeEventRequest` already carries
deviceId, version, previousVersion, releaseName, chip, ledCount, isMatrix, bootloaderSHA256, brand,
product, flashSize, partitionSizes, psramSize, psramPresent, repo, fsUsed, fsTotal, busCount,
busTypes, ledFeatures, peripherals, integrations, usermods. Every field we listed above maps onto one
of those except the layout dimensions, and `usermods` is the natural home for "which modules are
enabled". Sending it means shaping our report to their names, which is a small price for not owning
a server.

**What to settle with them before relying on it.** The repository has NO LICENCE file, so
strictly nobody may reuse it, and it was last pushed 2026-05-31, so it is quiet rather than dead:
both are conversations rather than blockers, but they are conversations to have first. Then the real
questions: are they willing to take another project's reports at all, who administers the box and
what happens to our data if that person stops, does the public dashboard gain a repo selector or
would we render our own from their API, and does the seven-day identifier retention this entry
commits to match what their server actually does (their earlier PR discussed it; the shipped schema
keeps a `device` row keyed by id, which suggests it does not).

If the answer is yes, this feature loses its blocker entirely and becomes a firmware change plus a
conversation. If it is no, the plan below stands.

**The server is the whole cost, and it is a standing commitment rather than a feature.** It is the
first infrastructure this project would own rather than borrow from GitHub, and it needs an
administrator, a domain, TLS, a retention job that actually runs, and a public dashboard. WLED
publishes their server ([netmindz/WLED_usage](https://github.com/netmindz/WLED_usage)), which is how
they answer "is the deployed code the published code": whatever runs here should be public for the
same reason the firmware is. Until someone owns that, this feature cannot ship honestly, and that is
the reason it is still in the backlog rather than in a branch.

**Order of work, and the first two are worth doing whether or not the rest ever lands.** Write the
privacy policy revision FIRST, since it is the promise everything else has to match, and the current
page says plainly that nothing of the kind exists. Then the report BUILDER as a pure function over
the state tree, with a unit test asserting that the forbidden fields cannot appear in its output:
that test is the design constraint made executable, and it is worth having even if nothing ever
sends. Only then the consent prompt, the one-time trigger, and last the server.

## A driven GPIO the Pins map never sees: bus padding, and a hidden clockPin

**Found:** 2026-08-21, on MM-S31, after a bench session that started as "the LED panel stopped working" and cost hours chasing a firmware regression that did not exist.

An ESP32-S31 driving a ColorLight receiver card over raw Ethernet showed the panel dark while every diagnostic said the transmit path was healthy: link negotiated at 1000 Mbit, ~4600 packets/s, zero drops, and a byte-for-byte dump of a 128x128 frame matched `ColorLight5A75Packet.h` exactly. The card's own activity LED never blinked. Four firmware versions across two ESP-IDF releases behaved identically.

The cause is a **GPIO collision that no part of the system could report**. `ParallelLed` carried `clockPin = 10`, and GPIO 10 is `txd2` on the S31's RGMII bus (`platform_esp32.cpp`: the EMAC's fixed IO_MUX pads are 8-19 plus MDC/MDIO on 5/6). The LED driver drove one of the four Ethernet transmit data lines, so every frame left the MAC counted-as-sent and arrived corrupt. Disabling the LED drivers fixed it instantly; moving the clock to GPIO 21 fixed it with all four drivers running.

Three separate defects made this invisible, and each is worth fixing on its own:

**1. Pins the map cannot see, because nothing declares them.** The shipped conflict soft-flag grades what modules *declare*, so an undeclared pin is invisible to it however hard the silicon drives the pad. Three cases, two now closed:

- ✅ **Bus-padded lanes** (fixed): a one-strand board had seven i80 lanes parked on `clockPin`, driven at bus-clock rate and listed nowhere. `spareLanesNeedPad()` stops the padding on a backend that routes its own GPIOs, so the pin is no longer driven and the map is truthful again.
- ✅ **RGMII data pads** (fixed): all twelve are now published by NetworkModule as read-only pin controls from one `platform::ethRgmiiPads` list that `ethInitEmac` also reads. Verified on MM-S31: `gpio 10` reports as `ethTxd2`.
- ✅ **P4 RMII data pins** (fixed): the six lines the EMAC drives are in `platform::ethFixedPads` and reported through `fixedPins()`, same as the S31's RGMII pads. Verified on MM-P4: Network owns all ten of its Ethernet GPIOs (28/29/30/34/35/49 plus MDC 31, MDIO 52, clock 50, reset 51), against four before.
- ✅ **Classic ESP32 RMII data pins** (fixed): TX_EN 21, TXD0 19, TXD1 22, CRS_DV 27, RXD0 25, RXD1 26, entered as a third `ethFixedPads` branch. Sourced from IDF's own RMII Data Plane GPIO table (`docs/en/api-reference/network/esp_eth.rst`): one IO_MUX choice per signal, which is why `ethInitEmac` never sets them. Verified on MM-Olimex: all ten Ethernet pins claimed, `Eth: 192.168.1.210 (100 Mbit)`.
- ✅ **MDC/MDIO on the classic ESP32** (fixed): the chip default was `mdc -1, mdio -1`, so `ethInitEmac` skipped the assignment, IDF applied its own 23/18, and the map claimed neither. `ethConfigDefault` now states 23/18, sourced from IDF's own classic default and our QuinLED Dig-Octa entry. Verified on MM-Olimex: `Eth: 192.168.1.210 (100 Mbit)` with `gpio 23 MDC` and `gpio 18 MDIO` in the map. NOTE: a board with -1 already persisted keeps it until those controls are set once or NVS is erased.

**The fix is the RGMII one, extended.** Being fixed in silicon does not make `gpioCapability`'s reserved list the right home: reserved means "routing I/O here corrupts the device" (flash, PSRAM, USB), which is unconditional, while an EMAC pad is only held while that interface runs. With `ethType = None` the init returns false and every one of those GPIOs is free for LEDs, so a static reserved list would permanently forbid pins a WiFi-only board can use. What makes a control the right shape is not that the pin is configurable (it is not) but that the claim is CONDITIONAL: published when the interface is selected, released when it is not, which is exactly what the pin map reads.

So the same `platform::ethRgmiiPads` treatment applies, with one wrinkle. On the **classic ESP32** the RMII data pins are silicon-fixed, so a second per-chip pad list serves them directly. On the **P4** 49/34/35/28/29/30 is the Waveshare NANO's *board* wiring that the IDF macro happens to default to, not a chip constant, so those belong in `deviceModels.json` beside the other per-board eth pins, letting a different P4 carrier declare its own. Both then reach the pin map through the control path already built, rather than a third mechanism.

Lower risk than the RGMII case (six pins rather than twelve, and nothing of ours currently collides), but the failure mode is identical: a driver claims one, the MAC still reports a healthy link, and every frame goes out corrupt.

**2. `busPinList()` pads spare lanes with `clockPin`, and MoonI80 routes them as data.** The i80 bus is always 8 or 16 bits wide (`ParallelLedDriver::busWidthPins`), so a board driving one strand gets seven lanes parked on the clock pin, and `configureGpio` (`platform_esp32_moon_i80.cpp`) routes every entry it is given. The padding exists because `esp_lcd` rejects an NC data pin, but the MoonI80 backend does not have that limit: its own comment says *"pins past `laneCount` go nowhere"*. It is simply never told the real lane count. Passing it would free six or seven GPIOs on every direct-mode board and remove the hidden claim at the source.

**3. `clockPin` defaults to 10 and is hidden.** `MoonLedDriver::clockPin = 10` is a hardcoded default that lands inside the S31's reserved RGMII block, and `addBusControls` hides the control unless `pinExpanderMode()` is on. So on this board the value was invisible on the card, unchangeable through the UI, and still driving a pad. A pin with a real effect must be visible, whatever mode it is in.

**This also closed the S31 Ethernet defect, open since 2026-07-26.** That entry (removed) blamed an RGMII Tx-clock mismatch at 100M for DHCP never completing, and had concluded "the frames never reach the router". The cause was the same collision: `ParallelLed`'s default `clockPin = 10` is `txd2`, so a DHCP DISCOVER was garbled exactly as the panel frames were. With the clock pin moved off the RGMII block the S31 leases normally, verified on the bench at `Eth: 192.168.1.125 (1000 Mbit)`. Two long-standing bugs, one GPIO.

## Input transports: foot pedals, USB game controllers, and MoonLive at the pins (2026-09-01)

`ButtonService` shipped with the [GPIO seam](../present/Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20(partial).md)
(`gpioInputBegin` / `gpioRead` / `gpioWrite`). It names a target as `Module.control` and writes it
through `Scheduler::setControl`, so a press and an OSC message are indistinguishable downstream.
Three follow-ups build on that seam rather than beside it.

**Foot pedals are two different things**, corrected in
[input-mapping-analysis.md](input-mapping-analysis.md): a **footswitch** is a switch on a TS jack,
which `ButtonService` in momentary mode already covers, while an **expression pedal** is a
potentiometer on a TRS jack, which is an ADC read mapping onto a fader and does not exist yet.
Neither is USB. **Action: document the footswitch, scope the expression pedal as an analog-input
service.**

**USB game controllers are their own project, and are S3/P4-only.** The mapping half is trivial once
reports arrive (buttons to pads, axes to encoders and faders, all through `setControl`); the
transport half is not:

- The **classic ESP32 cannot do it at all**: no USB Host peripheral, no USB OTG. So the Dig-2-Go,
  and every classic board, is out from the start.
- **S3 and P4** have USB-OTG and the IDF ships a USB Host stack with a HID class driver. A gamepad
  is a HID device whose report descriptor must be parsed to learn which byte is which button or
  axis, and that parsing is the real work: descriptors vary per vendor, and the well-known
  controllers each have quirks.
- **Desktop** would go through the OS gamepad API behind the same seam, a wholly separate
  implementation.

Expect a plan of its own, after the GPIO inputs have settled. The honest scope is "a HID report
parser plus a mapping UI", not "read a controller".

**MoonLive at the pins** is the piece that makes the seam pay twice. The direction is that MoonLive
gains driver scripts whose hello-world is "read from GPIO, write to GPIO", which needs:

- `gpioRead(pin)` / `gpioWrite(pin, on)` as builtins, mapping straight onto the platform seam.
- `setControl(module, control, value)` as a builtin. **This one needs a decision before it ships**:
  it is the same primitive every transport already uses, so exposing it is consistent, but it also
  lets any script write any control on any module. That is power worth granting deliberately rather
  than as a side effect.
- A `MoonLiveService` host module, the service twin of `MoonLiveEffect`: a `script` control, the
  compile and status path, the picker integration. Mostly a copy of the existing binding, and it is
  what turns "MoonLive can read a pin" into "a user can add a scripted service".

`ButtonService` is then the precompiled sibling of a script anyone could write, exactly the
relationship `ballpit.mle` has with `BallpitEffect`.

## Relay-gated boards, and what a driver is (2026-09-01)

Wiring a QuinLED Dig-2-Go found that its LED supply sits behind a relay on GPIO 12, the vendor's
"LED Relay enable pin". Undriven, the data line is perfectly correct and the strip stays dark, which
is a hard failure to diagnose from the firmware side. `Drivers` now carries a `relayPins` list that
follows the master `on` control.

**A list, because boards do not agree.** The Dig-2-Go has one relay for one LED output; the
Dig-Next-2 has **four relays for two outputs**, all carrying the same `Relay_LightsOn` role in
MoonLight's own model. So a relay maps to neither the device nor a driver, and every one of them
follows master power together. It lives beside `on` rather than on a driver because a relay gates
the SUPPLY, which several drivers share, where a driver's own controls describe that one driver's
output.

**Still open: the definition of a driver.** Two statements in the repo disagree.
[`drivers.md:3`](../../moonmodules/light/drivers.md) says "A driver sends lights somewhere", while
[`architecture.md:143`](../../explanation/architecture/index.md) frames drivers as the consumer half of producers vs
consumers. The product owner's definition is broader than both and is the one to adopt: **a driver
communicates with hardware or the network**, which explicitly includes talking to GPIOs.

The concrete tension is in code: `DriverBase` declares `virtual void setSourceBuffer(Buffer*) = 0`,
so every light driver is structurally required to consume the light buffer, and all sixteen do. The
resolution is that there are two families of one idea, split by whether the light buffer is
involved: a **light Driver** (`Drivers` container, consumes the buffer, outputs it) and a **core
Service** (`Services` container, a capability bridge, no buffer). `services.md` already says exactly
this. **Action: scope `drivers.md`'s claim to light drivers, and name the general sense in
`architecture.md`.** Documentation only, no code.

## The device catalog cannot seed a list row (2026-09-01)

`deviceModels.json` describes a board by the modules it adds and the controls it sets, and the
config push turns that into three ops: `planConfigOps` (`mooninstaller/config-ops.js`) emits `add`,
`set` and `clearChildren`, and `HttpServerModule`'s APPLY_OP handler accepts exactly those. **There
is no op for creating a row in a list control**, on either side.

Found rebuilding the infrared service around a mapping list. Its five old actions (on/off,
brightness up/down, palette next/prev) were meant to ship as default rows so a remote still worked
out of the box, and they cannot: a row is not a control, so `controls: {...}` cannot express one. The
service therefore starts empty and a user adds their first row by hand.

Seeding them in the module's own `setup()` was tried and reverted: it works, but it puts a board's
opinion in firmware, which is exactly what the rebuild removed, and it collides awkwardly with
`restoreList` (which runs first on a configured device, so the guard is "only seed an empty list" and
the interaction is subtle enough to have cost a debugging round).

**What it would take:** an `addListRow` op carrying the parent module, the list control's name, and
the row's fields, then a `setListRowField` per field (or one op with the whole row). The device side
already has both primitives on `ListSource`, so this is plumbing rather than design: the catalog
schema, the planner, the APPLY_OP encoding and the handler. Worth doing when a board genuinely ships
with pre-bound inputs (a panel with three labelled buttons), which is also when someone can say what
the rows should be.

Until then a list is user-populated, which is the honest behavior: the device knows the pin, the
user knows what the button should do.

## A desktop build reports firmware "unknown", so the update UI guesses from the browser (2026-09-06)

Spotted on the container's Firmware card, which showed **Device: macOS arm64** while the image is
Linux amd64. It was not reading the device: `desktopKeyForThisHost()` (`src/ui/install-picker.js`)
and `desktopAssetPrefix()` (`src/ui/app.js`) both sniff `navigator.platform`, which describes the
machine holding the BROWSER. The container makes it visible because there the two always differ, but
the same card lies on any desktop instance opened from another machine.

The download it offers is the failure mode worth naming: on a Mac the picker serves
`MoonLight-macos-arm64-*.dmg`, a real file that installs and runs perfectly, and updates the wrong
computer. Nothing errors, and the device stays on its old build.

**Root cause is one missing build flag.** `MM_FIRMWARE_NAME` already exists for exactly this
(`build_info.h`, surfaced as the `firmware` control): `build_esp32.py` passes the variant key, and
the desktop packaging passes nothing, so every published desktop binary falls through to the
`"unknown"` default. `build_info.h:43` even records the assumption that made this safe, "local
builds aren't published" — the container is a published desktop build, which is what retired it.

**What it would take:** have `package_desktop.py` pass
`-DMM_FIRMWARE_NAME="desktop-<os>-<arch>"` for the target it packages (the keys the picker's
`DESKTOP_LABEL` map already lists), then delete both browser-sniffing functions and read the
`firmware` control instead. `deviceFirmwareInfo()`'s `isDesktop` test (`firmware === "unknown"`)
needs the same treatment, since a named desktop build no longer matches it: test the
`desktop-` prefix. Local unflagged builds keep reporting `unknown`, where a guess is still better
than nothing.

Worth doing with the OTA path rather than alone: a desktop cannot install its own archive anyway, so
the honest end state may be a card that names the platform and links the release rather than
offering a Download button that only ever means "fetch a file and swap it by hand".

## P4 WiFi cascade into a dead co-processor link aborts the board (2026-09-08)

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

## The persisted `firmware` variant survives a flash to a different variant (2026-09-08)

`SystemModule` writes the compile-time `kFirmwareName` into its `firmware` control at
`defineControls()`, and its own comment states the intent: "written from kFirmwareName on every boot
rather than read from the file: the compile-time constant is the truth". But the control is
`addText`, which is PERSISTED, and the config load runs after `defineControls()`, so a saved value
from a previous image overwrites the compile-time one.

Bench (MHC-WLED P4 shield, flashed from `esp32p4rev1-eth-wifi` to `esp32p4rev1-eth`): the Firmware
card correctly reported `esp32p4rev1-eth` (it reads the running image), while `/api/modules/System`
still reported `esp32p4rev1-eth-wifi` from the old config. The two disagreed on the same board, and
the stale one is the field an outside reader trusts.

Why it matters beyond cosmetics: the comment explains this value exists so **MoonBase** can narrow
the recovery image list to one variant. A stale value points a recovering board at the wrong image,
which is the failure that list exists to prevent (picking an `esp32s3-n16r8` build for a Zero
installs a flash layout the board does not have).

The fix has to keep the value readable by another image (that is why it is persisted at all) while
making the compile-time constant win: re-assert `kFirmwareName` after the config load rather than
only at `defineControls()`, and pin it with a test that loads a config naming a DIFFERENT variant
and checks the control still reads the compiled one.

## MoonCloud: share a MoonLive script over MoonTalk (2026-09-10)

Sketched 2026-09-10 while Talk was built. **The unique feature nobody else has**: MoonLight ships a scripting language whose programs are about a kilobyte of text, and a message board between devices. Together they mean a script someone wrote on their wall is one tap from running on yours.

**What makes it plausible.** The scripts are tiny: the shipped `.mle` files run 780 to 1907 bytes and the whole library of twenty-odd is 36 KB. A script is self-contained by design, so the text is the artifact. And the device already compiles and runs arbitrary script text safely, which is the hard half and is done.

**The design work is that a script is five to seven times a message.** Talk caps a message at 280 characters, deliberately: a chat message is a sentence, and one caller must not fill the table. Three options:

- **A second endpoint** (`/api/script`) with its own size cap and table, and a message that references it by id. The chat table stays a chat table, a script is fetched when someone wants it, and a board read stays small. Most work, cleanest shape, and the recommendation.
- **An attachment column on the message**, capped separately. One table and one post, at the cost of every board read carrying script text unless the query is careful.
- **Share a URL.** A script already lives in the user's File Manager and a device could serve it over the LAN. Free, and limited to one network, which is the case that makes the feature interesting.

**A shared script is code from a stranger**, in a way chat text is not. Three questions the design answers rather than discovers:

1. **Loading is explicit and reversible.** A script arrives as something to look at, and taking it leaves what the user already has intact. `/moonlive` (user) shadowing `/.moonlive` (factory) is the existing trap.
2. **The device is the sandbox.** MoonLive reaches no filesystem and no network, so the blast radius of a hostile script is the LEDs and the render budget. Worth confirming rather than assuming: a script that never yields is a watchdog reset, which is a denial of service on someone's wall.
3. **Attribution and removal.** Any sender id can be claimed, and a message stays once posted. Fine for chat, weaker for code.

Depends on Talk shipping and on the server being deployed.

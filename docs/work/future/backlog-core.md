# Backlog: Core

To-build items for the domain-neutral core, grouped by module, each with its fields on one line. Other areas: [the future index](index.md).

## ControlModule

### A slot whose target is gone sends 0 to its followers (2026-10-08)

Kind: Bug

**Found** on the StadBeest after a repower: the legs' walking layer was saved disabled, so the script that `fader2` to `fader6` drive had no controls.
The slots could not follow their targets, stayed at 0, and the OSC feedback sent those zeros to the eyes, whose `bpm` and `sensitivity` then sat at their minimum.
Live slots are not saved, so after a boot only a target or a writer gives them a value.

**The fix to build:** a slot with no target to follow holds no value rather than 0, and the mirror sends nothing for it until the target exists.
The followers then keep what they last had, and a refresh repairs them once the leader's target is back.
Pinned by a test: a disabled target's slot produces no feedback datagram.

## SystemModule

### Memory: the largest buffers, by name

Kind: Feature

The System card shows heap used by type and the largest internal block, and each Tasks row shows a module's `dynamicBytes`, but nothing names the big buffers: the layer buffer, a driver's DMA frame or ring, the Art-Net handoff. A read-only list under System, sorted largest first and fed by each owner's self-report, makes a 48 KB buffer visible as itself. It samples existing accounting once a second and intercepts no allocation; a leak hunt is a separate build-flag `heap_trace` mode, as CPU% is for Tasks.

## DevicesModule

### DevicesModule: interop plugins and the command half

Kind: Feature

Discovery is shipped ([DevicesModule](../../moonmodules/core/system.md)). It stays a code-wired child of Network: it is fleet-scope rather than this-device, so it is neither a System nor a Services module, and it moves to a top-level container of its own only once a second fleet module exists to justify one. What remains is *growth on the seam*, each piece additive (one plugin file, no core change):

- **More discovery plugins**: ESPHome, Tasmota, Hue (*hub-shaped*: a bridge whose Zigbee bulbs are children behind it, with link-button auth). Each is a new `DevicePlugin` declaring its `discoveryPort()` + classifying the datagram (or, for a system that only does mDNS, a re-introduced advertise-side browse scoped to *foreign* services only, never the ones we advertise). Hue is the canonical "more than a flat device" case the seam is shaped for. (Note: Hue *control* already ships as an **output driver**, see [HueDriver](../../moonmodules/light/drivers.md#hue): bulbs as effect pixels; the driver also *lists* its bridge in DevicesModule with the color-light count. Two complementary follow-ups remain: (a) auto-fill the driver's bridge IP from discovery so the user doesn't type it (the mDNS-browse plugin above); (b) **pair once, not per driver**: pairing + the app key currently live on each HueDriver, so two drivers on one bridge pair twice. The clean end-state moves the bridge identity (IP + key + Pair button + light list) into DevicesModule and makes HueDriver a pure output that reads the paired bridge by IP; do this together with the discovery plugin, since both hinge on DevicesModule owning the bridge.)
- **The command half** — `DevicePlugin::command()` (+ per-plugin capability/auth), so MoonLight can *control* a discovered foreign device, not just list it: set WLED brightness via its JSON API, a Hue resource via the bridge's authenticated CLIP API, a Tasmota via `cmnd`. Built when a control consumer exists; the discovery seam is already shaped to accept it (incl. hub plugins). This is the **multi-ecosystem selling point** — one UI controlling WLED + ESPHome + Hue. Commands split by need (the rule, not "all REST"): must-arrive config over REST; latency-critical sync over UDP (~0.5–1 ms vs REST's 10–50 ms — REST would visibly de-sync).
- **Live peer state** — a discovered peer's brightness / on-off shown in our list, refreshed by polling its REST `/json` after discovery gives the IP (discovery = UDP/mDNS, state = REST). The read-side complement to the command half.
- **Non-IP transports (board-gated, far future)** — Tasmota-MQTT / zigbee2mqtt need an MQTT client; **direct Zigbee/Thread** (S31/C6/H2 802.15.4 radio) makes MoonLight the *hub itself*, driving bulbs over the mesh with no gateway — the standout differentiator, the biggest lift. Same plugin philosophy, a transport addition + board gate.

Full design + the reasoned transport split: `Plan-20260629 - UDP device discovery + mDNS advertise-only` (in the plans archive).

## EthernetModule

### An explicit `Custom` Ethernet preset is reseeded to a named one

Kind: Bug

`ethPresetIsUnset()` returns true for any editable row, and `Custom` is the only editable row, so it cannot tell "nothing has chosen a preset" from "the user chose Custom". A board saved as `Custom` whose pins happen to match a named preset is therefore reseeded to that preset on the next rebuild, and its pin rows are hidden: a hand-wired board silently adopts a board map it was never given.

The seam is what makes this awkward. `defineControls` runs before the config is restored, so the seed cannot simply latch a flag on the first render: that is the legacy path the seed exists for, a board whose file predates `ethBoard` and carries only pins. Distinguishing the two needs to know whether the restore supplied an `ethBoard` value at all, which is not visible where the seed runs. An attempt that latched on the first render broke the legacy path and was reverted.

**What it costs today**: nothing on a board the catalog configures, since those name their preset. It bites a user who deliberately picks `Custom` on a board whose pins match a preset, which is the hand-wired case `Custom` exists for.

**Where to look**: `FilesystemModule::applyNode` overlays values, calls `rebuildControls`, then overlays again. A control that wants to know whether IT was restored needs a signal from that pass, which no control has today. That is the general shape worth solving rather than a special case for one select.

### Live RMII Ethernet reconfigure

Kind: Feature · Platform: all

RMII (classic/P4 internal EMAC) still saves config and asks for a restart to apply, because the EMAC bring-up is fiddlier to hot-cycle cleanly. Make RMII live too: a hot `esp_eth_stop` + EMAC/netif teardown + re-init on config change, matching the W5500 path, so every interface honors the no-reboot principle.

## FilesystemModule

### A config written for next boot is overwritten by the next save (2026-10-04)

Kind: Bug

Network and Ethernet return false from `appliesConfigLive`, so a file written through `POST /api/file` waits on disk for the next boot.
Any save before that boot writes the module's in-memory values over it, so the upload is lost without a word.
Seen on the S3: a restored `NetworkModule.json` came back with the old `txPowerSetting` until the restore was followed straight away by a reboot.
The fix: `applyConfigFile` marks a module it does not apply live as pending on disk, and the flush skips that module's file until the next boot.
The uploaded file then wins with no UI change; the alternative, answering "applies at next boot" and offering a restart, still loses the file when the restart is declined.
Pin it with a test that writes the file, dirties the module, flushes, and finds the file unchanged.

The root fix is a network bring-up that reconciles instead of re-creating. `NetworkModule::setup()` (netif create, driver install, the eth→WiFi→AP cascade) runs once at boot and crashes if re-run live, which is why Network opts out of the live apply; once it reconciles, a restored `NetworkModule.json` applies live and the pending mark is no longer needed for it.

### Remove the flat config conversion (2026-10-08)

Kind: Restructure · Trigger: in the release after next

A config file in the flat format builds before 2026-10-08 saved is read as the state document it describes at the first boot and saved as one, so an in-place update keeps its config.
In the release after next, remove the conversion and everything that only serves it:
- `flatToStateDocument`, `isFlatConfig` and their helpers in `StateDocument.cpp` (`writeFlatNode`, `writeFlatValues`, `takeLive`, `flatName`, `distinctName`, `flatRoot`);
- the conversion in `FilesystemModule::loadSubtree`;
- the flat fallback in MoonBase's `ConfigScrape.h` (`flatChildKey`, `flatChildValue`);
- `configToDocument` and `isFlatConfig` in `src/ui/migrate.js`, after which an older backup no longer restores;
- the test "A config file in the flat format older builds wrote loads at boot and is saved back as a document", and the MIGRATING entry's lines on the conversion.

`NetworkModule::adoptLegacySettings` calls `isFlatConfig`, so the 6.0 network adoption above goes first or in the same change.

### Config backup and restore: identity, a recipe, and tiers 2 and 3

Kind: Feature

Tier 1, a browser-side bundle over the file API with the rename map and restore report, ships in the File Manager.

- **Restore clones a device's IDENTITY along with its config** (found answering
  [#76](https://github.com/MoonModules/MoonLight/issues/76), 2026-09-04). Backup bundles every
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
- **Tier 2**, a single-archive device endpoint (one request instead of a walk); **tier 3**, restore hosted on
  MoonBase's page, the migration answer for future partition-table moves.

## HttpServerModule

### The HTTP server is audited for the two PHP request-handling bug classes (2026-10-21)

Kind: Investigation

PHP's critical advisory of 2026-10-21 (CVE-2026-109618, CVE-2026-109619) names two bug classes any C++ server can have. **Out of memory mid-request**: an allocation fails while a request is half built, and cleanup or a later step treats the partial state as valid. That path is live here: a classic ESP32 fell to 916 bytes free during a page load. **State outliving its request**: a reused connection slot (`wsClients_`, `streams_`) keeps a pointer, length or buffer from its previous client. The audit is a read-only pass over `HttpServerModule` and the platform TCP layer, every allocation-failure path and every slot reuse, and each finding is pinned by a desktop test that forces the failure.

### A request body shares the two upload slots (2026-10-25, from the pre-merge review)

Kind: Bug

A request whose body trails its headers waits in a `FileTransfer` slot (`deferRequest`), of which there are two. With two uploads in flight, a `POST /api/control` whose body comes in a second segment answers 503 "every send slot is busy", and a slow-bodied control write holds an upload slot for the 10 s stall window. A slot of its own for `Kind::Request`, or a count kept apart, separates them; it waits for a user who meets it, since two uploads at once is rare.

## I2cBusModule

### A second I2C bus, once a board wires one (2026-10-06)

Kind: Feature · Trigger: a board that wires a second bus

`I2cBusModule` drives one bus on controller 0, which covers every catalog board: one bus carries about 112 addresses, so a codec, a sensor and a display share it.
A board needs a second bus for two chips on the same fixed address, devices at different voltages or pull-ups, or a long cable run kept off the onboard devices.
The shape then: the module becomes addable with a controller-number control, the platform keeps one bus per controller, and a device on a bus (`AudioService`'s codec first) gains a bus selector defaulting to 0, so the catalog's boards keep working unchanged.
The trigger is a real board; a touch controller or camera on its own pins next to the codec is the likely first, the Guition M3's touch wiring being unchecked.

## ImprovProvisioningModule

### Improv-as-REST follow-ups

Kind: Feature

What remains of "Improv = REST over serial" ([ImprovProvisioningModule](../../moonmodules/core/system.md)):

**Open follow-up: shared JS helpers across device-UI and mooninstaller.** `safeLocalGet` / `safeLocalSet` (3-line hostile-storage guards) are duplicated in `src/ui/install-picker.js` (device firmware, embedded as a C string via `embed_ui.cmake`) and `mooninstaller/devices.js` (web installer page, served from Pages). The two live in different build contexts so the shared extract isn't trivial — it'd need a new `src/ui/safe-storage.js` plus updates to: `embed_ui.cmake` (embed the new file), `ui_embedded.h` generator (new C array), HTTP server file routing (new path served), `release.yml` workflow staging, `preview_installer.py` staging. Five files for one 3-line helper is too much pre-merge. Worth doing when the next shared helper arrives — `relativeTime` and `formatBytes` are candidates. Two helpers earn the build-glue cost; one doesn't.

**Open follow-up: P4 Improv scan on a cold WiFi link (bench check).** `improvHandleScan` in `src/platform/esp32/platform_esp32_improv.cpp` calls `esp_wifi_scan_start`, which needs the WiFi driver started. On native ESP32/S3 the driver is up by the time a user provisions; on the P4 the radio lives on the C6 and comes up only after the esp_hosted prelude in `ensureWifiInit()` (triggered by `wifiApInit` / `wifiStaInit`). A scan requested on a P4 that has not initialised WiFi returns an error cleanly rather than scanning a cold link, so nothing crashes. The check: bench-verify whether a P4 provisioned from cold needs the link brought up first, and if so route the scan through the public `wifiAp`/`wifiSta` path.

## ModuleFactory

### ModuleFactory: registration can fail silently (uint8_t ceiling, unchecked bool)

Kind: Bug

**Found:** 2026-07-27, while adding the effects grid sweep; flagged by the 👾 Reviewer.

`ModuleFactory::count_` is a `uint8_t`, and at the 255 ceiling `grow()` returns false, registration fails, and the return value is discarded at every call site, including the static-init registrars, which cannot check it anyway. The failure is silent: the type is simply absent from `/api/types` and `create()` answers `nullptr` for it, far from the cause.

Production is not near the ceiling today (~90 types), so this is not urgent.

**Options, cheapest first:** (a) widen `count_`/`capacity_` to `uint16_t` (a handful of bytes, removes the ceiling as a practical concern); (b) make a failed registration loud in a debug build (assert or a boot-time log) so it can never be silent again. Both are probably the right pair.

## MqttModule

### Generic control and state topics over MQTT: the automation escape hatch

Kind: Feature · Stage: Integrate

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

**Open question — Homebridge example maps both `setBrightness` and `setHSV`.** In `homebridge-mqttthing`'s `lightbulb`, HSV's *value* (V) already carries HomeKit's Brightness characteristic, so mapping both topic pairs may double-drive brightness (mqttthing's docs lean toward using one or the other). The [MQTT § Homebridge example](../../moonmodules/core/system.md#mqtt) currently lists both, to show the device's full topic surface. Resolve on hardware: flash a board, run Homebridge + mqttthing with that config, and check whether the Home-app brightness slider misbehaves — if it does, drop the two `brightness` topics from the example; if not, it's a non-issue. (Flagged by CodeRabbit; parked here rather than changing the doc on an untested hunch.)

### HA update entity via MQTT discovery: release check

Kind: Feature · Stage: Integrate

**Still to build: device-side release check** (the update entity itself is shipped, [MQTT](../../moonmodules/core/system.md#mqtt)). `installed_version` and `latest_version` are equal today (both `MM_VERSION`), so HA renders the entity as up-to-date and the Install button is disabled. What's missing is a periodic poll of `https://api.github.com/repos/MoonModules/MoonLight/releases/latest`: at boot (~30 s post-network-up) and every 24 h thereafter, fetch the release JSON, extract `tag_name`, and if newer than `MM_VERSION` call `MqttModule::publishUpdateState()` with the updated `latest_version` (the publish path is already there: only the caller is missing). ~2 KB per check.

Add a `ReleaseCheckModule` (or a small extension inside NetworkModule / FirmwareUpdateModule — decide when building) that owns the poll timer + last-seen version cache and calls into `MqttModule::publishUpdateState()`. ~50 additional lines.

## NetworkModule

### The access point never blocks a WiFi retry (2026-10-09)

Kind: Bug

After losing its network, a device opens its access point and retries its known networks every minute (`retryKnown` in `NetworkModule.h`), but skips the retry while a client is on the access point.
A phone that stays on it, or rejoins it by itself, then keeps the device off its network indefinitely.
WiFi has preference, so the retry runs whoever is on the access point: drop the `wifiApClientCount() > 0` check.
- **The cost:** someone on the access point while a known network exists is interrupted for a few seconds once a minute; a first setup has no known network, so it never retries.
- **A test:** a client on the access point, the known network back, and the device joins it within the retry interval.

**Some devices do not rejoin their WiFi** (2026-10-09): now and then a device stays off its network after the router came back. This block is one suspect; the other is the path for losing the network while connected, where the WiFi driver can get stuck and need the radio restarted.
Next time one hangs, capture before touching it:
- **The Network card's status:** "No network", "AP: MM-…" or "WiFi STA: …" says which state it is stuck in.
- **The access point's `clients`**, which says whether a phone is holding it.
- **The serial log**, through `monitor_esp32.py`, whose `NetworkModule:` lines say what it tried last.

### Refresh a multicast membership on a fixed pace (2026-10-09)

Kind: Bug

A socket in a multicast group joins again after 3 seconds of silence, which repairs a router that forgets a member, with a gap of about 2 seconds each time.
The GL.iNet Slate AX forgets after about 18 seconds, so audio on the StadBeest drops every 20.6 seconds, measured on the bulb.
The standard fix is the router's multicast querier; this item is for a network that cannot be configured, such as a venue's.
- **The rule:** join again 10 seconds after the last join as well, so a membership is renewed before such a router forgets it.
- **The cost:** a leave and a join, two IGMP packets of about 60 bytes per socket every 10 seconds, and at most one lost packet in the moment between them.
- **Where:** `UdpSocket::keepMembership` in `platform.h`, one more condition, and a test beside the silence re-join's.

### Remove the 6.0 network adoption (2026-10-04)

Kind: Restructure · Trigger: once a release has carried it

`NetworkModule::adoptLegacySettings` and the two `adoptLegacy` it calls move 6.0's top-level network keys onto the Ethernet and WiFi cards at the first boot, so an in-place update keeps its network.
Remove all three, their test `unit_NetworkModule_legacy.cpp`, and the "update to this release before a later one" line in MIGRATING's WiFi and Ethernet entries, once a release has carried it and the field has moved past 6.0.

### Some devices stop answering mDNS over WiFi (2026-10-08)

Kind: Bug

**Found** on the StadBeest: after the legs restarted, their MIDI service could not resolve the bridge's `MM-MIDI-rtp.local` for five minutes, and by address it connected at once.
Browsing from a Mac on the same network listed `_http._tcp` for MM-Eye2 alone: the legs, the left eye and the bridge announced nothing, and no `_apple-midi._udp` record appeared.
The left eye announces nothing new, so the cause is wider than the RTP-MIDI announcement.
The WLED native app finds devices by browsing `_wled._tcp`, so it is the likely reason it does not always list every device.

**The fix to build:** find why a device stops answering, starting from what differs between MM-Eye2 and the others: WiFi power save dropping multicast between beacons, transmit power (the bridge runs at 8), the responder losing its interface after a reconnect, or the announcement order in `mdnsInit`.
Watch it with a working `dns-sd -B` from a computer and the device's serial log, and confirm the bridge's `_apple-midi._udp` record appears.
Pinned on the bench: every device answering its `.local` name and listed in the WLED app after a restart and after a WiFi reconnect.

### Advanced network settings (2026-10-03)

Kind: Feature

Left out of the plan that makes Ethernet, WiFi and the access point Network submodules, and expected to be asked for:

- **Roaming** between access points of one network while connected, by signal threshold (802.11k/v/r where the chip supports it).
- **Enterprise WiFi**: WPA2/WPA3-Enterprise with a username, a password or a certificate.
- **Pinning a network to one access point** (BSSID) or one band.
- **WiFi power save**: the modem-sleep mode, traded against latency for Art-Net and the UI.
- **The country code**, set by hand where adopting the router's country is not enough.
- **The access point's address**, fixed at 4.3.2.1.
- **IPv6 beyond link-local**: global addresses and a static one; the station and Ethernet carry a link-local address, which mDNS advertises.

### NetworkModule treats a panel-output NIC as the device's uplink (2026-09-28)

Kind: Bug · Platform: desktop

A NIC claimed for raw L2 panel output carries no LAN, but NetworkModule enumerates it anyway and reports `Connected via Ethernet` on it. When the panel link renegotiates, every flap runs the full cascade, which drops the browser's WebSocket and makes the UI look dead while the server is healthy. Seven flaps were logged in one bench session. The driver already announces itself through `ethClaimRawL2(true)`, so the claim is there to honor; the change is for NetworkModule to skip a claimed adapter. Crosses two modules, so it wants a decision rather than a patch.

### The persisted interface label carries the negotiated link speed (2026-09-28)

Kind: Bug · Platform: desktop

PanelCardDriver saves its adapter as `Realtek USB GbE Family Controller, 1 Gb`, speed included. A renegotiation to 100 Mbit rewrites that string, which dirties the config and rewrites `Drivers.json`. Matching already strips the suffix and compares the stable head, so the binding survives, but an identifier that changes when a cable renegotiates is the wrong thing to persist. Store the adapter's stable identity and render the speed for display only.

## PinsModule

### PinsModule: strict reject-on-add mode

Kind: Feature

[PinsModule](../../moonmodules/core/system.md) is shipped; [PinsModule.h](../../../src/core/system/PinsModule.h) records the design. The soft-flag choice obviated the reassignment broker (a live pin swap already works: set A→B's pin, then B→A's, the transient conflict clears).

The one still-open item is an **optional strict reject-on-add mode** for the installer/catalog path: a "clean tree" that *refuses* a module add whose pins collide, rather than accepting it soft-flagged. This is add-path *validation* (a different feature than the live soft-flag reassignment), wanted only if a stricter installer UX is desired. Spec + `/plan` if picked; the soft-flag default stays for live editing.


Why soft-flag is the default, and what strict mode must not break: a refusal on add wedges a legitimate live swap, since two drivers exchanging pins pass through a transient double claim, while a claim that lands and flags red never wedges, which is the degrade-visibly rule. Strict mode is for the installer and catalog path only, where a tree is built from scratch and no swap is in flight. It wants an explicit shared-pin opt-out for the one legitimate case, two consumers reading one input, as ESPHome's `allow_other_uses` does.
### A driven GPIO the Pins map never sees: bus padding, and a hidden clockPin

Kind: Bug

**Found:** 2026-08-21, on MM-S31, after a bench session that started as "the LED panel stopped working" and cost hours chasing a firmware regression that did not exist.

An ESP32-S31 driving a ColorLight receiver card over raw Ethernet showed the panel dark while every diagnostic said the transmit path was healthy: link negotiated at 1000 Mbit, ~4600 packets/s, zero drops, and a byte-for-byte dump of a 128x128 frame matched `ColorLight5A75Packet.h` exactly. The card's own activity LED never blinked. Four firmware versions across two ESP-IDF releases behaved identically.

The cause is a **GPIO collision that no part of the system could report**. `ParallelLed` carried `clockPin = 10`, and GPIO 10 is `txd2` on the S31's RGMII bus (`platform_esp32.cpp`: the EMAC's fixed IO_MUX pads are 8-19 plus MDC/MDIO on 5/6). The LED driver drove one of the four Ethernet transmit data lines, so every frame left the MAC counted-as-sent and arrived corrupt. Disabling the LED drivers fixed it instantly; moving the clock to GPIO 21 fixed it with all four drivers running.

One defect remains, the other two being fixed ([lessons](../past/lessons.md#esp32-s31-rgmii-ethernet-bring-up-four-bugs-between-code-compiles-and-link-up)):

**`clockPin` defaults to 10 and is hidden.** `MoonLedDriver::clockPin = 10` is a hardcoded default that lands inside the S31's reserved RGMII block, and `addBusControls` hides the control unless `pinExpanderMode()` is on. So on this board the value was invisible on the card, unchangeable through the UI, and still driving a pad. A pin with a real effect must be visible, whatever mode it is in. The first sighting was on an S3 (2026-07-12): an `i80` lane list `18,5,6,7,8,9,10,11` overlapped the hidden `clockPin` 10 and `dcPin` 11 defaults, so two lanes carried the bus clock and DC toggling instead of pixel data, and nothing flagged it.

### A bus pin that belongs to another peripheral is accepted

Kind: Bug

- **A bus pin that belongs to another peripheral is accepted, and takes the board off the
  network** (2026-09-06). An Olimex ESP32-Gateway given a WR/DC pin on its Ethernet MDIO/MDC
  (18/23) or its PHY reset line (5) lost its Ethernet link within seconds and looked crashed
  (the firmware kept ticking on serial; it stayed reachable only through its WiFi fallback).
  The pin registry already
  detects a double claim (`PinsModule::flagConflicts`) but only colors the edge, so a control
  write that lands on a GPIO another control owns should be refused with a status naming the
  owner, the way a reserved flash pin already is. A collision with a network pin is worse than
  most: it ends the session that could have fixed it, and the SMI pins stay re-muxed until a
  reboot even after the driver releases them.

## Scheduler

### Effects and Drivers find their source one way, not two (2026-10-08)

Kind: Restructure

Effects finds the top-level Layouts, and Drivers the top-level Effects, by type through `Scheduler::topOfType` when nothing was injected, which is what lets a state document build a tree with no wiring.
`setLayouts` and `setEffects` remain beside it for the unit-test rigs, 171 calls in 74 files, so two mechanisms do one job.
The fix: a rig adds its containers to a Scheduler, as the scenario runner does through `createTopLevel`, and the setters go.

### A script's setControl rebuilds a control subtree on every write (2026-09-06)

Kind: Bug

Measured on the P4 at .139: **2 fps**, with `MoonLive-2` at 251 ms and `MoonLive-3` at 245 ms per
tick, together 498 ms of a 506 ms frame, and HTTP down to a 0.5 s round trip because it is served
from the same loop. Both services ran `sweep.mls`, whose `tick20ms` writes four faders. Two copies
at 50 Hz is 400 control writes a second, and `Scheduler::setControl` calls `rebuildControls()`
unconditionally on each one, so each fader write tears down and re-creates the whole `Control` subtree. A person moving a slider
does this a few times a second and nobody notices; a script at 50 Hz multiplies it by hundreds.

Two things to fix, and they are independent. **The rebuild should be conditional**: a `live`
control's value change cannot alter the schema, and `rebuildControls` already computes
`schemaSignature()` before and after to decide whether to notify, so it knows. Rebuilding only when
the shape can actually change (what `setLive` and `affectsPrepare` already distinguish) removes the
cost for every value write, scripted or human. **And the script tick is too fast for its job**: a
fader sweep does not need 50 Hz, so `sweep.mls` should run on a slower tick, which also caps the
damage any future script can do through this path.

Not a regression from the RMT work: it predates it and was found while measuring an unrelated slow
board.

## Across modules

### A Progress total rides the value patch (2026-10-19)

Kind: Feature

A Progress control's total travels only in `writeControlMetadata` (Control.cpp), so a changed total needs a schema resync, which `rebuildControls()` triggers because `aux` is in the schema hash.
One line in `writeControlValue` and one in `app.js`'s patch handler would let the total ride the value, and the hash could then skip it, so a track change in FseqPlayerEffect stops resyncing every browser.
Pin it with a test that changes the total, patches, and finds the browser-side max moved with no resync.

### What disable means: release the memory, or keep the module hot (2026-10-20)

Kind: Investigation

A disabled module keeps its settings only ([coding-standards](../../contributing/coding-standards.md)), and deleting a module already releases everything, so the question is whether disable should release too or keep the module hot for an instant resume. The contract test (`unit_ModuleContract.cpp`) lists the modules that keep runtime memory while disabled, Layer and Drivers with their pixel buffers the largest, and fixing them would settle the answer, so they wait for it. The analysis weighs: presets and DemoReel switching many modules, where kept memory grows with what is added rather than what runs; a quick toggle, where hot means an instant resume; the measured re-enable cost of every module, its `prepare()` time, gathered with the contract test's placement; three shapes, release on disable, keep everything hot until delete, or keep the settings and what is slow to rebuild while releasing the large buffers; and how comparable systems treat a disabled or paused element. The product owner decides, and the rule in coding-standards, the contract's lists and the Layer and Drivers fix follow.

### Playlist in core: one sequencer for FSEQ, effects and DemoReel (2026-10-19)

Kind: Restructure

A playlist is a list of items with an optional duration each, played in order or shuffled once per pass, repeated off, one or all, under play, pause, stop, previous and next: the media player's model. Two copies of that sequencing exist: `DemoReelEffect::advance()` cycles effects, and `FseqPlayerEffect` cycles files with its own cursor, shuffle pass and loop mode, written as the same shape. An effects playlist would be the third, so it starts by putting the model in core once, a `Playlist` over core's editable `ListSource` whose owner supplies how to play item *i* and when it ends: the FSEQ player's items are files that end with the track, an effects playlist's are presets applied through the state-document engine that end after their duration, as the presets plan foresees, and DemoReel becomes an effects playlist of effects. Preset looping, rotating between a first and last [preset](../../reference/presets.md) slot on a timer, is an effects playlist whose items are presets. The change that builds the effects playlist moves FSEQ's and DemoReel's sequencing into it.

### A device without a deviceModel is not told its catalog half is missing (bench, 2026-08-26)

Kind: Bug

A board ran for days with `deviceModel` empty and `ethType` 0: Improv and access-point provisioning restore the WiFi credentials only, and nothing tells the user the catalog half (Ethernet wiring, per-board settings) is missing. Candidates: MoonDeck re-pushes the catalog on discovering a device whose deviceModel is empty, or the UI badges the state.

### One board-preset helper for Ethernet and the SD card (2026-10-12)

Kind: Restructure

EthernetModule and SdCardService each carry the same board-preset machinery: the per-chip filtered option list, selection by label, the applied tracker, applying a preset when the selection moves, and reading the preset back off the pins.
A `core/module/BoardPresets.h` helper would own the options, index, count, selection and applied state, with the module supplying its table and two functions: write a preset into the pins, and match the pins against one.
Do it before a third preset table appears (a display's or a microphone's wiring), so that one becomes a table rather than a third copy.

### `ListSource` methods compile twice: ~8.6 KB of thunks (2026-10-01)

Kind: Restructure

Every module that is `MoonModule` and `ListSource` gets its list methods emitted twice on ESP32: once as the method, once as a "non-virtual thunk" for the second base, and GCC copies the whole body into the thunk instead of a jump.
Measured on the S3 image with `xtensa-esp32s3-elf-nm -S`: 8.6 KB in all, the largest `GamepadService` 1.6 KB, `ButtonService` 1.3 KB, `FixtureProfilesModule` 1.2 KB, `ControlModule` 1.1 KB, `InfraredService` 0.9 KB, `AnalogService` 0.9 KB.
Candidates, to measure rather than assume: a `ListSource` that is a member the module hands out (no second base, so no thunk), or keeping the bodies out of line so the thunk can only jump.
Done when the thunk total in the S3 symbol table is near zero, with the delta in repo-health.

### An early state push costs 3 to 5 ms on the S3 (2026-10-01)

Kind: Investigation · Platform: S3

`notifyValuesChanged()` patches one module on the next 20 ms tick, at most 25 times a second; with a MIDI desk following a self-playing Pong it took the S3 from about 325 to 295 fps, 3 to 5 ms per push.
Building that patch walks one module, so the time is most likely the WebSocket write on the render thread, but the split is unmeasured: one write per frame instead of two saved nothing.
Time the build and the send separately on the S3, then decide between moving the write off the render thread and a lower cap.

### `applyControlValue` should say whether the value changed (2026-10-03)

Kind: Restructure

The boot reapply in `FilesystemModule::reapplyNode` detects a changed value by serializing each control before and after the apply and comparing the bytes, a list control included.
`applyControlValue` reports only whether the value parsed and applied. The proposed shape is the apply also reporting whether storage moved, an `ApplyResult::Unchanged` or a flag, so `Scheduler::setControl` can skip the reaction to a write that changes nothing.
Cold path today, so a tidy-up rather than a cost.

### Runtime board presets (multi-commit, partially landed)

Kind: Feature

The firmware-vs-board separation, the runtime pin controls and the catalog that carries per-board defaults are shipped ([MoonInstaller](../../explanation/architecture/mooninstaller.md#the-three-words)). What remains:

**Prior art: the predecessor's per-board pin database** ([ModuleIO.h](https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Modules/ModuleIO.h)). The predecessor MoonLight (our own earlier project) already models exactly this for ~25 boards across ESP32-D0 / S3 / P4: a `pins[]` array of `{GPIO, usage, index}` plus board-level `maxPower`, `ethernetType`, `ethPhyAddr`, `ethClkMode`. Don't copy the file or paste its tables here: read it when building the catalog and write our own. Its `usage` enum enumerates the hardware functionalities a board preset here *could* drive; those still without a device-side consumer:

- **LED output pins**, a gap with two halves now the parallel drivers take 16 lanes:
  - **Per-model usable-GPIO map (the data).** Identify **which up to 16 GPIOs each device model actually exposes for LED output**: not the chip's full pin count, but the pins broken out to a usable header/connector AND safe to drive (exclude strapping, flash/PSRAM, input-only, and pins already owned by eth-RMII / I²C / the onboard LED). The codebase knows the chip *ceiling* (`MM_MAX_GPIO` from `CONFIG_SOC_GPIO_PIN_COUNT`, [Control.h:13](../../src/core/module/Control.h)) and the live *ownership/reserved* grading ([PinsModule](../../moonmodules/core/system.md)), but NOT the per-board *exposed-and-safe* set, which is board knowledge (schematic/pinout per model). The authoritative source is the **annotated pinout image per model under `docs/assets/deviceModels/`** (e.g. [`esp32-s3-n16r8-dev.png`](../../assets/deviceModels/esp32-s3-n16r8-dev.png)) (and MoonLight's `ModuleIO.h` `pins[]` as prior art: read it, write our own against `deviceModels.json`). The per-MCU reading is in [gpio-usage § Usable LED-output GPIOs](../../reference/hardware/gpio-usage.md#usable-led-output-gpios-per-mcu-quick-reference); do the same read per model.
  - **Encode as per-model defaults + coverage (the catalog).** The 16 usable lane GPIOs become the model's default `pins` string, so a fresh flash of e.g. "Serg UniShield V5" comes up with the *right* lane pins pre-filled, not blank/generic. Coverage is uneven: of 25 models, **7 carry no LED-pin config** (Olimex Gateway, LOLIN D32, Generic ESP32 Dev, ESP32-S3 N16R8 Dev, LightCrafter 16, SE 16 V1, and any bare dev board), and those default to nothing and force the user to guess. Fill every model's LED-capable pin default, and **document the per-model map** (the annotated-pin images are the natural home). **The per-board usable set is wider than the exposed header:** a pin the board commits to a peripheral it *doesn't mount* is fair game (the Olimex Gateway leaves 6 clean LED pins only if you count the **unmounted micro-SD** pins 4/13/14), so the per-model map must record *which non-exposed/repurposable pins are safe on this board*, not just the header breakout. **Note the i80 gotcha:** an i80 model should pick `clockPin`/`dcPin` clear of its data lanes; an overlap is a *warning*, not a blocker (the driver still runs; that one lane just carries the clock/DC waveform, which is fine for an unused parked lane but garbles an active strand), so a catalog default that overlaps an *active* lane would ship a subtly-broken board.
- **Power budget**: `maxPower` (Watts) per board, for a future current-limit / brightness-cap control.
- **RS485 / DMX**: TX/RX/DE pins (wired DMX-512 output beyond the current Art-Net path). The driver that consumes this is the [RS-485 / DMX-512 wired output item](backlog-drivers.md#rs-485-and-dmx-512-wired-output-the-physical-dmx-driver) in the light backlog.

Sequencing rule (unchanged): each functionality lands a device-side control first, then its preset field; the catalog grows one earned consumer at a time, never as a speculative pin dump.

### Hot path: move blocking work off the render callbacks (architecture)

Kind: Restructure

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

### Clock sync: a shared monotonic clock across devices

Kind: Feature

The second half of the core [multi-device runtime](../../explanation/architecture/mooncore.md#multi-device-runtime); discovery ships, this does not. The design: one leader sends its elapsed time over OSC, the protocol that already carries the shared controls, with the same `addressing`; followers compute their offset, targeting sub-millisecond accuracy. A shared monotonic clock is the foundation any cross-device coordination builds on, which is why it is core rather than light-domain.

The light-domain payoff is a wall of controllers animating in lockstep: effects already animate off elapsed time, so feeding them the leader's synced clock instead of each device's local one is the whole change on the render side. Device-to-device light *distribution* is a separate topology question and rides the existing ArtNet / E1.31 / DDP standards rather than a bespoke protocol.


Three render-side consequences. Time-seeded randomness takes a frame index derived from the synced clock rather than raw milliseconds, so devices a millisecond apart hash alike. `hashInt` is the default for anything a synced effect randomizes, and the `Random8` stream stays for local-only effects. Particles, ripple, fire and the cellular automata expose a deterministic re-seed from (time, seed), so a late device is placed into the same state by replay or keyframe.
### Config provenance: firmware/MCU → deviceModel (catalog follow-ups)

Kind: Feature

The model is shipped ([MoonInstaller § Config provenance](../../explanation/architecture/mooninstaller.md#config-provenance-mcu-devicemodel)). The remaining forward-looking pieces, a `devices.json`/MCU-layer split and annotated-pin images, stay gated by the sequencing rule (no catalog field ahead of a consumer).

### Persistence overlay: partial-save and schema-change audit

Kind: Investigation

The absent-key fix (`json::hasKey` guard in `applyControlValue`, so a saved file omitting a key no longer zeroes the control's default — the P4 `ethType` no-DHCP root cause) closed the acute hole. A broader audit would harden the overlay against the rest of the schema-drift surface, now that controls carry meaningful non-zero defaults:
- **Type-change safety (not migration, see MIGRATING.md).** `ethType` changed `int16` → `uint8` (Select). A persisted file written under the old type still loads (the flat parser reads the number either way, and Clamp snaps an out-of-range value to the new bounds). The task is to *confirm* each type transition degrades gracefully through the robust loader (absent → default, stale → clamp, unknown → ignore) — especially Text/Password/IPv4 buffer-size changes — NOT to add migration code. A transition that would lose data silently is documented as a break in the documented break, per the settled policy.
- **Test coverage.** `unit_Control_apply_absent_key` pins the absent-key contract; extend with a type-change round-trip (save int16, load into uint8 Select) and a narrowed-range clamp case so a future schema change can't regress silently.
This is hardening, not a known bug — the shipped fix is correct for the cases that occur today.

### A second instance says it is serving, serves nothing, and shares the first one's config (2026-09-29)

Kind: Bug · Platform: desktop

Launching the desktop binary twice leaves a silent zombie. The second process prints `MoonLight is running: http://localhost:8080/` and `HTTP server -> http://<ip>:8080`, then listens on no TCP or UDP port at all, and carries on rendering at about 14% of a core. Measured on Windows with a 128x128 layout: the first instance kept the port and kept serving throughout, so the deliberate omission of `SO_REUSEADDR` does its job and the port is never stolen. `HttpServerModule` prints `HTTP server failed to open port`, but the banner in `main_desktop.cpp` prints regardless.

The sharper half is the filesystem. The second instance mounts the SAME data root and writes to it: `saved /.config/NetworkModule.json` landed while the first instance was live and serving. Two processes owning one config directory is a corruption waiting for the right interleaving, and neither of them knows the other exists.

A bind that fails is a startup failure and wants to read as one: exit, or say plainly that another instance holds the port, with the banner following what actually bound rather than what was intended. Whether a second instance should be refused outright is the open design question, since the data root is per user rather than per process and two of them cannot both own it.

### A non-ASCII Windows profile path defeats the desktop settings directory (2026-08-23)

Kind: Bug · Platform: desktop

`std::getenv("LOCALAPPDATA")` returns the ANSI form of the path, so a Windows user whose profile name carries characters outside the system codepage (CJK and Cyrillic on a Western machine; most accented Latin survives cp1252) gets `?` where those characters were. `?` is not legal in a Windows filename, so `create_directories` fails, `fsMount` returns false, and the driver reports `cannot use ..., persistence disabled` naming the mangled path. It degrades visibly rather than corrupting anything, which is the standard Principle 5 asks for, but that user has no working persistence.

Reading the variable wide is only half of it. `toFsPath` composes a `std::filesystem::path`, which stores wide on Windows, but every open in the layer goes back through `.string()` to reach `std::fopen` (fsRead, fsReadAt, fsWriteAtomic, fsWriteStream, and the mount probe), a narrowing the code comments on deliberately at `fsRead`. So a wide `LOCALAPPDATA` alone would produce a correct path that still cannot be opened: the fix is one `_wfopen`-on-Windows helper shared by all five sites, plus a test with a non-ASCII root.

Not done with the per-user data directory because it reverses a documented decision across the whole desktop filesystem layer, four of whose five call sites predate that change. What the change did do is make the root capable of containing a username, where it was previously the literal `build`. Worth closing the next time this file is opened for other reasons.

### Drivers' palette and fixture-profile lists, sent once rather than in every view (2026-10-13)

Kind: Restructure

Drivers is the largest root in the state, 35 KB of the Teensy's view where Services is 10 KB, and two lists are 25 KB of it, mostly repetition.
The `palette` options carry 66 palettes with 16 hex colors each for the dropdown's swatches, which change only with the firmware or a palette script: the state carries the names, and the page fetches the colors from a route the browser caches across reloads.
FixtureProfiles' `profiles` summary is 1.9 KB, and its `detail`, 14.5 KB, repeats each expanded row's editor fields, their name, type and bounds, for every profile: the row schema is sent once, as `optionSets` already sends the role names, and each row carries its values.
Together Drivers falls to about 12 KB, and the largest single control, the block a streamed state still needs whole, from 15 KB to a few.


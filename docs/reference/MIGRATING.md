# Migrating

The log of **breaking changes**, what changed between versions, and the action to take.

MoonLight ships **no migration code**: the persistence layer is robust by default (an absent key keeps the control's default, a stale value clamps to the new bounds, an unknown key is ignored), which absorbs almost all schema drift with zero migration-specific code. The rare change that a robust reader *cannot* absorb is **documented here instead of migrated**. A patching framework is deferred rather than rejected: it becomes the right tool if breaking format changes get frequent enough that ad-hoc losses pile up (a rough bar: more than five across a few releases) and users hold persisted state too valuable to re-derive. At that point build the recognizable version-stamp plus ordered-patch-chain pattern, not a bespoke one and its rationale.

**The File Manager's Backup (⤓) / Restore (⟲) carries config across these breaks.** [src/ui/migrate.js](https://github.com/MoonModules/MoonLight/blob/main/src/ui/migrate.js) is the **authoritative, dated log of every machine-mappable break** (file, type, control, and value renames): Restore applies it in the browser and reports what did not carry over, so entries below describe only what a map cannot express, behavior changes, semantics to re-check, and erase-flash moves. It works even on a freshly erased device: join its `MM-XXXX` SoftAP, open `http://4.3.2.1`, restore there, and take the offered restart; the bundle carries the WiFi credentials, so the device comes back on your network. For a device still on old firmware (no Backup button yet), the [installer page](https://moonmodules.org/MoonLight/install/) offers the same backup as a bookmarklet.

**Read this when upgrading a device that already holds persisted state.** Entries are newest first. Each says what changed and what to do; most need nothing at all, because the lost value re-populates on next use.

**MoonLive is exempt until it launches.** Nobody is running scripts on a device yet, so a break in the script language or its storage cannot strand anyone, and an entry here would describe an upgrade path no user can take. Its breaking changes are recorded in the commit and PR record instead. This exemption ends at the first release that ships MoonLive as a supported feature; from then it follows the same rule as everything else.

**Action legend**, how much work an entry costs you:

| Action | Meaning |
|---|---|
| *nothing* | Self-heals. The value re-populates on next use, or the default is correct. |
| *re-set a control* | One value resets to its default; set it again in the UI if you had changed it. |
| *re-add a module* | The module vanishes from the tree on boot; add it again and re-enter its controls. |
| *update a file* | An on-device file must be edited or replaced. |
| *erase flash* | A full flash erase is required (the heaviest, a full reconfigure follows). |

---

## Unreleased

### A MoonBase from before this release misses the Ethernet wiring and the power cap

**Action: *nothing* on WiFi; on an Ethernet-only or power-capped board, install the matching MoonBase.**

MoonBase reads the app's saved network settings, and the Ethernet wiring and the WiFi power cap moved under the new Ethernet and WiFi cards.
A MoonBase from before this release still joins the first known WiFi network, but finds neither of those.
On an Ethernet-only board it then opens an open access point named `MoonBase`.
An update started from a browser on the cable loses the device until you join that network.
On a board the device catalog caps, it runs the radio at full power, which can brown out a weak supply.
Before the next firmware update, install the matching MoonBase from the Firmware card's MoonBase tab, which writes it over the network.

### WiFi has its own card under Network, with a list of known networks

**Action: *re-set a control*.**

The WiFi settings moved from the Network card to a new WiFi card below it: the network becomes the first row of `known`, with its IP settings, and `txPowerSetting` moved with it.
A backup restored through the File Manager carries the network and the power cap across.
Updated in place without a restore, the device knows no network: it opens its access point, where the network is entered again.
A MoonBase from before this release reads part of the new layout: see the MoonBase entry above.

### Ethernet has its own card under Network

**Action: *re-set a control*.**

The Ethernet settings moved from the Network card to a new Ethernet card below it: `ethBoard`, `ethType`, `ethPhyAddr` and the pins.
The wired interface also has its own IP settings now: `ipSettings` (DHCP or Static) with `ip`, `gateway`, `subnet` and `dns`.
Each known WiFi network has its own as well.
A backup restored through the File Manager carries the Ethernet settings across, and gives the wired interface the IP settings the device had.
Updated in place without a restore, the Ethernet card starts from its defaults.
Pick the board under `ethBoard` again, and set a static address if the device used one on its cable.
A device model from the web installer sets Ethernet on the new card.

### Light presets are fixture profiles

**Action: *re-set a control*.**

The library of named channel wirings (RGB, GRB, RGBW, a moving head's channels) takes the DMX industry term. It no longer shares a name with the Control card's presets, which keep theirs.
The module `LightPresets` is `FixtureProfiles`, its list `presets` is `profiles`, and each driver's `lightPreset` control is `fixture`.
A backup restored through the File Manager carries all of it.
Updated in place without a restore, the saved `LightPresetsModule` entry names a type that no longer exists, so it is skipped.
The built-in profiles are back and your custom rows are gone.
Each driver's saved choice is ignored too, so it falls back to its default, GRB for strips and RGB for network sinks.
Re-add the custom profiles and re-pick each driver's `fixture`.
An API client or an automation that sets `lightPreset` sets `fixture`.

### The palette is saved and reported by name

**Action: *nothing*.**

`Drivers.palette` is saved by its name, such as `Ocean` or `fire.mlp`, so adding a palette script no longer moves a scripted choice to its neighbor after a reboot.
A saved number still loads, and a write takes a number or a name.
A client reading the value through the API gets the name where it got a number.

### Network services share `addressing` and `hosts`

**Action: *re-set a control*.**

Every service that sends to many receivers names the choice the same way: `addressing` (unicast, multicast, broadcast, as the protocol allows) and `hosts`, a list of addresses and names such as `panel-01.local`.
The Network Send driver's `ips` and `lightsPerIp` become `hosts` and `lightsPerHost`; OSC's `feedbackTo` becomes `hosts`; Devices' `wledCompatible` becomes `addressing` (on is `multicast + broadcast`).
A backup restored through the File Manager carries all of these over; on a device updated in place, enter the hosts and the addressing again.
The `E1.31 multicast` protocol option is gone: such an output comes back as `E1.31`, so set its `addressing` to `multicast`.
Presence packets now mark which copy they are. A device on an older firmware therefore lists a newer one as MoonLight and as WLED in turn, so update every board in one go.
Devices' `addressing` defaults to `multicast + broadcast`, so WLED devices list MoonLight devices without a setting.

### Inputs target the control surface only

**Action: *re-set a control*.**

A button, analog, infrared or gamepad row names a surface control (`Control.switch1`, `encoder3`, `fader2`, `pad5`); the [Control](../moonmodules/core/system.md#control) card decides what that control drives.
A saved row that named another control directly, such as `Drivers.on`, comes back unassigned, and the service's status says how many.
Point the row at a surface control and give that control the target the row had.
OSC's `/mm/control/<Module>/<control>` address is gone for the same reason; send to `/mm/fader/N`, `/mm/encoder/N` or `/mm/switch/N`.

### Pong keeps its own clock

**Action: *nothing*.**

Pong's `audioReactive` is gone, since a ball that moved only on the beat read as a stalled game.
The rally always runs on its clock, and a saved value is ignored.

### A v5 device updates through v6.0.0

**Action: *nothing* on a device already on v6.0.0. A device on v5 installs v6.0.0 first, then this release.**

The firmware image now carries the name `MoonLight` where it carried `projectMM`, and a device checks that name before it installs anything.
v5 knows only the old name, so it refuses this release with "that image is MoonLight, not this project"; v6.0.0 knows both, which is what it shipped for.
Update a v5 device to v6.0.0 from the Firmware card first, then to this release, or install this release over USB from the [installer page](https://moonmodules.org/MoonLight/install/).

### The recovery image carries the new name too

**Action: *nothing*, unless a device came from v5 over the air: install MoonBase images only from the Firmware card or the installer page, never from MoonBase's own page.**

MoonBase, the recovery image on 4 MB devices, is now `MoonLight-moonbase` where it was `projectMM-moonbase`.
An update over the air replaces only the app.
A device that went from v5 to v6 that way still carries the v5 MoonBase, which knows only the old name.
Its own install page refuses a recovery image by that old name, but it takes the new one for an app and writes it over the app.
After that, only a USB cable brings the device back.
The Firmware card's MoonBase update and the installer page both recognize the new name.
Either one also replaces the old MoonBase, which closes this for good on that device.

## v6.0.0

### The desktop settings folder and the installer's device list carry the new name

**Action: *update a file*, on a desktop install: copy the settings folder across. Bookmarks on the installer page are added again.**

A desktop install keeps its config, presets and scripts in a folder named after the product, and that folder is now `MoonLight` where it was `projectMM`:

- Windows: `%LOCALAPPDATA%\MoonLight`
- macOS: `~/Library/Application Support/MoonLight`
- Linux: `$XDG_DATA_HOME/MoonLight` or `~/.local/share/MoonLight`
- the container: `/data/MoonLight`

The old folder is left alone rather than read, so the app starts on its defaults.
The app itself installs beside an existing projectMM on every desktop platform, and the two compete for port 8080, so uninstall projectMM first.
Copy the contents across to keep what you had.
A Backup taken in the old install and restored into the new one carries it across too.

The installer page's saved device list moves the same way, from `projectMM.devices.v1` to `MoonLight.devices.v1` in the browser's local storage.
The page opens with no remembered devices, and each one is added again by address.

### The Debian package, the container image and the macOS bundle carry the new name

**Action: *update a file*, on a Linux install from the `.deb` or a container deployment.**

The Debian package is now `moonlight` where it was `projectmm`, so `apt` treats it as a different package and installs it beside the old one.
Remove the old one first with `sudo apt remove projectmm`, then install `moonlight_X.Y.Z_<arch>.deb`.
A systemd unit written from the install guide names the old binary: point its `ExecStart` at `/usr/bin/MoonLight`.

The container image is `ghcr.io/moonmodules/moonlight`, and the compose file's service, container and volume are named `moonlight` and `moonlight-data`.
A deployment pulling the old image name stays on v5.0.0, so change the image name.
Then copy `/data/projectMM` to `/data/MoonLight` inside the existing volume, or Restore a Backup into a new one.

The macOS bundle identifier is `org.moonmodules.moonlight`, which macOS reads as a new app.
A permission granted to the old one, such as the microphone, is asked for again.

### MQTT topics and the Home Assistant entity carry the product's new name

**Action: *update an automation*, on a device you drive over MQTT or through Home Assistant.**

The topic root is now `MoonLight/<last6-of-MAC>` where it was `projectMM/<last6-of-MAC>`.
The Home Assistant discovery object, its unique id and the client id follow the same root.
A broker subscription or an automation written against the old root stops matching, and Home Assistant keeps the old retained config, so the previous entity goes unavailable while a new one appears alongside it.

Delete the stale entity in Home Assistant and repoint any automation or dashboard at the new one, then rewrite subscriptions and publishes to the new root.
Nothing on the device needs changing: the root is derived from a single constant, so every topic moves together.

### Audio arrives on every device, and its modes are reordered

**Action: *re-set a control*, on a device whose Audio you had configured.** Restore maps the value for you, so this asks something only of a device upgraded in place.

Audio is now wired at boot rather than added by hand, because the default effect reacts to sound. A device without the module showed none of that: the lights moved without answering whether anything was heard. It defaults to **simulate**, a synthesized signal, so a device demonstrates the behavior before a microphone is wired to it. A board that has a microphone selects `local audio` in its catalog entry, the way it already names its pins.

The mode options are reordered to run simple to advanced: **simulate, receive network, local audio**, where the order was local, receive, simulate. The default is now the first entry rather than an index that depended on whether the platform had a network. The selection is persisted as that index, so every saved value moves: what read 0 for local audio now reads 2, and what read 2 for simulate now reads 0. [Restore](../how-to/backup-and-restore.md) carries both. A device with no network has two options rather than three, and its old `1` is ambiguous, so re-pick that one by hand.

## v5.0.0

The last release under the projectMM name. Its [release notes](https://github.com/MoonModules/projectMM/releases) summarise what these entries ask of you.

### Renames Restore carries for you

[migrate.js](https://github.com/MoonModules/MoonLight/blob/main/src/ui/migrate.js) maps each of these, so a Backup taken on an older firmware restores onto this one with the value intact. Restore reports what it could not carry. They are listed rather than described, because the map is the description.

| Was | Is now |
|---|---|
| `Layers` container, `Layers.json` | `Effects`, `Effects.json` |
| `Noise2DEffect` | `NoiseEffect`, which renders the same field |
| `IrService` | `InfraredService` |
| `MultiPinLedDriver`, `MoonLedDriver`, `ParlioLedDriver`, `I80LedDriver`, `MoonI80LedDriver` | `ParallelLedDriver` with a `peripheral` select |
| a driver's `preset` or `lightPreset`, and its `presetRef` | `fixture`, and `fixtureRef` |
| `LightPresetsModule` and its `presets` list | `FixtureProfilesModule` and its `profiles` list |
| `soundReactive` | `audioReactive` |
| `forceRing` | `useRing` |
| `sync` on AudioService | `mode`, beside a new `send audio` |
| `fps` on PreviewDriver | `targetFps` |
| peripheral values `i80`, `MoonI80` | `LCD-IDF`, `LCD-MM` |

Three residues a map cannot carry:

- **A re-learned remote.** `InfraredService` keeps the module but not its codes: a learned code used to be a control's value and is now a row. Press the remote's keys again against the rows you want.
- **`simulate` on AudioService** collapsed from five options to two, so a saved value past the second is clamped rather than mapped.
- **A driver's `peripheral`** is chip-dependent where the old type did not say which bus it used. Restore flags it for review rather than guessing.

An external tool that POSTs to a control by name follows the same renames; the device answers only to the current name.

### Boards move to the MoonBase partition table (4 MB on 2026-08-26, esp32-16mb on 2026-08-28)

**Action: erase flash** (USB re-flash). Back up first: the File Manager's ⤓, or the installer's bookmarklet on older firmware. Restore after the install brings WiFi, config and scripts back.

The dual-OTA layout gives way to [MoonBase](../explanation/architecture/moonbase.md), which keeps one app slot and a recovery image rather than two app copies. On the 4 MB variants (`esp32`, `esp32-wrover`, `esp32-eth`) the app slot grows 1856 to 2496 KB and the filesystem 256 to 548 KB. On `esp32-16mb` the filesystem grows 7168 to 11264 KB and the app slot keeps its full 4096 KB. Both gain the same recovery story: a power cut mid-install boots MoonBase, and the update is retried over the network.

**Every partition moves, so the new table looks elsewhere for the filesystem volume.** Without a backup, WiFi credentials, module config and scripts all re-enter through provisioning. A partition table only changes over USB. A device still on the old table keeps OTA-updating within it for as long as the app fits, and the web installer is the migration path. 8 MB boards keep their layout.

### System: `expertMode` became `mode`, with three levels

**Action: re-set a control, and only if you had expert mode on.**

The switch that revealed advanced controls is now a three-way select: `user`, `expert` (🎚️) and `developer` (🔧), each level showing what the one below it shows. A saved `expertMode` no longer matches a control and is dropped, so a device comes up in `user` mode whatever it held before. Pick the level you want again on the System card.

The old flag could only say "show more" or "show less", which left diagnostics that mean nothing without the source sitting beside the controls a light show is built from.

### Audio: `floor` is now the silence threshold in both level modes

**Action: re-set `floor` on a device whose microphone you had tuned.**
Affects any device running the Audio module with a local microphone or line-in.

`levels = automatic` is tuned with `floor` alone: how hard the learner levels, and how far it may lift a band, are constants rather than controls, because both act on a per-band range the conditioner has already normalized per rig, so one value serves every source.

`floor` is what changes meaning, and why re-setting it is worth a minute. It is now the **silence threshold** in both modes: below it a band reads zero and the learner does not learn from it. That is what stops a quiet room being amplified to full scale, but it also means a `floor` tuned under the old behavior can now gate audible sound. Raise it until a silent room reads still, then stop; there is no second knob to compensate with. `gain` remains manual-only and keeps its meaning.

Two behavior changes ride along and need no action. The spectrum now starts at 40 Hz rather than ~11 Hz, dropping a first band that could only ever hold mains hum, DC drift and rumble. And AudioSpectrum's VU bar reads the raw level instead of the smoothed one, because it is the audio test instrument and wants maximum response; every other effect keeps the calm smoothed VU.

### MoonBase serves the OTA routes under the application's names

**Action: nothing on most devices; a serial flash on a MoonBase device updated from a browser.**
Affects the 4 MB classic, `esp32-16mb` and the S3-Zero, the variants that carry MoonBase.

MoonBase served `/install`, `/install-url`, `/boot-app`, `/last-url` and `/cancel` while the application served `/api/firmware/upload`, `/api/firmware/url` and `/api/firmware/moonbase`: two names for one operation, across images that a single browser page talks to in turn during one update. It now serves them under the application's names.

The break is between the two images on a device, not between a device and its config. A device whose MoonBase predates this change still answers only the old names, so an updated application handing over to it leaves the browser calling routes that image does not have. The way through is the same as any MoonBase update: flash both images over serial once ([building.md](../how-to/building.md#flashing-a-running-device-over-the-network)). A device flashed serially from this version on is consistent and needs nothing.

### AudioVolume is gone

**Action: pick another effect.** Affects any device with an AudioVolume effect on a layer.

It drew one bar from the audio level, which every audio-reactive effect does as a side effect of what it draws. There is no successor to map it onto, so a restored config carrying an `AudioVolumeEffect` node finds no such type and the layer comes up without it. `GEQ` is the nearest thing if a literal meter is what you want.

### A fixture profile's Dimmer channel is now driven

A profile that declares a `Dimmer` role previously left that channel at 0, because nothing ever wrote it: `Correction` resolved only the color roles. A fixture on such a profile therefore emitted nothing at all, whatever its color channels said. The shipped `IRGB` preset ("CH1 master intensity") could never light a fixture.

The dimmer is now held open (255) every frame, with per-light brightness staying in the color values as before. **If you drive a fixture on `IRGB` or another dimmer-carrying profile, it will light up where it previously stayed dark.** Nothing to change; the previous behavior was a defect.

Routing brightness to the dimmer channel rather than holding it open is the better model and is [backlogged](../work/future/backlog-light.md), so this value will change again.

### WLED apps find a device only when you ask them to

Device discovery now announces on the multicast group `239.255.77.77` and, by default, **not** on the broadcast address WLED apps and devices browse. A MoonLight device therefore stops showing up in them until you turn on `wledCompatible` in the Devices module.

MoonLight devices still find each other either way: presence always goes to the group and every device always joins it, so a fleet can mix the setting freely.

The reason for the default: a broadcast at discovery cadence makes every phone, printer and laptop on the LAN take an interrupt and parse a packet none of them want. Multicast reaches only the devices that joined the group. See [multicast and IGMP snooping](../explanation/architecture/moonlight.md#multicast-and-igmp-snooping) for when that saving is real (a switch that snoops) and when it is not.

### PreviewDriver's `fps` becomes `targetFps`, and now trades resolution (2026-08-25)

The control is renamed and its meaning changed, so the rename is the point rather than cosmetic.

**Before:** `fps` was a ceiling. The driver never exceeded it, but a link short of that rate delivered fewer frames and the control did nothing about it.

**Now:** `targetFps` is the rate you *want*. The driver still never exceeds it, and when the link cannot keep up it **trades preview resolution** to get closer, lower it for full detail at a slower rate, raise it for a smoother but coarser preview. That makes the slider the place where you choose between detail and smoothness, which is what users were reaching for.

**Action: none required.** The preview is a view, not output. A device that had a non-default `fps` saved falls back to the default 24 on first boot with this firmware, because the persisted key changed; set `targetFps` if you had tuned it. Mixed versions degrade soft: an old UI against new firmware sends no detail request and gets full detail (capped by memory); a new UI against old firmware sends an uplink message the device ignores.

### The desktop build keeps its files in `build/fs`

**Action: move your data, or lose your settings.** Affects the DESKTOP build only, and only a
developer running it from a repository checkout; devices are unaffected.

A desktop install used the build directory itself as the device's filesystem. The File Manager's root therefore listed CMake caches, object archives and every build folder alongside the four directories a device carries. It now roots at `build/fs`, so what the desktop shows is what a board shows.

An existing checkout starts with an empty-looking device, because its `.config` is one level up.
Move what you want to keep:

```sh
mkdir -p build/fs
mv build/.config build/moonlive build/.hls build/fs/ 2>/dev/null
```

Nothing is deleted if you skip this: the old directories stay where they are, and the device starts fresh. `MM_DATA_DIR` still overrides the location, and a packaged desktop install (which uses the per-user data directory) is unchanged.

### Desktop settings move to a per-user directory (2026-08-23)

The desktop build wrote its configuration to `build/.config`, resolved against whatever directory the process happened to start in. That is a source-checkout layout, and it shipped: a downloaded binary either could not write there at all, failing every save and logging one line per save, or it wrote settings that belonged to that *folder* rather than to the user, so moving the executable lost them.

Settings now live with the user: `%LOCALAPPDATA%\MoonLight` on Windows, `~/Library/Application Support/MoonLight` on macOS, and `$XDG_DATA_HOME/MoonLight` on Linux, falling back to `~/.local/share/MoonLight` when that is unset. `MM_DATA_DIR` overrides it. **A source checkout stays in the tree**, under `build/fs` since the entry above moved it there, so its settings are at `build/fs/.config` and every gate script behaves as before.

**Action: *nothing*, unless your settings persisted before.** The old behavior had two modes, and only one of them leaves anything to move:

- **Saves were failing.** The log showed `write failed for /.config/...` on every change and nothing survived a restart. Nothing to carry across.
- **Saves were succeeding, per folder.** They are in a `build/.config` folder beside wherever you launched from: the folder you unzipped into on Windows and Linux, and `~/build/.config` on macOS, because the `.app` launcher starts in your home directory. **Action: *move a folder*.** Move the `.config` directory itself into the new per-user directory, so it lands as `<data directory>/.config` rather than spilling its files into the root. Or leave it and reconfigure from scratch.

ESP32 is unaffected: LittleFS mounts at a fixed partition and never used this path.


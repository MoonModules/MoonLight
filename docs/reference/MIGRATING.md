# Migrating

The log of **breaking changes**, what changed between versions, and the action to take.

MoonLight ships **no migration code**: the persistence layer is robust by default (an absent key keeps the control's default, a stale value clamps to the new bounds, an unknown key is ignored), which absorbs almost all schema drift with zero migration-specific code. The rare change that a robust reader *cannot* absorb is **documented here instead of migrated**. A patching framework is deferred rather than rejected: it becomes the right tool if breaking format changes get frequent enough that ad-hoc losses pile up (a rough bar: more than five across a few releases) and users hold persisted state too valuable to re-derive. At that point build the recognizable version-stamp plus ordered-patch-chain pattern, not a bespoke one and its rationale.

**The File Manager's Backup (⤓) / Restore (⟲) carries config across these breaks.** [src/ui/migrate.js](https://github.com/MoonModules/projectMM/blob/main/src/ui/migrate.js) is the **authoritative, dated log of every machine-mappable break** (file, type, control, and value renames): Restore applies it in the browser and reports what did not carry over, so entries below describe only what a map cannot express, behavior changes, semantics to re-check, and erase-flash moves. It works even on a freshly erased device: join its `MM-XXXX` SoftAP, open `http://4.3.2.1`, restore there, and take the offered restart; the bundle carries the WiFi credentials, so the device comes back on your network. For a device still on old firmware (no Backup button yet), the [installer page](https://moonmodules.org/projectMM/install/) offers the same backup as a bookmarklet.

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

### MQTT topics and the Home Assistant entity carry the product's new name

**Action: *update an automation*, on a device you drive over MQTT or through Home Assistant.**

The topic root is now `MoonLight/<last6-of-MAC>` where it was `projectMM/<last6-of-MAC>`, and the Home Assistant discovery object, its unique id and the client id follow the same root.
A broker subscription or an automation written against the old root stops matching, and Home Assistant keeps the old retained config, so the previous entity goes unavailable while a new one appears alongside it.

Delete the stale entity in Home Assistant and repoint any automation or dashboard at the new one, then rewrite subscriptions and publishes to the new root.
Nothing on the device needs changing: the root is derived from a single constant, so every topic moves together.

### Audio arrives on every device, and its modes are reordered

**Action: *re-set a control*, on a device whose Audio you had configured.** Restore maps the value for you, so this asks something only of a device upgraded in place.

Audio is now wired at boot rather than added by hand, because the default effect reacts to sound. A device without the module showed none of that: the lights moved without answering whether anything was heard. It defaults to **simulate**, a synthesized signal, so a device demonstrates the behavior before a microphone is wired to it. A board that has a microphone selects `local audio` in its catalog entry, the way it already names its pins.

The mode options are reordered to run simple to advanced: **simulate, receive network, local audio**, where the order was local, receive, simulate. The default is now the first entry rather than an index that depended on whether the platform had a network. The selection is persisted as that index, so every saved value moves: what read 0 for local audio now reads 2, and what read 2 for simulate now reads 0. [Restore](../how-to/backup-and-restore.md) carries both. A device with no network has two options rather than three, and its old `1` is ambiguous, so re-pick that one by hand.

## v5.0.0

The last release under the projectMM name. Its [release notes](https://github.com/MoonModules/projectMM/releases) summarise what these entries ask of you. <!-- rename-keep: this records what shipped, and the sweep would rewrite it into a claim that was never true. -->

### Renames Restore carries for you

[migrate.js](https://github.com/MoonModules/projectMM/blob/main/src/ui/migrate.js) maps each of these, so a Backup taken on an older firmware restores onto this one with the value intact. Restore reports what it could not carry. They are listed rather than described, because the map is the description.

| Was | Is now |
|---|---|
| `Layers` container, `Layers.json` | `Effects`, `Effects.json` |
| `Noise2DEffect` | `NoiseEffect`, which renders the same field |
| `IrService` | `InfraredService` |
| `MultiPinLedDriver`, `MoonLedDriver`, `ParlioLedDriver`, `I80LedDriver`, `MoonI80LedDriver` | `ParallelLedDriver` with a `peripheral` select |
| a driver's `preset` | `lightPreset` |
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

### A light preset's Dimmer channel is now driven

A preset that declares a `Dimmer` role previously left that channel at 0, because nothing ever wrote it: `Correction` resolved only the color roles. A fixture on such a preset therefore emitted nothing at all, whatever its color channels said. The shipped `IRGB` preset ("CH1 master intensity") could never light a fixture.

The dimmer is now held open (255) every frame, with per-light brightness staying in the color values as before. **If you drive a fixture on `IRGB` or another dimmer-carrying preset, it will light up where it previously stayed dark.** Nothing to change; the previous behavior was a defect.

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

Settings now live with the user: `%LOCALAPPDATA%\projectMM` on Windows, `~/Library/Application Support/projectMM` on macOS, and `$XDG_DATA_HOME/projectMM` on Linux, falling back to `~/.local/share/projectMM` when that is unset. `MM_DATA_DIR` overrides it. **A source checkout stays in the tree**, under `build/fs` since the entry above moved it there, so its settings are at `build/fs/.config` and every gate script behaves as before.

**Action: *nothing*, unless your settings persisted before.** The old behavior had two modes, and only one of them leaves anything to move:

- **Saves were failing.** The log showed `write failed for /.config/...` on every change and nothing survived a restart. Nothing to carry across.
- **Saves were succeeding, per folder.** They are in a `build/.config` folder beside wherever you launched from: the folder you unzipped into on Windows and Linux, and `~/build/.config` on macOS, because the `.app` launcher starts in your home directory. **Action: *move a folder*.** Move the `.config` directory itself into the new per-user directory, so it lands as `<data directory>/.config` rather than spilling its files into the root. Or leave it and reconfigure from scratch.

ESP32 is unaffected: LittleFS mounts at a fixed partition and never used this path.


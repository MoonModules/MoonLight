# Processes inventory

Step 1 of [Processes, described once](../present/Plan-20261015%20-%20Processes%2C%20described%20once%20(16%20of%2023).md), the Extract phase: every process MoonLight has, recorded by its content rather than the format it is stored in, counted on 2026-10-16 by three read-only Researchers. Part 1 is the end user's journey, Part 2 the contributor's, product owner's, agents' and CI's, and Part 3 the executable step lists: the scenarios and the MoonTube clips.

## What it found

**71 entries**: 22 end-user processes, 23 contributor, agent and CI processes, and 26 executable ones (4 runners, 6 scenario families covering all 32 scenarios, and the 14 clips). They overlap where one process is also a test or a film: the ESP32 install is a how-to, a clip and a scenario family at once.

**The journey, as a story map's backbone**: read the docs, install (ESP32 from the browser, desktop, Linux and Docker, Teensy), first setup (Improv or the access point), first look and a first light show, everyday use (presets, the gallery, control surfaces, smart home, the help), advanced use (MoonLive scripts, panels, multi-device, the API, drivers and services), maintenance (firmware updates and MoonBase, backup and restore, migrating from WLED, diagnosing), reporting an issue, contributing (pick, spec, plan, branch, build and test, document, pin a test, commit, merge), and releasing.

### Most duplicated

| Process | Copies | Where |
|---|---|---|
| Install on an ESP32 from the browser | 5 | gettingstarted, the installer README, the installer page and its flow comment, clip 01-install-esp32, scenario family C for the device side |
| Build a light show | 5 | the first-light-show tutorial, gettingstarted, clips 05 and 06, scenario family B, clip 04 filming a scenario, with three different effect casts for one story |
| Update the firmware | 5 | updating-firmware, building, the installer, MoonBase's page, the app |
| The desktop test step | 5 | CLAUDE.md, MoonDeck.md, testing.md, building.md, the agents' memory |
| Staging and "commit now" | 6 | CLAUDE.md and five memory files |
| ESP32 bring-up | 13 | two docs pages and eleven memory files |
| First look, backup and restore, presets, the plan lifecycle | 3 to 4 each | consistent copies |

### No home of its own

- **In the agents' memory only**: iterating a MoonLive script on a board, the three-variant ESP32 gate, never `git stash`, `--keep` on clips that build something, the fork-PR push target, who triggers the CodeRabbit review (auto-review is off), and the order of a release's human steps.
- **On GitHub**: no issue template, no pull request template, no CONTRIBUTING.md; the issue checklist and the PR size rule live in docs only.
- **For a user**: joining a device through its access point (its only complete copy is a scenario), turning help on (the UI spec only), the installer's Teensy and "Your devices" branches (code comments only), the Ethernet branch of first setup (prose only), the multi-board network matrix (two scripts outside the scenario vocabulary).
- **Filmed but not on a how-to page**: clips 07-drivers and 09-services are embedded only on the catalog pages, through `@video` in DriverBase.h:20-21 and Services.h:8; the gallery is filmed by no clip.

### Copies that disagree

- The `{ }` button opens a popup with copy, diagnostics and save-as-preset, while ui.md, gettingstarted and logging-an-issue describe a new tab; gettingstarted names the card `?` button the app no longer has.
- Restore applies live (backup-and-restore) or on reboot (integrating); which variants carry MoonBase (updating-firmware against building); arm64 Linux and Docker (installing-on-linux and building against release.yml); the IDF pin (building against release.yml).
- CLAUDE.md lists `check_prose` as always run, MoonDeck.md keeps it out of the gate table; testing.md says to run `ctest`, against CLAUDE.md; testing.md cites a gate numbering CLAUDE.md no longer has.
- release.yml's header counts four firmwares and two desktop binaries; test.yml names a branch its trigger lacks.
- Two memory files contradict the repo: board provisioning is no longer two steps, and the GCC build rule differs from CLAUDE.md's.
- Two generations of live measurement are described side by side: the baseline flags with `scenario-baseline.json`, and the contract and observation blocks in the scenario files.

### The two step vocabularies

The scenarios (584 steps over 12 operations) and the clips (274 steps over 25 actions) map one-to-one on intent: set a control, add, replace, delete, clear, expect, set up a precondition. Both address a target as a module and a control, the keys `help.json` already uses, which is the hook the plan describes. What one cannot say is the other's surface: HTTP expectations, reboots and measurements against opening cards, waiting and starting processes. Preconditions carry three names for one intent (a scenario's fixture, reset and top; a clip's setup; the boot state in `reset_device.py`). Only 4 of 274 clip steps carry an expectation, so 12 of 14 UI tests prove an element exists rather than that the device agrees.

**Defects found on the way**: the in-process scenario runner passes an unknown operation silently (`scenario_runner.cpp` has no else after its measure branch), where the live runner prints SKIP; `skip_on`, `bounds` and per-target `expected` are documented and used by no scenario; the `live_only` flag is marked legacy yet all 8 device scenarios carry it.

### For step 2, the model

- The journey has eleven stages and two actors' halves that meet at contributing, the question the plan leaves open.
- One step shape covers both vocabularies: an action on a module and control, a precondition, an expectation; Given, When, Then, with the help's keys as the target.
- The scenarios are the most complete step lists for the network processes, and the docs for the install, so the model starts from both rather than either.
- What lives in memory only is the first content to move into process files, since nobody but the agents can read it today.


## Part 1: the end user's journey

Sources read: README.md, docs/index.md, docs/gettingstarted.md, docs/tutorials/*.md, docs/how-to/*.md, docs/reference/integrating.md, docs/moonmodules/core/ui.md, mooninstaller/ (README, index.html, install-orchestrator.js, devices.js, backup-snippet.js), src/ui/app.js (gallery, presets, `{ }`, GIF), moontube/clips/*.json (names only), test/js, test/scenarios. Paths are repo-relative; "clip" means a MoonTube clip filming the same steps.

#### Install MoonLight on an ESP32 from the browser
- Actor: end user
- Goal: a blank board runs MoonLight, is on the network, and carries its model's defaults.
- Steps: open the installer in Chrome/Edge · plug in · pick the USB port (chip detected) · pick the device card · tick Erase (defaults auto-tick) · Install (erase, write, progress) · WiFi form → Connect, or Skip · defaults pushed over serial as APPLY_OP documents · "Device is online!" link · remembered under Your devices
- Written in: docs/gettingstarted.md:17-104 (full); mooninstaller/README.md:5-28 (full, mechanism); mooninstaller/install-orchestrator.js:12-24 (full, flow comment); mooninstaller/index.html:96-108,176-182,290-430 (full, the modal sections); README.md:7,165, docs/index.md:7-8, panel-cards.md:122-128, updating-firmware.md:52-54 (mentions)
- Executed by: the installer page; clip `01-install-esp32` (steps 1-10 on the preview installer); `improv_smoke_test` (MoonDeck) on the provisioning path
- Verified by: test/js/installer-*.test.mjs (manifest merge, flash progress, baud, S31, eth-only, state modules); test/python/test_improv_smoke_test.py, test_installer_manifests.py. The serial push and the online step: the clip and the PO's eyes only
- Branches: WiFi (Improv) / Skip → access point / Ethernet (no Improv reply → needs-IP form, "Try Improv again", Skip) / Teensy (HalfKay, "Choose the bootloader", "Show the board's address", orchestrator:504-506) / "Apply device defaults now, without flashing" / Windows without a serial driver (building.md § Windows USB-serial drivers)
- Touches: Network.WiFi (ssid, password, txPowerSetting via SET_TX_POWER), System.deviceModel, every container in the model's `state`, Drivers last
- Duplicates: 4 full copies. "Apply device defaults" gates the whole config push (index.html:103-107, README:23-28) and gettingstarted never names it. index.html:181 says "open 4.3.2.1, enter credentials" where gettingstarted:86 says the sign-in screen opens by itself.
- Gaps: the Teensy and needs-IP branches and "Your devices" (Visit / Erase / Forget, devices.js:6-8) exist only in code comments; the defaults push has no check beyond the clip.

#### Join a device through its own access point
- Actor: end user
- Goal: a device with no network becomes reachable and learns the WiFi.
- Steps: device opens `MM-XXXX` · join with a phone · sign-in screen opens by itself, or `http://4.3.2.1` · pick the network, password, Connect · the card links to the device at home · (or restore a backup there and take the restart)
- Written in: gettingstarted.md:86-87, troubleshooting.md:36-37 (incl. the 4x power-cycle reopening), backup-and-restore.md:44-46 (all partial); updating-firmware.md:46, migrating-over-the-air.md:30, index.html:181 (mentions)
- Executed by: test/scenarios/core/scenario_AccessPoint_first_setup_from_its_own_network.json (every step, this computer as the phone); scenario_AccessPoint_protected_and_always_on.json; scenario_Firmware_MoonBase_opens_the_devices_access_point.json
- Verified by: those scenarios' expect_http / expect_control
- Branches: app vs MoonBase AP; open vs password-protected; credentials vs a backup
- Touches: Network.WiFi known-network rows (password, connect, forget), `opens`, System factory reset
- Duplicates: 6 partial copies, none complete; only the scenario holds the whole flow.
- Gaps: no user page owns it; the power-cycle reopening lives only in troubleshooting.

#### Install MoonLight on a desktop
- Actor: end user
- Goal: MoonLight runs on the computer at `http://localhost:8080/`.
- Steps: installer, Install to = This computer · Download · Windows: Keep anyway · run the setup / drag the .dmg, right-click Open / `apt install ./moonlight_*.deb` · start it · the browser opens · settings in the per-user folder · updating: the ⬆ badge, then steps 1-4 again
- Written in: installing-to-desktop.md:5-121 (full, Windows); README.md:167-171, gettingstarted.md:264-274 (partial); installing-on-linux.md:26-53 (full, .deb); building.md:44-56 (settings directory)
- Executed by: the installer page; clips `01-install-desktop`, `02-first-look-desktop`
- Verified by: test/js/installer-desktop-download.test.mjs; the SmartScreen / Gatekeeper / quarantine steps by nothing
- Branches: setup.exe / zip + `Install-MoonLight.cmd` / Defender quarantine; .dmg; .deb / .tar.gz; `--port`, `--no-browser`
- Touches: FirmwareUpdate (the ⬆ badge)
- Duplicates: consistent; "where settings live" has two homes (building.md:44-56, installing-to-desktop.md:82-95).
- Gaps: none.

#### Run MoonLight on a Linux board, kept running
- Actor: end user
- Goal: an always-on Pi or NanoPi runs MoonLight after every reboot.
- Steps: `uname -m` · .deb (or: OS image, first boot, ssh, prerequisites, clone, `build_desktop`, `run_desktop`) · open `:8080` · systemd unit · or `docker run --network host -v … --restart unless-stopped`
- Written in: installing-on-linux.md:9-230 (full); building.md:95-135 (Docker, full)
- Executed by: human only
- Verified by: nothing
- Branches: package / source / Docker; overlay-root hosts; host vs bridge networking
- Touches: none
- Duplicates: the Docker recipe twice, contradicting: installing-on-linux:186 "one image carries amd64 and arm64", building.md:134 "amd64 only"; port 8080 host-net vs 8081 mapped.
- Gaps: the arm64 contradiction.

#### First look at the interface
- Actor: end user
- Goal: know what every card is before changing anything.
- Steps: three regions · ⏻ and the 🕒/🧠 readouts · the buttons on every card · System, Firmware, Network · Layouts → Effects → Drivers · mode user/expert/developer
- Written in: gettingstarted.md:108-238 (full); tutorials/how-moonlight-works.md:16-136 (full); moonmodules/core/ui.md:41-107 (full, spec); faq.md:20-24 (partial)
- Executed by: clips `02-first-look-desktop`, `02-first-look-esp32`, `03-second-look` (every button pressed, mode switched)
- Verified by: the clips' `expect` on Grid width/height; test/js/ui-*.test.mjs per rendering piece
- Branches: desktop vs device
- Touches: System.mode, Layer.enabled, Grid.width/height, ✎ × { }
- Duplicates: 3 full copies. gettingstarted:155 says a "?" button opens the docs; ui.md:97 and app.js:2849 have the Docs link inside the help box and no "?" glyph. gettingstarted:155 and ui.md:91 say `{ }` opens a new tab; app.js:2138-2147 opens a popup (copy, diagnostics, save as preset).
- Gaps: `{ }` and "?" are out of date against app.js.

#### Build a first light show
- Actor: end user
- Goal: a chosen layout, two effects, a modifier, a blended layer and a real driver, persisted.
- Steps: Grid 16x16, serpentine · + add module → Bouncing Balls, numBalls, grav · add Ripples · second Layer, blendMode, opacity · Mirror · Drivers brightness 20 → RmtLedDriver pins / NetworkSendDriver / Preview · reboot to prove it
- Written in: tutorials/first-light-show.md:9-67 (full); gettingstarted.md:216-238 (partial); faq.md:16-17 (mention)
- Executed by: clips `05-layouts`, `06-layers` (the same build), `07-drivers-desktop`, `07-drivers-esp32`
- Verified by: scenarios light/Layers_stack_and_blend_live, Layouts_resize_reallocates_live, Modifiers_reshape_the_mapping, Drivers_output_and_brightness; core/System_identity_survives_reboot
- Branches: pin / network / preview only
- Touches: GridLayout.width/height/serpentine, Layer.blendMode/opacity, BouncingBallsEffect.numBalls/grav, MirrorModifier, Drivers.brightness, RmtLedDriver.pins
- Duplicates: 2. how-moonlight-works.md:115 puts modifiers between layout and effect; first-light-show.md:42 between the effect and the lights.
- Gaps: none.

#### Write a first MoonLive script
- Actor: end user
- Goal: an effect typed in the browser runs compiled on the device with its own control.
- Steps: add MoonLive · `mine.mle` in script · write the class, save · beatsin · addControl → slider · setRGB per light · break a brace, see the error, fix it
- Written in: tutorials/first-script.md:9-102 (full); integrating.md:118-154 (full, over HTTP); generative-effects.md:190-200 (partial); multi-board-installation.md:150-157 (partial, `/api/file` iteration)
- Executed by: clip `08-moonlive-effects`; scenario light/Palettes_a_script_drives_the_effects
- Verified by: the clip; test/python/test_moonlive_catalog_scripts.py; MoonLive unit tests
- Branches: card editor vs `POST /api/file` + set `script`; `/moonlive` shadows `/.moonlive`
- Touches: MoonLiveEffect.script, the script's own controls
- Duplicates: 2 full copies, consistent.
- Gaps: none.

#### Update the firmware over the air, and recover from MoonBase
- Actor: end user
- Goal: a newer MoonLight on a running device, settings kept; a way back when it fails.
- Steps: back up · Firmware card · pick the image (latest / tagged, matching variant) · Install · reboot · (MoonBase variant: stage the URL, reboot into MoonBase, it writes, reboots back) · (MoonBase image: the selector) · recovery: MoonBase's page → Boot the app / From a file / From a URL
- Written in: updating-firmware.md:7-46 (full); gettingstarted.md:169-176,242-261 (full for recovery); integrating.md:192-205 (full, curl); building.md:296-333 (full, curl incl. the 413 and 000 cases); multi-board-installation.md:327-335 (partial); installing-to-desktop.md:97-105 (desktop); troubleshooting.md:47-49 (mention)
- Executed by: the Firmware card; `POST /api/firmware/url|upload|moonbase|boot-app`
- Verified by: scenarios core/Firmware_reports_what_is_running, Firmware_MoonBase_joins_the_apps_network, Firmware_MoonBase_opens_the_devices_access_point; test/js/installer-firmware-merge.test.mjs. No automated install.
- Branches: two app slots / MoonBase variant / desktop; URL vs upload; app vs MoonBase image
- Touches: FirmwareUpdate.image, Install, Restart in MoonBase; MQTT `update/set`
- Duplicates: 5 copies. updating-firmware.md:24 names the MoonBase variants as 4 MB classic, esp32-16mb, S3-Zero; building.md:171,300 adds esp32-wrover, esp32-eth, qemu.
- Gaps: the variant list disagrees; no run performs an OTA.

#### Migrate a WLED device to MoonLight over the air
- Actor: end user
- Goal: a bulb moves to MoonLight without a cable.
- Steps: chip on WLED's Info · download `shared-migrate-esp32c3.bin` · WLED Config → Security & Updates → Manual OTA · wait 10 s · open MoonBase (or join `MM-XXXX`) · Install the release · PWM Light driver, RGBCCT fixture, 1x1 grid
- Written in: migrating-over-the-air.md:21-37 (full); multi-board-installation.md:84-86 (mention)
- Executed by: human only · Verified by: nothing in this group · Branches: WiFi carried / AP; WLED / other firmware
- Touches: PwmLightDriver pins, FixtureProfiles RGBCCT, GridLayout 1x1
- Duplicates: 1 home · Gaps: no test of the user path.

#### Back up and restore a device
- Actor: end user
- Goal: every setting in one file, put back on the same or another device.
- Steps: File Manager → Backup (⤓) · keep it private · Restore (⟲), confirm twice · read the report · take the offered restart (network, port) · (old firmware: the bookmarklet) · (blank device: via its AP) · (clone: check pins, rename)
- Written in: backup-and-restore.md:9-56 (full); index.html:205-225 (full, bookmarklet and erase-restore order); integrating.md:175-190 (partial, file API); gettingstarted.md:71-72,87, updating-firmware.md:48-50, troubleshooting.md:62-64 (mentions)
- Executed by: the File Manager buttons; the bookmarklet (backup-snippet.js)
- Verified by: test/js/backup-bundle.test.mjs, backup-snippet.test.mjs, migrate.test.mjs; scenario core/FileManager_writes_and_reads_back
- Branches: button / bookmarklet; same device / clone; through the AP (config files left out)
- Touches: FileManager Backup / Restore, Network (at boot), HttpServer port
- Duplicates: 3 substantive copies. integrating.md:190 "a restore lands on the next reboot" vs backup-and-restore.md:29 "applies live, module by module".
- Gaps: integrating.md's restore timing is out of date.

#### Save, edit and apply a preset
- Actor: end user
- Goal: a look, geometry, hardware or service setup recalled with one click.
- Steps: click an empty pad (or right-click / long-press) → editor · name, container radio, "save current state here" · or a card's `{ }` → save as preset · click a pad to apply · right-click to rename, re-save, delete · drag to move
- Written in: presets.md:9-57,77-105 (full); integrating.md:156-173 (full, HTTP: `/api/list/Control/presets/<name>/apply`); home-automation.md:171-186 (the HA half); faq.md:29 (mention)
- Executed by: clip `10-control` (click an empty pad, type `balls`, save); scenarios core/Control_presets_capture_and_restore, Control_a_palette_preset_changes_only_the_palette
- Verified by: those scenarios; test/js/ui-gallery.test.mjs ("a pad saves the container its editor shows", "a refused name stops the save")
- Branches: pad editor vs card `{ }`; four containers
- Touches: Control.presets, Control.source/name/slot/save (app.js:3713-3722)
- Duplicates: 2 full copies, consistent. presets.md:11 names only right-click / long-press to save; app.js:4255-4272 and the clip use a plain click on an empty pad.
- Gaps: that gesture is undocumented.

#### Install a preset or script from the gallery
- Actor: end user
- Goal: something shared lands on a pad or in `/moonlive`.
- Steps: Control → Gallery · search / kind chip / sort · open a thumbnail (preview, prev/next) · "add to a pad" (missing scripts fetched first) / "try now" (asks when it sets pins or geometry) / "install" · newer-firmware warning
- Written in: presets.md:59-71 (full); app.js:6770-7080 (full, code); integrating.md:97 (mention)
- Executed by: the browser alone (raw.githubusercontent.com → `/.config/presets`, `/moonlive`, or `PATCH /api/state`)
- Verified by: test/js/ui-gallery.test.mjs (14 tests)
- Branches: preset vs script; look-only vs rig-bound; free pad; name taken
- Touches: Control.presets, `/moonlive`, `PATCH /api/state`
- Duplicates: 1 home plus code · Gaps: no clip films it; no scenario (browser-only by design).

#### Share a preset in the gallery, with a moving preview
- Actor: end user
- Goal: an entry in MoonLight-Gallery with a GIF.
- Steps: `{ }` → copy · apply the preset, press ⏺ on the preview bar · 3 s saved as a GIF · open an issue in the gallery repo
- Written in: presets.md:73-75 (full); ui.md:39 (mention); app.js:5911-5935 (code)
- Executed by: the browser; the rest in MoonLight-Gallery · Verified by: test/js/ui-gif.test.mjs (the encoder) · Branches: none
- Touches: the `{ }` popup, the preview bar ⏺
- Duplicates: 1 home · Gaps: the gallery-side steps are in another repository.

#### Log an issue
- Actor: end user
- Goal: a bug report with the device's state attached.
- Steps: search issues · check the Firmware version · steps / expected / actual · `{ }` on System, Firmware, the affected card, Layer, Drivers → copy → fenced blocks · a photo · if it will not boot: LEDs, what changed, repeat?, serial log, bootReason
- Written in: logging-an-issue.md:13-94 (full); troubleshooting.md:66-68, gettingstarted.md:155,282, how-moonlight-works.md:33,134, README.md:222, faq.md:50-52 (mentions)
- Executed by: human; clip `03-second-look` films `{ }` · Verified by: nothing · Branches: serves a page / crashed
- Touches: every card's `{ }`, System.bootReason
- Duplicates: 1 full home, 6 mentions. logging-an-issue.md:47-48 "select all in the new tab" vs app.js:3742-3800 (popup, copy button, diagnostics link).
- Gaps: the `{ }` steps are out of date.

#### Diagnose dark or flickering lights
- Actor: end user
- Goal: find which half is at fault before touching hardware.
- Steps: preview lit? → downstream, else upstream · brightness · a driver added · counts match · module status · (flicker) preview clean → loopbackTest PASS → sweep txPowerSetting → level shifter, resistor, wire, ground
- Written in: troubleshooting.md:5-16, led-signal-integrity.md:6-17, led-drivers.md:41-50 (loopback incl. through a '595), panel-cards.md:308-322 (all full for their symptom)
- Executed by: human; `loopbackTest` on the device · Verified by: the loopback test only · Branches: strip / card / HUB75
- Touches: Drivers.brightness, RmtLedDriver.loopbackTest/loopbackRxPin/loopbackStrand, Network.txPowerSetting, PanelCard status
- Duplicates: 3 homes for three symptoms, consistent · Gaps: none.

#### Connect a control surface
- Actor: end user
- Goal: faders under the hands drive the Control card and follow it back.
- Steps (OSC): install Open Stage Control · download the session · send `<ip>:9000`, osc-port 9001, load · Services → OSC listen + feedback · start · phone via its web page. (MIDI/gamepad): add the service · plug into the browser's computer · allow MIDI (mark a board's origin secure once) · assign on Control. (One desk, many boards): desktop OSC feedback → boards listen. (Bridge): MIDI `source` USB + `share`; leader `source` network + `host`. (Buttons): ButtonService rows writing a fader
- Written in: control-surface.md:25-43,85-107,115-121,142-261 (full, four routes); multi-board-installation.md:196-239,279-284,310-313 (full for the StadBeest); hold-to-play.md:24-52 (full, one document); faq.md:30 (mention)
- Executed by: clip `10-control` (assign a fader, OscService, `send_osc`); `run_open_stage_control.py`, `send_osc.py`
- Verified by: test/js/ui-midi-bridge.test.mjs, ui-gamepad-bridge.test.mjs; OSC by the clip only
- Branches: OSC / browser MIDI / gamepad / USB-host desk / RTP-MIDI / share / multicast vs unicast
- Touches: OscService.listen/feedback/feedbackPort/port/addressing/hosts/group, MidiService.profile/source/share/host, GamepadService, Control.faderN/encoderN/switchN targets, ButtonService
- Duplicates: the OSC leader/follower values appear in control-surface.md:96-107 and multi-board:227-239; "mark the origin secure" in control-surface:38-43 and referenced from multi-board:283.
- Gaps: no scenario drives OSC or MIDI.

#### Bring the device into a smart home
- Actor: end user
- Goal: on/off, brightness, color from Home Assistant, Apple Home or Homebridge; or Hue bulbs as pixels.
- Steps (HA): nothing, zeroconf finds it · optional MQTT: broker/port/user/password, haDiscovery on, `mqtt_status` connected · HomeKit Bridge. (Homebridge): MQTT → broker · mqttthing accessory · pair. (Hue): HueDriver, bridgeIp · link button + pair · room/light · run an effect
- Written in: home-automation.md:28-116 (full, three recipes), 188-354 (infrastructure); gettingstarted.md:206-214 (partial); README.md:31, integrating.md:215-222 (mentions)
- Executed by: human; clip `02-first-look-desktop` opens the Mqtt card
- Verified by: test/python/test_wled_json_shape.py; nothing for MQTT discovery or Hue
- Branches: WLED zeroconf / MQTT discovery / Homebridge / Hue; HAOS on Hyper-V vs Pi
- Touches: Mqtt.broker/port/username/password/haDiscovery/mqtt_status, HueDriver.bridgeIp/pair/appKey/room/light
- Duplicates: the broker steps twice on one page (:41-50, :77-85).
- Gaps: :198-285 (HAOS, Hyper-V, Zigbee) is not a MoonLight process.

#### Drive LED panels through a receiving card
- Actor: end user
- Goal: a ColorLight wall lit from an ESP32 or a desktop.
- Steps: LEDVision: panel, cabinet, arrangement, send, save · board (S31 / P4 / S3) · flash · wire · Grid = wall · an effect · PanelCard: format, interface (desktop), fps · brightness and localBrightness · status 1000 Mbit · (v13 card: LEDUpgrade 4.0 → 11.09, power-cycle, set `firmware`)
- Written in: panel-cards.md:11-22 (short form), 93-104,122-181,197-244,285-301 (full); hub75-panels.md:5-9, faq.md:35-36 (mentions)
- Executed by: human; clips `07-drivers-*` add and remove the driver · Verified by: nothing drives the path
- Branches: ESP32 / Windows (Npcap) / Linux (setcap) / macOS (sudo, ChmodBPF); gigabit switch
- Touches: GridLayout, PanelCardDriver.format/interface/fps/start/count/firmware/localBrightness, Drivers.brightness, FixtureProfiles
- Duplicates: the short and long form on one page · Gaps: the LEDVision steps are marked unverified (:89-91).

#### Set up a driver or service for the hardware
- Actor: end user
- Goal: the right LED driver, HUB75 panel, audio input or video output, with its few fixed settings.
- Steps: pick the driver by strands and chip · pin expander: `i80`/`MoonI80`, pinExpander, latchPin · HUB75: board, scanRate, peripheral, clockEdge, bitDepth · Audio: three I2S pins or PDM, then floor and gain, loopback device per OS · video: ffmpeg / NDI runtime, open the `url` in VLC/ffplay
- Written in: led-drivers.md:7-50, hub75-panels.md:12-36, audio-input.md:7-49, watching-the-output.md:7-73 (full each); faq.md:32-38 (mentions)
- Executed by: human; clips `07-drivers-esp32` (RmtLed pins/count, ParallelLed, Hub75), `07-drivers-desktop` (NDI, RTSP, HLS), `09-services` (Audio)
- Verified by: unit tests on the modules only · Branches: per chip, per OS
- Touches: RmtLedDriver.pins/timing, ParallelLedDriver.peripheral/pinExpander/latchPin, Hub75Driver.board/scanRate/clockEdge/bitDepth, AudioService.micMode/sckPin/wsPin/sdPin/levels/floor/gain/device, HlsDriver.encoder/scale/targetFps, NdiDriver.sourceName, RtspDriver.url
- Duplicates: single homes; the loopback test also in led-signal-integrity.md:8 · Gaps: none.

#### Run a multi-device installation (the StadBeest)
- Actor: end user
- Goal: several devices act as one piece: shared controls, one beat, unattended.
- Steps: one device per part · effects written for the hardware · iterate scripts on the device · one surface layout per device · OSC leader (feedback, multicast, 9001) and followers (listen 9001) · one audio-sync source · autopilot service on `switch8` · APC40 or a MIDI bridge · update unreachable devices over the network · router: IGMP snooping off, reservations
- Written in: multi-board-installation.md:73-373 (full); control-surface.md:96-107, audio-input.md:45-49 (partial)
- Executed by: the StadBeest itself
- Verified by: scenarios device/NetworkSend_one_sender_many_receivers, NetworkReceive_two_boards (frames, not controls)
- Branches: start simple: one device, then several on one beat, then one leading
- Touches: OscService, AudioService.addressing/hosts, MoonLiveService.script/run/seconds, MidiService.source/share/host, Control slots
- Duplicates: the OSC and MIDI setups repeat control-surface.md · Gaps: no check of a leader driving a follower.

#### Script against the HTTP API
- Actor: end user (integrator)
- Goal: add a module, set a control, apply a document, find the device.
- Steps: `POST /api/modules` (returns the name) · `POST /api/control` (numbers as numbers) · `GET /api/types` first · `PATCH /api/state` (`$patch: replace`, null removes) · errors as reasons · mDNS `<name>.local`
- Written in: integrating.md:14-116,207-238 (full); hold-to-play.md:24-49 (a worked document); mooninstaller/README.md:56-110 (the same shape for the catalog); presets.md:28-47 (partial)
- Executed by: `run_live_scenario.py` and every scenario JSON; the installer (serial) and MoonDeck (HTTP) apply catalog documents
- Verified by: scenario core/State_one_document_adds_an_effect_with_its_controls; test/js/installer-state-modules.test.mjs; HttpServer unit tests
- Branches: HTTP / MQTT / WebSocket · Touches: any module and control by name
- Duplicates: the state-document shape in four places, consistent · Gaps: none.

#### Turn help on in the app
- Actor: end user
- Goal: a help line under every module and control.
- Steps: press ⓘ in the status bar · agree once to download help.json from the docs site · a tap opens the guide, picture, Docs, source and tests links
- Written in: ui.md:55,97 (full, spec); no user page
- Executed by: the browser (`mm_helpConsent`, `mm_help`) · Verified by: test/js/ui-help.test.mjs, ui-help-cache.test.mjs
- Branches: no / yes; release copy vs site copy; `?help=local`
- Touches: the status bar ⓘ, every card's help box
- Duplicates: 1 home · Gaps: no user-facing description; gettingstarted:155 describes a per-card "?" instead.

#### Contributor pages parked in docs/how-to (not inventoried here)
building.md (build, flash, monitor, Docker, IDF pin, Windows serial drivers, OTA via curl) and adding-a-stream-source.md (a developer recipe ending in PR rules at :92-95). Clips `11-moondeck` and `04-scenario-testing` film that ground.

### Summary

22 user processes, plus 2 contributor pages in docs/how-to.

**Most duplicated**: the ESP32 install (4 full copies: gettingstarted, installer README, the orchestrator comment, index.html); the firmware update (5 copies); the first look (3 full); backup/restore (3); the OSC leader/follower values (twice, same numbers); Docker (twice, contradicting on arm64 and the port); the state-document shape (4 homes, consistent).

**No home in a user page**: joining the device through its access point (6 partial mentions; the complete copy is a scenario JSON); the installer's Teensy, needs-IP and "Your devices" branches (code comments only); turning help on (ui.md spec only); "Apply device defaults" (installer README and index.html only).

**Out of date against the code**: `{ }` is a popup with copy, diagnostics and save-as-preset (app.js:2138, 3742) where ui.md:91, gettingstarted:155 and logging-an-issue.md:47 describe a new tab; gettingstarted:155 names a "?" card button app.js lacks; presets.md:11 omits the plain click on an empty pad that the code and clip `10-control` use; integrating.md:190 (restore on reboot) vs backup-and-restore.md:29 (live); updating-firmware.md:24 vs building.md:171 on which variants carry MoonBase; installing-on-linux.md:186 vs building.md:134 on arm64 Docker.

**Surprises**: the scenarios are the most complete step lists for the network processes, richer than any doc; the installer flow is written four times and verified end to end by a clip and the smoke test only; the gallery is the one process whose code carries more steps than its doc, and it has no clip; home-automation.md spends 90 of 372 lines on Hyper-V and Zigbee.

## Part 2: contributor, product owner, agents and CI

Sources: CLAUDE.md, docs/contributing/*.md, building.md, logging-an-issue.md, MoonDeck.md, moondeck_config.json, the four workflows, .coderabbit.yaml, SECURITY.md, README.md, docs/work/*/index.md, testing.md, and the agent memory. Lines cite the files on disk on main.

### Script-to-process map (moondeck_config.json, 50 cards)

| Process | MoonDeck scripts it calls |
|---|---|
| Desktop build and run | build_desktop, run_desktop, show_crash_log, compile_tests |
| Commit gate | check_specs, check_prose, check_docgen, check_code, build_desktop, test_desktop, scenario_pipeline, check_esp32_built, collect_kpi (runs repo_health), build_docs, test_host, check_devices, check_firmwares, improv_smoke_test |
| Merge gate | the same over `git diff main...`, plus build_desktop --gcc --tests |
| Release | every check plus build_esp32 per shipped variant; release_version; wled_migration_test (MoonDeck.md:1284) |
| ESP32 bring-up | setup_esp_idf, clean_esp32, build_esp32, flash_esp32, erase_flash_esp32, monitor_esp32, improv_provision, improv_probe, serve_firmware |
| Teensy bring-up | setup_teensy, build_teensy, flash_teensy, monitor_teensy |
| Live verification | live_scenario, network_live_test, network_roundtrip, fseq_play, preview_health |
| Docs and help | build_docs, gen_help, screenshot_modules, install_playwright, history_report |
| Film a clip | ui_measure, ui_clip, ui_voice, ui_narrate, ui_project |
| On-demand analysis | check_module, check_clang_tidy, check_clang_query, check_nonblocking, check_codeql, check_footprint, bench_kernels, check_encodings, qemu_run, preview_installer, run_open_stage_control |

In MoonDeck.md with no card: check_prose, bench_kernels, gen_help, ui_measure, ui_voice, ui_narrate, release_version, serve_firmware. With a CI role and neither a card nor a section: check_sanitizers.py (test.yml:118), verify_version.py, package_desktop.py, compute_version.py, generate_manifest.py.

### The processes

#### Pick, spec and plan a topic
- Actor: product owner (pick, approve), Architect and Researcher (spec, plan)
- Goal: a module, effect, driver or capability has an approved plan before code.
- Steps: PO names one topic; Architect shapes a spec, Researcher maps prior art; Architect plans, PO approves; plan written to `docs/work/present/Plan-YYYYMMDD - title.md`; a restructure names 2 to 4 end states and builds the picked one; at the PR the plan becomes the description and the file is deleted in that PR.
- Written in: CLAUDE.md:100-129 (full, diagram); docs/work/present/index.md:1-8 (partial, the deletion rule); docs/work/future/index.md (partial, item leaves future to become a plan); docs/work/past/index.md:1-9 (partial, pre-rule archive); memory never-delete-a-plan-unasked, branches-bundle-topics (small additions skip the plan).
- Executed by: human and agent. Verified by: PO judgment only.
- Branches: small addition (no plan, memory only) vs restructure (2 to 4 end states); a draft waits in `future/`.
- Duplicates: 4 copies, agreeing.
- Gaps: present/index.md says "written in plan mode", a term CLAUDE.md never defines; 21 past plans map to PRs by title words, 4 unresolved.

#### Branch and work on it
- Actor: product owner (branches), agent (works)
- Goal: feature work lands on a PO-created branch, main stays releasable.
- Steps: PO creates the branch; agent works on whatever branch is given, asks when a change does not belong; one combined commit per cycle, a branch may bundle topics; a verified hotfix commits to main directly.
- Written in: CLAUDE.md:100-104 and 237 (full); memory branches-bundle-topics (same, as a correction record).
- Executed by: human. Verified by: nothing. Branches: hotfix to main vs feature branch.
- Duplicates: 2, agreeing.

#### Build, run and test on the desktop
- Actor: Developer, Tester
- Goal: prove a change on the host before any board build.
- Steps: build_desktop (zero warnings); run_desktop (kills the previous instance); build_desktop --tests then test_desktop; run_scenario for a pipeline; run_live_scenario --host localhost:8080 for timing; build_desktop --no-jit when MoonLive changed; build_desktop --gcc after a CI-only failure; hand to the PO and stop.
- Written in: CLAUDE.md:135-172 (full, diagram); building.md:28-36 (partial); MoonDeck.md:29-98, 763-774 (full per script); testing.md:491-535 (partial); coding-standards.md:155-168; 7 memory files (run-desktop-via-moondeck, desktop-binary-path, build-desktop-tests-flag-skips-the-app, stale-test-binary-two-paths, pipe-masks-exit-code, flaky-network-effect-tests, run-checks-through-moondeck).
- Executed by: the scripts above. Verified by: test_desktop, scenario contracts, sanitizer CI lanes.
- Branches: host build dir per OS; `--tests` builds tests only, bare builds the app only.
- Duplicates: 5 copies of the run-tests step. **Disagreement**: testing.md:14 and :501-502 say "Run via `ctest` or `./build/<host>/test/mm_tests`" while CLAUDE.md:168 says never run ctest when a script wraps it; testing.md:136 cites "CLAUDE.md Event 1, gate 10", a numbering that no longer exists.
- Gaps: `--no-jit` and `--gcc` have no card; the flaky-test list and the stale-binary traps live in memory only.

#### Build, flash and monitor an ESP32
- Actor: Developer, with PO approval per flash
- Goal: a bench board runs the change the desktop could not prove.
- Steps: clone ESP-IDF v6.1 at `~/esp/esp-idf`; setup_esp_idf once; build_esp32 --firmware X (three gate variants, one classic, one S3, one P4, sequentially); re-probe the port; flash_esp32 --firmware X --port P; monitor_esp32 --port P --firmware X; reproduce the symptom; hand the board to the PO.
- Written in: building.md:137-187 (full); MoonDeck.md:979-1137 (full per script); CLAUDE.md:63-75 (the approval rule); 11 memory files (dont-build-flash-between-steps, boards-are-free-test-rigs, esp32-gate-three-variants, local-esp32-gate-misses-variants, esp32-builds-share-managed-components, esp32-build-regenerates-catalog, s3-first-build-flash-test, bench-serial-ports, moondeck-json-device-registry, serial-open-resets-the-board, p4-usb-jtag-reset-via-esptool).
- Executed by: the ESP32 tab scripts. Verified by: check_esp32_built (freshness), the PO's eyes.
- Branches: S31 toolchain fetched separately (building.md:177); Windows `COM*` ports and driver install (building.md:191-230); sdkconfig, JIT or host-ISA change means build every shipping variant (memory).
- Touches: `System.deviceModel`, Firmware card `build` control for confirmation.
- Duplicates: 3 prose copies agree. **Disagreements**: building.md:141 pins commit `fff9895c`, release.yml:182 says "v6.1-rc1, commit 44f0c59f"; MoonDeck.md:1025-1031 lists four firmware keys, firmwares.json ships more. The "three variants, sequentially" gate rule exists in memory only; CLAUDE.md names check_esp32_built without a firmware.

#### Update a running board over the network
- Actor: Developer, end user (via the Firmware card)
- Goal: new firmware on a board with no cable.
- Steps: on a MoonBase variant, POST /api/firmware/moonbase, wait ~4 s, POST the app image to /api/firmware/upload with `--http1.1 -H "Expect:"`; expect no HTTP status; confirm via GET /api/modules/Firmware `build`; without MoonBase, upload directly. Or serve_firmware and paste the URL into the Firmware card.
- Written in: building.md:296-333 (full); MoonDeck.md:1099-1116 (serve_firmware); memory ota-a-4mb-board-over-the-network (adds step 0: install MoonBase from the app via /api/firmware/moonbase-update), stadbeest-eyes-no-usb, firmware-partition-value-is-image-size.
- Executed by: curl or the UI update button. Verified by: reading the build back.
- Branches: MoonBase variant vs direct; a partition-table change needs a cable.
- Touches: Firmware card `build`, `partition`.
- Duplicates: 2; the memory adds a step building.md lacks (installing MoonBase from the app).

#### Provision a freshly flashed board
- Actor: Developer (bench), the installer (end user)
- Goal: a flashed board joins WiFi, carries its name and its device model's module tree.
- Steps: improv_probe; improv_provision --port P --device-model M (TX-power cap, credentials, APPLY_OP state document); or improv_smoke_test (probe, provision, GET /); for a device AP phase run one script that joins, acts and rejoins home.
- Written in: MoonDeck.md:1174-1270 (full); CLAUDE.md:196 (the smoke test as a recommended gate); testing.md:253-270 (host_wifi scenario ops); memory board-provision-two-steps (says SET_DEVICE_MODEL sets the name only and the module tree needs a separate inject, which MoonDeck.md:1180 contradicts: the script now pushes the `state` document), device-ap-phase-runs-as-one-script.
- Executed by: improv_provision.py, improv_smoke_test.py, scenario_AccessPoint_first_setup_from_its_own_network. Verified by: smoke-test exit codes; the Improv frame tests.
- Branches: weak-powered model (cap 8 dBm); LED pins on the UART (one-way).
- Touches: `System.deviceModel`, `System.deviceName`, Network card.
- Gaps: memory board-provision-two-steps is out of date.

#### Build, flash and monitor a Teensy
- Steps: setup_teensy once; build_teensy; flash_teensy; monitor_teensy; network update via the Firmware card or curl.
- Written in: building.md:336-350 (full); MoonDeck.md:1321-1381 (full); test.yml:72-104 and release.yml:505-560 (CI builds every shipping Teensy firmware); memory teensy-sd-sdfat-lessons.
- Verified by: CI build artifact `firmware-teensy`. Duplicates: 2, agreeing.

#### Run MoonLight in Docker
- Steps: `docker compose up -d`, open :8081; or `docker run ghcr.io/moonmodules/moonlight:latest --no-browser`; upgrade with pull or build --pull.
- Written in: building.md:95-135 (full); release.yml:765-884 publish-container (the producer). Executed by: compose; Verified by: nothing.
- Gaps: building.md:134 says "amd64 only, the release ships no arm64 Linux binary" while release.yml:458-503 builds desktop-linux-arm64 and the container is multi-arch (release.yml:763). Out of date.

#### Run pre-commit, then commit
- Actor: product owner (says the words, stages, says "commit now"), agent (runs, reports), Reviewer on ~10 files
- Goal: a reviewed, measured commit.
- Steps: PO says "run pre-commit"; Reviewer first on a large diff; run the checks the diff triggers (always check_specs, check_prose; .md: build_docs --strict, check_docgen, test_host --python, check_taglines; src/test: check_code, build_desktop, test_desktop, run_scenario, check_docgen, check_esp32_built, --no-jit, collect_kpi; src/ui: test_host --js, check_devices; moondeck/moonlive: test_host --python, check_firmwares; provisioning path: improv_smoke_test); report PASS/FAIL/SKIP per line and stop; a failure is reported, never fixed-and-rerun; PO stages what they reviewed; PO says "commit now", void after any edit; commit message in the fixed shape with the collect_kpi one-liner and account.
- Written in: CLAUDE.md:178-243 (full); coding-standards.md:198-200 (pointer); MoonDeck.md per script; .coderabbit.yaml:1-12 (reviews on demand); 10 memory files (commit-now-is-void-after-any-edit, staged-means-po-reviewed, no-stash-ab-for-size-deltas, never-surface-the-gate-list, run-checks-through-moondeck, always-write-docgen-report, reports-show-the-real-situation, process-review-findings-without-asking, coderabbit-reviews-per-commit, gcc-build-before-pushing-a-new-test).
- Executed by: human plus scripts, check by check; no script runs the list (the event scripts were removed, memory run-checks-through-moondeck).
- Verified by: the checks; CI on push.
- Branches: trigger per path; "restart" for a full rerun; a fixed failure reruns one check.
- Duplicates: the list exists once (CLAUDE.md); five memory files restate the staging and "commit now" rules CLAUDE.md:225-237 already states.
- Disagreements: CLAUDE.md:191 lists check_prose under "always"; MoonDeck.md:159-161 says it "is not in the gate table until those are swept". `check_taglines` (CLAUDE.md:192) exists nowhere else. Memory says run `--gcc --tests` before pushing any new test, CLAUDE.md:149 says after a CI-only failure. Memory only: never `git stash`, the final number comes from the card's log, never `--noreport`.

#### Run pre-merge, then merge
- Steps: PO says "run pre-merge"; write the whole gate list out first, judgment gates included; Reviewer over the branch diff; the same checks over `git diff --name-only main...`; `build_desktop --gcc --tests` when CI failed on something clang built; judgment gates: review feedback addressed, docs in sync, PR title and description against `git log main..HEAD`, perf snapshot when the tick path changed, README when build/flash/first run changed; report every line; PO pushes and merges.
- Written in: CLAUDE.md:245-275 (full); memory pre-merge-judgment-gates-not-just-scripts (its countermeasure is CLAUDE.md:249 too), fork-pr-branch-push-target, pr137-after-pr136-merge (a one-off).
- Executed by: human and agent; CodeRabbit on a `@coderabbitai review` comment (.coderabbit.yaml:5-9).
- Verified by: CI on the PR (test.yml python, js, teensy, sanitizers; prose.yml; release.yml desktop jobs on main push only).
- Gaps: CLAUDE.md:247 says "external review runs on the PR" but auto-review is off, so someone must post the comment; nowhere says who. A fork PR's push target problem lives in memory only.

#### Contribute a pull request (external contributor)
- Steps: one topic, max 400 lines and ten files, tests included, generated files outside the count; based on main; a step of a larger topic may stack; findings processed on the branch.
- Written in: CLAUDE.md:131-133 (full); README.md:218-225 (mention: "the process is in CLAUDE.md"); docs/contributing/principles-and-process.md (embeds CLAUDE.md).
- Executed by: human. Verified by: CI, CodeRabbit, PO review.
- Gaps: **GitHub has no pull request template** and no CONTRIBUTING.md, so the 400/10 rule reaches a contributor only if they read CLAUDE.md. The embed page title says "Principles & process" with an ampersand the prose rules would flag.

#### Release
- Actor: product owner (tags), CI (builds and publishes)
- Goal: a tagged release with every binary, the installer manifests, the help and the docs site.
- Steps: "run pre-release" runs every commit and merge check over the tagged tree plus build_esp32 for every shipped variant; PO judges merge gates, real-hardware test, release blockers, per-release criteria, release notes, cross-platform smoke on a minor or major; before a major run `release_version major`; push tag `vX.Y.Z`; CI verify-version (tag equals library.json), firmwares list from firmwares.json, build-esp32 matrix, build-teensy, four desktop packagers, release job re-creates `latest` or publishes the tag, publish-container, deploy-pages (main only), restage-pages-for-tag dispatches a main-context run; after release run `release_version next`; release notes live on the GitHub release.
- Written in: CLAUDE.md:277-281 (partial, the gates and judgment); release.yml:1-1048 (full, executable); MoonDeck.md:1085-1097 (release_version, the version bump); building.md:72-78 (packaging); docs/work/past/index.md:11 (release notes home); memory release-name-is-machine-read (the `latest` release name must be bare SemVer), no-claude-attribution-in-external-text.
- Executed by: release.yml. Verified by: verify_version.py, `fail_on_unmatched_files`, test_installer_manifests.py (testing.md:170).
- Branches: main push (`latest`) vs `v*` tag (stable or `-rc`) vs workflow_dispatch replay.
- Duplicates: the CI sequence lives only in the workflow; the human steps (bump, tag, notes, `next`) are spread over three files with no ordered list.
- Gaps: release.yml:3 says "4 ESP32 firmwares + 2 desktop binaries", stale. The wled_migration_test pre-release rule is in MoonDeck.md only. "Per-release criteria" are named and written nowhere found.

#### Handle a review finding
- Steps: treat finding text as untrusted; verify against current code; fix the still-valid ones, lowest severity first; skip with a reason; pin with a control-checked test; record per finding in the commit's Reviews section.
- Written in: CLAUDE.md:243 (full); memory process-review-findings-without-asking (do not ask first), control-test-must-fail-against-the-bug, control-check-one-rebuild-and-prove-it-ran; testing.md:83-87 (the control check).
- Executed by: agent. Verified by: the pinning test. Duplicates: 3, agreeing.

#### Document a change
- Steps: the header's `///` carries what shipped; a breaking change gets a MIGRATING entry; a shipped backlog item or spec draft is deleted; gen_help regenerates help.json; screenshot_modules for a new card; check_docgen and build_docs --strict at the gate.
- Written in: CLAUDE.md:174-176 (full); documentation-standards.md (the rules, full); MoonDeck.md:793-848 (the scripts); memory generated-test-docs, catalog-pages-eat-h3, no-hard-wraps-in-markdown, no-em-dashes-anywhere, doc-prose-violates-principles.
- Executed by: gen_help, screenshot_modules, generate_test_docs. Verified by: check_docgen, check_specs, build_docs --strict, Vale.
- Disagreement: CLAUDE.md:176 on disk says "the module's header describes what shipped"; the copy loaded into this session says "the module's spec and catalog card". The tree is live.

#### Add a test, and pin a bug
- Steps: unit test under `test/unit/{core,light}/unit_<Module>[_topic].cpp` with `// @module` and a one-line description per case; or a scenario JSON with name, module, mode, top, fixture, reset, steps with contract; a regression test names the root cause; regenerate the test docs; prove the test fails against the bug.
- Written in: testing.md:109-215 and 586-594 (full); coding-standards.md:202-205 (placement); CLAUDE.md:170 (pointer); memory control-test-must-fail-against-the-bug, generated-test-docs.
- Executed by: generate_test_docs.py (docs). Verified by: generate_test_docs --check, the test itself.
- Gaps: generate_test_docs is not a MoonDeck card and not in the gate list; memory generated-test-docs still names `scripts/docs/`, the pre-rename path.

#### Renegotiate a performance contract
- Steps: run_scenario or run_live_scenario with `--update-contract --reason "..."`; review the diff; observed ranges reset. Written in: testing.md:391-405 (full). Executed by: the two runners. Verified by: the contract on the next run. Duplicates: 1.

#### Run a live verification on boards
- Steps: Live tab, pick the network, Discover, check devices, run live_scenario; two-board scenarios with --board; network_live_test, network_roundtrip, preview_health, fseq_play as diagnostics.
- Written in: testing.md:537-584 (full); MoonDeck.md:850-977 (full per script); CLAUDE.md:147 (run_live_scenario --host). Executed by: the scripts. Verified by: scenario contracts and `min_pct` bounds.
- Gaps: testing.md:575-584 "Hardware Verification" states results from one Olimex board as current facts, with no date.

#### Record a MoonTube clip
- Steps: ui_measure the captions; start what the run needs; ui_clip --run clip.json (with --keep for clips that leave their build standing); ui_voice; ui_project to cut; publish lands in docs/assets/moontube only on a clean take.
- Written in: MoonDeck.md:876-928 (full); testing.md:560-573 (the same runs as UI tests); memory moontube-keep-flag.
- Executed by: mtmeasure, mtvideo, mtvoiceover, mtcompose; test_host --ui runs the same files. Verified by: each step's `expect`; the leftover check.
- Gaps: `--keep` is documented in memory only. ui_measure, ui_voice and ui_narrate have no card though MoonDeck.md documents them as a three-step sequence.

#### Iterate a MoonLive script on a board
- Steps: POST /api/file to `/moonlive/<name>.mle`, read the module status for compile errors, leave the last upload working, copy into `moonlive/` when the PO says finished.
- Written in: memory moonlive-iterate-on-the-board and moonlive-two-script-dirs only. Executed by: curl. Verified by: the module status, test_scripts_are_cpp.py once in the tree. **No home in the repo.**

#### Log an issue, report a vulnerability
- Steps (issue): search existing issues; check the Firmware card version; steps, expected, actual; attach the `{ }` output of System, Firmware, the affected card, Layer or Drivers; a photo; a crashed device reports LEDs, what changed, repeat, serial log, `bootReason`.
- Steps (vulnerability): private report through GitHub advisories; fix and advisory published together.
- Written in: logging-an-issue.md:1-101 (full); README.md:218-225 (mention); SECURITY.md (full for vulnerabilities).
- Executed by: human. Verified by: nothing.
- Touches: the `{ }` link on every card, System `bootReason`, Firmware `version`.
- Gaps: **GitHub has no issue template**, so the checklist and the card list are reachable only through the docs page. Step 22 of the plan (an issue form generated from the process file) is the fix.

#### Bump the ESP-IDF pin, adopt a v6.x change
- Steps: bump PINNED_IDF_COMMIT and PINNED_IDF_VERSION in setup_esp_idf.py and the Docker tag plus cache key in release.yml; re-clone or check out; full ESP32 build and hardware re-test; each ecosystem adoption is its own commit with its own re-test, inside the v6.0 floor.
- Written in: building.md:232-276 (full); memory idf-env-stale-idfpath, idf-switch-recursive-submodules, sdkconfig-cache-silently-reverts.
- Executed by: human. Verified by: nothing scripted. Duplicates: 1. Gaps: release.yml:136 cache key `esp-idf-v6.1-...-v1` is a third place the pin lives.

#### Set up the editor, install Windows serial drivers
- Written in: building.md:58-70 and 191-230 (full). Human only, verified by nothing, one copy each.

#### CI on a pull request
- Steps: test.yml (python incl. the 34 scripts as C++, js, teensy build, three sanitizer lanes) on the paths it lists; prose.yml Vale over added lines, never failing; codeql.yml on main push, weekly and dispatch, never a gate; release.yml desktop and firmware jobs on main push only.
- Written in: the four workflows (full); testing.md:34-63 (partial); coding-standards.md:200 overstates ("CI runs the same checks on every PR": the C++ suite runs on a PR only under sanitizers, firmware builds on main push).
- Gaps: test.yml:3-4 says "main/next-iteration", the trigger lists main only; prose.yml and MoonDeck.md:159 both announce check_prose's future deletion.

### Summary

23 processes. Most duplicated: the desktop test step (5 copies, with testing.md still naming `ctest`), the pre-commit rules (CLAUDE.md plus five memory files restating staging and "commit now"), the ESP32 bring-up (building.md, MoonDeck.md, ten memory files), the plan lifecycle (4 copies, agreeing). No home in the repo: iterating a MoonLive script on a board, the three-variant sequential ESP32 gate, never `git stash`, `--keep` on clips, the fork-PR push target, the ordered human release sequence (bump, tag, notes, `next`), and who triggers CodeRabbit. No GitHub issue or pull request template exists, so the issue checklist and the 400-line PR rule are docs-only. Surprises: `check_taglines` is in the gate diagram and nowhere else; check_prose is "always" in CLAUDE.md and "not in the gate table" in MoonDeck.md; building.md still says the release ships no arm64 Linux binary while release.yml builds one and a multi-arch image; eight scripts MoonDeck.md documents have no card; the memory's "provision is two steps" note is contradicted by the script it describes.

## Part 3: the executable step lists

Source group: 32 scenario JSONs under `test/scenarios/{core,light,device}/`, 14 clip JSONs under `moontube/clips/`, their runners, and the UI test lane. Paths are repo-relative. Counts come from reading every file: 584 scenario steps, 274 clip steps.

### 1. The runner processes

#### Run a scenario in-process on the desktop
- Actor: test (the commit gate and `collect_kpi` run it)
- Goal: replay one scenario against a scheduler built inside the test binary and record what each step costs.
- Steps: refuse a stale `mm_scenarios` → read the JSON → honor `skip_on`, `mode`, `live_only` (SKIPPED=2, never 0) → create the `top` containers as main.cpp boots them → replay `fixture` → start the scheduler → replay `reset` → replay `steps` → a `measure` ticks for a wall-clock window and prints `MEASURE name: tick= heap= block=` → check `contract[<target>]` → the wrapper widens `observed[<target>]` in the JSON unless `--no-write`; `--update-contract --reason` rewrites the contract.
- Written in: `test/scenario_runner.cpp:1-129` (full), `moondeck/scenario/run_scenario.py:1-16,181-290` (full), `docs/reference/testing.md:282-470,510-535` (full), `moondeck/MoonDeck.md:763-774` (mention), `docs/reference/tests/scenario-tests.md:1-5` (generated, mention).
- Executed by: `uv run moondeck/scenario/run_scenario.py [--name|--module]`.
- Verified by: its exit code; the commit gate; `collect_kpi.py`.
- Branches: `construct` (in-process only) vs `mutate` (needs `fixture` or `top` in-process); `optional` honored on apply_state, set_control, measure only.
- Duplicates: op semantics written three times (runner header, testing.md, scenario-tests.md intro), in agreement; tolerance defaults declared twice by design (`KEEP IN SYNC`).
- Gaps: `skip_on`, `bounds` and `expected[<target>]` are documented but used by no scenario (grep: 0 files). `reboot` is a no-op in-process. An op the runner does not know passes silently (no `else` after `measure`, `scenario_runner.cpp:600-815`), where the live runner prints `SKIP (unknown op)`; the five access-point scenarios avoid that path only because they lack `top` or are `live_only`.

#### Run a scenario against a live device
- Actor: test (PO names the device; the MoonDeck Live tab wraps it)
- Goal: the same JSON over REST against a running desktop or board, leaving the device as found.
- Steps: skip `on_request` unless named, skip `construct`, skip unless every `boards` name has `--board NAME=HOST` → pre-flight every `set_control` id → with `host_network`, bind `{network.ssid}`, `{network.password}`, `{device}`, `{access_point}` from `moondeck.json` and register an atexit that puts the host's WiFi back → snapshot the containers → run `reset` then `steps`, each op a REST call (`PATCH /api/state`, `POST /api/control`, `/api/list/...`, `/api/file`, reboot) → `measure` reads `/api/system` after `--settle` → restore containers, controls and files (passwords excepted) → write `observed`, roll back contract edits on a failed run.
- Written in: `moondeck/scenario/run_live_scenario.py:1-6,640-820` (full), `docs/reference/testing.md:253-281,537-558` (full), `moondeck/MoonDeck.md:855-874` (partial).
- Executed by: `uv run moondeck/scenario/run_live_scenario.py --host H [--network N] [--board a=H2] [--name ...]`.
- Verified by: itself; clip `04-scenario-testing` films one run.
- Branches: desktop vs board; one board vs `boards`; home network vs the device's access point.
- Duplicates: MoonDeck.md:855 lists `--update-baseline`/`--compare-baseline` and testing.md:541-550 describes `min_pct` bounds and `scenario-baseline.json`; the step files carry `contract`/`observed` only. Two generations of one mechanism are described.
- Gaps: `run_network_live.py` and `run_network_roundtrip.py` are live processes outside the JSON vocabulary (testing.md:556: loops and per-round state), so one family has two shapes.

#### Perform a clip as a UI test
- Actor: test (on request only; never a gate)
- Goal: prove the UI does what every published clip shows.
- Steps: `test_host.py --ui` → `pytest moontube` → skip when nothing answers `/api/state` on `MOONLIGHT_HOST` or chromium is absent → per clip: resolve `requires` against `moondeck/moondeck.json` or `host` → `reset_device.py` sets boot state (Grid 16², one Layer with Pulse, FixtureProfiles + Preview, one AudioService, Party palette, consent off) → apply the clip's `setup` over REST → `mtrun.Driver.run_all` performs each `action` through the UI and checks each `expect` by reading `/api/state` → leftovers are reported, not deleted.
- Written in: `moontube/conftest.py` (full), `moontube/test_pipeline_run.py` (full), `moondeck/moontube/reset_device.py:1-60` (full), `moondeck/moontube/moontube.md:1-14,108-136` (full), `moondeck/MoonDeck.md:99-120` (partial), `docs/reference/testing.md:560-573` (partial), CLAUDE.md (mention).
- Executed by: `uv run moondeck/test/test_host.py --ui`.
- Verified by: `test_run_uses_known_actions`, `test_every_action_is_documented` (moontube.md vs `mtrun.ACTIONS`), `test_project_clips_exist`, `test_clips_are_runs_not_slide_scripts`.
- Branches: device UI (contract `data-module`, `data-mid`, `data-tab-mid`) vs another surface (`host`: installer :8421, MoonDeck :8420) by raw selector; `requires: audio` picks a bench board.
- Duplicates: the boot state is code in `reset_device.py` and prose in moontube.md:112; "REST reads, the UI writes" is written four times (mtrun.py, moontube.md:108, testing.md:571, MoonDeck.md:893).
- Gaps: 4 of 274 clip steps carry an `expect` (two each in `02-first-look-desktop` and `03-second-look`), so for 12 of 14 clips a pass means every action found its element, not that the device agrees.

#### Film a clip, and cut a series
- Actor: film (PO)
- Goal: a narrated `docs/assets/moontube/<clip>.webm`; a cut `media/video/<project>.mp4`.
- Steps: `mtmeasure.py` speaks each caption in the run's `voice` and writes `speech` into the step → `mtvideo.py` resets the device, applies `setup`, performs the steps while Playwright records, burns captions, writes `<clip>-raw.captions.json`, refuses a take with failures or leftovers, re-encodes at `speed`/`width` → `mtvoiceover.py` lays the voice over and reports overruns → `mtnarrate.py` renders a slide deck without a device → `mtcompose.py` concatenates clips in bars with music, overlay and inset.
- Written in: `moontube.md:138-207` (full), `MoonDeck.md:876-928` (full).
- Executed by: the five `mt*.py` scripts; MoonDeck cards `ui_measure`, `ui_clip`, `ui_voice`, `ui_narrate`, `ui_project`.
- Verified by: mtvideo's refusal rules; `test_project_clips_exist`.
- Gaps: `hero` is in `mtrun.ACTIONS` and moontube.md but no clip uses it. The two projects are edit lists; `full-series.json` carries a third copy of every clip's title ("Paint them" vs the clip's chapter "Layers" vs the doc heading "2. Paint something on it").

### 2. The scenarios, as processes

All are `mode: mutate` except `Effects_pipeline_builds_and_renders` (construct). 276 of 584 steps carry a `description`, printed by the generated `scenario-tests.md`; 308 are named only. Grouped by the process they perform.

#### A. A setting applies on the next frame and survives a restart (10)
- Files: core `System_identity_survives_reboot`, `Network_identity_and_discovery`, `MoonCloud_consent_is_off_until_asked`, `Services_audio_drives_the_effects`, `Firmware_reports_what_is_running`, `Control_presets_capture_and_restore`, `FileManager_writes_and_reads_back`; light `Drivers_output_and_brightness`, `Layouts_resize_reallocates_live`, `Palettes_a_script_drives_the_effects`.
- Actor: test. Goal: the control a user touches takes live and is still there after a power cycle.
- Steps (shared shape): `expect_control` the default → `set_control` → `expect_control` it took → (`measure`) → `reboot` → `expect_control` it survived → put it back (`reset` block or trailing set_control).
- Written in: each JSON's `description`; `testing.md:216-251` (full); the module specs (mention).
- Executed by: in-process and live; the reboot step does nothing in-process.
- Verified by: `expect_control` with `equals`, `not_equals`, `within`.
- Touches: `System.deviceName`; `Network.mDNS`, `Devices.addressing`; `Stats.consent`, `Talk.consent`; `Audio.mode/sampleRate/gain`; `Firmware.version`; `Control.fader1Target/fader1` → `Drivers.brightness`; `Drivers.brightness/on`; `Grid.width/height`; `Drivers.palette/paletteScript/shade` with a `write_file` of `/moonlive/scenario-palette.mlp`.
- Duplicates: the shape is hand-written ten times. Clips `02-first-look-desktop` 4-5 and `03-second-look` 18-19 (Grid 256² with `expect`) are the UI twin of `Layouts_resize`.
- Gaps: no scenario sets a value through the UI and no clip reboots, so "applies live" is proven over REST and "survives a reboot" only live, never in one process.

#### B. Build and mutate the light pipeline while it renders (6)
- Files: light `Effects_pipeline_builds_and_renders`, `Effects_swap_while_running`, `Effects_teardown_under_a_running_pipeline`, `Layers_stack_and_blend_live`, `Modifiers_reshape_the_mapping`; core `State_one_document_adds_an_effect_with_its_controls`.
- Actor: test. Goal: layout, layer, effect, modifiers and drivers added, swapped, stacked, blended, reshaped and removed, measured after each change.
- Steps (`Layers_stack`): clear Effects and Layouts → Grid 32² → Base + Plasma → measure → Top + Metaballs → measure → `blendMode` 1, `opacity` 160 → Sparks + Fireworks + Mirror → measure → swap Base to Nebula → grid 48² → WheelLayout → Top off → remove Sparks → measure.
- Written in: JSON descriptions; `testing.md:282-321`; `docs/tutorials/first-light-show.md:9-61` (the user's version, full); `docs/gettingstarted.md:216-240` (partial).
- Executed by: in-process (fixture builds the tree) and live.
- Verified by: `expect_control` on a control of the new module; `measure` deltas; `apply_state` with `error`/`at`; `round_trip_state`.
- Touches: `Layouts.Grid` (GridLayout, WheelLayout); `Effects.<Layer>` with Plasma, Metaballs, Fireworks, Nebula, Rainbow, Pulse, AudioSpectrum; Mirror, Multiply, Checkerboard modifiers; `Drivers.Net`; `Preview.targetFps`; `Services.Audio`.
- Duplicates: clips `05-layouts` and `06-layers` (BouncingBalls + Mirror, Layer2 Ripples + Circle, `opacity` 0.45) perform the same intent through the UI; `04-scenario-testing` films `Layers_stack` itself; `first-light-show.md` names the clip's cast, the scenario another: three casts for one story.
- Gaps: `Effects_swap` stops after the swap because the two tiers name the new slot differently, a seam in the shared vocabulary.

#### C. First setup, with the computer as the phone (5)
- Files: core `AccessPoint_first_setup_from_its_own_network`, `AccessPoint_protected_and_always_on`, `WiFi_known_networks_fall_back_in_order`, `Firmware_MoonBase_opens_the_devices_access_point`, `Firmware_MoonBase_joins_the_apps_network`.
- Actor: test standing in for the end user. Goal: a device with no network opens its access point, the user joins, sees the sign-in screen, picks a network, finds the device at home; MoonBase does the same.
- Steps (`first_setup`): `AccessPoint.opens` on failure → `list_row` delete the known network → `reboot` without waiting → `host_wifi` join the access point → `expect_http` the captive check redirects to `http://4.3.2.1/?open=WiFi` → `list_row` type the password into the scanned row → `list_row` press `connect` → rejoin → `WiFi.address` non-empty within 40 s → `host_wifi` home → `Network.mode` = `WiFi STA` → `expect_http {device}.local/api/system` 200 → network in `WiFi.known`.
- Written in: JSON step descriptions (full); `testing.md:253-270` (full); `docs/gettingstarted.md:81-105` (one paragraph: Skip, `MM-XXXX`, sign-in screen, 4.3.2.1; partial); `docs/how-to/updating-firmware.md:22-46` (MoonBase, partial).
- Executed by: live only, `on_request`, `--network`.
- Verified by: `expect_http` status, location, body regex; `expect_control within`.
- Branches: WiFi vs Ethernet (the doc has it, no scenario does); `opens` on failure vs always; app vs MoonBase.
- Touches: `AccessPoint.opens/clients`, `WiFi.known/available/address`, `Network.mode`, `/api/firmware/moonbase`, `/api/firmware/boot-app`, `/moonbase`.
- Duplicates: clip `01-install-esp32` 7-11 (SSID, password, Connect, `#done-url`) is the installer's route to the same state; the scenario is the device's own route; gettingstarted.md:81-105 describes both.
- Gaps: no clip films the access-point route; the Ethernet branch has no executable.

#### D. Presets capture a look and put it back (1 scenario + clip steps)
- Files: core `Control_a_palette_preset_changes_only_the_palette`; clip `10-control` 24-36.
- Steps: scenario: palette Rainbow, brightness 40 → `write_file` two presets under `/.config/presets/` → `list_row apply` the look → `list_row apply` the palette → palette Ocean, brightness unchanged, look still runs → both pads `active` → delete. Clip: replace Pulse with BouncingBalls → click an empty pad → name → save → replace with Ripples → second pad → press pad 1, pad 2.
- Written in: `docs/how-to/presets.md` (full), the scenario, the clip captions.
- Verified by: `list_row match {active: true}`; the clip has no `expect`.
- Touches: `Control.presets`, `Drivers.palette/brightness`, `/.config/presets/*.json`.
- Duplicates: three copies; the scenario stages presets as files, the clip saves through the pad, so neither covers the other's write path.

#### E. Measure what a configuration costs on a board (8)
- Files: device `perf_light`, `perf_full`, `peripheral_grid_sweep`, `peripheral_switch`, `GridLayout_resize`, `Aurora_fps`, `Fluid_solver`; core `Network_hardware_reconfigures_live`.
- Actor: test on a bench board. Goal: a per-step tick and heap series so a regression shows as a jump.
- Steps (shared): clear the containers → Grid 16² + Layer + one effect → measure → add one thing → measure → sweep the grid 16→32→64→128 → remove it → next thing → cleanup. `Network_hardware` cycles `Network.mDNS`, `Mqtt.haDiscovery`, `Ethernet.ethType` 1→3→2→0.
- Written in: JSON descriptions (full); `testing.md:323-405` (contracts, full); `docs/reference/performance.md` (mention).
- Executed by: live only (`live_only`); peripheral steps `optional`.
- Verified by: `contract[<target>]` on 2 of 32 files; otherwise observations only.
- Touches: `Grid.width/height/depth`, `Audio.enabled`, `RmtLedDriver.pins/ledsPerPin`, `ParallelLedDriver.pins/peripheral/doubleBuffer/pinExpander`, `Aurora.layers/octaves/warp/segments`, `Fluid.iterations/jets/persistence`.
- Duplicates: the grid sweep is written out 4× in `peripheral_grid_sweep` and 3× in `perf_full`; `perf_light` is a declared subset of `perf_full`.
- Gaps: `live_only` is marked legacy in the runner ("prefer mode=mutate") yet all eight carry it; the files run 1.4k to 4.7k lines because `observed` blocks dominate.

#### F. Lights over the network between boards (2 scenarios + 2 scripts)
- Files: device `NetworkReceive_two_boards` (`boards: [sender]`), `NetworkSend_one_sender_many_receivers` (`boards: [sender, receiver2]`); `moondeck/scenario/run_network_live.py`, `run_network_roundtrip.py`.
- Steps: sender: Grid + Rainbow + `NetworkSendDriver` with `hosts: "{host},{receiver2}"`; each receiver: Grid + `NetworkReceiveEffect`; per size and `serpentine`/`mapped_by` combination `measure` with `read: [sender:fps, NetworkReceive.tick, .packets_per_s, .frames_per_s]`; cycle `Send.protocol` and `addressing`. The scripts: every online board is sender in turn, three protocols, the listener's `/ws` preview must show the color; the roundtrip script times desktop → device → desktop.
- Written in: JSON (full), `testing.md:272-281` (full), `MoonDeck.md:930-951` (full for the scripts), `docs/how-to/multi-board-installation.md` (user, partial).
- Verified by: `read` rows only (no expect); the scripts assert color.
- Duplicates: clips `07-drivers-*` add a `NetworkSendDriver` and delete it unpointed.
- Gaps: the per-round loop is what the JSON cannot say, so this process has two homes and no single one.

### 3. The clips, as processes

All 14 are performed by `test_host --ui` and filmed by `mtvideo`. Mirrors are the pages embedding the clip (grep of `docs/**/*.md` for `moontube/`).

| Clip | Steps | Mirrors | Touches |
|---|---|---|---|
| `01-install-desktop` | goto installer → choose "This computer" → wait for the build name → Install | `gettingstarted.md:264-268`; `how-to/installing-to-desktop.md:18-70` (8 steps, SmartScreen and Defender, no clip); `README.md:163-170` | installer :8421 |
| `01-install-esp32` | goto with device+firmware → pick device → erase → Install → detected → writing → SSID, password → Connect → `#done-url` | `gettingstarted.md:17-105` (6 steps, 12 screenshots: a third copy); README (mention) | installer; family C is the device-side twin |
| `02-first-look-desktop` | 21 `open_card`, Grid 256² (`expect`), add NetworkSendDriver, press `Stats.consent`, `Talk.consent` | `tutorials/how-moonlight-works.md:12`, `gettingstarted.md:272` | every top-level card; `MoonCloud_consent` is the REST twin |
| `02-first-look-esp32` | the tour on a board, add `RmtLedDriver`, I2cScan, Pins, Firmware, MoonBase | `gettingstarted.md:108-215` (prose per card, partial) | `requires: audio`, setup Grid 64² |
| `03-second-look` | `System.mode` user/expert/developer → `{ }` link → `Layer.enabled` off/on → replace Pulse↔Ripples → delete Layer, add back → Grid 256² (`expect`) | `gettingstarted.md:149-155`, `how-moonlight-works.md:129` | `System.mode`, `Layer.enabled`, `Grid.width/height` |
| `04-scenario-testing` | `start_process run_live_scenario … Layers_stack_and_blend_live` → open cards while it runs → `wait_process` | `reference/testing.md:20` | the scenario |
| `05-layouts` | clear Layouts → Ring 240 → Spiral 900 → Cube 24³ → Tubes 24 → MoonLiveLayout `rose.mll`, `sixteen-rings.mll` | `first-light-show.md:9-20` (which asks for Grid 16², not these shapes) | `RingLayout.nrOfLEDs`, `SpiralLayout.ledCount`, `CubeLayout.width/height/depth`, `TubesLayout.nrOfTubes`, `MoonLiveLayout.script` |
| `06-layers` | clear Layer → BouncingBalls → Mirror → Layer2 → Ripples → Circle → drag `opacity` 0.45 | `first-light-show.md:22-49` (full) | `Layer.opacity` |
| `07-drivers-desktop` | add and delete Ndi, Rtsp, Hls, NetworkSend, PanelCard, Hue | DriverBase.h:20 `@video` (catalog); `how-to/watching-the-output.md`, `first-light-show.md:51-61` cover it | Drivers |
| `07-drivers-esp32` | Rmt `pins` 18, `count` 1 → 65535 (status warns) → `pins` "18,38" → ParallelLed → Hub75 → PanelCard → NetworkSend | DriverBase.h:21 `@video` (catalog); `how-to/led-drivers.md` covers it | `RmtLedDriver.pins/count`; `requires: audio` |
| `08-moonlive-effects` | clear Layer → MoonLiveEffect `nebula.mle` → delete → new effect → `new_script mle-demo.mle` → four `type_script` versions → `bands` 16 | `tutorials/first-script.md:31` (its prose writes a different script), `moonmodules/light/moonlive.md:8` | `MoonLiveEffect.script`, the script's `bands` |
| `09-services` | Button, Analog, Infrared (click `.list-add-btn`) → MoonLiveService `sweep.mls`, `button.mls`, `power.mls`, `chase.mls` → scroll to `Control.fader5` | Services.h:8 `@video` (catalog) | Services, `Control.fader5` |
| `10-control` | `switch1` off/on → drag `fader1` → `encoder1` 40 → assign fader2 → Pulse.bpm → OscService `listen` → `start_process send_osc.py /mm/fader/2` → presets (D) | `how-to/control-surface.md:8` (313 lines, 9 OSC steps, full) | `Control.switch1/fader1/encoder1/fader2`, `OscService.listen`; `Control_presets_capture` sets `fader1Target` over REST |
| `11-moondeck` | goto :8420 → click tabs and status dots for build, run, test, check, repo_health, flash, qemu, live_scenario, ui_clip | `how-to/building.md:5-27` | MoonDeck |

### 4. The two step vocabularies, side by side

Scenario `op` (584 uses; `scenario_runner.cpp:600-830`, `run_live_scenario.py:868-1290`) against clip `action` (274 uses; `mtrun.ACTIONS` at `mtrun.py:1669-1700`, table at `moontube.md:40-78`).

| Intent | Scenario op (uses) | Clip action (uses) | Note |
|---|---|---|---|
| Set a control | `set_control` 209 (`id`, `key`, `value`, `measure`, `optional`, `device`) | `set_number` 14, `set_text` 3, `drag_slider` 4, `choose` 3, `click_control` 6, `pick_file` 7 (`module`, `control`, `value`) | same intent; the clip splits by widget; `id`/`key` vs `module`/`control` |
| Add a module | `apply_state` 127, a typed member | `add_module` 32 (`parent`, `type`, `as`) | the scenario names the module; the device names it in the clip, hence `as` |
| Replace a module | `apply_state`, new `type` under the same name | `replace_module` 4 | same |
| Remove a module | `apply_state` with `null` | `delete_module` 17 | same |
| Empty a container | `apply_state` `{"$patch":"replace"}` | `clear_children` 7 | same |
| Expect a value | `expect_control` 71 (`equals`, `not_equals`, `within`) | `expect` block on any step (4 uses) | a step vs an attribute |
| Expect an HTTP answer | `expect_http` 16 (`status`, `location`, `body_matches`, `body_lacks`, `resolve`) | none | live only |
| Expect a page state | none | `wait_for` 5 (`selector`, `text`, `timeout`) | the clip's expectation off the module tree |
| Measure | `measure` 117, or `measure: true`; `read` on boards | none | |
| A list row | `list_row` 12 (`add`, `match`, `field`, `apply`, `delete`, `to`) | `click` on `.list-add-btn`, `.list-pad*`, `type_into .list-field-input` (10 of 37 `click`s) | same intent; the clip has no list action, so it drops to raw selectors |
| Press a route or button | `expect_http` `method: POST`; `list_row field: connect` | `click_control` on a button control; `click` | |
| Files | `write_file` 4, `delete_file` 3, `expect_file` 2 | `new_script` 1, `type_script` 4 | editor vs `fsWriteAtomic` |
| Restart | `reboot` 13 (`wait`, `timeout`) | none | no-op in-process |
| Move the host's radio | `host_wifi` 9 | none | |
| Whole-state round trip | `round_trip_state` 1 | none | |
| Open a card | implicit (ids are global) | `open_card` 88 | 32 % of clip steps are navigation the scenario never needs |
| Narration, framing | `description` on 276 of 584 steps | `chapter` 14, `caption` on every step, `wait` 1, `follow_link` 1, `scroll_to` 4, `hero` 0 | |
| Reach outside | `device` on a step (another board) | `start_process` 3, `wait_process` 1, `goto` 3, `choose_option` 3, `type_into` 4 (another surface) | |
| Precondition | `fixture` (in-process), `reset` (both), `top` | `setup` (REST before the camera), `reset_device.py` | same intent, three names |
| Skip rules | `skip_on` (unused), `live_only`, `on_request`, `boards`, `optional` | `requires`, `host` | |

Both vocabularies address a step's target as module + control (the scenario spells it `id` + `key`), the key `help.json` already uses.

### Summary

**Processes**: 4 runner processes, 6 scenario families over 32 scenarios, 14 clips, 2 projects: 26 recorded above.

**Most duplicated**: install on an ESP32 (gettingstarted.md's 6 steps and 12 screenshots, the clip, README, the installer's own flow, family C for the device side: 5 copies); build a light show (first-light-show.md, gettingstarted.md:216, clips 05 and 06, family B, clip 04 filming one of them: 5); presets (how-to, scenario, clip: 3); "REST reads, the UI writes" (4 prose copies); the op semantics (3 prose copies, in agreement); clip titles (chapter step, project file, doc heading: 3, all different).

**No home**: the multi-board network matrix (two scripts outside the JSON vocabulary); the Ethernet branch of first setup (prose only); the desktop install's SmartScreen and Defender steps (most of the how-to, no executable); the access-point sign-in route (scenario only, no clip, one doc paragraph); clips `09-services` and both `07-drivers` reach the catalog pages only, no how-to page.

**Surprises**: 4 of 274 clip steps check anything, so the UI lane mostly proves elements exist. Three scenario features (`skip_on`, `bounds`, `expected`) and one clip action (`hero`) are documented and used by nothing. The in-process runner accepts an unknown op silently. `live_only` is marked legacy yet all eight device scenarios carry it. A contract exists on 2 of 32 scenarios; the rest carry observations with no promise. The two vocabularies map one-to-one on intent (set, add, replace, delete, clear, expect, precondition) under different names for each, and what one cannot say (`expect_http`, `reboot`, `measure` vs `open_card`, `wait_for`, `start_process`) is exactly the other's surface.

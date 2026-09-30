# Plan: MoonLight, from v5.0.0 to the rename

projectMM becomes MoonLight. **v5.0.0 is the last release under the old name and v6.0.0 is the first under the new one.** This file is the whole record: what ships before the switch, what happens at it, what follows, and the decisions already taken along the way. It replaces the five files that held pieces of it.

## The three decisions that shape everything

**Migration from the predecessor is finished.** No further effects, layouts or modifiers are ported from the old MoonLight. What exists today is what ships, and the gap tables in the old plans are closed rather than outstanding. New effects are written on merit from here, not to match a list.

**A user's configuration survives both releases.** v5.0.0 upgrades in place, subject only to the breaks [MIGRATING](../../reference/MIGRATING.md) already records. v6.0.0 carries configuration across too, by Backup on v5 and Restore on v6, because **nothing persisted carries the product name**: config files are named after module types (`Effects.json`, `Drivers.json`) and the sweep leaves every type and `namespace mm::` untouched. The migration engine in [migrate.js](../../../src/ui/migrate.js) therefore has no rename to apply for the rename itself, which is the easiest case it can be handed.

**No compatibility code ships with MoonLight**, which is [the standing rule](../../../CLAUDE.md#principles) applied to the rename: nothing translates a projectMM identity into a MoonLight one, no alias for a renamed key, no shim reading a predecessor's file, no branch asking which name a device was flashed under.

**Backup on v5, Restore on v6 is the one supported path**, and every upgrade question is answered with it. Where something does not carry, the answer is to erase the flash and install clean. A Home Assistant entity re-appearing under a new identity is the same trade: the alternative is a permanent pin to the old name, and a new product does not inherit one.

**Live interoperation survives**, which the rehearsal got wrong: a peer is classified by the numeric marker `0x014d4d00` rather than by any name, so a projectMM device and a MoonLight device still see each other. What goes stale is a saved device list, whose rows re-type themselves on the next discovery sweep.

## Where we stand

Verified against the tree on 2026-09-29 rather than read from the plans, because several of their status lines had gone stale.

| | Count | Note |
|---|---|---|
| Effects | 68 headers, ~64 registered | Migration complete by decision |
| Modifiers | 12 | All 9 predecessor ones plus three of ours |
| Layouts | 18, 17 registered | Three install-specific ones absent, and staying so |
| MoonLive | 33 `.mle` scripts, 5 `.mlp` palettes | Palettes shipped |
| HUB75 | Ships | `Hub75Driver` is registered |
| Fidelity | 4 of 6 settled | Two open, both bench questions rather than code |

## What the rename touches

The sweep script measures **1405 occurrences across 242 tracked files** (dry run, 2026-09-22), against the 542 across 113 recorded when it was written: the documentation sweep and the effect library both grew the prose that names the product. The categories still matter more than the count, and the growth is spread across the tree rather than concentrated, so it is real rather than an exclude-list gap.

`rename_to_moonlight.py` (deleted once the sweep had run) handles almost all of it and runs dry by default. It replaces two tokens, `ProjectMM` then `projectMM`, which is correct for every form because `projectMM` is never a substring of another token. Its file list comes from `git ls-files`, so build output is excluded without a blocklist. `MoonLive`, the predecessor's own name, and `namespace mm` are provably never touched.

### The device builds its own OTA URL, and that turns out to be safe

[MqttModule.cpp:324](../../../src/core/system/MqttModule.cpp) formats `github.com/MoonModules/projectMM/releases/download/v<version>/firmware-<name>-v<version>.bin` in firmware, so a device flashed today asks the old repository for its updates forever. Three things make that work anyway, and all three were verified in the code rather than assumed:

- GitHub issues a permanent redirect for a transferred repository, for the API and for release assets.
- The OTA client follows redirects deliberately: [platform_esp32_ota.cpp:117](../../../src/platform/esp32/platform_esp32_ota.cpp) sets `disable_auto_redirect = false` with a redirect count of 10, and raises the header buffer specifically because GitHub's asset redirect overflows the default. It was hardened for this shape of URL already.
- The asset filename is `firmware-<variant>-v<version>.bin`, which carries no product name and so does not change at the rename.

**A v5 device therefore finds and installs v6 with no code change.** Only recreating `MoonModules/projectMM` would break the redirect, and the name sits inside an organisation we control, so nothing to do beyond leaving it alone.

This was worth checking rather than believing: reading the URL construction alone suggests a hard break, and only the fetch path shows there is none.

### What a clean break still costs

Two things the sweep changes that a user feels, neither needing code:

- **The sACN source name** at [E131Packet.h:55](../../../src/light/util/E131Packet.h) is a fixed nine-byte literal a receiving console displays, so it changes with the product rather than staying. Peer discovery does not: it classifies on the numeric marker and already reads `"MoonLight"`, so a mixed network keeps working.
- **The `MM-` device prefix** in [SystemModule.h:57](../../../src/core/system/SystemModule.h) is every device's mDNS name and Home Assistant entity id. **It stays** (decided 2026-09-29): `MM` is MoonModules, the organisation does not change at the rename, so the prefix keeps naming the thing it always named. Changing it to `ML-` would rename every mDNS name and entity id, which Restore does not carry back, for a prefix that was never wrong.

## The cutover

Dates are the product owner's. The two fixed points are Windows day and release day, and the rest hangs off them.

### Sept 22: v5.0.0 shipped

The release an in-field device updates *from*, and the only one that carries the installed base across the move. Nine firmware variants, four desktop packages, the container image and the installer manifest, all built from `21319a09`. An S3 on that commit reports `5.0.0-dev`, boots clean and renders at 373 fps.

Small by design: its value is a known-good, widely-installed baseline for the rename to be measured against. Two questions stay open under the old name, both bench work rather than code: the audio `volume` scale (0..1 float against our 0..255 `level`), and a cross-check of effects whose predecessor source was incomplete.

The eight days that follow are what the gap exists for.

### Sept 22: the sweep rehearsed

The script ran with `--apply` on a throwaway branch and the gate set ran over the result. Four things it got wrong, each silent rather than a build error, which is why rehearsing was worth a session:

1. **`kFallbackRepo` was rewritten to the successor**, leaving both OTA constants naming one repository and the fallback dead. A device that cannot reach the new name would have had nowhere left to look. **Marked `rename-keep`.**
2. **`projectMM-moonbase` was renamed in the image check**, so new firmware would reject the recovery image already in a v5 device's flash, leaving it with no recovery path. **Protected by content**, and it changes only when a MoonBase built under the new name ships.
3. **A historical MIGRATING entry became false.** The v5.0.0 heading reads "the last release under the projectMM name", and the sweep rewrote it into a claim that was never true. **Marked `rename-keep`.** Every entry describing what already shipped has the same hazard.
4. **`TextEffect`'s golden frame failed.** The default text is the product name, so renaming it changes rendered pixels. This one SHOULD rename, and its golden moves in the same commit, which is what [golden_frame.h](../../../test/unit/light/golden_frame.h) already asks for.

The script now honors a `rename-keep` marker on a line, so an exception lives beside the thing it protects rather than in a list that drifts. The desktop build, 1998 of 1999 tests and the docs build all passed on the swept tree, so nothing else structural is hiding.

Also measured: the sweep is **1405 hits across 242 files**, of which about 789 are documentation prose and code comments. Doing those early was considered and rejected: it leaves switch day's dangerous 616 unchanged, and finding 3 shows that even prose is not uniformly safe.

### Sept 23: a default worth a first impression, and the install path end to end

Thursday's work happened on Wednesday, and it changed what the first minute of the film shows.

**The boot default is now [Pulse](../../moonmodules/light/effects.md#pulse).** A device came up on Noise, which is dense: it proves the lights work and hides everything else. A first boot has to answer three questions at once, and the third one is new, since a board with a microphone that shows no reaction to sound reads as a board without one. Pulse is expanding shells from a drifting origin, sparse enough that a single beat is unmistakable, moving on an idle clock when the room is silent, and the same code on a strip, a panel and a volume because a shell is a distance and every layout has distances. It costs 250 to 280 us at 16x16 on an S3.

**The device models no longer pin an effect.** Four of them added their own under the Layer, so the firmware default was invisible on exactly the boards the film uses. All five entries are gone, and every model now boots on whatever the firmware chose. That is also one rule instead of four, in the spirit of the catalog describing hardware rather than taste.

**Two recorder defects, each a silently wrong take rather than an error.** `wait_for` read only the present, so a state shorter than the gap between two steps was missed: the S3's erase lasts about twelve seconds and the step waiting for it starts later than that, which failed a run that had in fact gone perfectly. It now records what a watched element showed and counts a state that already passed. And `type_into` typed on top of a field rather than into it, so the installer's prefilled SSID provisioned a device for `MoonModulesMoonModules`, which joins nothing. Both are pinned by tests, and the second is a defect for anyone re-installing rather than only for the camera.

**The clips are numbered in the order the work happens**, `01-install` through `98-react-to-sound`, so the directory reads as the path a newcomer takes. Both the install and the tour exist twice, once per platform, and each pair shares its number because it is one beat on two machines. Installing splits because the routes share no step, one flashing a chip and the other downloading an app. The tour splits because the trees differ: a board drives LED pins and hears a real microphone, where a computer previews and sends over the network. Each tour opens every module and every tab inside it. `02-first-look-esp32` is embedded where Chapter 2 of [getting started](../../gettingstarted.md) begins, since Chapter 1 flashed a board and the tour should be of that board. It is the recording from v5.0.0 for now, and re-records against the current firmware in the next pass.

Both clips were recorded against a real erase-and-flash of the S3, ending on a provisioned device at `192.168.1.158` with its microphone tracking music in the room.

**What this does to the week.** Thursday's run-file work is largely done, so Thursday absorbs what the script asks for rather than starting from nothing. The two recorder fixes make Friday's filming cheaper, since a take no longer fails on a state that went by too quickly. One thing moved the other way: the clips were recorded before the script is final, so they are rehearsal footage by the plan's own rule, and Saturday re-records whatever the script changes. That was always the shape; it just started a day early.

### Sept 24: the rename starts landing, and a blanket replace proves dangerous

Cutover day went from 1376 lines to 1206, in two passes. The first moved 99, the second the free renames in `src/` and `test/`, and the second found three breaks the rehearsal had called safe.

More useful than the count is what moving them taught, because every one of these was a thing the rehearsal had reported as safe.

**A hand-counted length survives a rename by luck.** MQTT sized its topic buffer as `9 + 1 + 6 + 1`, counted from `projectMM`. MoonLight is nine characters too, so the sweep would have passed and any other name would have truncated every topic silently. The length now derives from the string with `sizeof`, which is the general form: a literal's length belongs to the literal, never to a comment that counts it.

**A round trip has as many ends as it has, and the tests know.** The device-type label looked like a pair, one plugin writing it and one comparison reading it. It was three: `devTypeStr` emits the string that gets persisted. Renaming two of the three broke four tests, which is the guardrail working exactly as intended. Assume a third end exists until the suite says otherwise.

**Discovery was already rename-proof, and the plan said otherwise.** A peer is classified by the numeric marker `0x014d4d00`, not by any name, so a renamed device and an old one still recognise each other on the wire. The paragraph claiming live interoperation breaks at v6.0.0 was wrong about the mechanism. What actually goes stale is a persisted device list, which self-heals on the next discovery sweep.

**A comment saying a line is fixed does not stop a sweep.** The MoonCloud salt carried "changing this re-identifies every installation in the world exactly once, so it is fixed" and would have been rewritten anyway. It now carries a `rename-keep` marker, which the script honours mechanically. A rule worth stating is worth stating where the tool reads it.

**The sweep broke the one supported upgrade path.** Restore compares a bundle's `format` against a literal carrying the product name, so a swept reader rejects every backup a user already saved, with "not a config backup". The reader now accepts both spellings behind a `rename-keep` marker while the writer emits the new one. This is the case the whole migration promise rests on, and a blanket token replace inverted it.

**The sweep orphaned every desktop user's configuration.** The desktop data directory is built from the product name, so a renamed build reads an empty profile and loses presets, layouts and scripts, silently and with no error. Devices have Backup and Restore; the desktop had nothing, and the plan had not noticed because its migration section is written entirely about devices. Resolved by letting the directory take the new name and documenting the move in MIGRATING: principle 3 weighs what a user loses, which here is copying one folder across or a Backup and Restore.

**The sweep blinded the prose checker.** `.vale.ini` names its style and vocabulary by directory, so renaming the references while the directories kept the old name left Vale reading no rules at all and reporting zero findings for every header. The directories moved with the config, and the proof is Vale reporting `.cpp` findings again, which is the control the file's own comment asks for.

**A blanket replace edits quotations.** The product owner's own words inside a block quote were rewritten, turning "projectMM V1, V2 and V3" into a sentence they never wrote. A quote is evidence rather than prose, so the sweep has no business inside one.

**What genuinely cannot move early**, checked rather than assumed: the OTA project guard, where `moonbase/CMakeLists.txt` stamps the image and `FirmwareImage.h` checks that exact string, so moving either early makes every v5 device refuse the new MoonBase image. And the repository URLs, which resolve only once the repo itself is renamed.

Three more joined that list once the free renames were taken. The **CMake project name** stamps the ESP-IDF descriptor `kProjectImageName` is compared against, so the two flip in one commit or every firmware is refused. The **desktop asset filenames** are parsed by firmware already in the field, which makes the packager, the release workflow, the install picker and their test fixtures a single lockstep set. And the **Home Assistant domain** in the installer manifest names an integration that has to exist before it is offered.

The rest is 1206 lines, almost all of it documentation prose and the handful of identities above.

**The method that found all of this** is worth repeating on whatever is left: take the sweep's own file list, read every hit in the top files rather than trusting the count, and ask of each whether anything outside this repository keys on the string. Almost all of them were comments.

### The rename lands in batches, not one sweep

A branch over roughly 100 files loses its external review layer, and the free renames alone reach 200. So the sweep runs in passes, each its own commit with the whole gate set behind it, rather than as one change nobody can read.

The order is by risk, cheapest first. `src/` and `test/` went first, because the compiler and 2000 tests judge them: a mistake there fails rather than ships. Documentation prose is next and carries no executable risk. The identities that outside systems key on go last, on the day, and they are now a short enough list to read in one sitting.

What makes this safe is that the sweep is idempotent and its own report is committed, so a batch can be regenerated at any time and the remaining reach was always visible in the report beside the script, both deleted once the sweep had run.

### The week, day by day

**The script is the deliverable.** It says what MoonLight is, in the order a newcomer needs it, and everything else is a rendering of it: the run files perform it, `mtvideo.py` films it, the tutorials follow the same sequence in prose. Written once, so a change lands in all three. [The scenario](#the-introduction-a-draft-scenario) below is the draft to work from.

**Every shot is a script, so filming is repeatable.** A run file names the steps and their captions, and `mtvideo.py` performs them against a live device. Re-shooting is re-running, which is what makes the schedule below possible: the takes are cheap and the thinking is not. Footage is ready by Sept 30.

Two rules hold all week. **Anything found before Tuesday is fixed under the old name**, since a defect discovered after the rename is a defect in two releases. And **the published cut is filmed after the switch**: the UI carries the product name in its page title and header, so an earlier take says MoonLight in the pixels. Earlier takes still earn their place as rehearsal, because re-running is cheap.

**✅ Sept 22 and 23: the script.** Thinking, and it decides everything after it.

- Write what MoonLight is, in the order a newcomer meets it.
- Name each beat, what it shows, and what it says.
- Check each beat against what exists today, so the script is shootable now.
- Done when the scenario below is revised into the one you want.

**✅ Thu 24: the scenario suite, and the rename brought forward.** Hands on, and it went somewhere the plan had not put it.

- Rebuilt the scenario suite: 27 archived, 11 written, one per top-level card plus the reboot-persistence one.
- Closed the hole that made them look green: a skip returned the pass code, so ten of eleven asserted nothing while the gate said `11 passed`. A skip is now counted as a skip, and a scenario that asserts nothing fails.
- Swept the free renames in `src/` and `test/`, taking cutover day from 1277 lines to 1206. Documentation prose is the next batch and the largest.
- The fourteen run files in `moontube/clips/` are numbered and current; matching them to the script is Friday's work, alongside the first cut.

**✅ Fri 25 to Mon 28: the cut, and what filming exposed.** The footage came, and so did a week of defects the camera found.

- Every clip recorded and narrated, and the numbered series settled at seventeen: fourteen clips and three slide decks, about forty-eight minutes.
- Filming is a test pass. It found a scripted `circle` that hung the render thread at the 16-bit edge, a black preview behind a TLS proxy, three hot-path checks that reported numbers nobody could act on, and three UI controls the recorder could not drive at all.
- An ESP32 is a **device**, not a board, through the installer and the scripts: a board is a bare PCB and a device is that board in an enclosure with whatever is wired to it.

**✅ Tue 29: MoonTube, and the recorder's silent failure.** Not on the plan, and it had to come first.

- The series is a product with a home: `moontube/` at the root rather than under `test/`, since a run file is the source of a published video as much as it is a test.
- **The recorder had been broken silently.** It resolved cards and nav entries by a test id this interface has never carried, so every `open_card` on a top-level module failed. `test_host --ui` is never a gate, so nothing ran often enough to notice. Two one-line fixes took the desktop suite from broken to nine of nine.
- Four more defects the recording found: a five-second REST timeout that suits the desktop app and strands a real device, `clear_children` that never checked its own claim, preset pads that accumulated across every take, and nine clips published silent because nothing said a voiceover pass remained.
- All seventeen videos re-recorded and voiced against the fixes.

**✅ Tue 29: [Windows day](#sept-29-windows-day), in parallel.** Ran on the Windows machine and merged in as PR #117, the first time anything had been tested on that platform. The MoonTube work above ran on the Mac the same day.

**🚧 Wed 30: [the switch](#sept-30-moonlight-v600), then the final take.** The rename lands, and the footage is re-recorded against it the same day.

- **The scenarios run on Windows, in parallel, and merge before the move.** That machine is the only platform with no scenario column in the matrix, and the in-process tier has never run there at all: the UTF-8 preamble that makes it possible was added this morning, after the Windows day had already shipped its own four sites. Two things to read in what comes back. Whether the in-process tier runs clean, since a fifth encoding site would surface as a scenario failure rather than as an encoding one. And whether the 245 ms, 564 ms and 458 ms tick averages from the 29th reproduce: the observation block now carries `max` and the raw samples, so a single blocked tick inside a good second is finally distinguishable from a uniformly slow one, which is what that entry asked for and could not answer.

- Re-run every run file with `mtvideo.py` against a v6.0.0 device, then `mtvoiceover.py` over each: a clip is three passes and the middle one publishes silent.
- Re-cut with `mtcompose.py`, which consumes the same project file.
- Done when the published cut says MoonLight in every frame.

**🚧 Thu 1 Oct: publish.** The announcement, the video, and the tutorials together.

**Testing rides along.** Every run file is a UI test, so Thursday and Friday exercise the newcomer's path harder than a test pass would, and the boards get theirs on Monday's slack. What that covers is in [the two threads](#the-two-threads-behind-the-week).

### Sept 29: Windows day

Windows works, and has been tested here by hand before. What it does not have is anything that notices when it stops working: [release.yml:318](../../../.github/workflows/release.yml) is the only Windows runner in the repository and it compiles and packages without invoking `mm_tests`, the scenarios, or anything else. So between hand sessions the platform drifts unobserved, and the gap since the last one is about three weeks. That is what this day is for: not proving Windows works, but catching what has rotted since anyone last looked.

Four features are a genuinely different program on Windows rather than a thin shim, and they come first:

1. **Raw Ethernet output** (the L2 panel driver). Loads Npcap's `wpcap.dll` at runtime and batches through `pcap_sendqueue`, where POSIX opens a raw socket and sends per packet. Needs Npcap installed. The struct layouts must match the installed Npcap exactly.
2. **The Ethernet interface picker.** Enumerates through `GetIfTable2` and matches GUIDs against pcap names. A Hyper-V switch or a VPN adapter can empty the list or report another adapter's link speed.
3. **RTSP and HLS video out.** `CreateProcessA` takes a hand-built command string where POSIX passes an argv array, and the stop path is `TerminateProcess` plus `CancelIoEx` rather than a signal and a pipe EOF. Both were written this week and neither has run. Test that the stream plays, and that changing the layout while it plays does not hang the app.
4. **NDI.** Resolves `Processing.NDI.Lib.x64.dll` off the PATH the NDI installer sets, so a missing runtime reports "not installed" rather than failing loudly.

Then the JIT, which fails as a crash or as wrong pixels rather than an error:

5. **MoonLive scripts.** The x86-64 backend emits for the **Win64 ABI**, a different register assignment and a 32-byte shadow space, with its own hand-assembled blobs and patch offsets. Open a script and watch it render. Executable memory comes from `VirtualAlloc` with `PAGE_EXECUTE_READWRITE`, which antivirus or a hardened policy can refuse outright, taking all scripting with it.

Then the things a user meets on day one:

6. **The installer.** Install, launch from the Start menu, upgrade over a running instance (NSIS runs `taskkill` first), and uninstall.
7. **Saving configuration.** Windows gets a plain `fopen` with no owner-only ACL, and `std::filesystem::rename` over an open file fails where POSIX replaces it, so a save can fail silently. Save a preset twice.
8. **Port binding.** `SO_REUSEADDR` is deliberately omitted because on Windows it means "steal the port", so a second instance behaves the opposite way from macOS. Start two and bind DDP twice.
9. **Audio input.** miniaudio switches to WASAPI, so the device list, the default entry and whether loopback works are all Windows-specific.
10. **Serial ports and flashing.** The dropdown reads `SERIALCOMM` from the registry, and [_idf_win_shim.py](../../../moondeck/build/_idf_win_shim.py) forces a UTF-8 locale because idf.py refuses to start under cp1252, which only a non-English Windows reproduces.
11. **The web installer.** Its documented DTR/RTS reset bug is worse on Windows 11, and the Windows-only hint rows are user-agent gated, so confirm they appear.

Smaller checks to make while the above runs: the browser opens on first launch, HTTPS reaches the cloud through WinHTTP and the Windows certificate store, and the crash log's timestamp is not garbled, since `localtime_s` takes its arguments in the opposite order to `localtime_r`.

**Anything found here is fixed before the rename**, not after: a Windows defect discovered in v6.0.0 costs a patch release under a brand-new name.

**✅ What the day found, on both machines at once.** Windows and macOS ran in parallel and met in a merge, so the record below is one day's work from two benches.

On Windows, the rot was in the tooling rather than in the firmware. A path compared with `str()` instead of `as_posix()` meant the sdkconfig path never matched on Windows, so **every ESP32 build was judged stale**: fourteen minutes where an incremental build wanted seconds. Redirected stdout takes cp1252, and four scripts died on a tick mark in their own output, one of them reporting a scenario the device had PASSED as failed. Repo-health recorded a single flat `desktop` number, so whoever ran the gate last silently overwrote the other machine's figures; the desktop metrics are now keyed per host the way scenario observations always were, and both machines show side by side. Firmware capacity was read from whatever sdkconfig sat in a local build dir rather than from the registry, which had `esp32` reporting 111% of a slot it fits inside.

On macOS, the day went to the boards. **ESP-IDF moved from the v6.1 release candidate to v6.1 final**, and CI moved with it: the workflow still named the candidate, so every shipped binary would have been built against an IDF no bench run had used. All four variants were rebuilt, flashed, and swept: **28 scenario runs across a classic, an S3, an S31 and a P4, all passing**. Two Ethernet defaults only a real board could expose came out of that. A chip offering exactly one preset opened on Custom rather than on its own board, and the preset's pin map never reached the fields on a virgin board at all, so a freshly erased P4 selected P4-NANO and still booted with its interface at none.

**The P4's WiFi co-processor is measured rather than described now.** Over Ethernet the WiFi-capable build costs 51 ms per request against the Ethernet-only build's 14, and over WiFi the tail reaches 870 ms: usable on a wired link, unreliable on a wireless one. Three explanations died on the bench that day, and the measurement that survives was taken in August and says the render task burns 2.6x the CPU while the SDIO tasks sit idle, which is memory contention rather than anything a scheduler can reach. The eth-only build is what to run when a P4 has to be dependable, and both variants still ship.

**One gap closed the next morning**, before it could cost anything: `run_scenario.py` never got the UTF-8 preamble its sibling did, and it relays output from a binary that prints arrows. The in-process tier had only ever run on macOS, so nothing had caught it. A Windows run would have read the encoding failure as a scenario failure.

### Sept 30: MoonLight v6.0.0

One repository transfer, one sweep commit, one release. The installer manifest, the release asset names and the in-firmware URL builder are a lockstep set, so a half-applied rename leaves devices unable to update. `namespace mm::` stays throughout: it is not the product name, and renaming it would touch every file for nothing.

**The name is occupied, so the transfer is two moves in one moment.** `MoonModules/MoonLight` is the predecessor, a fork serving twelve releases, and GitHub refuses a rename onto a name that exists. So it vacates to `ewowi/MoonLight` and `projectMM` renames into the space it leaves, back to back. Between those two clicks `MoonModules/MoonLight` does not exist, and a device asking there falls through to `MoonModules/projectMM`, which redirects throughout: the gap is safe because the firmware already names both.

**✅ Done on 2026-09-29, so the day starts here rather than at step 1**: the readiness gate reads Ready at 464 lines re-baselined, the `MM-` prefix is decided (it stays), and two backups are on disk.
Step 3 is done on macOS: both benches ran their gates, the device sweep passed 28 of 28 across four chips, and the two branches met in a merge that builds and tests clean. The Windows half runs on that machine and merges before the move.
The two backups are `MoonLight-config-MM-testbench-P4-2026-09-30.json`, a device with configuration only, and `MoonLight-config-MM-S31-2026-09-30.json`, which carries 23 files including 14 MoonLive scripts and is the one that exercises the renamed-and-mapped restore path.

#### Before the moment

1. ✅ **`uv run moondeck/repo_rename/check_rename_ready.py`** prints Ready, at 467 lines against the rehearsed 464. It dry-runs the sweep and asserts the reach is as rehearsed, every `rename-keep` line survives, and the OTA still names two different repositories. The three new hits are prose naming the old repository, which is what the sweep is for, and all six protected lines survive. A drift that fails the gate is a prompt to read the new hits rather than to re-baseline past them.
2. ✅ **Both backup bundles open** and name the device expected. They are the evidence for the migration claim and cannot be taken once the repository has moved.
   They are BROWSER downloads: `src/ui/app.js` builds the name and hands it to `a[download]`, so each sits in the download folder of whichever machine had the interface open, rather than anywhere the device or a script wrote.
   ✅ Both open and name their device. `MoonLight-config-MM-testbench-P4-2026-09-30.json` is the configuration-only half, six files with the firmware and build stamped in. `MoonLight-config-MM-S31-2026-09-30.json` is the rich one, 23 files: nine of configuration and fourteen MoonLive scripts.
   Two things the S31 bundle does NOT carry, both worth knowing before it is the evidence for anything. Its scripts are the FACTORY catalog under `/.moonlive`, since that device's own `/moonlive` was empty, so a restore proves the path rather than a user's work surviving it. And it holds no preset, so the preset half of the restore claim rests on a device that has one.
   Its `firmware` field is empty where the P4's names a variant. Restore reads `version` alone, so the bundle restores regardless: the gap is in the record, not in the file.
3. ✅ **The full gate set on main is green** (2026-09-30): 2016 unit tests, 294 Python, 170 JS, 21 scenarios, and all eight checks. The sweep lands on a tree that was already good, so a failure after it belongs to the sweep.

Between step 3 and step 4 sits the ordinary rhythm: whatever is in flight is committed on a branch and merged, so main carries no uncommitted work when the repository moves. The Windows scenario run merges here too. A sweep diff that also contains a day's edits is two changes wearing one commit, and the sweep is the one that has to be readable.

#### The moment

4. ✅ **`MoonModules/MoonLight` moved to `ewowi/MoonLight`**, freeing the name and leaving a redirect behind it.
5. ✅ **`MoonModules/projectMM` renamed to `MoonModules/MoonLight`**, immediately after, which overwrites that redirect as intended: the name is ours and the predecessor keeps its content under `ewowi`.
   The release workflow was mid-run when step 4 finished and was allowed to complete first, since it publishes assets into the repository the rename was about to move. It also built all twelve ESP32 variants against IDF v6.1 for the first time in CI, which was the last untested consequence of the pin move.
6. ✅ **The old documentation path 404'd, exactly as this step warned.** Two stub pages now serve it from `MoonModules/moonmodules.github.io`, which is the repository owning the domain: `docs/projectMM/install/index.html` and `docs/projectMM/index.html`, each a meta refresh with a canonical link and visible text for whatever ignores the refresh. Pages serves static files and has no rewrite rules, so a meta refresh is the mechanism available. Both verified live at 200.
   **The domain repository stays where it is.** It carries the `CNAME` that makes `moonmodules.org` resolve at all, so moving it would take down the MoonLight site along with the path this step fixed. The 404 was a path inside a working site rather than a question of ownership.
   Still to do there, and not urgent: `products/projectmm.md` sits in that site's navigation and names the old product. It is content rather than a broken link, so it can wait for the announcement.

#### After the moment

7. ✅ **`uv run moondeck/repo_rename/rename_to_moonlight.py --apply` ran** on `rename-sweep`, off the renamed repository: 468 hits across 116 files, and the diff was read in full. The four the rehearsal found all hold: `kFallbackRepo` still names the old repository, the MoonBase image check still reads `projectMM-moonbase`, MIGRATING's v5.0.0 heading still says projectMM, and `TextEffect` already carried its new default text and golden from an earlier batch.
   **The sweep found a fifth, and it was the worst of them.** It flipped `kProjectImageName`, the string a RUNNING v5 device compares an incoming image's ESP-IDF descriptor against, so every v5 device would have refused the v6 image before writing a byte. The constant and `project()` in `esp32/CMakeLists.txt` keep the old name behind `rename-keep`, and the Home Assistant card title that had borrowed the constant reads the MQTT root `kPrefixRoot` instead, which is the product name already.
   **The sweep rewrites contents and cannot rename files**, so every reader it flipped pointed at a file still carrying the old name: the three Windows installer scripts, which broke the package build, two images, the friend-repos page and the first tutorial. All seven moved with `git mv`.
   **It lands as two commits in one PR**, code first and prose second, since the tree is 140 files and the external review declines past 100.
8. ✅ **The identity set is flipped in the same change**: binary name, release asset names, the manifest `name` and `home_assistant_domain`, and the docs domain. The `MM-` prefix stays, decided on 2026-09-29.
   **The set was larger than this step listed**, and the ruling on the day was that everything takes the new name unless a shipped device compares the old one. So the desktop data directories, the installer's saved device list, the Debian package, the container image and its compose names, the macOS bundle identifier, the backup bookmarklet's format and the host override variable all moved, each with its [MIGRATING](../../reference/MIGRATING.md) entry where a user feels it.
   **What keeps the old name, and why**: the OTA image name and the MoonBase image name, which a v5 device checks; `kFallbackRepo`, which has to differ from the primary; the MoonCloud salt; and the second format the Restore reader accepts, which is what the two backups from step 2 carry. The product owner wants these gone in time, so each is a candidate once no v5 device is left to upgrade.
   **The rename tooling is gone.** `moondeck/repo_rename/` held the sweep, its readiness check and their reports, and a tool that has run once and cannot run again is weight. The `rename-keep` markers went with it, leaving each kept string its plain reason. The scripts reading an ESP32 build take the image's filename from three constants in `build_esp32.py`, so the old spelling has one home there instead of twelve.
   **The ESP32 build artifact keeps the old name with them.** ESP-IDF names `projectMM.bin` and `projectMM.elf` after `project()`, so every script reading the build output reads that name: the release workflow, the manifest generator, the merged-image builder, flash, monitor, QEMU and the checks. The sweep flipped those readers and left the writer, which would have failed the release job on its first firmware and shipped a manifest with no app part. Nobody outside the build sees the name, since a release asset is `firmware-<variant>-v<version>.bin`.
   **When the image names flip: v6.0.0 is the bridge, decided on 2026-09-30.** The gate is one-sided: a device refuses any image whose name differs from its own. So the flip takes two releases. First a bridge, whose gate accepts both `projectMM` and `MoonLight` while its own image still says `projectMM`. Then a release whose `project()` says `MoonLight`, at which point the artifact name and every reader flip in the same commit. A device has to pass through the bridge, and a v5 device updating straight to the second release is refused. The cheapest bridge is v6.0.0 itself, as one extra compare in `platform_esp32_ota.cpp`, because every release shipped with the single-name gate adds devices that need a stepping stone later. That is compatibility code, which the third decision at the top rules out, and the product owner took the exception: v6.0.0 accepts both names, so the release after it can ship under the new one. `kAppImageNames` and `kMoonBaseImageNames` in `FirmwareImage.h` hold each pair, and every install path asks `isAppImage` or `isMoonBaseImage` where it had compared a literal.
   **`projectMM-moonbase` follows the same two steps, and is slower to finish.** Five places compare it, doing two jobs. The app's OTA and upload paths and MoonBase's own install path refuse an image by that name as an app, because a recovery image in the app slot leaves both partitions holding it and only a cable recovers that. And `moonBaseRejection` admits only that name into the factory slot. Flipped in one step, a v5 app refuses the renamed MoonBase safely, but a v5 MoonBase offered it as an app no longer recognises it, so its guard stays silent and it writes the recovery image over the app. The bridge therefore accepts both names in all five places before any image carries the new one. OTA replaces only the app, so a device upgraded over the air keeps its v5 MoonBase, and that exposure ends per device only when its MoonBase is reinstalled.
   - The documentation path follows the repository name on its own: GitHub Pages serves a project site under `/<repo>/` even on the custom domain, so the sweep updates `site_url`, `repo_url` and `site_name` to match. Check the web installer at its new path first, since it is the link a newcomer follows.
9. 🚧 **Run the full gate set again** on the swept tree, then tag and release v6.0.0. The release preparation rides in the rename PR: `library.json` says `6.0.0`, MIGRATING's entries sit under `## v6.0.0`, and the compose example pins `6.0.0`.
   - ✅ **The gate set passed on the branch and on main.** PR #119 merged as `f9832755`. The Release workflow's first run on the renamed tree published `latest` as `6.0.0-dev.35`, 55 assets under the new names: `MoonLight-*` archives for macOS, Windows and Linux on x64 and arm64, `moonlight_*.deb`, and firmware for 13 variants.
   - ✅ **The live scenarios passed on the testbench S3**, on the 6.0.0 build: 20 of 21, the last being the one that only runs in-process.
     It found a fault in the runner rather than in the firmware. Two scenarios restarting a board back to back failed the second one, because the restart was judged by whether the new uptime was lower than the old, and a board read seconds after its previous restart has already passed that figure by the time it answers. The runner now compares uptime against the wall clock.
   - ✅ **The NanoPi runs the new container and passed the scenarios**, 20 of 21, recorded as `desktop-docker-arm64`. It was updated by pulling `ghcr.io/moonmodules/moonlight:latest` and starting it as `moonlight` on a fresh `moonlight-data` volume, with the old container stopped and its volume kept.
     The first attempt recorded the NanoPi as `desktop-macos`, into the Mac's own figures, and started nine copies of the desktop binary on the Mac: the runner named a desktop build after the machine running the runner, and restarted one by launching a local binary. It now names a desktop build on another host after the platform that build reports, and relaunches only a local one. The Mac's figures were restored from history.
   - ✅ **The bridge and the download stall timeout are built and verified on hardware.** Both install loops, the app's and MoonBase's, abort once no byte has arrived for a minute, since ESP-IDF reports a read timeout as still in progress and a silent connection otherwise holds an install for good. 2020 unit tests pass.
     The timeout: a server that sent 400,000 bytes and went silent left the S3 reporting "the download stalled" about a minute later and still running, and left the classic board's MoonBase reporting "download failed, retrying", which is its own retry taking over.
     The bridge: the S3 was offered its own image under three names. `Stranger` and `MoonLight-moonbase` were refused by name, and `MoonLight` was accepted and flashed in full, failing only the final checksum that editing the name by hand breaks. An image actually built under the new name is the next release's test.
   - 🚧 **Tag and release v6.0.0**, from main once the `next-iteration` branch carrying the bridge, the timeout and the recordings has merged and its Release run is green.
   - **How the NanoPi step was planned**, kept for the reasoning. It is the one Linux arm64 machine on the bench and runs the container, on `4.0.0-dev.134` from 13 September. The image matching this tree exists only once main has built it, as `ghcr.io/moonmodules/moonlight:latest`, so the order is: merge, let CI publish `latest`, update the NanoPi under the new image, service and volume names, then run the live scenarios against `192.168.1.156:8080`. Pulling it under the new name is itself the test of the container entry in MIGRATING. The recorded observations land in a commit of their own, as the Windows run did, and before the tag, so an arm64 failure is fixed in v6.0.0 rather than in a v6.0.1. The machine jumps two major versions, so every MIGRATING entry for v5.0.0 and v6.0.0 applies to its configuration at once.
10. 🚧 **Verify the two claims**: a v5.0.0 device finds and installs v6.0.0 over OTA, and the rich backup restores onto it with layouts, effects, presets and scripts intact. The firmware asset name carries no product name (`firmware-<variant>-v<version>.bin`), so a v5 device's URL survives the rename unchanged.
    - ✅ **The first claim was rehearsed before the merge, on 2026-09-30**, because after the release a failure cannot be rolled back. MM-testbench-S3 (`esp32s3-n16r8`) was sent to the published v5.0.0 over OTA, by the asset URL under the old repository name, and reported `5.0.0`, build `21319a09`. It then installed this branch's build from `serve_firmware.py` and came back on `6.0.0`, with the same 27 modules in the same order and only runtime readings differing. So the image-name gate in a real v5.0.0 accepts the v6 image, and the redirect from the old repository name serves a device.
    - ✅ **The MoonBase path passed too**, on MM-Olimex (classic `esp32`): from the published v5.0.0 it staged the URL, MoonBase installed the CI-built `6.0.0-dev.35` from GitHub, and the app came up on it.
      Three earlier attempts on that board failed or stalled, and the same image then installed seven times running. The board was on WiFi with its Ethernet unplugged, its log shows the access point going missing at boot, and all three errors were network ones, so the link is the likely cause rather than the image. The stall is what led to the timeout in step 9.
    - 🚧 Still open: a device finding v6.0.0 by itself, which needs the release to exist, and the restore of a backup.
    - **Three backups from 2026-09-30 are the restore test**, each imported through Restore on a v6.0.0 device and checked for what arrived. They sit in the download folder of the Mac that took them.
      - `projectMM-config-MM-NanoPi-2026-09-30.json`: 8 files, from the container on `4.0.0-dev.134`. It carries the old format name, so it is the one that proves the second entry in `kBackupFormats`, and it crosses two major versions, so every mapped rename in `migrate.js` for v5.0.0 and v6.0.0 applies to it.
      - `MoonLight-config-MM-S31-2026-09-30.json`: 23 files, nine of configuration and fourteen MoonLive scripts, which makes it the one that shows scripts surviving.
      - `MoonLight-config-MM-testbench-P4-2026-09-30.json`: 6 files of configuration only.
      - The two board bundles come from a development build of 29 September, after the backup writer had taken the new format name. So none of the three was taken on the published v5.0.0 itself, and a bundle from a device on that release is still worth taking before the claim is called proven.
11. 🚧 **Hand-edit `moondeck/moondeck.json`**, which is gitignored and outside the sweep. ✅ It carries no mention of the old name, so the rename leaves it alone. 🚧 The local checkouts below are still to do.
    Each local checkout wants the same treatment, and none of it is urgent because GitHub redirects the old remote: `git remote set-url origin https://github.com/MoonModules/MoonLight.git` so the address is named rather than inferred, then rename the working folder, whose path now says the old name. The predecessor's own checkout sits beside it under the name the project is taking, so that one is renamed out of the way first and repointed at `ewowi` or retired, depending on whether anything uncommitted in it still matters. Contributor forks keep their own names, which are their owners' to change.
12. 🚧 **Re-record the clips the rename is visible in**, which is every clip showing the interface, since the title bar carries the product name. Each is a three-pass run: `mtmeasure.py` sizes the holds from the spoken length, `mtvideo.py` records, `mtvoiceover.py` speaks the captions. Recording alone is what published nine silent clips before.
    - ✅ **Each clip gets its own voice, so the series sounds like the team that made it.** The mechanism, the assignment and a sample line per voice are built; the clips themselves still carry their old narration until they are re-recorded. Planned as its own branch after the rename merged, since it is about twenty files and the rename is already past the review limit.
      - **The voice lives in the run file.** A clip's JSON names its `voice`, and `mtmeasure`, `mtvoiceover` and `mtnarrate` read it, with `--voice` as an override and Alba as the fallback. Before this the voice was a command-line flag on each pass, so nothing kept the measure pass and the voice pass on the same one, and a clip measured with one voice and spoken with another drifts.
      - **Luna is Alba**, and the three slide decks stay hers by carrying no `voice` at all.
      - **The fourteen clips alternate between a male and a female voice**, no two neighbours the same. The Piper models that exist at medium quality: `alan` and `northern_english_male` on one side, `jenny_dioco`, `cori` and Prudence from `semaine` on the other. `semaine` also holds Spike, Obadiah and Poppy, and `aru` holds twelve speakers, so the third male voice and any extras are picked by ear when it is built. `southern_english_female` is left out: it exists only at low quality.
      - A changed voice needs all three passes again, which this step runs anyway.
    - ✅ **The intro ends on an overview of the clips to come**, placed before the closing slide so the intro still ends on Luna's sign-off; its narration is a draft for the product owner to put in their own words. One more slide in `00-intro`, listing the sixteen that follow. The list is already written down once, as the clip titles in `moontube/projects/full-series.json`, so the slide reads it from there.
    - Two clips carry edits made since their last take and want re-recording whatever the rename did: `07-drivers-desktop` names moving heads and par cans on the network-sender step, and `07-drivers-esp32` describes a single parallel lane rather than two.
13. 🚧 **Render the full series** with `mtcompose.py --project moontube/projects/full-series.json`. ✅ The corner window carries the caption "made with MoonLight", set per project as `inset.caption`. The corner window cycles whatever sits in `media/examplevideos/`, which now carries ten effect clips beside the nineteen rough cuts, so the finished film shows the effects as well as the interface driving them.
14. 🚧 **Publish to YouTube**, one video with the seventeen clips as chapters. The description names the new repository, the documentation site at its new path, and the installer URL, all of which only exist after step 5. Publishing before the rename would hand every viewer a link that dies within the hour.

If step 10 fails, the release stays and the fix is a v6.0.1: the repository has already moved by then, so rolling back is not on the table. That is why the checks and the backups come first.

#### What still says projectMM, and whether it will change

Counted on 2026-09-30 over the tracked tree, in either letter case: 57 lines in 15 files, down from 122 in 36 before the rename tooling was deleted and the image filenames got one home.

| What | Where | Lines | Will it be renamed? |
|---|---|---|---|
| The app image name | `project()` in `esp32/CMakeLists.txt`, the first of `kAppImageNames` in `FirmwareImage.h`, four lines of `unit_FirmwareImage.cpp` | 6 | **Yes**, in the release after v6.0.0, which is the bridge |
| The app image filenames | `APP_BIN` and `APP_ELF` in `build_esp32.py`, one `cp` in `release.yml`, the manifest test's fixture, one sentence in `MoonDeck.md` | 5 | **Yes**, in the same commit as the app image name, since ESP-IDF derives them from it |
| The MoonBase image name | `project()` in `moonbase/CMakeLists.txt`, the first of `kMoonBaseImageNames`, six lines of `unit_FirmwareImage.cpp`, `MOONBASE_BIN`, one glob in `release.yml`, one line in `MoonDeck.md` | 11 | **Yes**, by the same path, once a MoonBase under the new name ships |
| `kFallbackRepo` | `FirmwareUpdateModule.h` | 1 | **No.** It names the old repository on purpose, and it is removed rather than renamed when the fallback goes |
| The second backup format | `kBackupFormats` in `src/ui/app.js` | 1 | **No.** It is removed once no v5 backup is left to restore |
| The MoonCloud salt | `MoonCloudModule.h` | 1 | **No, never.** A new salt re-identifies every installation |
| What each break replaced | `MIGRATING.md` | 8 | **No.** Naming the old value is what the entry is for |
| History | This plan, and one line in each of two older plans | 24 | **No.** `docs/work` records what was |

The first three rows are 22 lines that the release after v6.0.0 renames. Everything below them either goes away on its own schedule or stays for good.

### After: the week following

Watch for what only real users hit: OTA from versions older than v5.0.0, the documentation redirect, and a mixed network where someone has not updated both devices.

## The spoken introduction

What is said to camera before the clips run, in the product owner's own words and voice. The clips that follow are the proof of what it claims, which is why it comes first and why every claim in it is checked against the repository rather than remembered.

> Hi! Welcome to a new MoonModules video. It's been a while. About a year ago I made a number of MoonLight videos. But since then MoonLight has had a complete makeover!
>
> You could say "I made a thing", but did I? Because I literally did not write one line of code, or one word of documentation.
> It is all done by AI agents! From the ground up: code, documentation, test scripts, build scripts, gifs, screenshots and even videos!
>
> It's not that I did nothing. I wrote prompts! And lots of them, as AI agents have a mind of their own, drift a lot, and tend to forget things. This resulted in 3 failed attempts before the new MoonLight is something I (think I) have under control:
> - Building the right guard rails is the key: unit tests, scenario tests, live scenarios
> - Claude.md containing the principles and processes: pre-commit/merge/release gates
> - Hook in the human:
>     - every change checked!!
>     - Triggering any GitHub action (commit, push, merge)
> - So I won't call this vibe coding
>
> So why did I do this? The old MoonLight was not perfect but it worked and was highly tuned. So why give up on all of this?
> The reason is simple: because AI agents offer a revolutionary new paradigm and although I have a lot of worries about AI in its current context, it is not going away, so as an IT guy talking about AI already back in the 80s, I cannot pretend it is not there or that it will blow over.
>
> I did give up on MoonLight code, but I did not give up on the MoonLight principles! They are a few years old, formulated when I was working on WLED and WLED-MM, first tried in StarLight, then MoonLight, then projectMM V1, V2 and V3 and now the new MoonLight, and the principles were extended over time:
> - 3D from the ground up
> - Everything is a module, this was inspired by WLED usermods, now a MoonModule
> - UI is derived from the MoonModule, not written for each
> - Layers
> - Hottest hot path: shortcut the pipeline when possible: one layer, no modifiers, identity grid, default color ordering, full brightness
> - Fastest pipeline: effects are producers, drivers are consumers, working in parallel, using multiple cores and offloading CPU using DMA where possible
>     - In optimal cases 2 cores are not even needed as the DMA runs in parallel
> - Unbreakable
>     - not enough memory: step down
>     - Any changes made will be checked
> - Never reboot: Any change to pins, to LED drivers etc will work immediately
>
> And one more "principle" needs special attention: No libraries! When setting up MoonLight it became clear that libraries could not provide the level of test-guarding MoonLight needs to keep agentic coding in control. Also libraries do not "exactly" do what you want: they do more, using more code, and they do less than what you need. So you are depending on their willingness to implement our needs and on their release schedules. Plus it turned out that the principles and architecture we set up make it "damn easy" to write our own code, back to back, fewer lines of code, doing exactly what we need.
>     - No libraries except Espressif's own. Not just lighting libraries: no async web server either, we wrote that too. The only exceptions are four components from the chip vendor (mDNS, LittleFS, Improv and one Ethernet PHY driver), which is the chip's own plumbing rather than someone else's idea of how anything should work.
>
> And this brings me to the final point before I will show some MoonLight: making everything ourselves, do we steal code? This is a big debate, using AI agents especially. I personally worked on and with different systems and libraries which I included in the past (WLED(-MM), FastLED, Asynchronous Web Server, Clockless LED Drivers, Live Scripts, ...) and now don't need any more. MoonLight has a few principles to deal with this:
> - Use industry standard algorithms, naming, spec sheets etc. Explicitly don't use existing code!
> - Use attribution where we are inspired by others
> - Steal ideas, not code: what travels is the approach, the technique someone proved works on real hardware, the mistake worth not repeating
> - Transform rather than imitate: full testability and live reconfiguration force a different shape, so an imitation could not have satisfied them anyway
>
> So this is where we stand. Time to show some MoonLight. The coming clips show MoonLight running.
> After that I will come back telling how to get started, how to get involved and a glimpse of the future.
> And yes, I did not make these clips, my team did ;-)
> Enjoy.

### What the introduction claims, and where it is true

Checked against the tree rather than taken on trust, because a spoken claim is the one nobody can grep.

| Claim | Where it holds |
|---|---|
| Unit tests, scenario tests, live scenarios | 207 unit-test files, 11 scenarios plus 27 archived, [run_live_scenario.py](../../../moondeck/scenario/run_live_scenario.py) |
| Pre-commit, merge and release gates | [CLAUDE.md § The Process](../../../CLAUDE.md), which names all three |
| 3D from the ground up | 27 effects declare `Dim::D3` |
| Everything is a MoonModule | [MoonModule.h](../../../src/core/module/MoonModule.h), one lifecycle for every part |
| UI derived from the module | `writeControlMetadata` builds each control's widget from its declaration |
| Not enough memory: step down | [Layer.h](../../../src/light/layers/Layer.h) reduces the buffer and says so rather than failing |
| Never reboot | [live reconfiguration](../../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame) |
| Attribution where inspired | 67 origin lines in the [effects catalog](../../moonmodules/light/effects.md) |
| No libraries | True of every library but Espressif's own four (mDNS, LittleFS, Improv, one PHY driver). The HTTP server is ours, on our own `TcpConnection`, so the no-async-web-server claim holds too |

## The introduction: a draft scenario

A first draft to argue with. Nine beats, each naming an existing run file where one fits, and each carrying the one sentence it says. The shape is a promise, then proof, then an invitation: show what it does before explaining how, and leave the viewer able to start.

**Length:** about three minutes. Long enough to earn the last beat, short enough to watch twice.

### 1. The wall, alone

`01-show-the-preview` · 8 seconds · no words yet

Lights moving, filling the frame. No UI, no cursor, nothing to read. The viewer decides in this shot whether to keep watching.

> One ESP32. Thousands of lights, and every change on the next frame.

### 2. What you are looking at

`04-change-layout` · 15 seconds

The grid resizes and the preview reshapes with it. The point is that a layout is a description of where lights are, rather than a mode the effect had to be written for.

> Tell it where your lights are. Everything after that is the same, whether it is a strip, a panel or a cube.

### 3. An effect, and its controls

`05-add-an-effect` · 25 seconds

Add one, then drive its controls and watch the wall answer. Every control applies on the next frame, which is the thing to see rather than say.

> Sixty-one effects. Every control live, with nothing to recompile and nothing to reboot.

### 4. Layers and modifiers

`07-add-a-layer`, `06-add-a-modifier` · 35 seconds

A second layer blending over the first, then a modifier folding the result. Where the product stops being a list of effects and starts being a pipeline.

> Stack them. Mirror them. The pipeline is yours, and the lights follow it live.

### 5. It hears the room

`98-react-to-sound` · 20 seconds

A microphone on the board, an effect following the music. Audio is the feature people arrive wanting.

> A microphone, or your desktop's own audio. The show follows the music.

### 6. Write your own

`09-write-an-effect` · 30 seconds

Type an effect in the browser, save, and the lights change. MoonLive compiled on the device, which is the part nobody expects.

> Write an effect in the browser. It compiles on the device and runs at native speed.

### 7. Out of the device

**New run file.** 20 seconds

The wall leaving as video: NDI into OBS, or a player opening the RTSP stream. The beat that says this belongs in a real production.

> Send the wall out as video, into OBS, Resolume, or any player.

### 8. Real hardware

**New run file**, or footage of a panel · 20 seconds

A HUB75 panel lit from the board's own pins, or the installer flashing a board. Proof that this drives things rather than simulating them.

> Strips, panels, moving heads, DMX. Driven from the board itself.

### 9. Start in two minutes

`02-install-firmware` · 20 seconds

The web installer: pick a port, a release, a board, flash. Ending on the action the viewer can take.

> Open the installer, flash your board, and you are running.

### What the draft leaves out, deliberately

MoonBase, backup and restore, MoonCloud, control surfaces, the driver catalog, and the architecture. Each is real and none is a first impression: a newcomer wants to know what it does and whether they can start. The tutorials carry the rest, in the same order.

### Decided

- **The beats keep their order**, audio at 5 and scripting at 6. Scripting is the more surprising claim, so it closes the build section where the stronger position is.
- **Every shot is a screen capture.** Beat 8 keeps its place without footage of a physical panel, so the whole film is reproducible from run files and a re-shoot stays a re-run. Beats 7 and 8 still need run files the repo lacks.
- **Captions carry it, with no voice track.** That is the three-minute pacing the draft assumes, and it keeps a re-shoot cheap. It is also what the tooling does: [mtcompose.py](../../../moondeck/moontube/mtcompose.py) mixes one music bed cut to the beat, so narration would need a second track and ducking beneath it, which is a change to the tool rather than to the script.

## The two threads behind the week

### Testing covers MoonLight as a product

A newcomer arriving after the rename meets everything at once, and every part of it is equally new to them. So the week covers the path they take: install, provision, add a layout and an effect, drive it, save a preset, write a script, stream it somewhere. A defect in a three-release-old path costs a first impression exactly as much as one in the video drivers.

The ten run files in `moontube/clips/` describe precisely that path, numbered in the order a newcomer meets them, which is why they lead the week. `test_host --ui` performs every step and checks its `expect` blocks. They run on request rather than as a gate, by design, which leaves them the largest untested surface in the tree that a laptop can reach.

### The introduction is written this week and filmed after the switch

[mtvideo.py](../../../moondeck/moontube/mtvideo.py) records a run file with Playwright, captions and a cursor; [mtcompose.py](../../../moondeck/moontube/mtcompose.py) cuts published clips into one video on the beat with a music track. The ten clips and the `getting-started` project are the raw material, so the writing, the rehearsing and the edit all happen before the switch, and the recording follows it.

## After v6.0.0

Wired DMX-512, Ants, Spiral Fire, LightsControl, the IMU, and the per-band onset and BPM work. None of it blocks the rename, and none is easier before it.

## Decisions already on record

### Improvements over the predecessor

The migration mandate was fidelity, so every deliberate divergence was registered rather than left as drift. Product-owner ruling, 2026-07-01: improvements that increase user satisfaction are allowed.

| Effect or primitive | Predecessor behavior | Ours | Why it is better |
|---|---|---|---|
| `math8::map8` | `lo + scale8(in, hi-lo)`, so the input top never reaches `hi` and a one-step span collapses to 0 | `lo + in*(hi-lo)/255`, reaching `hi` exactly | Audio bars reach full height, and a 1-row bar becomes possible. Matches FastLED's documented `map8` |
| FreqSaws | Each band's physics advanced once per **column**, so a band spanning K columns ran K times too fast | Each of the 16 bands integrates once per frame | Speed no longer depends on panel width: identical on a 32-wide and a 256-wide grid |
| SphereMove | An integer divide meant the shell only advanced on whole ticks, about 20 updates a second at 60 fps | The expression stays in float | Smooth motion at all speeds. The predecessor intended float here, so this is also more faithful |
| Lissajous | A 1-wide or 1-tall grid mapped every sample to coordinate 1, which clips, so nothing drew | The size-1 axis maps to coordinate 0 | Visible output on thin grids; normal grids unchanged |
| PaintBrush | Oscillator endpoints truncated into `uint8_t`, so grids past 256 per axis swept only a low corner | Oscillators generate 0..255 then scale to the grid | Strokes span any grid and use the full palette range. Grids up to 256 per axis are pixel-identical |
| FixedRectangle | On RGBW the W channel was written on every box cell, tinting colored tiles and leaving W stale | W follows the checker, cleared to 0 on colored tiles | Colored tiles render as pure RGB, and the checker actually alternates |
| GEQ3D sweep | A per-frame counter, so the sweep tracked frame rate and ran faster on a quicker board | A time-based triangle wave | `speed` means the same on every device, which is the MoonLight convention |
| GEQ3D bars | Bar width `cols / NUM_BANDS` truncates to 0 when columns are fewer than bands, piling every bar at x=0 | The drawn band count is clamped to the column count | Bars render on narrow grids; a no-op on normal ones |
| AudioFrame | One level value, where WLED exposes both instant and smoothed | Added `levelSmoothed`, an EMA beside the raw `level` | Effects that should glide no longer jitter per audio block, and beat-reactive ones stay snappy |

Invisible fixes, listed for the record rather than as behavior: overflow guards on huge grids in GEQ, StarSky and PaintBrush; the Tetrix 49-day `millis` wrap; a GoL 3D out-of-bounds read; RubiksCube float to int, which is pixel-identical. StarField's blur control was flagged as inverted and turned out to match the predecessor, so only its comment changed.

### Fidelity tensions

Four of six are settled. The two open ones are listed under v5.0.0 above, and both are bench questions rather than code:

- **The audio level scale.** The predecessor normalizes `volume` to 0..1 where ours is a 0..255 `level`. A real INMP441 cross-check against the synthetic reference settles whether any effect reads differently.
- **Reconstructed logic.** Where the predecessor's source was incomplete, the behavior was reconstructed: the Tetrix fall cadence, the FreqMatrix scroll, Blurz dot placement, the FreqSaws band response, the GEQ peak fall, the NoiseMeter drift. A bench pass confirms each looks right. Only three `RECONSTRUCTED` markers survive in the tree, all in layouts, so the effect-side markers are gone and this is a visual check rather than a grep.

The accepted-as-is entry: `scale8` against integer `*bri/255` rounding in SolidEffect and elsewhere, kept faithful, no change wanted.

## Verification

1. A device running the current release upgrades to v5.0.0 and keeps its configuration, layouts and scripts, with only the [MIGRATING](../../reference/MIGRATING.md) entries behaving differently.
2. A v5.0.0 device on the bench updates itself to a v6.0.0 release after the repository has moved. This is the gate the v5 release exists for, and it can only be tested once both exist.
3. The sweep's diff is read in full before it is committed, since a blanket replace is how a symbol gets renamed by accident.
4. The full gate set passes on the swept tree: every ESP32 variant, the tests, the scenarios, `check_devices`, `check_specs`.
5. `moonmodules.org/projectMM` redirects to the new documentation site.
6. A v5.0.0 backup restores onto a v6.0.0 device with its layouts, effects and scripts intact, which is the claim that replaces the erase.

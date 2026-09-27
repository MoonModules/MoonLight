# Plan: MoonLight, from v5.0.0 to the rename

MoonLight becomes MoonLight. **v5.0.0 is the last release under the old name and v6.0.0 is the first under the new one.** This file is the whole record: what ships before the switch, what happens at it, what follows, and the decisions already taken along the way. It replaces the five files that held pieces of it.

## The three decisions that shape everything

**Migration from the predecessor is finished.** No further effects, layouts or modifiers are ported from the old MoonLight. What exists today is what ships, and the gap tables in the old plans are closed rather than outstanding. New effects are written on merit from here, not to match a list.

**A user's configuration survives both releases.** v5.0.0 upgrades in place, subject only to the breaks [MIGRATING](../../reference/MIGRATING.md) already records. v6.0.0 carries configuration across too, by Backup on v5 and Restore on v6, because **nothing persisted carries the product name**: config files are named after module types (`Effects.json`, `Drivers.json`) and the sweep leaves every type and `namespace mm::` untouched. The migration engine in [migrate.js](../../../src/ui/migrate.js) therefore has no rename to apply for the rename itself, which is the easiest case it can be handed.

**No compatibility code ships with MoonLight**, which is [the standing rule](../../../CLAUDE.md#principles) applied to the rename: nothing translates a MoonLight identity into a MoonLight one, no alias for a renamed key, no shim reading a predecessor's file, no branch asking which name a device was flashed under.

**Backup on v5, Restore on v6 is the one supported path**, and every upgrade question is answered with it. Where something does not carry, the answer is to erase the flash and install clean. A Home Assistant entity re-appearing under a new identity is the same trade: the alternative is a permanent pin to the old name, and a new product does not inherit one.

**Live interoperation survives**, which the rehearsal got wrong: a peer is classified by the numeric marker `0x014d4d00` rather than by any name, so a MoonLight device and a MoonLight device still see each other. What goes stale is a saved device list, whose rows re-type themselves on the next discovery sweep.

## Where we stand

Verified against the tree on 2026-09-22 rather than read from the plans, because several of their status lines had gone stale.

| | Count | Note |
|---|---|---|
| Effects | 67 headers, ~64 registered | Migration complete by decision |
| Modifiers | 12 | All 9 predecessor ones plus three of ours |
| Layouts | 18, 17 registered | Three install-specific ones absent, and staying so |
| MoonLive | 33 `.mle` scripts, 5 `.mlp` palettes | Palettes shipped |
| HUB75 | Ships | `Hub75Driver` is registered |
| Fidelity | 4 of 6 settled | Two open, both bench questions rather than code |

## What the rename touches

The sweep script measures **1405 occurrences across 242 tracked files** (dry run, 2026-09-22), against the 542 across 113 recorded when it was written: the documentation sweep and the effect library both grew the prose that names the product. The categories still matter more than the count, and the growth is spread across the tree rather than concentrated, so it is real rather than an exclude-list gap.

[rename_to_moonlight.py](../../../moondeck/repo_rename/rename_to_moonlight.py) handles almost all of it and runs dry by default. It replaces two tokens, `ProjectMM` then `MoonLight`, which is correct for every form because `MoonLight` is never a substring of another token. Its file list comes from `git ls-files`, so build output is excluded without a blocklist. `MoonLive`, the predecessor's own name, and `namespace mm` are provably never touched.

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
- **The `MM-` device prefix** in [SystemModule.h:57](../../../src/core/system/SystemModule.h) is every device's mDNS name and Home Assistant entity id. Changing it to `ML-` renames all of that, and unlike the configuration it is not something Restore carries back. Keeping `MM-` costs an odd prefix forever; changing it costs every user their bookmarks and automations once. Product owner's call, in the same sweep commit either way.

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

**A hand-counted length survives a rename by luck.** MQTT sized its topic buffer as `9 + 1 + 6 + 1`, counted from `MoonLight`. MoonLight is nine characters too, so the sweep would have passed and any other name would have truncated every topic silently. The length now derives from the string with `sizeof`, which is the general form: a literal's length belongs to the literal, never to a comment that counts it.

**A round trip has as many ends as it has, and the tests know.** The device-type label looked like a pair, one plugin writing it and one comparison reading it. It was three: `devTypeStr` emits the string that gets persisted. Renaming two of the three broke four tests, which is the guardrail working exactly as intended. Assume a third end exists until the suite says otherwise.

**Discovery was already rename-proof, and the plan said otherwise.** A peer is classified by the numeric marker `0x014d4d00`, not by any name, so a renamed device and an old one still recognise each other on the wire. The paragraph claiming live interoperation breaks at v6.0.0 was wrong about the mechanism. What actually goes stale is a persisted device list, which self-heals on the next discovery sweep.

**A comment saying a line is fixed does not stop a sweep.** The MoonCloud salt carried "changing this re-identifies every installation in the world exactly once, so it is fixed" and would have been rewritten anyway. It now carries a `rename-keep` marker, which the script honours mechanically. A rule worth stating is worth stating where the tool reads it.

**The sweep broke the one supported upgrade path.** Restore compares a bundle's `format` against a literal carrying the product name, so a swept reader rejects every backup a user already saved, with "not a config backup". The reader now accepts both spellings behind a `rename-keep` marker while the writer emits the new one. This is the case the whole migration promise rests on, and a blanket token replace inverted it.

**The sweep orphaned every desktop user's configuration.** The desktop data directory is built from the product name, so a renamed build reads an empty profile and loses presets, layouts and scripts, silently and with no error. Devices have Backup and Restore; the desktop had nothing, and the plan had not noticed because its migration section is written entirely about devices. Now held by a `KEEP_PATH_KEYS` list, control-tested so an ordinary comment still sweeps.

**The sweep blinded the prose checker.** `.vale.ini` names its style and vocabulary by directory, so renaming the references while the directories kept the old name left Vale reading no rules at all and reporting zero findings for every header. The directories moved with the config, and the proof is Vale reporting `.cpp` findings again, which is the control the file's own comment asks for.

**A blanket replace edits quotations.** The product owner's own words inside a block quote were rewritten, turning "MoonLight V1, V2 and V3" into a sentence they never wrote. A quote is evidence rather than prose, so the sweep has no business inside one.

**What genuinely cannot move early**, checked rather than assumed: the OTA project guard, where `moonbase/CMakeLists.txt` stamps the image and `FirmwareImage.h` checks that exact string, so moving either early makes every v5 device refuse the new MoonBase image. And the repository URLs, which resolve only once the repo itself is renamed.

Three more joined that list once the free renames were taken. The **CMake project name** stamps the ESP-IDF descriptor `kProjectImageName` is compared against, so the two flip in one commit or every firmware is refused. The **desktop asset filenames** are parsed by firmware already in the field, which makes the packager, the release workflow, the install picker and their test fixtures a single lockstep set. And the **Home Assistant domain** in the installer manifest names an integration that has to exist before it is offered.

The rest is 1206 lines, almost all of it documentation prose and the handful of identities above.

**The method that found all of this** is worth repeating on whatever is left: take the sweep's own file list, read every hit in the top files rather than trusting the count, and ask of each whether anything outside this repository keys on the string. Almost all of them were comments.

### The rename lands in batches, not one sweep

A branch over roughly 100 files loses its external review layer, and the free renames alone reach 200. So the sweep runs in passes, each its own commit with the whole gate set behind it, rather than as one change nobody can read.

The order is by risk, cheapest first. `src/` and `test/` went first, because the compiler and 2000 tests judge them: a mistake there fails rather than ships. Documentation prose is next and carries no executable risk. The identities that outside systems key on go last, on the day, and they are now a short enough list to read in one sitting.

What makes this safe is that the sweep is idempotent and its own report is committed, so a batch can be regenerated at any time and the remaining reach is always visible in [rename_to_moonlight.md](../../../moondeck/repo_rename/rename_to_moonlight.md).

### The week, day by day

**The script is the deliverable.** It says what MoonLight is, in the order a newcomer needs it, and everything else is a rendering of it: the run files perform it, `uivideo.py` films it, the tutorials follow the same sequence in prose. Written once, so a change lands in all three. [The scenario](#the-introduction-a-draft-scenario) below is the draft to work from.

**Every shot is a script, so filming is repeatable.** A run file names the steps and their captions, and `uivideo.py` performs them against a live device. Re-shooting is re-running, which is what makes the schedule below possible: the takes are cheap and the thinking is not. Footage is ready by Sept 30.

Two rules hold all week. **Anything found before Tuesday is fixed under the old name**, since a defect discovered after the rename is a defect in two releases. And **the published cut is filmed after the switch**: the UI carries the product name in its page title and header, so an earlier take says MoonLight in the pixels. Earlier takes still earn their place as rehearsal, because re-running is cheap.

**Sept 22 and 23: the script.** Thinking, and it decides everything after it.

- Write what MoonLight is, in the order a newcomer meets it.
- Name each beat, what it shows, and what it says.
- Check each beat against what exists today, so the script is shootable now.
- Done when the scenario below is revised into the one you want.

**Thu 24: the scenario suite, and the rename brought forward.** Hands on, and it went somewhere the plan had not put it.

- Rebuilt the scenario suite: 27 archived, 11 written, one per top-level card plus the reboot-persistence one.
- Closed the hole that made them look green: a skip returned the pass code, so ten of eleven asserted nothing while the gate said `11 passed`. A skip is now counted as a skip, and a scenario that asserts nothing fails.
- Swept the free renames in `src/` and `test/`, taking cutover day from 1277 lines to 1206. Documentation prose is the next batch and the largest.
- The fourteen run files in `test/uiscenarios/clips/` are numbered and current; matching them to the script is Friday's work, alongside the first cut.

**Fri 25: the first cut, filmed.** Hands on, and the rehearsal that finds what reads badly.

- Record every run file with `uivideo.py` against a v5.0.0 device.
- Cut it with `uicompose.py`: music, order, beats.
- Watch it. A beat that drags or confuses is a script problem, so fix the script.
- Done when a watchable cut exists, old name and all.

**Sat 26: what the first cut taught.** Hands on.

- Revise the script and the run files against what Friday showed.
- Re-record what changed, which is a re-run rather than a re-shoot.
- Done when the cut says what the script meant.

**Sun 27: the tutorials.** Thinking, which suits a short day.

- Follow the same sequence in prose, since the script already settled the order.
- Decide which beats are a page and which are a paragraph.
- Done when the documentation and the film tell one story.

**Mon 28: slack.** Whatever the week turned up.

- Fix what testing found, and re-run anything a fix touched.
- Done when the tree is releasable under the old name.

**Tue 29: [Windows day](#sept-29-windows-day).** A full day, the first time anything has been tested there.

**Wed 30: [the switch](#sept-30-moonlight-v600), then the final take.** The rename lands, and the footage is re-recorded against it the same day.

- Re-run every run file with `uivideo.py` against a v6.0.0 device.
- Re-cut with `uicompose.py`, which consumes the same project file.
- Done when the published cut says MoonLight in every frame.

**Thu 1 Oct: publish.** The announcement, the video, and the tutorials together.

**Testing rides along.** Every run file is a UI test, so Thursday and Friday exercise the newcomer's path harder than a test pass would, and the boards get theirs on Monday's slack. What that covers is in [the two threads](#the-two-threads-behind-the-week).

### Sept 29: Windows day

The desktop build runs on Windows and is packaged by CI, but **no test has ever run there**. [release.yml:318](../../../.github/workflows/release.yml) has the only Windows runner in the repository and it compiles and packages without invoking `mm_tests`, the scenarios, or anything else. Every item below is therefore unverified rather than lightly verified.

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

### Sept 30: MoonLight v6.0.0

One repository transfer, one sweep commit, one release. The installer manifest, the release asset names and the in-firmware URL builder are a lockstep set, so a half-applied rename leaves devices unable to update. `namespace mm::` stays throughout: it is not the product name, and renaming it would touch every file for nothing.

One day, in order, with a stop at each gate:

1. **`uv run moondeck/repo_rename/check_rename_ready.py`**, which dry-runs the sweep and asserts what the rehearsal established: the reach is near the last measured pass, every `rename-keep` line survives, and the OTA still names two different repositories. Exit 0 means proceed. The rehearsal already ran the gate set over a swept tree, so this is a check rather than an investigation, and it is worth running any day before the switch to see the drift early.
2. **Back up a configured v5.0.0 device** and keep the bundle. This is the evidence for the migration claim, and it has to be taken before anything moves.
3. **Transfer the repository**, which leaves the old URLs redirecting.
4. **Run `uv run moondeck/repo_rename/rename_to_moonlight.py --apply`** on a branch off the renamed repo, read the diff in full, commit it as one change. Then check the four the rehearsal found: `kFallbackRepo` still names the old repository, the MoonBase image check still reads `projectMM-moonbase`, MIGRATING's v5.0.0 heading still says projectMM, and `TextEffect`'s golden moves with its new default text rather than failing.
5. **Flip the identity set in that same commit**: binary name, release asset names, the manifest `name` and `home_assistant_domain`, the docs domain, and the `MM-` prefix if it changes.
   - The documentation path follows the repository name on its own: GitHub Pages serves a project site under `/<repo>/` even on the custom domain, so `moonmodules.org/projectMM/…` becomes `moonmodules.org/projectMM/…` at the transfer, and the sweep updates `site_url`, `repo_url` and `site_name` to match. Check the web installer at `/MoonLight/install/` first, since it is the link a newcomer follows.
   - **Check the OLD installer path too, and do not assume it redirects.** GitHub's permanent redirect for a transferred repository covers `github.com` URLs, which is what the OTA client follows; a Pages path on a custom domain is a different mechanism and is not covered by that promise. Every board already shipped carries `/MoonLight/install/` on its QR code and in its documentation, so a 404 there strands the people most likely to be upgrading. Open it right after the transfer: if it does not land on the installer, publish a redirect from the old path before announcing anything.
6. **Run the full gate set again** on the swept tree, then tag and release v6.0.0.
7. **Verify the two claims**: a v5.0.0 device finds and installs v6.0.0 over OTA, and the backup from step 2 restores onto it with layouts, effects and scripts intact.
8. **Hand-edit `moondeck/moondeck.json`**, which is gitignored and outside the sweep.

If step 7 fails, the release stays and the fix is a v6.0.1: the repository has already moved by then, so rolling back is not on the table. That is why steps 1 and 2 happen first.

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
> I did give up on MoonLight code, but I did not give up on the MoonLight principles! They are a few years old, formulated when I was working on WLED and WLED-MM, first tried in StarLight, then MoonLight, then MoonLight V1, V2 and V3 and now the new MoonLight, and the principles were extended over time:
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
- **Captions carry it, with no voice track.** That is the three-minute pacing the draft assumes, and it keeps a re-shoot cheap. It is also what the tooling does: [uicompose.py](../../../moondeck/uiscenario/uicompose.py) mixes one music bed cut to the beat, so narration would need a second track and ducking beneath it, which is a change to the tool rather than to the script.

## The two threads behind the week

### Testing covers MoonLight as a product

A newcomer arriving after the rename meets everything at once, and every part of it is equally new to them. So the week covers the path they take: install, provision, add a layout and an effect, drive it, save a preset, write a script, stream it somewhere. A defect in a three-release-old path costs a first impression exactly as much as one in the video drivers.

The ten run files in `test/uiscenarios/clips/` describe precisely that path, numbered in the order a newcomer meets them, which is why they lead the week. `test_host --ui` performs every step and checks its `expect` blocks. They run on request rather than as a gate, by design, which leaves them the largest untested surface in the tree that a laptop can reach.

### The introduction is written this week and filmed after the switch

[uivideo.py](../../../moondeck/uiscenario/uivideo.py) records a run file with Playwright, captions and a cursor; [uicompose.py](../../../moondeck/uiscenario/uicompose.py) cuts published clips into one video on the beat with a music track. The ten clips and the `getting-started` project are the raw material, so the writing, the rehearsing and the edit all happen before the switch, and the recording follows it.

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

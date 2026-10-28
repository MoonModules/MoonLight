# Videos

Every clip of the series, in the order you would meet them. Each one plays on the page that documents its subject; the link under the title goes there.

## What is new in 7

<video src="assets/moontube/17-whats-new-in-7.webm" controls playsinline width="720" title="What version seven adds that you can see on a screen: a game, the FSEQ player, network streams and the Gallery."></video>

## How they are made

No clip is filmed by hand. [MoonTube](../moondeck/moontube/moontube.md) performs a run file in the real interface, the way a person would, narrates its captions with a synthesized voice and records the result. The same run file is a test, so a clip that drifts from the interface fails instead of going stale, and the whole series can be recreated against each release with one command per clip.

## Start

| | |
|--|--|
| [<img src="assets/moontube/00-intro.jpg" alt="Why MoonLight was rebuilt" width="220">](getting-started.md#the-introduction) | **Why MoonLight was rebuilt**<br>Luna introduces MoonLight: what MoonModules built before, what she built from their prompts, and the principles that survived the rewrite.<br>[Getting started](getting-started.md#the-introduction) |
| [<img src="assets/moontube/01-install-desktop.jpg" alt="No hardware needed" width="220">](getting-started.md#three-ways-in) | **No hardware needed**<br>The same installer, pointed at this computer: pick a release and download the app.<br>[Three ways in](getting-started.md#three-ways-in) |
| [<img src="assets/moontube/01-install-esp32.jpg" alt="Install it" width="220">](getting-started.md#three-ways-in) | **Install it**<br>The web installer onto a device: erase, flash, join WiFi, and open the UI that comes back.<br>[Three ways in](getting-started.md#three-ways-in) |

## Look around

| | |
|--|--|
| [<img src="assets/moontube/02-first-look-desktop.jpg" alt="Look around" width="220">](explanation/how-moonlight-works.md) | **Look around**<br>The same tour on a computer: every module, and what sits inside each one.<br>[How MoonLight works](explanation/how-moonlight-works.md) |
| [<img src="assets/moontube/02-first-look-esp32.jpg" alt="On real hardware" width="220">](explanation/how-moonlight-works.md) | **On real hardware**<br>The same tree on real hardware: what a device adds that a computer cannot, from LED pins to a microphone.<br>[How MoonLight works](explanation/how-moonlight-works.md) |
| [<img src="assets/moontube/03-second-look.jpg" alt="Change your mind freely" width="220">](explanation/how-moonlight-works.md) | **Change your mind freely**<br>The parts of the interface every card shares, each one pressed so you see what it does.<br>[How MoonLight works](explanation/how-moonlight-works.md) |

## Build a show

| | |
|--|--|
| [<img src="assets/moontube/05-layouts.jpg" alt="Say where the lights are" width="220">](explanation/how-moonlight-works.md) | **Say where the lights are**<br>Where the lights physically are: compiled layouts for the common shapes, and scripted ones for anything else.<br>[How MoonLight works](explanation/how-moonlight-works.md) |
| [<img src="assets/moontube/06-layers.jpg" alt="Paint them" width="220">](explanation/how-moonlight-works.md) | **Paint them**<br>An effect, a modifier reshaping it, and a second layer doing the same, blended over the first.<br>[How MoonLight works](explanation/how-moonlight-works.md) |
| [<img src="assets/moontube/07-drivers-desktop.jpg" alt="Get the frame out" width="220">](moonmodules/light/drivers.md) | **Get the frame out**<br>What carries the frame out of the device, on a desktop: a preview, the video senders, and the network protocols a fixture listens on.<br>[Drivers](moonmodules/light/drivers.md) |
| [<img src="assets/moontube/07-drivers-esp32.jpg" alt="And out of a device" width="220">](moonmodules/light/drivers.md) | **And out of a device**<br>What carries the frame out of a device: the LED pins only real hardware has, the panel protocols, and the network senders it shares with a desktop.<br>[Drivers](moonmodules/light/drivers.md) |
| [<img src="assets/moontube/08-moonlive-effects.jpg" alt="Write your own" width="220">](moonmodules/light/moonlive.md) | **Write your own**<br>Scripting the lights in the browser: a library effect downloaded and run, then one written from nothing, from a single line to a particle system.<br>[MoonLive](moonmodules/light/moonlive.md) |

## Control it

| | |
|--|--|
| [<img src="assets/moontube/09-services.jpg" alt="Everything that drives a control" width="220">](moonmodules/core/services.md) | **Everything that drives a control**<br>Inputs: the compiled services that read a button, a pin or a remote, and a scripted one for anything with a condition in it.<br>[Services](moonmodules/core/services.md) |
| [<img src="assets/moontube/10-control.jpg" alt="One surface, pointed anywhere" width="220">](reference/control-surface.md) | **One surface, pointed anywhere**<br>The surface: eight switches, encoders and faders, each pointed at any control on the device, and the one place external control arrives.<br>[Control surface](reference/control-surface.md) |
| [<img src="assets/moontube/16-restore-a-backup.jpg" alt="Restore a backup" width="220">](reference/backup-and-restore.md) | **Restore a backup**<br>A backup from before config files were state documents, restored through the File Manager's Restore.<br>[Backup and restore](reference/backup-and-restore.md) |

## The project

| | |
|--|--|
| [<img src="assets/moontube/04-scenario-testing.jpg" alt="Tested the way you use it" width="220">](reference/testing.md) | **Tested the way you use it**<br>How MoonLight checks itself: a scenario driving the running device while you watch.<br>[Testing](reference/testing.md) |
| [<img src="assets/moontube/11-moondeck.jpg" alt="The console it is built from" width="220">](explanation/architecture/moondeck.md) | **The console it is built from**<br>The developer console: one page that builds, flashes, tests and checks the project, and why it is ours rather than PlatformIO.<br>[MoonDeck](explanation/architecture/moondeck.md) |
| [<img src="assets/moontube/14-help-and-moonwalk.jpg" alt="Help and MoonWalk" width="220">](moonwalk.md) | **Help and MoonWalk**<br>Help on every card and control, the MoonWalk step a card takes part in, and the test that checks a control.<br>[MoonWalk](moonwalk.md) |
| [<img src="assets/moontube/15-the-website.jpg" alt="The website" width="220">](getting-started.md#the-introduction) | **The website**<br>The documentation site, tab by tab: the home page, getting started, the catalogs generated from the code, MoonWalk, the tests, and the reference and developer pages.<br>[Getting started](getting-started.md#the-introduction) |
| [<img src="assets/moontube/12-getting-involved.jpg" alt="How to start" width="220">](contributing/index.md) | **How to start**<br>How to start, how to contribute, and what comes next.<br>[Contributing](contributing/index.md) |
| [<img src="assets/moontube/13-attribution.jpg" alt="Standing on shoulders" width="220">](reference/attributions.md) | **Standing on shoulders**<br>Who we learned from, and the standard algorithms MoonLight is built on.<br>[Attributions](reference/attributions.md) |

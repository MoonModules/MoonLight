# FAQ

Questions people ask on a first run, in the order they meet them: installing, then first light, then the interface they are looking at, then effects, then the driver that reaches real hardware.

A symptom rather than a question belongs in [Troubleshooting](troubleshooting.md), which starts from what you can see.

## Installing

- **Which firmware does my board take?** The web installer picks it from the board you choose. See [Install MoonLight](../moonwalk.md#install).
- **Can I run it without a board?** Yes, on macOS, Linux and Windows. See [Install MoonLight](../moonwalk.md#install).
- **Which hardware do the heavy effects want?** The ones computing a field per light cost CPU, so a desktop or a small always-on machine runs them where a microcontroller cannot. See [Installing on Linux](installing-on-linux.md).
- **How do I update?** Over the air from the Firmware card, or by URL. See [Update MoonLight](../moonwalk.md#update-firmware).

## First light

- **Where do I set the number of lights?** On a Layout, not on the driver. See [Layouts](../moonmodules/light/layouts.md).
- **Where do I add an effect?** Expand a Layer under Effects, then **+**: a layer is what composites effects and holds the blend mode. See [Build your first light show](../moonwalk.md#first-light-show).
- **Nothing lights up.** See [Troubleshooting](troubleshooting.md#the-lights-are-dark).

## The interface

- **What is the number on a module card?** That module's own tick time, shown from expert mode up. Hold it to peek at the rate, which inverts it: a module ticking in 22 µs reads as 45K fps. The device's frame rate is `fps` on the [System module](../moonmodules/core/system.md#system).
- **The card is busy, and I do not need half of it.** Set `mode` on the [System module](../moonmodules/core/system.md#system) to `user`, `expert` or `developer`: each control names the mode it needs.
- **The preview changed resolution.** It follows the layout, so a geometry change moves it. See [Preview](../moonmodules/light/drivers.md#preview).

## Effects and scripting

- **Can I write my own effect?** Yes, in MoonLive, on the device, live. See [Write a MoonLive effect](../moonwalk.md#first-script).
- **How do I save a look?** Presets. See [Presets](presets.md).
- **Can I drive it from a control surface or a DAW?** Over MIDI, OSC and DMX. See [Control surface](control-surface.md).

## Drivers and panels

- **Which driver do I need?** One per output kind: LEDs on pins, a panel card over Ethernet, HUB75 direct, or a network protocol. See [Drivers](../moonmodules/light/drivers.md).
- **Which board drives a panel card?** One with gigabit Ethernet, so an S31 or a desktop. An S3 or P4 is 100 Mbit and wants a gigabit switch between it and the card. See [Panel cards](hardware/panel-cards.md#the-one-hardware-fact-that-decides-everything).
- **My ColorLight card does almost nothing.** These cards need a 1000 Mbps link, and the driver reports the negotiated speed in its status line. See [Panel cards](hardware/panel-cards.md).
- **My HUB75 panel is one column out.** Set `clockEdge` to `falling`: some panel chips sample the shift clock on the other edge. See [Drivers](../moonmodules/light/drivers.md).
- **The HLS stream lags by seconds.** HLS buffers whole segments, so the delay is the format. For a live view use [Preview](../moonmodules/light/drivers.md#preview) instead of [HLS](../moonmodules/light/drivers.md#hls).

## Network

- **Ethernet and WiFi fight.** Pick one in the [Network module](../moonmodules/core/system.md#network)'s `mode`, because both leaves two default routes.

## Developing

- **What is MoonDeck, and do I need it?** A browser console that builds, flashes, runs, tests and checks the project, and drives the boards on your bench. Everything it offers is a script under `moondeck/`, so the CLI and the console run the same code and neither is required. See [MoonDeck](../explanation/architecture/moondeck.md).
- **Why not PlatformIO or pioarduino?** The ESP32 build is ESP-IDF-native, tracking the IDF against a pinned commit for chips like the P4 and S31. The hot-path drivers use the vendor APIs directly. The tooling also covers far more than compile-upload-monitor. See [why our own scripts](../explanation/architecture/moondeck.md#why-our-own-scripts).
- **How do I start?** `uv run moondeck/moondeck.py`, then open <http://localhost:8420>. Build and run the desktop first: it needs no board and proves the toolchain. See [Build, run and test on the desktop](../moonwalk.md#desktop-build-test).

## Still stuck

Name the geometry, the driver, the peripheral and the measured refresh. Those four decide which path you are on. See [Log an issue](logging-an-issue.md).

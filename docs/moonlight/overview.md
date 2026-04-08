# MoonLight Overview

<a href="https://www.youtube.com/watch?v=bJIgiBBx3lg">
  <img width="380" src="https://img.youtube.com/vi/bJIgiBBx3lg/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch the functional overview](https://www.youtube.com/watch?v=bJIgiBBx3lg)**

MoonLight is open-source software that lets you control a wide range of DMX and LED lights using ESP32 microcontrollers — for home use, art installations, and professional stages.

<img width="163" src="https://github.com/user-attachments/assets/1afd0d4b-f846-4d5b-8cc9-8fa8586c405b" />

---

## What's inside MoonLight

MoonLight is organised into modules, each covering a specific area of functionality:

| Module | What it does |
|---|---|
| [Lights Control](https://moonmodules.org/MoonLight/moonlight/lightscontrol/) | Global brightness, colour, presets and external control (DMX, IR, Home Assistant) |
| [Effects](https://moonmodules.org/MoonLight/moonlight/effects/) | Visual effects and modifiers that run on virtual layers |
| [Drivers](https://moonmodules.org/MoonLight/moonlight/drivers/) | Layouts and drivers that send data to physical lights |
| [Channels](https://moonmodules.org/MoonLight/moonlight/channels/) | Live monitor of the raw channel data going to your lights |
| [Live Scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/) | Write and run custom effects directly on the device |
| [MoonLight Info](https://moonmodules.org/MoonLight/moonlight/moonlightinfo/) | Firmware build info and system diagnostics |

---

## About MoonModules

MoonLight is created by [MoonModules](https://moonmodules.org/) — a collective of light artists, hardware and software developers who love building DIY lighting solutions based on ESP32. You can find our work on GitHub and reach us on [Discord](https://discord.gg/TC8NSUSCdV).

We develop our products for a large community of light enthusiasts and show them at art exhibitions, in venues, and at festivals. See also [about MoonModules](https://moonmodules.org/about/).

We started creating **WLED-MM**, a fork of WLED focused on driving more LEDs, supporting new platforms (e.g. ESP32-P4), and better audio. As most of the MoonModules team are also core WLED developers, WLED-MM functionality is gradually moving back into WLED.

Starting in 2023 we began building **MoonLight** — a modern alternative to WLED with a reactive UI, 1D/2D/3D layout support, live scripts and flexible LED drivers.

---

## Why build MoonLight when WLED already exists?

| Pain point | How MoonLight addresses it |
|---|---|
| **Complex UI** — two separate UI systems, hard to extend | Single reactive SvelteKit UI, fully generic module rendering |
| **ESP8266 legacy** — constrains the codebase | ESP32-only, no legacy baggage |
| **Outdated libraries** — ArduinoJson, FastLED, ESP-IDF | Philosophy: keep dependencies up to date |
| **1D/2D effects only** — lots of hardcoding | 3D at the core; effects don't know fixture size |
| **Strip/panel mapping** — no 3D, hacks for rings/hexagons | Inverse mapping: just define LED coordinates |
| **Accumulated tech debt** — hard to maintain audio-reactive | Clean architecture with Node-based isolation |
| **Too large to change** — deep architectural changes are nearly impossible | Built from scratch with extensibility in mind |

What I love most about WLED and wanted to keep: **UserMods** ❤️ — small, isolated, opt-in code pieces each with their own UI. That idea evolved into MoonLight's **Nodes** class: self-contained objects that initialise, run, expose controls, and clean up after themselves.

---

## Current status

As of late 2025 MoonLight's foundation is solidly in place.

Earlier releases involved major structural changes that sometimes required fresh installs. Future updates should generally be seamless, though major version upgrades may still include exceptions.

**MoonBase** is now a flexible IoT foundation you can use independently of lights. **MoonLight** is a ready-to-use lighting application built on top of it.

That doesn't mean the work is done — this is just the beginning. MoonLight is open for collaboration. You're warmly invited to help shape its future; see the [Develop](https://moonmodules.org/MoonLight/develop/) pages to get started.

### What's been done

**MoonBase**

- ESP32-SvelteKit foundation (SvelteKit UI, PsychicHttp, Stateful services, Event Sockets, RESTful API, mDNS, MQTT)
- [Modules](https://moonmodules.org/MoonLight/develop/modules/): generic building blocks for server + UI functionality activated via a JSON definition
- [Nodes](https://moonmodules.org/MoonLight/develop/nodes/) 🥜: generic building blocks that run code and expose controls in the UI
- Utilities: system logging, 3D coordinate system, string and memory management
- MoonBase features: Connections, WiFi, System, Files, Tasks, Devices, IO
- ESP32-P4 support
- Ethernet
- Specific board profiles; see [Premade boards](https://moonmodules.org/MoonLight/gettingstarted/hardware/#premade-boards)

**MoonLight**

- [Layers](https://moonmodules.org/MoonLight/develop/layers/): physical and virtual layers; physical layer manages real LEDs/lights via layouts and drivers; virtual layers run effects and modifiers
- Control, Channels and Info
- Effects & Modifiers
- Layouts & Drivers — FastLED, parallel LED driver, DMX, Art-Net, DDP, E1.31, WLED Audio Sync
- Live Scripts (already in MoonLight, planned for MoonBase)
- Multiple layers
- DMX / Network In (Art-Net, DDP, E1.31)
- Palettes: more options, custom, audio-reactive

### Looking ahead

**MoonBase**

- MIDI (light control desks)
- SuperSync 🥜 — distributed effects across multiple devices
- HA / MQTT improvements

**MoonLight**

- Virtual driver
- More effects (FastLED, Animartrix, Physics, Live Scripts)

### Where you can help

- Add effects (FastLED, WLED, particle systems, Animartrix, Soulmate Lights…), modifiers, layouts and drivers
- Improve the UI (Svelte 5, DaisyUI 5, Tailwind 4) — File Manager, multi-row layout, Monitor (WebGL)
- Tune FastLED, add FastLED 2D effects
- CI automation

Come say hi in the MoonLight channels on [Discord MoonModules](https://discord.gg/TC8NSUSCdV) 👋

---

## Emoji key

Effects, Modifiers, Layouts and Drivers use emojis to show their capabilities at a glance:

| Emoji | Meaning |
|---|---|
| 🔥 | Effect |
| 🐙 | WLED origin |
| 💫 | MoonLight origin |
| ⚡️ | FastLED origin |
| 🚨 | Moving head — colour effect |
| 🗼 | Moving head — move effect |
| 💎 | Modifier |
| 🚥 | Layout |
| ☸️ | Driver |
| ♫ | Audio-reactive (FFT-based) |
| ♪ | Audio-reactive (volume-based) |
| 💡 | Supports 0D and up |
| 📏 | Supports 1D and up |
| 🟦 | Supports 2D and up |
| 🧊 | Supports 3D |

---

## Reporting issues

Found a bug or want to request a feature?

👉 **[Open an issue on GitHub](https://github.com/MoonModules/MoonLight/issues)**

### Checklist

- Search existing issues first — your issue may already be reported
- For bugs, include the module API output (see below)
- For feature requests, describe the use case

### Bug report template

See [A github issue](https://github.com/MoonModules/MoonLight/issues/164) for an example.

**1. Describe the problem**

```
Steps to reproduce:
1. ...
2. ...

Expected behavior:
[What should happen]

Actual behavior:
[What happens instead]

```

**2. Attach module API outputs**

The API output is a JSON snapshot of your device state — ⚠️ **this is the most useful diagnostic data**!

**Get the API output:**

1. Open the affected module in the MoonLight UI (e.g. Effects, Drivers)
2. Click the **API icon** (↔) at the bottom-right of the module card
3. Copy the entire JSON, paste it in your issue in a code block

**Include the following API output:**

1. Always: [System status](../system/status.md)
2. If lights related: [MoonLight Info](./moonlightinfo.md)
3. If pins related: [Board preset](../moonbase/inputoutput.md)
4. If drivers related: [Drivers](./drivers.md)
5. Affected module, e.g.:

    - [Effects](./effects.md)
    - [Lights Control](./lightscontrol.md)
    - [Channels](./channels.md)

use ```text to have all the output on one line to avoid very long issue texts like this:

````text
API output:

- System status 
```text
{"esp_platform":"ESP32-S3","firmware_version":"0.9.1","firmware_date":"20260406","firmware_target":"esp32-s3-n8r8v","platform_version":"pioarduino-55.03.37","ma...}
```

- MoonLight info
```text
{"nrOfLights":256,"channelsPerLight":3,"nrOfChannels":768,"size":{"x":16,"y":16,"z":1},"nodes#":2,"layers":...}
```

- Module IO Board preset
```text
{"boardPreset":"LightCrafter16","modded":false,"maxPower":500,"pins":...}
```

- Module Drivers
```text
{"nodes":[{"name":"Panel â¹ï¸ ðŸš¥","on":true,"controls":[{"name":"panelWidth","value":16,"default":16,"p":1009379704,"type":"number","valid":true,"min":1,"max":65536,"size":16},]}]...}
```

````

**3. Ask for analysis (optional)**

Add a comment to your issue:

```
@coderabbitai, read this issue description and analyze the problem and provide a solution
```

CodeRabbit will analyze the logs and suggest a fix.

### Feature requests

Describe the use case, expected behaviour, and alternatives you've considered.

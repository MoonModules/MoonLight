# MoonLight v1.0.0 and Beyond

## v1.0.0 is the last release of MoonLight

Active development is moving to **[ProjectMM](https://github.com/ewowi/projectMM)**. MoonLight v1.0.0 still works fine (see [MoonLight in production](#moonlight-in-production) below), but no new features are planned for this codebase.

---

## Why

MoonLight grew to a size where it really needs a proper testing framework: unit tests, integration tests, and hardware-in-the-loop tests. Not added later, but built in from the start.

The problem is that you can't bolt that onto MoonLight without breaking things. The module and node boundaries, the shared JSON document, and the dual-core task setup are all tightly coupled. Untangling them for testability would essentially mean rewriting the project anyway.

What changed is that AI-assisted development (agents like Claude Code, CodeRabbit) now makes it realistic to build a fully tested system from scratch. Writing tests, reviewing code, and catching regressions continuously are things AI handles well, which shifts the cost-benefit of test-driven development entirely. MoonLight's [AI principles](https://moonmodules.org/MoonLight/develop/standardsguidelines/#artificial-intelligence) already reflect this: AI as a tool that speeds up the work, with humans keeping control.

So rather than fight the existing structure, [ProjectMM](https://github.com/ewowi/projectMM) starts fresh, keeping what worked in MoonLight (the module system, nodes, physical/virtual layer separation) and building the rest around continuous testing from day one.

---

## ProjectMM

**[ProjectMM](https://github.com/ewowi/projectMM)** is the successor. See **[Why ProjectMM](https://ewowi.github.io/projectMM/why-project-mm/)** for the full rationale.

- **Everything is a module** (concept inherited from [StarLight](https://github.com/ewowi/StarLight))
- **Unit tests and live tests on everything** (a USB hub used as an ESP32 rack for continuous hardware-in-the-loop testing)
- **Releases and sprints** (predictable cadence)
- **[FastLED-MM](https://github.com/MoonModules/FastLED-MM)**: showcase for using ProjectMM as a library

---

## MoonLight in production

MoonLight has been used in large setups and still is.

- Still the preferred tool for large LED displays, DMX lights, or Art-Net setups
- Will be replaced by ProjectMM at some point
- Bug reports follow the [issue process](https://moonmodules.org/MoonLight/moonlight/overview/#reporting-issues)
- PRs welcome

---

## What's been done

Earlier releases had major structural changes that sometimes required fresh installs. v1.0.0 is the consolidated final form.

**MoonBase** is a flexible IoT foundation that works independently of lights. **MoonLight** is the lighting application built on top of it.

**MoonBase**

- ESP32-SvelteKit foundation (SvelteKit UI, PsychicHttp, Stateful services, Event Sockets, RESTful API, mDNS, MQTT)
- [Modules](https://moonmodules.org/MoonLight/develop/modules/): generic building blocks for server + UI functionality activated via a JSON definition
- [Nodes](https://moonmodules.org/MoonLight/develop/nodes/) 🥜: generic building blocks that run code and expose controls in the UI
- Utilities: system logging, 3D coordinate system, string and memory management
- MoonBase features: Connections, WiFi, System, Files, Tasks, Devices, IO
- ESP32-P4 support
- Ethernet
- Specific board profiles, see [Premade boards](https://moonmodules.org/MoonLight/gettingstarted/hardware/#premade-boards)

**MoonLight**

- [Layers](https://moonmodules.org/MoonLight/develop/layers/): physical and virtual layers (physical layer manages real LEDs/lights via layouts and drivers; virtual layers run effects and modifiers)
- Control, Channels and Info
- Effects & Modifiers
- Layouts & Drivers: FastLED, parallel LED driver, DMX, Art-Net, DDP, E1.31, WLED Audio Sync
- Live Scripts (already in MoonLight, planned for MoonBase)
- Multiple layers
- DMX / Network In (Art-Net, DDP, E1.31)
- Palettes: more options, custom, audio-reactive

---

## Where you can help

Community PRs to v1.0.0 are still welcome:

- Add effects (FastLED, WLED, particle systems, Animartrix, Soulmate Lights...), modifiers, layouts and drivers
- Improve the UI (Svelte 5, DaisyUI 5, Tailwind 4): File Manager, multi-row layout, Monitor (WebGL)
- Tune FastLED, add FastLED 2D effects
- CI automation

Come say hi in the MoonLight channels on [Discord MoonModules](https://discord.gg/TC8NSUSCdV) 👋

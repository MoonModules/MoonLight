# MoonLight Overview

<a href="https://www.youtube.com/watch?v=bJIgiBBx3lg">
  <img width="380" src="https://img.youtube.com/vi/bJIgiBBx3lg/maxresdefault.jpg" alt="Watch MoonLight Demo">
</a>

**[▶️ Watch the functional overview](https://www.youtube.com/watch?v=bJIgiBBx3lg)**

MoonLight is an open-source lighting platform that scales from art installations to professional stages.

Drive lots of lights or thousands of LEDs with stunning effects running on ESP32 microcontrollers. For creators who demand professional results without enterprise costs. Scales from small installations to large displays.

<img width="163" src="https://github.com/user-attachments/assets/1afd0d4b-f846-4d5b-8cc9-8fa8586c405b" />

MoonLight consist of the following modules:

* [Lights control](https://moonmodules.org/MoonLight/moonlight/lightscontrol/)
* [Effects](https://moonmodules.org/MoonLight/moonlight/effects/)
* [Drivers](https://moonmodules.org/MoonLight/moonlight/drivers/)
* [Channels](https://moonmodules.org/MoonLight/moonlight/channels/)
* [Live Scripts](https://moonmodules.org/MoonLight/moonlight/livescripts/)
* [MoonLight info](https://moonmodules.org/MoonLight/moonlight/moonlightinfo/)

## MoonLight by MoonModules

MoonLight is created by [MoonModules](https://moonmodules.org/). MoonModules is a collective of light artist, hardware- and software developers. We love creating DIY lighting solutions based on microcontrollers, mainly ESP32. Our work can be found on GitHub and we use mainly [Discord]([Discord](https://discord.com/channels/700041398778331156/1203994211301728296)) for communications. We develop our products for a large community of light enthousiasts and make products we show to the world, e.g. on art-exhibitions, in venues, or at festivals. See also [about](https://moonmodules.org/about/).
Our flagship product is WLED-MM, a fork of WLED focussing on driving more LEDs, new platforms (e.g. ESP32-P4), better audio and so on. As currenyly most of the MoonModules team are also core developers of WLED, WLED-MM functionality is slowly moving to WLED.

Started in 2023 we are also developing a new product based on our experience working with WLED called MoonLight. MoonLight is a modern alternative to WLED, with a modern UI, supporting 1D, 2D and 3D layouts, effects and modifiers, live scripts and led drivers.

## Why MoonLight

So why build MoonLight when we already have WLED(-MM)?

This is a personal overview by me (ewowi). It’s not meant to burn down WLED, quite the opposite. I worked on it for years (mainly on 2D: first in WLED SR, then in WLED/WLED, and later in WLED-MM) and I was, and still am, deeply impressed with what it offers. But along the way I ran into some limitations that eventually convinced me to start MoonLight.

* **UI**: extending or modifying the UI is difficult, not only because the code is complex and non-intuitive, but also because of memory and flash constraints. On top of that, WLED has two UI systems: one for the main screen, and another for setup/configuration, which adds complexity. The UI stack is “classic” HTML, JavaScript, and CSS, without modern concepts like reactive frameworks.

* **ESP8266 support**: WLED still supports ESP8266, and that legacy support complicates the codebase a lot. MoonLight takes a different stance: if you need ESP8266, use an older WLED release. But don’t hold back development for it, most advanced features won’t fit on the ESP8266 anyway.

* **Outdated libraries**: WLED relies on older versions of FastLED, ArduinoJson, ESP-IDF, and others. MoonLight’s philosophy is to keep libraries up to date. “Don’t change what works” only delays the inevitable, you’re forced to upgrade eventually.

* Effects and fixture **dimensions**: WLED effects are designed around 1D and 2D, with a lot of hardcoding. MoonLight supports up to 3D at the core, with no hardcoded constraints. An effect doesn’t “know” the fixture or its size, so you can run a 1D effect on 3D, or 2D on 1D, without issues. MoonLight doesn’t use effect IDs, an effect is a class (Node). Modifiers, layouts, and drivers are also Node types. This eliminates hacks like shared data, limited effect controls, effect metadata, virtual strips, expand1D, etc. (some of which I originally introduced in WLED, sorry 😜).

* **Mapping system**: WLED’s mapping is based on strips and panels, and requires hacks to handle shapes like rings or hexagons. 3D isn’t supported. MoonLight uses “inverse mapping”: instead of placing LEDs on a grid, you just define their coordinates, and the system maps everything internally.

* **Technical debt**: WLED has accumulated a lot of code that “just evolved.” Audio reactive is the clearest example. It’s hard to maintain, and in practice only a few people really can. WLED started with a strong architecture, but that hasn’t evolved much over time. (Yes, I share some of the blame 🙈)

* **Too big** to change: WLED has grown so large, with so many users, that making deep architectural changes is nearly impossible. The last major thing I worked on in WLED (2023) was “SuperSync”, linking multiple microcontrollers to drive a single fixture, inspired by drone swarms. I quickly hit the system’s boundaries, and the plans we had seemed unachievable within WLED.

* What I love most in WLED: **UserMods** ❤️! Small, isolated pieces of code you can enable or disable, each with its own UI integration. This makes WLED incredibly versatile, with a huge library of UserMods. MoonLight originally was named “StarMod” (or *Mod: everything as a module). That idea evolved into the Nodes class: self-contained objects that initialize, run, expose controls, and can be destructed.

## Current status

As of fall 2025, MoonLight can best be described as having its foundation solidly in place.

Earlier releases involved major structural changes that often required fresh installs or renaming. Future updates should generally work seamlessly without erasing existing settings, though major version upgrades might still include exceptions.

MoonBase now serves as a flexible foundation for any IoT project, while MoonLight provides a ready-to-use, plug-and-play application built on top of it.

That doesn’t mean the work is done, quite the opposite. This is just the beginning. MoonLight began as a solo project, but it’s open for collaboration. You’re warmly invited to help shape its future; check out the [Develop](https://moonmodules.org/MoonLight/develop/) pages to learn how to get started.

### What has been done so far

MoonBase

* ESP32-SvelteKit foundation (Svelte framework, PhysicsHttp, Stateful service, Event Sockets, Restful API, MDNS, MQTT)
* [Modules](https://moonmodules.org/MoonLight/develop/modules/): Generic building block to create server and UI functionality which can be activated through the menu by means of a json definition.
* [Nodes](https://moonmodules.org/MoonLight/develop/nodes/)🥜: Generic building block to setup and run specific code and shows controls in the UI.
* Utilities (System logging, 3D coordinate system, String Management, Memory Management)
* Functionality, See [MoonBase](https://moonmodules.org/MoonLight/moonbase/) overview
    * Sveltekit: Connections, WiFi, System. 
    * MoonBase: Files, Tasks, Devices, IO.

MoonLight 

* [Layers](https://moonmodules.org/MoonLight/develop/layers/): Physical and Virtual layers. Physical layer manages the real physical Lights or LEDs (using Layouts and Drivers). Multiple (currently 1) virtual layers (effects & modifiers) are mapped to the physical layer.
* Functionality, See [MoonLight](https://moonmodules.org/MoonLight/moonlight/) overview
    * Control, Channels and Info
    * Effects & Modifiers
    * Layouts & Drivers.
        * DMX / Art-Net out
        * WLED Audio Sync

### Looking ahead

MoonBase

* ESP32-P4 support
* MIDI support (light control desks) 
* Ethernet
* Live Scripts, see [Live Scripts](https://moonmodules.org/MoonLight/moonbase/module/liveScripts/), included in MoonLight
* SuperSync 🥜: See above: development basically stopped in 2023 but the ideas are still there: device groups, sync effects, distributed effects.
* HA / MQTT
* UI enrichments
* Specific board profiles, E.g. see [Premade boards](https://moonmodules.org/MoonLight/gettingstarted/hardware/#premade-boards)

MoonLight 

* Multiple Layers
* DMX / Art-Net in
* Virtual driver
* Effects: Limited nr of effects but easy to add (tutorial)
    * Help needed!
    * FastLED
    * Animartrix
    * Physics
    * Live Scripts

### Where you can help

* Add more effects (FastLED, WLED, Particle system, Animartrix, Soulmate lights, ...), modifiers, layouts and drivers
* Improve the [MoonLight Installer](https://moonmodules.org/MoonLight/gettingstarted/installer/)
* Improve the UI (Svelte 5, DaisyUI 5, TailWind 4)
  * e.g. File Manager, Multi row layout, Monitor (WebGL)
* Tune FastLED, add FastLED 2D effects
* CI automation (Generate daily builds in Github and use by installer)
* Palettes: more, custom, audio reactive
* ...

Contact us in the MoonLight channels on [Discord MoonModules](https://discord.gg/TC8NSUSCdV)

## Emoji coding

Effects, Modifiers, Layouts and drivers use emoji's to visualize their usage. See below for an overview:

* 🔥 Effect
    * 🐙 WLED origin
    * 💫 MoonLight origin
    * ⚡️ FastLED origin
    * 🚨 Moving head color effect
    * 🗼 Moving head move effect
* 💎 Modifier
* 🚥 Layout
* ☸️ Driver

* ♫ Audio reactive FFT based
* ♪ Audio reactive volume based
* 💡 supports up to 0D
* 📏 supports up to 1D
* 🟦 supports up to 2D
* 🧊 supports up to 3D
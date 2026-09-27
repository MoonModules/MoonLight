# MoonLight

High-performance LED &amp; DMX lighting control for ESP32 and beyond.

[:material-flash: Flash an ESP32 from your browser](/MoonLight/install/){ .md-button .md-button--primary } &nbsp; [:material-github: GitHub](https://github.com/MoonModules/projectMM){ .md-button }

!!! tip "New here?"
    The [Getting started](gettingstarted.md) guide walks you from a blank ESP32 to your first running light show, step by step, with no build tools required.

## What it is

MoonLight drives large LED installations and DMX fixtures. You build a light show by stacking simple blocks: a **layout** (how the LEDs are arranged), one or more **effects** (what they animate), **modifiers** (mirror, rotate, mask…), and a **driver** (how the pixels reach the hardware). Every setting takes effect live; there is no reboot to apply a change.

One source tree drives ESP32, Teensy, Raspberry Pi, macOS, Windows and Linux.

## Find your way

<div class="grid cards" markdown>

-   :material-download: **Get running**

    Flash a board from your browser and light your first pixels.

    [Getting started](gettingstarted.md) · [Web installer](/MoonLight/install/)

-   :material-palette: **Build a show**

    Browse the effects, layouts, modifiers, and drivers you compose into a show.

    [Effects](moonmodules/light/effects.md) · [Layouts](moonmodules/light/layouts.md) · [Modifiers](moonmodules/light/modifiers.md)

-   :material-check-decagram: **See what's verified**

    Every behavior is pinned by a test. When a bug is fixed, a test proves it.

    [Unit tests](reference/tests/unit-tests.md) · [Scenario tests](reference/tests/scenario-tests.md)

-   :material-code-braces: **Go deeper**

    System design, the module model, and the per-module reference.

    [Architecture](explanation/architecture/index.md) · [Core modules](moonmodules/core/supporting.md) · [Light pipeline](moonmodules/light/supporting.md)

-   :material-speedometer: **Numbers and people**

    Measured frame rates per device, how the project works, why the code is ours, and who inspired what.

    [Performance](reference/performance.md) · [Why our own code](explanation/why-we-write-our-own.md) · [How we work](https://github.com/MoonModules/projectMM#how-we-work) · [Credits](https://github.com/MoonModules/projectMM#credits)

</div>

The web installer works in Chrome &amp; Edge (Web Serial), with no download required.

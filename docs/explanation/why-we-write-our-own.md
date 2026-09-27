---
title: Why we write our own code
---

# Why we write our own code

MoonLight pulls in no third-party libraries: no FastLED, no ESPAsyncWebServer, no ArduinoJson. A library that is genuinely needed lives behind the platform boundary in `src/platform/`, never in core or the light domain. The *what*, with the replacement for each, is [below](#third-party-libraries). The *why* comes first.

## A dependency is a hole in the test coverage

Every dependency is a part of the system you can observe but cannot reason about. You can test *around* it, feeding it inputs and checking what comes out, hoping the middle behaves. You cannot test *through* it: you cannot force it into the state you need, make it fail on demand to see what your code does next, or instrument what it does under memory pressure.

On an embedded target that is exactly backwards. The failures that matter here are timing, fragmentation, and behavior that only appears after four hours of running. Testing around a black box puts the interesting failures precisely where you cannot look.

When everything is ours, the [test suite](../reference/testing.md) reaches the whole stack with nothing exempt. That is what makes the [regression rule](../reference/testing.md) affordable: when a bug is found, the fix includes a test that reproduces it, with the root cause named in the test. Regression stops being something to hope about and becomes something to eliminate. The same holds for memory behavior, timing, and what the platform layer does when something goes wrong: when it is all ours, *why did it do that* is always a question with an answer.

The trade is real. A much larger surface to maintain, every bug ours, and nobody upstream fixing things while we sleep. A bigger surface that can be tested completely is still easier to live with than a smaller one with holes in it.

## Why this became possible

This is not a decision that could have been made a few years ago, and it is not the result of better judgment.

Writing your own version of a mature library used to be irrational for a project this size. Not impossible: irrational. The budget was evenings, and libraries exist precisely to buy time that is not there. Taking the dependency was the correct call, and it was taken, repeatedly, for years.

What changed is the effort of writing code, and what changed it is AI agents. That is the whole reason. A rebuild that would have been years of Saturdays became something worth attempting, and the architecture that follows from full ownership, testable end to end with no black boxes, became reachable rather than theoretical. How that work is run, and the rules the agents work under, is in [Principles & process](../contributing/principles-and-process.md).

## Why agents at all

Using agents to build open-source software is contested, and a page that credits them with making this project possible cannot reasonably skip past that.

AI agents are a new way of working, and a genuinely different one rather than a faster version of what came before. We have worries about AI as it stands, and it is also not going away. Having followed it since the eighties, we would rather understand it than pretend it is absent.

So the position is neither enthusiasm nor resignation. A tool this consequential is better understood by using it seriously than by arguing about it from outside. Learning what one does, where it is strong and where it quietly fails, means running a real project on it. It also means holding that project to a standard that would expose the failures. What follows is that standard, and what it costs.

## The limits of this argument

It is not a verdict on the libraries we moved away from. They work, they have thousands of users, and they were built by people solving real problems on hardware we have never touched.

It is also not arms-length criticism. We built, maintained and contributed to the projects this one descends from, and the code we spent years inside was written by other people *and by us*. Those lessons are recorded in [what we built](../work/past/index.md).

And it is not a general recommendation. No-dependency is right for *this* project because of what this project is for: total control of the target, and a test system with no blind spots. For most software it would be a bad trade.

## Good theft and bad theft

Austin Kleon's *Steal Like an Artist* has a chart worth borrowing. Good theft: honor, study, steal from many, credit, transform, remix. Bad theft: degrade, skim, steal from one, plagiarize, imitate, rip off. His own test is whether the person you stole from would shake your hand if you met them in a stalled elevator.

Writing your own implementation of a known idea can land in either column, and which one has nothing to do with the tools used to type it. Four rows carry the weight here.

**Study, not skim.** This is the row that AI agents genuinely threaten, and it is worth naming rather than glossing. An agent can reproduce a working pattern without anyone involved understanding why it works, which is skimming with better output. The countermeasure is structural: each feature is spec'd from the primary source, the datasheet, the standard, the textbook algorithm, before it is written, and every line and every spec is reviewed. If the reasoning behind a piece of code cannot be stated, it does not go in. That standard is more work, not less.

**Ideas, not code.** We are not trying to acquire anyone's implementation. What travels is the idea: an approach to a problem, a technique someone proved works on real hardware, a mistake worth not repeating. Most of what MoonLight implements is publicly defined. Art-Net, E1.31/sACN, DDP, WS2812 timing, the peripheral datasheets, textbook DSP: those are industry standards, not anyone's property, and we implement them from the primary source. Textbook algorithm, textbook name, our implementation.

**Steal from many, not one.** A rewrite that is one library with the names changed is a rip-off, whoever or whatever typed it. What is here comes from several sources, from the standards themselves, from what this hardware forces on you, and from years of our own prior work.

**Transform, not imitate.** The architecture is not the old design retyped. Full testability, a [single module model](architecture/moonmodule.md) and [live reconfiguration](architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame) force a different shape; an imitation could not have satisfied them.

Credit is the fifth row, and it needs care for a mechanical reason: rewriting removes the easiest form of attribution there is. Take a dependency and the author's name appears in the manifest automatically, as a side effect of the build. Write it yourself and that disappears, even when the idea, the approach or the algorithm came straight from someone else's work. So it has to be deliberate: named in the README's Credits, named in each module's Prior art notes, named in the [friend-repo digests](../friend-repos/index.md), in the place where it can be checked against the source.

If something here came from your work and is not credited where it should be, [open an issue](../how-to/logging-an-issue.md) or find us on [Discord](https://discord.gg/TC8NSUSCdV). We would much rather hear it directly.

## ESP-IDF directly, rather than Arduino

The ESP32 target uses ESP-IDF directly for three reasons:

- **Direct hardware control.** RMT peripheral for LED protocols, FreeRTOS task pinning with explicit stack sizes, `heap_caps_malloc` with SPIRAM/8BIT caps, `esp_timer` microsecond timing. Arduino wraps these with abstractions that add overhead and hide control.
- **Native CMake.** ESP-IDF's build system *is* CMake (`idf.py` wraps it). No impedance mismatch. Arduino-on-ESP-IDF adds a compatibility layer that complicates the build.
- **Version stability.** ESP-IDF APIs are stable. Arduino-esp32 version churn caused recurring breakage in MoonLight.

Arduino can be added as an ESP-IDF component later if a specific Arduino library is needed; this is officially supported by Espressif and doesn't require restructuring.

## Third-party libraries

The platform abstraction layer replaces what libraries typically provide. Today no third-party libraries are pulled in:

| Library | Why not | What replaces it |
|---|---|---|
| [FastLED](https://github.com/FastLED/FastLED) | Arduino-dependent. LED protocol drivers (RMT, SPI) are available natively in ESP-IDF; FastLED's color math is small enough to reimplement. | Own color math in core. Own LED drivers per platform in `src/platform/`. |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | Arduino-dependent. Past memory-leak issues. Ties us to Arduino. | Own HTTP server via ESP-IDF's `esp_http_server` (ESP32) or BSD sockets (desktop). Reconsider if Arduino-as-component is added. |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | Works on ESP-IDF, but heavy: dynamic allocation, large footprint. | Own `JsonSink`, which serves both the API and persistence: each module's state is written as a flat `/.config/<TypeName>.json` and read back through `loadSubtree`. |

When a library is genuinely needed (e.g. FastLED for specific hardware support), it lives inside `src/platform/` and is not referenced from core or light-domain code.

# Plan: HUB75 on a Raspberry Pi, native outputs

Plan of 2026-10-30, a Feature, rewritten on 2026-11-16 for the native route the product owner chose in the [Pi HUB75 thread](https://discord.com/channels/700041398778331156/1571096940576510053). A Raspberry Pi drives HUB75 panels from its header through MoonLight's own outputs of `Hub75Driver`: the RP1's PIO on a Pi 5 and a CPU bit-bang engine with a PWM-timed OE on a Pi 3 or 4. The work lands as five pull requests, each reviewed alone, each bringing its own benchmarks.

## The route

- **Native, our own code.** Reference, don't copy ([coding standards](../contributing/coding-standards.md)): the Pi 5 PIO output speaks the kernel's own `/dev/pio0` ioctls, with no library; a feature library such as rpi-rgb-led-matrix is what MoonLight writes itself.
- **The library route is dropped.** rpi-rgb-led-matrix vendors Adafruit Piomatter's PIO program (GPL-2.0-only) for its Pi 5 backend, so a Pi build with it cannot ship ([Piomatter #95](https://github.com/adafruit/Adafruit_Blinka_Raspberry_Pi5_Piomatter/issues/95)), and a second output for the same panels would be two mechanisms for one job. The Matrix output and its RGB matrix seam leave the tree of the fork's `moonpi` branch, and upstream never gets them.
- **Credits.** hzeller's Pi 1 to 4 design (row order, PWM pulser, Stripe mapping), and the BSD-3-Clause `hub75.pio` programs of pico-examples and pimoroni-pico that the Pi 5 PIO program follows, are each credited in `@attribution` and on the [attributions page](../reference/attributions.md); any code adapted from hzeller's files is rewritten as our own before PR 4.
- **Dithering and the dark end are the native outputs' own.** The GPIO output carries the fraction each pixel loses to its next refresh (`carryBits`); the PIO output shows 8 real planes. The correction handing more than 8 bits to drivers that take them serves both.

## What exists

- **`moonpi`** (on upstream `main` 9990420d): the PIO and GPIO outputs and engines, the carry, the shared encoder at `src/platform/Hub75Encode.h`, the bench record (hub75-bench.md, arriving with PR 3), all documentation checks at zero errors and no rise; desktop 3,622/3,622.
- **On the bench:** the Pi 5 wall (256x192, six 128x64 panels) on PIO and the Pi 4 sign (96x64, Stripe) on GPIO, readings in the bench record.

## Steps

1. ✅ **PR 0, the add retry** ([#249](https://github.com/MoonModules/MoonLight/pull/249)): an add retries only on a taken name and shows the server's refusal. Browser-checked; merged (upstream e54825cf).
2. ✅ **This plan update**, before PR 1: delivered, read by the product owner's agent (seven points, folded in here).
3. ✅ **Before PR 1, the conditions:** on the fork's `moonpi` branch, `check_code` back to main's counts (1,607 findings, 710 duplicated blocks; the branch read 1,862 and 920), except 14 recorded HUB75 test pairs (1,618 findings against 1,607): the outputs' shared code pulled into their base, a shared test fixture, the engines' `double` to `float`; the Matrix output and its seam removed from that branch; the notes-style prose ✅ (the documentation sweep); the commit format ✅.
4. ✅ **PR 1, the shared encoder, alone** ([#254](https://github.com/MoonModules/MoonLight/pull/254)): merged (upstream 38db7ed9). `Hub75Encode.h` moved to `platform/`, so that platform code can encode (no platform file includes light, and the Pi engines that encode from `platform/rpi` arrive in PR 3); hold words let a narrow row's lowest plane light a whole word, with exact binary weights on a stretched row and the rounding of the pass on a wider one, up to 8 planes. The ESP32 port keeps its 6-bit cap, for the reason beside `kHub75PortMaxBitDepth` in `Hub75Bus.h`; 8 bits on an ESP32 stays with the card "Repeated planes: full top brightness and 8-bit depth", which this PR names as its next step. Verified on an S3 with 16- and 32-column layouts at 6 bits (ewowi's MoonHub75 can run it too), with the cost of 33 words a pass instead of 16 on a 16-column row at 6 bits measured there; a 32-column row is unchanged. Docs: hub75-refresh.md (the plane weights and the hold words).
5. 🧪 **PR 2, the output strategy:** in progress. `Hub75Driver` drives its panels through an output strategy, the ESP32 bus output its first strategy and the one both of main's backends (LCD_CAM and Parlio) use; the strategy is to be picked by backend, which is where the Pi outputs plug in. Frames byte-identical on the S3. Docs: the Hub75Driver header and its generated help.
6. ⏳ **PR 3, the Pi 5 PIO output:** `Hub75PioOutput` and its engine on the rp1 backend, which issues `/dev/pio0`'s ioctls itself (the structs redeclared from the kernel's `rp1_pio_if.h`), so every Linux aarch64 build has the PIO output and needs no library, submodule or package. In this PR's files: the pico-examples and pimoroni-pico credits, and the British spellings (`colour`) and the `piollib` file name fixed. Pi 5 suite, the wall. Docs: hub75-panels.md (the shared Pi wiring, the Pi 5 section), hub75-bench.md (the Pi 5 readings), attributions.md.
7. ⏳ **PR 4, the Pi 3/4 GPIO output:** `Hub75GpioOutput` and its engine with Stripe, credited to hzeller's design. The carry (`carryBits`, a first-order temporal dither) is an addition this PR names, with its own tests. In this PR's files: the British spellings (`kColourSlotBits`, `kGpioColourMask`, the `colour` members), the GPIO engine's literal stretch factor and the GPIO stub's literal 500 fixed. The sign. Docs: hub75-panels.md (the Pi 3/4 section), hub75-bench.md (the Pi 4 readings), hub75-test-pattern.md, attributions.md.

## Benchmarks each PR brings

| PR | Measured |
|--|--|
| 1 | S3: refresh and encode time at 6 bits, the 16-column row's 33-word cost, binary size |
| 2 | S3: frames byte-identical before and after, refresh unchanged |
| 3 | Pi 5 wall: refresh by bit depth and pixel clock, encode time, CPU load, memory per frame buffer, binary size |
| 4 | Pi 4 sign: refresh by bit depth, slowdown and LSB, the carry's price, CPU load, memory per frame buffer |

## What comes out

On the fork's `moonpi` branch: the library route and its plan, the Matrix output, the RGB matrix seam, the rpi-rgb-led-matrix fetch at configure time, and the brightness thread's C2 (the library's CIE table).

## Backlog

- Mappings beyond Stripe, and panel init (FM6126A and kin), for the native outputs.
- The correction handing more than 8 bits to drivers that take them: a card to file.
- The Pi's DPI video output as a DMA path to HUB75 (the product owner's design of 2026-11-07), once a plan.

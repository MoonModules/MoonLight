# Plan — Native HUB75 output: drive panels directly, without a receiving card

Answers [issue #102](https://github.com/MoonModules/projectMM/issues/102). Phase 1 only; Phase 2 (PWM panels) is deliberately out of scope and § "What this plan does not do" says why.

## The gap

MoonLight drives HUB75 panels one way: [PanelCardDriver](../../../src/light/drivers/PanelCardDriver.h) emits ColorLight frames over raw Ethernet to a 5A-75B/E receiving card, which does the HUB75 driving. That path is excellent above roughly 16,384 pixels and it is what the [panel-cards guide](../../how-to/panel-cards.md) documents.

Below that size it is the wrong shape. The user buys a receiving card, configures it with LEDvision (a Windows tool, not ours), and runs a dedicated Ethernet link, all to light one 64×64 panel that a $10 board could drive from its own pins. WLED does this natively; MoonLight does not. That is the gap, and it is the whole of Phase 1.

## What decides the design: two hardware ceilings

A HUB75 panel is **scanned, not addressed**. One frame is `rows/2` scan lines (RGB1 and RGB2 drive both half-panels at once), each line clocked out as `width` parallel words, and brightness comes from repeating the whole thing once per **bit plane**. So the DMA buffer is:

```
frame bytes = (height / 2) × width × bitDepth
```

Measured against the peripherals MoonLight already has:

Measured from the encoder's own formula (`Hub75Geometry::frameBytes`, pinned by [unit_Hub75Slots.cpp](../../../test/unit/light/unit_Hub75Slots.cpp)), at 1/32 scan:

| Panels | Geometry | 6-bit | 8-bit | Parlio (65,535 B cap) | i80/LCD_CAM (PSRAM) |
|---|---|---:|---:|:--|:--:|
| 1 | 64×64 | 12,480 B | 16,640 B | ✅ both | ✅ |
| 4 | 128×128 | 49,344 B | 65,792 B | ✅ 6-bit, ❌ 8-bit | ✅ |
| 16 | 256×256 | 196,800 B | 262,400 B | ❌ | ✅ |

**Four panels at 8-bit misses the Parlio cap by 259 bytes.** That is worth stating precisely rather than as "borderline": a user with four panels gets full depth on an S3 and 6-bit on a Parlio-only chip, and the driver picks the backend that can carry what they asked for.

**This is the fact the issue does not account for.** #102 proposes the P4-Nano as lead candidate because Espressif's reference uses PARLIO, but [platform_esp32_parlio.cpp:238](../../../src/platform/esp32/platform_esp32_parlio.cpp) records the hardware cap: `kParlioMaxTransferBytes = 0x7FFFF / 8` = 65,535 bytes, width-invariant. PARLIO carries one panel comfortably, reaches its limit at four, and cannot do sixteen at any useful depth.

The route that scales is the one the [shift-register analysis](../future/shift-register-driver-analysis.md) already established for a structurally identical problem: **i80/LCD_CAM, whose DMA reaches PSRAM**, proven on this exact path to 16,384 lights. `hasLcdCam` is true on S3, P4 and S31.

### GPIO requirements

HUB75 is pin-hungry in a way the memory table hides. One port needs:

| Group | Lines | Count |
|---|---|---:|
| Colour | `r1 g1 b1 r2 g2 b2` | 6 |
| Row address | `a b c` (1/8 scan), `+d` (1/16), `+e` (1/32) | 3-5 |
| Control | `clk lat oe` | 3 |
| | **Total** | **12-14** |

Against the per-chip free sets in [gpio-usage.md](../../reference/hardware/gpio-usage.md), which exclude flash/PSRAM, input-only, USB, UART0 and strapping pins:

| Chip | Usable output GPIOs | 1/16 scan (13 pins) | 1/32 scan (14 pins) |
|---|---:|:--:|:--:|
| **ESP32-S3** (N16R8) | 16 | ✅ 3 spare | ✅ 2 spare |
| **ESP32-P4** (P4-NANO) | 20 | ✅ 7 spare | ✅ 6 spare |
| **ESP32-S31** | board-specific | take from the [coreboard reference](../../reference/hardware/esp32-s31-coreboard.md) | |
| **ESP32 classic** | 13 | ❌ | ❌ |

**This settles the target list, and it removes a chip the issue implies.** The classic ESP32 has 13 usable output GPIOs and would need every one for a 1/16-scan port, leaving nothing for the LED strand, a button or a mic; a 1/32 panel does not fit at all. gpio-usage.md already records the same squeeze for 16-lane i80 ("a 16-lane set must borrow strap pins, and GPIO 12 is the flash-voltage strap: driving it at reset can brick the boot"). So HUB75 is an **S3, P4 and S31 feature**, and the classic is out on pins before memory is even considered.

Two consequences for the plan:

- **The board catalog entries in step 5 are load-bearing, not polish.** An S3 has exactly 3 spare pins at 1/16 scan, so a user picking 13 by hand will land on octal-PSRAM (33-37), USB (19-20), UART0 (43-44) or a strap (0, 3, 45, 46). [PinsModule](../../moonmodules/core/system.md) flags a conflict after the fact; a catalog entry prevents it.
- **Defaults stay unset.** Even with the pins available, a default would guess the user's wiring. Same rule the LED drivers follow, and the P4's own history is the argument: its first LED-driver default landed on strapping pins ([lessons.md](../past/lessons.md)).

### Refresh

The second ceiling is **refresh rate**, and it is the one users will feel. Every bit plane costs a full scan pass, so depth trades directly against flicker:

```
line time  = width x clock period + latch/OE overhead
frame time = (height / 2) x bitDepth x line time
refresh    = 1 / frame time
```

Predicted at a 20 MHz shift clock (the rate the i80 path already runs, per the [shift-register analysis](../future/shift-register-driver-analysis.md)), ignoring latch overhead, so these are ceilings rather than promises:

| Geometry | 4-bit | 6-bit | 8-bit |
|---|---:|---:|---:|
| 64×64 (1 panel) | 2441 Hz | 1628 Hz | 1221 Hz |
| 128×128 (4) | 610 Hz | 407 Hz | 305 Hz |
| 256×256 (16) | 153 Hz | 102 Hz | 76 Hz |

**The arithmetic says refresh is not the wall it looked like.** Even 16 panels at 8-bit clears 76 Hz before overhead, and flicker becomes visible somewhere below about 60 Hz. What will actually bite first is latch/OE overhead per line and the render cost of filling a 256 KB buffer every frame, neither of which this calculation includes. So the honest position is: **memory is the hard ceiling (§ above), refresh is a soft one**, and the numbers here are what a tester should compare their reading against.

`bitDepth` is a user control rather than a driver choice, because the tradeoff is theirs: the table is what makes it an informed one. And the driver **reports its achieved refresh in its status line**, so a discrepancy between this table and a real panel is visible to whoever is holding it.

## Design

### The driver

`Hub75Driver : DriverBase`, registered in [main.cpp](../../../src/main.cpp) beside the others, gated on `platform::hasLcdCam || platform::parlioLanes` so it is offered only where it can run — the same inert-on-wrong-chip rule [ParlioLedDriver](../../../src/light/drivers/ParlioLedDriver.h) follows. That gate happens to exclude the classic ESP32, which § GPIO requirements shows could not host a port anyway.

It is **not** a `LedPeripheral` backend. That seam is shaped around the parallel WS2812 orchestrator (`supportsPinExpander`, `lanesAvailable`, slot encoding), and HUB75 shares none of it: different wire protocol, different buffer shape, different control set. Reusing it would mean a base class whose every method one subclass ignores, which is the abstraction CLAUDE.md's minimalism rule exists to prevent. What they legitimately share is the **platform DMA seam**, and that is where the sharing belongs.

Controls, all defaulting to unset because a soldered pin must never be guessed ([lessons.md](../past/lessons.md)):

| Control | Why |
|---|---|
| `r1 g1 b1 r2 g2 b2` | The six colour lines |
| `a b c d e` | Row-address lines; `e` only on 1/32-scan panels |
| `clk lat oe` | Clock, latch, output-enable |
| `bitDepth` (2..8) | The refresh tradeoff, named and the user's to make |
| `scanRate` | 1/8, 1/16, 1/32 — panel-dependent, not derivable from size |
| `sPWM` | The [WLED-MM sPWM commit](https://github.com/MoonModules/WLED-MM/commit/778ae558) case |
| `peripheral` | Which silicon block drives the panel, where the chip has more than one |

**Geometry is not a driver control.** [PanelLayout](../../../src/light/layouts/PanelLayout.h) and [PanelsLayout](../../../src/light/layouts/PanelsLayout.h) already model a serpentine panel and an M×N tiling of them, with configurable axis order and per-axis direction. A layout emits coordinates and the driver owns pins: that separation is the architecture, and HUB75 must not re-litigate it. The issue asks for "UI features to set panel dimensions and arrangement", and the honest answer is that MoonLight already has them.

### The platform seam

Four functions, mirroring the shape the Parlio and i80 seams already use:

```cpp
bool     hub75Init(Hub75Handle& h, const Hub75Pins& pins, uint16_t width,
                   uint16_t height, uint8_t scanRate, uint8_t bitDepth);
uint8_t* hub75Buffer(const Hub75Handle& h, uint8_t buffer);
bool     hub75Transmit(Hub75Handle& h, uint8_t buffer);
void     hub75Deinit(Hub75Handle& h);
```

Two backends behind it, and **the user picks which**, through a `peripheral` select exactly like the one [ParallelLedDriver](../../../src/light/drivers/ParallelLedDriver.h) already carries. Each backend self-registers at static init, and `main.cpp` links it only where its SOC macro says the silicon exists, so the dropdown's options are per-chip rather than a fixed list: LCD_CAM on an S3 or S31, LCD_CAM and PARLIO on a P4, nothing on a classic.

**An earlier draft of this plan had the platform choose, on memory alone. That was wrong, and the reason is contention rather than capacity.** A P4 has both peripherals and only one of each. A user driving WS2812 strips from PARLIO needs HUB75 on LCD_CAM; another user wants the reverse. Both are correct, the difference is what else is plugged into that board, and the platform cannot know it. The sibling claim guard stops two drivers colliding on one block, but it cannot guess which driver should win — that is the user's call, and a select is how the repo already asks it.

The default is whichever backend fits the geometry, so a fresh driver works without a decision. What the plan does NOT do is silently downgrade: four panels at 8-bit is 65,792 bytes against PARLIO's 65,535-byte cap, and the driver says so rather than quietly dropping to 6-bit.

A desktop stub returning false from everything keeps `mm_tests` and every non-HUB75 target compiling untouched.

### The encoder

The one genuinely new piece: rendered RGB → bit-plane-major scan buffer. Per bit plane, per scan row, pack six colour bits per pixel-column into the parallel word alongside the row address and the latch/OE timing.

This is the same class of work as [ParallelSlots.h](../../../src/light/drivers/ParallelSlots.h) (serialise a frame through a parallel bus under a DMA ceiling) and is testable the same way: a host unit test that walks a known pattern through the encoder and asserts the exact bytes, including a walking-one per colour line. The '595 investigation's hardest lesson was that an encoder proven byte-for-byte on the host still left a wall-visible artifact, so the test pins the encoder and hardware verification stays the PO's.

## End states considered

**A. i80/LCD_CAM only.** Simplest, scales to 16 panels, covers S3/P4/S31. Loses the C6/H2 class entirely and ignores the issue's own P4 reference.

**B. PARLIO only.** Matches the issue's stated direction and Espressif's example, and is the simpler peripheral. Caps at ~4 panels, which forecloses the size range that makes the feature worth having.

**C. Both, behind one platform seam.** *(chosen)* Costs one extra implementation. Buys the full size range, keeps the driver ignorant of the peripheral, and means the ceiling moves when silicon does.

**D. A `LedPeripheral` backend.** Maximum apparent reuse. Rejected: the seam is WS2812-shaped and HUB75 would ignore most of it, producing exactly the abstraction that has to be unpicked later.

## Steps

1. **Platform seam + LCD_CAM implementation.** `hub75*` in `platform.h`, an S3/P4/S31 implementation, a desktop stub. Verified by a build on every target.
2. **The encoder, with its host test.** Bit-plane packing pinned byte-for-byte, walking-one per colour line.
3. **`Hub75Driver`.** Controls, pin claims through the existing `PinsModule` registry, status line reporting geometry and achieved refresh, registration in `main.cpp`.
4. **A backend registry, then the PARLIO backend.** The seam built in step 1 picks its peripheral internally; this turns it into an interface the driver selects, mirroring `LedPeripheral` without reusing it (that one is WS2812-shaped — same pattern, own interface). Then PARLIO behind it, and the `peripheral` select on the card. This REVISES step 1 rather than only adding to it.
5. **Board catalog entries**: Adafruit S3 Portal, Waveshare RGB Matrix, Lilygo S3, ESP32-S3-16MB-PSRAM. Catalog data in `deviceModels.json`, not driver work, and it is what makes the feature findable.
6. **Docs**: a how-to beside [panel-cards.md](../../how-to/panel-cards.md), the driver's catalog card, and a line in panel-cards.md saying which of the two paths a given wall wants.

Steps 1-3 are the shippable unit. Steps 4-6 each stand alone.

## Verification

**No bench session gates this.** The PO has no HUB75 panel, and buying one to prove a driver that users will test for free is the wrong order. So the driver ships behind the same rule every other unproven path follows: it is offered only where it can run, it degrades visibly, and it says what it is doing.

- **Host, and this is the whole of the pre-merge gate**: encoder byte-for-byte (walking-one per colour line, per bit plane), driver control and claim behaviour, a scenario pinning tick cost and memory. All of it runs on a desktop with no panel attached.
- **The driver measures itself.** Its status line reports geometry, `bitDepth`, and the **achieved** refresh in Hz. That is a few lines of code and it is what makes community testing useful: a report reading "128×128, 8-bit, 87 Hz, flickers" is a data point, where "it flickers" is not.
- **The docs carry the predicted refresh** (§ Refresh, computed not measured), so a tester can see whether what they observe is the predicted ceiling or a defect. A prediction that turns out wrong is itself a useful report.
- **Community testing is the hardware gate**, after merge. The board catalog entries (step 5) exist to make that possible: a tester should not have to pick 13 pins correctly before the driver can be judged.

What would make Phase 1 *proven* rather than shipped: a 128×128 wall at 8-bit depth, flicker-free, reported by someone who is not us. Until then the driver's docs say it is new and what is untested about it.

## Risks

- **Refresh at depth may disappoint on 16 panels.** The encoder cost is linear in bit planes and the scan is serial. **Settled by publishing the prediction, not by measuring first**: § Refresh computes the ceiling per geometry and depth, the driver reports what it actually achieves, and the docs name PanelCard as the path above whatever that turns out to be. A user meets the limit with an explanation rather than a surprise.
- **The [48×256 white-flash](../future/backlog-light.md) is open on the i80 path.** Six theories ruled out, cause unfound. A HUB75 driver rides the same peripheral, so it may inherit it. **Settled by shipping and watching**: Phase 1 targets 1-4 panels, a size at which the flash has never been seen, so it does not block. If a tester on a large HUB75 wall reports it, that is the seventh theory the backlog entry is missing — the flash following a non-WS2812 encoder through the same peripheral would exonerate the WS2812 slot encoding, which six hardware tests could not do.
- **Pin count is the tightest constraint on the classic ESP32 and shapes the target list.** See § GPIO requirements: the classic cannot host a full port without a strap pin, so it is excluded rather than half-supported.

## What this plan does not do

**Phase 2 (PWM panels) is not a continuation of Phase 1.** The issue lists it as the next step, but PWM panels need per-panel calibration data and a different drive model; `rpi-rgb-led-matrix` achieves it with a Pi's GPIO and a real OS. It is a separate project with its own spec, and bundling it here would make Phase 1 unshippable. Revisit once Phase 1 is on real walls.

**No second HUB75 port.** One port, one chain. A second doubles the pin cost and the DMA, and nothing in the issue asks for it.

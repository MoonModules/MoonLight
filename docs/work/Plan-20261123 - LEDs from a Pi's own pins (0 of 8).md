# Plan: LEDs from a Pi's own pins

Plan of 2026-11-23, a Feature, for approval. **Goal: a Raspberry Pi 3, 4 or 5 drives WS2812 strips from its own header, straight or through the 74HCT595 pin expander, with the parallel LED driver ESP32 and Teensy boards use; and HUB75 on every one of them.** The backlog card "LED output from a Raspberry Pi's own pins" (robwomp, 2026-10-11) asked for the strips.

**Starts once [HUB75 on a Raspberry Pi](Plan-20261030%20-%20HUB75%20on%20a%20Raspberry%20Pi.md) is complete** (product owner, 2026-11-24): the Pi 5 route builds on its PIO engine, which opens `/dev/pio0` and issues the kernel's ioctls itself ([#258](https://github.com/MoonModules/MoonLight/pull/258)), and the Pi 3/4 route on its GPIO backend's `/dev/mem` mapping and Pi detection ([#267](https://github.com/MoonModules/MoonLight/pull/267)).

## What exists

- **`ParallelLedDriver` drives parallel WS2812 through one backend chosen at runtime** (`LedPeripheral`, the Strategy pattern): i80, Parlio and LCD-MM on an ESP32, GPIO DMA on a Teensy. The driver owns the controls, the correction and the encode (`ParallelSlots.h`): each WS2812 bit becomes three slots, transposed so one slot carries a bit of every lane. A backend only brings the bus up, hands back its buffer, sends it and waits.
- **The pin expander is part of that encode.** With `pinExpander` on, each data line feeds a chain of '595 shift registers, eight strands per line; the slot stream gains the shift clocks, and `latchPin` is a data lane. Strands top out at 64, since the active-strand mask is a `uint64_t` (`kMaxStrands`).
- **On an ESP32 the expander's frame is the limit:** above about 96 lights per strand it no longer fits internal RAM, which the S3's DMA needs at 20–26.67 MHz (`ParallelLedDriver.h`, "Expander limits").
- **A Pi 5 drives HUB75 through the RP1 chip's PIO** (`src/platform/rpi/`): `Rp1Backend` loads a PIO program through the kernel's `/dev/pio0`, sets pins and clock, and feeds whole frames by DMA; `StubBackend` runs the same engine on a machine with no Pi. The card's prerequisite, a `src/platform/rpi/` backend, therefore exists.
- **HUB75 on a Pi 3 or 4** is [Plan-20261030 - HUB75 on a Raspberry Pi](Plan-20261030%20-%20HUB75%20on%20a%20Raspberry%20Pi.md), step 7: a CPU bit-bang engine with a PWM-timed OE, credited to hzeller's design, in review as [#267](https://github.com/MoonModules/MoonLight/pull/267), read on the Pi 4 sign.
- **`LedHwBlock::PiHeader`** names the Pi's header as one peripheral block, so a strip driver and a HUB75 driver on one Pi refuse to share it.

## The routes

| | Pi 5 | Pi 3, Pi 4 |
|--|--|--|
| Strips, straight | the RP1's PIO: an `out pins` program, one slot each 417 ns, 8 or 16 lanes | SMI, the Broadcom chip's parallel bus, fed by DMA: 8 or 16 lanes on its data lines (GPIO 8–23) |
| Strips, expander | the same program with the shift clock as side-set, up to 64 strands | SMI with its write strobe (GPIO 7) as the shift clock, up to 64 strands |
| HUB75 | the PIO output (exists) | the GPIO output, the HUB75 plan's step 7 |

SMI (Secondary Memory Interface) is the Pi 1–4's counterpart of the ESP32's i80 bus: a DMA-fed byte or halfword stream on up to 18 data lines, paced by a strobe, so it carries the same slot stream the encode makes. iosoft's rpi_pixleds (Jeremy Bentham) drives 16 parallel WS2812 strands with it and is study material, credited; rpi_ws281x's route (DMA through the PWM or PCM peripheral, two or one lanes) is the narrower alternative.

## End state

- `ParallelLedDriver` offers a `PIO` peripheral on a Pi 5 and an `SMI` peripheral on a Pi 3 or 4: 8 or 16 strands straight, or up to 64 through the pin expander, with the same controls, correction, encode and loopback test as on an ESP32.
- A Pi's memory holds an expander frame of any length, so strand length is bounded by the wire time (30 µs a light: about 550 lights a strand at 60 fps) and by what the bus carries, not by RAM.
- `Hub75Driver` runs on every Pi: the PIO output on a Pi 5, the GPIO output on a Pi 3 or 4.
- A host build without a Pi runs each backend against a stub, so the suite pins them.

## Steps

**Pi 5**

1. ⏳ **Measure the link to the RP1.** A PIO program that clocks a byte stream out of 8 header pins from DMA, through the existing device seam. The sustained MB/s it gives decides how far the expander goes: 16 straight lanes need about 4.8 MB/s (2.4 M slots a second, two bytes each), the expander at a 20 MHz shift clock about 20 MB/s. Recorded here before step 2 starts.
2. ⏳ **`PioPeripheral`, straight and expanded.** A `LedPeripheral` over the RP1 seam: `busInit` loads the program for the mode, `busTransmit` hands the encoded frame to DMA, `busWait` waits for the drain; the encode is unchanged, byte for byte, and the backend claims `PiHeader`. Tests: the stub's stream equals the encoder's output in both modes, the HUB75 driver and this one refuse to share the header.

**Pi 3 and Pi 4**

3. ⏳ **Measure SMI.** A DMA-fed SMI stream on 8 pins of a Pi 4 and a Pi 3, by the access route this step chooses: the kernel's `bcm2835-smi` driver (`dtoverlay=smi`, `/dev/smi`) or SMI and DMA registers mapped through `/dev/mem`, as rpi_ws281x does. The textbook choice is the kernel driver where its rate holds; the sustained rate and the strobe timing are recorded here.
4. ⏳ **`SmiPeripheral`, straight and expanded.** The same `LedPeripheral` contract on SMI, the write strobe as the expander's shift clock, the backend claiming `PiHeader`. The Pi 3 and 4 differ in the peripheral base address and the core clock, which the platform reads from the device tree. Tests as step 2, against an SMI stub.
5. ⏳ **HUB75 on a Pi 3 or 4**, the HUB75 plan's step 7, with one addition here: its refresh thread keeps a core of its own, so while it runs the render workers number the cores less two (`Effects::setup`, the layers plan).

**Every Pi**

6. ⏳ **The machine picks the backend.** `platform/machine.h`'s Pi cases offer `PIO` on a Pi 5 and `SMI` on a Pi 3 or 4 in the driver's `peripheral` list, and the ones the machine lacks are absent, as an ESP32 lists only its chip's.
7. ⏳ **On hardware.** A Pi 5, a Pi 4 and a Pi 3, each with a 74HCT245 level shifter, straight and then 64 strands through a '595 board; readings (frame time, CPU load, the longest clean strand) in the scenario's observations.
8. ⏳ **Docs.** The driver's guide, the Linux install page (the overlays a Pi 3 or 4 needs), the MoonWalk driver-setup step and the attributions page (iosoft, hzeller).

## Open questions

- Whether the triple HUB75 bonnet's buffered 5 V lines (eighteen data outputs on fixed pins) serve as strip outputs too, which would give a Pi level-shifted strips with no extra board; on a Pi 3 or 4 they would have to match SMI's data lines.
- Whether 64 strands is enough on a Pi, or the active-strand mask widens past `uint64_t` where the bus carries more.
- SPI as the fallback every model shares, one lane per bus (a Pi 4 has several through overlays), where neither SMI nor PIO is available.
- A Pi Zero 2 W has the Pi 3's chip and takes its route; a Pi 1, 2 or Zero is out of scope.

## What it removes

Nothing: it adds two backends behind an existing seam, and the card's prerequisite is already met by the HUB75 work.

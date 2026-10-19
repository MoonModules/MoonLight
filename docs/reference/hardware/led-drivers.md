# Choosing and wiring an LED driver

Addressable LEDs (WS2812B, SK6812, WS2811) go out through one of two drivers: [RMT LED](../../moonmodules/light/drivers.md#rmtled) for a few strands, and [Parallel LED](../../moonmodules/light/drivers.md#parallelled) for many. Both read the same shared buffer, so switching between them, or between Parallel LED's peripherals, is a change on the Drivers card with no reflash. The steps to add one, set its pins, count and brightness and test the wire are the process [Set up a driver or service for the hardware](../../moonwalk.md#driver-setup); the reference behind them follows.

## Which one

| Want | Use |
|---|---|
| A few strands, any ESP32 | **RMT LED**, the simple default |
| Many strands, up to 16 | Parallel LED on **`i80`**, the proven scale path |
| Up to 16 strands on a **P4** | Parallel LED on **`Parlio`**, which needs no clock pin |
| More lights than one DMA buffer holds, or more strands than you have GPIOs | Parallel LED on **`MoonI80`** |
| Up to 16 strands on a **Teensy 4.1** | Parallel LED on **`GPIO-DMA`**, the one peripheral there |

Start with `i80`. Choose `MoonI80` only when you hit one of those two limits. It drives the same pins the same way, streams the frame instead of holding it whole, and drives a 74HCT595 pin expander.

## Where each one runs

| Driver or `peripheral` | Chip | Strands |
|---|---|---|
| **RMT LED** | any ESP32 | one per RMT channel: 8 on the classic, 4 on an S3 or P4 |
| **`i80`** | S3, P4, S31, and the classic ESP32 | 1 to 16, or 8 per pin through an expander on the S3, P4 and S31 |
| **`MoonI80`** | S3, P4, S31 | 1 to 16, or 8 per pin through an expander |
| **`Parlio`** | P4 | 1 to 16 |
| **`GPIO-DMA`** | Teensy 4.1 | 1 to 16, on pins 0, 1, 14 to 27 and 38 to 41, in any order |

## Two limits

A strand holds at most **2,048 lights**, on every peripheral. On the classic ESP32 the `i80` DMA cannot reach PSRAM, so the whole frame must also fit internal RAM. Past either limit the driver idles with a status rather than crashing.

## Lane, pin, strand

A lane is one bus data line, a strand is one chain of LEDs. Wired directly, one pin is one lane is one strand. Through an expander, each data pin feeds one '595 and fans out to 8 strands, so 8 data pins reach the driver's ceiling of 64 strands.

## Wiring a 74HCT595 pin expander

On an S3, P4 or S31 with `i80` or `MoonI80` as the `peripheral`, `pinExpander` on and `latchPin` set, each data pin drives one register and its 8 outputs drive 8 strands.

The expander buys pins, not memory: the register is serial-in, so the DMA frame grows eightfold, about 1.1 KB per light. `i80` holds the whole frame, which past about 96 lights per strand falls to PSRAM too slow for the expander's clock. `MoonI80` streams the frame and drives 128 reliably.

## Testing the wire with the loopback

`loopbackTest` sends a known pattern out of a strand and reads it back through a jumper to `loopbackRxPin`, with its verdict in the status field. A `PASS` means the chip and the peripheral are clean, so flicker that remains is [signal integrity](../../explanation/led-signal-integrity.md).

Wired directly, jumper a data GPIO straight to `loopbackRxPin`.

Through a '595, the data GPIO carries the serial stream rather than pixel data, so the jumper comes from a register **output**. Strand S sits on data pin S / 8's register at position S % 8. A '595 shifts MSB first, so the strand appears on output Q(7 - S % 8), and strand 0 is Q7. Set `loopbackStrand` to the strand you tapped, or walk it from 0 upward until the test passes. No continuity check runs in shift mode, so a mis-wired jumper shows as a bit failure rather than "jumper not detected".

!!! warning "The '595 runs at 5 V"
    An ESP32 GPIO is not 5 V tolerant, and a 5 V signal can damage it. Put a level shifter or a divider between the register output and the GPIO: 1 kOhm from the output to the GPIO, 2 kOhm from the GPIO to ground.

## Timing for 12 V WS2811

RMT LED's `timing` defaults to 800 kHz, which most strips take. A 12 V WS2811 in low-speed mode needs `400kHz WS2811`; `custom` exposes the three bit timings for anything else.

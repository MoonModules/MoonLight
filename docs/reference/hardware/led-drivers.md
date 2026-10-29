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

RMT LED cannot drive an expander: each channel emits its own self-timed stream with no clock line, and a '595 needs a shift clock.

## Two limits

A strand holds at most **2,048 lights**, on every peripheral. On the classic ESP32 the `i80` DMA cannot reach PSRAM, so the whole frame must also fit internal RAM. Past either limit the driver idles with a status rather than crashing.

A frame takes the longest strand's wire time, about 30 µs a light, whatever the lane count: 256 lights a strand is about 7.5 ms and 1,024 about 30 ms. More strands of fewer lights is how a wall gets its frame rate, and `frameTime` on the card reports that floor.

## Lane, pin, strand

A lane is one bus data line, a strand is one chain of LEDs. Wired directly, one pin is one lane is one strand. Through an expander, each data pin feeds one '595 and fans out to 8 strands, so 8 data pins reach the driver's ceiling of 64 strands.

## Wiring a 74HCT595 pin expander

On an S3, P4 or S31 with `i80` or `MoonI80` as the `peripheral`, `pinExpander` on and `latchPin` set, each data pin drives one register and its 8 outputs drive 8 strands.

A rig costs its data pins plus two GPIOs: the shift clock on `clockPin` and the latch on `latchPin`, shared by every register. The clock is the peripheral's own pixel clock and costs no DMA bytes; the latch rides as a bit in every bus word.

The expander buys pins, not memory: the register is serial-in, so the DMA frame grows eightfold, about 1.1 KB per light. `i80` holds the whole frame, which past about 96 lights per strand falls to PSRAM too slow for the expander's clock. `MoonI80` streams the frame and drives 128 reliably.

## Sizing the MoonI80 ring

`MoonI80` streams the frame through a ring of small buffers in internal RAM, refilled from an interrupt as the wire drains each one. The refill encodes faster than the wire drains, so the ring only absorbs the moments the interrupt is held off, up to about 1.1 ms. What prevents a lost frame is the ring's lead over the wire in rows, not its number of buffers. On a 3,840-light expander rig, 45 rows ran clean and 40 lost frames.

`ringAuto` cuts the ring into 3-row buffers and takes enough for 54 rows of lead. It stops where free internal RAM would fall under 100 KB, and the status says when that cuts it short. With it off, `ringRows` and `ringBufs` are yours to set, at about 600 bytes a row on an 8-pin expander.

`ringSnapshot` copies each frame into internal RAM before the ring sends it, 3 bytes per RGB light (11.5 KB for 3,840 lights). Off, the ring reads the live frame, and refuses where that frame is in PSRAM. A flash save on the other core switches PSRAM off in the middle of a refill.

`ringDbg` shows the ring's counters once a second. `ab` and `ml` say whether the depth holds.

| Field | Meaning |
|---|---|
| `sn`, `lv` | where the snapshot and the live frame live: `I` internal RAM, `P` PSRAM |
| `sl/bf` | slices per frame, over buffers |
| `co`, `cr` | refills deferred while the flash cache was off, and their longest run |
| `ab` | stalls abandoned, each a lost frame: 0 on a healthy ring |
| `dn`, `ld` | frames done, and the last drain |
| `lt` | slices refilled after their drain began; the lead prevents it, so it stays 0 even while frames are lost |
| `tx` | the frame's wire time, in µs |
| `ipb`, `ci` | DMA items per buffer, and consumed |
| `tn`, `de` | the chain terminator, and descriptor errors |
| `enc`, `ea` | the slowest and the average refill, in µs |
| `sg`, `se` | gather and emit cycles per row |
| `tw`, `ts`, `tp` | the tick's wait, snapshot and prime, in µs |
| `gap@` | the longest gap between refills within a frame in µs, and the slice it ended at |
| `ml` | the fewest slices the refill stayed ahead of the drain |
| `c` | the core the refill runs on |

## Testing the wire with the loopback

`loopbackTest` sends a known pattern out of a strand and reads it back through a jumper to `loopbackRxPin`, with its verdict in the status field. A `PASS` means the chip and the peripheral are clean, so flicker that remains is [signal integrity](../troubleshooting.md#stray-pixels-or-wrong-colors-on-lights-that-should-be-off).

Wired directly, jumper a data GPIO straight to `loopbackRxPin`.

Through a '595, the data GPIO carries the serial stream rather than pixel data, so the jumper comes from a register **output**. Strand S sits on data pin S / 8's register at position S % 8. A '595 shifts MSB first, so the strand appears on output Q(7 - S % 8), and strand 0 is Q7. Set `loopbackStrand` to the strand you tapped, or walk it from 0 upward until the test passes. No continuity check runs in shift mode, so a mis-wired jumper shows as a bit failure rather than "jumper not detected".

!!! warning "The '595 runs at 5 V"
    An ESP32 GPIO is not 5 V tolerant, and a 5 V signal can damage it. Put a level shifter or a divider between the register output and the GPIO: 1 kOhm from the output to the GPIO, 2 kOhm from the GPIO to ground.

## Timing for 12 V WS2811

RMT LED's `timing` defaults to 800 kHz, which most strips take. A 12 V WS2811 in low-speed mode needs `400kHz WS2811`; `custom` exposes the three bit timings for anything else.

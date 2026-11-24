# HUB75 bench record

What the benches measured on the Raspberry Pi outputs of the [HUB75 driver](../../moonmodules/light/drivers.md#hub75): `shown` rates, the conditions of each reading, and predictions against readings. Setup and settings are in [Driving a HUB75 panel](hub75-panels.md#raspberry-pi-5-rp1-pio) for the Pi 5 and [for the Pi 3/4](hub75-panels.md#raspberry-pi-34-pi-gpio). `shown` is the driver's measured refresh. BENCH-V2 and BENCH-V8 name the bench rounds of 2026-11-13 on the wall, and BENCH-V3 to V7 those of the Pi 4 sign.

First light and the baseline ran on builds whose engine reached the PIO through piolib, a library the kernel-interface backend (`/dev/pio0`, [Kernels](hub75-panels.md#kernels)) replaces. [The kernel-interface backend](#the-kernel-interface-backend-2026-11-20) measures that backend against the baseline's binary on one wall, and on a second kernel.

## Raspberry Pi 5 (RP1 PIO)

The bench is a 256x192 wall on a Pi 5: six 128x64 panels, 2 to a lane, 3 lanes in parallel.

### Conditions

| | First light and BENCH-V2, V8 | Baseline | Kernel-interface backend |
|--|--|--|--|
| Date | 2026-11-13 | 2026-11-18, 11:15 to 11:19 UTC | 2026-11-20, 13:24 to 13:39 UTC on 6.18; 16:14 to 17:17 UTC on 6.12 |
| Kernel | 6.18.60+rpt-rpi-2712 | 6.18.60+rpt-rpi-2712 (DietPi, Debian 13.7) | 6.18.60+rpt-rpi-2712 (the same card), and 6.12.109+rpt-rpi-2712 (Raspberry Pi OS Lite 64-bit, Debian 12) |
| Binary | - | `/usr/bin/MoonLight`, md5 d0bcb409, a piolib build of the PIO work of 2026-11-16 | md5 8ef6e72e, version 7.0.0-dev.1, from `release.yml`'s `build-linux-arm64` job; on 6.18 in place of `/usr/bin/MoonLight`, on 6.12 from its `.deb`. A build from before the commit review's fixes; the build after them was not retaken on this wall |
| Backend | piolib, raspberrypi/utils 4284bd7 | piolib, raspberrypi/utils 4284bd74 (2026-11-12), static | the kernel's `/dev/pio0` ioctls |
| Card | rising clock edge, latch blank 0 | falling clock edge (`clockEdge` 1), latch blank 1, curve CIE 1931, local brightness 255 | falling clock edge (`clockEdge` 1), latch blank 1 |
| Method | `bitDepth` and `pixelClock` set on the Hub75 card and `shown` read | `bitDepth` and `pixelClock` set through `POST /api/control`, `shown` read from `GET /api/modules/Hub75`, three readings per setting of a 5 s window with `shown` sampled once a second; thread CPU from `/proc/<pid>/task/*/stat` over the same window | as the baseline |
| Content | - | `live`: the layers as found (about 690 main-loop fps); `static`: the layers switched off, so no new frames reach the output | `live` and `static` as the baseline, with the layers as found on each card |
| Throttle | - | `throttled=0x0` before and after | `throttled=0x0` before and after |

### First light, 2026-11-13

The PIO output drove the wall: correct picture on all three lanes, 0 submit errors, encode about 1.28 ms a frame, the integrated output at 169 to 170 fps. The measured refresh of about 169 Hz sits under the computed 203.5 Hz because the DMA feed is the ceiling ([Refresh at 6-bit and 8-bit](#refresh-at-6-bit-and-8-bit)).

### Refresh at 6-bit and 8-bit

`shown` on the wall, median of three readings, with the 2026-11-13 reading beside it. Frame time is 1000 / `shown`. Wire time is computed, rows / 2 x bits x (256 + latch blank) words at one word per pixel clock; on 2026-11-13 it was computed with the 256 words alone, about 0.4% shorter.

| bits | pixel clock | content | `shown` | 2026-11-13 | frame ms | wire ms | frame / wire | Hub75 thread CPU % |
|------|-------------|---------|---------|------------|----------|---------|--------------|--------------------|
| 6 | 5 MHz | live | 101 | 101 (V2) | 9.90 | 9.87 | 1.00 | 14.8 |
| 6 | 10 MHz | live | 170 | 169 (V2) | 5.88 | 4.93 | 1.19 | 28.7 |
| 6 | 11.446 MHz | live | 182 | | 5.49 | 4.31 | 1.27 | 28.1 |
| 6 | 20 MHz | live | 225 | 225 (V2) | 4.44 | 2.47 | 1.80 | 34.6 |
| 8 | 5 MHz | live | 76 | 76 (V8) | 13.16 | 13.16 | 1.00 | 19.0 |
| 8 | 10 MHz | live | 127 | 127 (V8) | 7.87 | 6.58 | 1.20 | 28.7 |
| 8 | 11.446 MHz (the wall's own) | live | 136 | | 7.35 | 5.75 | 1.28 | 30.8 |
| 8 | 20 MHz | live | 170 | 169 (V8) | 5.88 | 3.29 | 1.79 | 38.7 |
| 8 | 11.446 MHz | static | 136 | | 7.35 | 5.75 | 1.28 | 0.4 |
| 8 | 20 MHz | static | 169 | | 5.92 | 3.29 | 1.80 | 0.6 |
| 8 | 11.446 MHz, `Drivers.brightness` 64 | live | 136 | | 7.35 | 5.75 | 1.28 | 30.8 |

- **The spread is 0 or 1 fps at every setting.** Every 1 Hz sample in a window held one value, except at 6-bit 11.446 MHz, where samples read 181 or 182.
- **The 2026-11-18 baseline equals 2026-11-13 or reads 1 fps higher,** within the counter's 1 fps resolution.
- **The feed is the ceiling above 5 MHz.** Frame / wire is 1.0 at 5 MHz, about 1.2 at 10 MHz and about 1.8 at 20 MHz at both depths. At 20 MHz that is about 44.5 MB/s of 32-bit words (computed: words x 4 x `shown`), the "mid-40s MB/s" of 2026-11-13. The cause is untraced.
- **Refresh is the same whether frames arrive or stop:** `static` reads 136 and 169 against `live`'s 136 and 170 at 11.446 and 20 MHz. The encode on that piolib build's refresh thread left `shown` unchanged on this kernel, which matches a submit that returns once queued from kernel 6.18.39 on ([Kernels](hub75-panels.md#kernels)).
- **Brightness 64 against 255 reads 136 and 136:** the engine dims by lit time within the shift.
- **Hub75 thread CPU** is 0.4 to 0.6% when frames stop, so the submit itself costs a fraction of a percent. With moving content it is 15 to 39% and rises with `shown`: about 2.3 to 2.5 ms a frame at 8-bit and 1.5 to 1.7 ms at 6-bit, derived from one encode per refresh. The thread is identified by inference, since it is re-made on every reopen.
- **Memory:** RSS 28,016 kB with content and 27,664 kB when frames stop; the Hub75 card's `dynamicBytes` is 294,912 B (288 KiB) at 6-bit and 8-bit alike.

### The kernel-interface backend, 2026-11-20

A/B/A' on the wall: A is the baseline's piolib binary (d0bcb409), B the kernel-interface build (8ef6e72e), A' the piolib binary again, in that order on one boot of 6.18, three readings of 5 s per setting. The 6.12 column is B's binary on the 6.12 card, with the same card settings. MoonLight CPU is the whole process; the busiest thread is the highest of its threads.

| bits | pixel clock | content | `shown` A / B / A' | `shown` B on 6.12 | MoonLight CPU % A / B / A' | busiest thread % A / B / A' |
|--|--|--|--|--|--|--|
| 6 | 5 MHz | live | 101 / 101 / 101 | 101 | 54.6 / 52.7 / 53.9 | 27.6 / 20.9 / 27.0 |
| 6 | 10 MHz | live | 170 / 170 / 170 | 198 | 62.3 / 53.1 / 62.1 | 28.0 / 20.5 / 27.2 |
| 6 | 11.446 MHz | live | 182 / 182 / 182 | | 63.7 / 53.1 / 63.1 | 28.0 / 20.5 / 27.2 |
| 6 | 20 MHz | live | 225 / 227 / 225 | 243 | 69.1 / 53.5 / 68.1 | 28.0 / 20.7 / 27.0 |
| 8 | 5 MHz | live | 76 / 76 / 76 | 75 | 54.8 / 57.7 / 53.8 | 28.2 / 20.5 / 27.2 |
| 8 | 10 MHz | live | 127 / 127 / 127 | 148 | 62.6 / 58.2 / 61.5 | 28.2 / 20.7 / 27.0 |
| 8 | 11.446 MHz | live | 136 / 136 / 136 | | 64.3 / 58.3 / 63.1 | 28.2 / 20.7 / 27.4 |
| 8 | 20 MHz | live | 170 / 169 / 170 | 182 | 69.5 / 58.8 / 68.3 | 28.0 / 20.9 / 26.8 |
| 8 | 11.446 MHz, `Drivers.brightness` 64 | live | 136 / 136 / 136 | 148 (at 10 MHz) | 64.0 / 58.4 / 63.3 | 28.0 / 20.7 / 27.0 |
| 8 | 11.446 MHz | static | 136 / 136 / 136 | | 48.5 / 47.0 / 48.5 | 20.5 / 19.9 / 20.5 |
| 8 | 20 MHz | static | 169 / 169 / 169 | 180 | 53.5 / 47.2 / 53.4 | 25.5 / 19.9 / 25.5 |

- **The two backends refresh the wall at the same rate on 6.18:** B is within 2 fps of A and A' at every setting.
- **B's threads hold flat across clocks.** In A the second-busiest thread, the refresh that also encodes, rises from 11.6% at 5 MHz to 26% at 20 MHz; in B the busiest holds at 20.5 to 20.9%, and the process uses 53 to 59% at 20 MHz against A's 68 to 70%.
- **Memory:** B's RSS is 224 to 528 kB above A's (26,944 to 27,232 kB against 26,704 to 26,720 kB).
- **On 6.12 the feed falls less far behind the wire above 5 MHz.** Frame / wire at 8-bit is 1.01, 1.03 and 1.67 at 5, 10 and 20 MHz, against 1.00, 1.20 and 1.80 on 6.18. The binary and the Hub75 card are the same; the two SD cards differ in the kernel, the Debian release (12 against 13), the content and `Drivers.brightness` (20 on the 6.12 card), and on both kernels `static` reads within 2 fps of `live` and `Drivers.brightness` 64 reads as the card's own: the rate holds across content and brightness. The kernel is the hypothesis, and the cause is untraced.
- **Reopen:** ten changes of `pixelClock` by 1 kHz, each re-preparing the output in the same process, reach a `shown` rate in a median 2.01 s (1.16 to 2.02) on 6.18 and 1.99 s (1.34 to 2.03) on 6.12. `shown` read 136 fps each time on 6.18, the rate of its 8-bit 11.446 MHz row, and 144 to 149 fps per cycle on 6.12, whose run kept 8 bits at 10 MHz, where its row reads 148.
- **After the service stops, OE reads high (`pinctrl get 18`: `hi`) on both kernels:** the panel is dark.
- **The kernel log on 6.12** holds zero timeout lines before and after the runs, so its timeout paths ([Kernels](hub75-panels.md#kernels)) stay unexercised on this wall.
- **The `.deb` on Raspberry Pi OS** gives its service user `/dev/pio0`'s group (`gpio`) at install.

### 8-bit color

`shown` at 8-bit reads 76, 127 and 169 to 170 fps at 5, 10 and 20 MHz, the 6-bit readings scaled by 6/8, as the frame holds 8/6 the words. The wall's 8-bit frame is 65,536 words, 256 KiB. At brightness 48 the 2026-11-13 bench saw steps in a dark ramp (BENCH-V8): below brightness 64 the lowest plane drops out ([8-bit color](hub75-panels.md#8-bit-color)).

## Raspberry Pi 3/4 (Pi GPIO)

The bench is the Pi 4 sign: 96x64, one 64x32 panel a lane, three lanes, with Stripe multiplexing. The wall (256x192) and the computed refresh rates for it are pending on-device testing.

### This output on the sign

Read on 2026-11-23 on the Pi 4 sign (kernel 6.18.50), each setting held 8 s and then read three times 3 s apart, against the moonpi build (d0bcb409) read before and after it on the same content. The moonpi build times OE from a 500 MHz clock where a Pi 4's PLLD runs at 750 MHz (`plld_per` read on the sign), so its pulses are two thirds of their setting; this output's are the length set, and the rows where a plane's lit time outlasts the shift (8-bit) read lower for it:

| Setting | moonpi build, before | this output | moonpi build, after |
|--|--|--|--|
| 6-bit, 160 ns LSB, true slowdown 1 | 607 to 609 fps | 682 | 603 to 606 |
| 6-bit, 160 ns LSB, true slowdown 2 | 577 to 579 | 647 to 649 | 576 to 577 |
| 6-bit, 160 ns LSB, true slowdown 3 | 547 to 553 | 620 | 550 |
| 8-bit, 306 ns LSB, true slowdown 2 | 403 to 404 | 469 | 401 to 403 |
| 8-bit, 433 ns, brightness 33, carry off / 2 bits | 412 to 413 / 374 to 375 | 435 / 435 | 410 to 411 / 368 to 374 |
| 6-bit, 433 ns, brightness 255, carry off / 2 bits | 576 to 579 / 487 to 492 | 649 / 648 | 573 to 576 / 479 to 491 |

The encode runs on its own thread beside the refresh thread, so the refresh dumps one frame while the next encodes: the settings the shift bounds read 12 to 13% higher, and the carry costs no refresh rate. The carry's re-encode shows on the encode thread instead, 9.0% of a core with the carry off and 18.8% with 2 bits (8-bit, brightness 33), while the realtime refresh thread holds one core either way. The process held 16.4 MB resident.

The [test pattern](hub75-test-pattern.md) renders right on this output: red rows 0, 10, 30 and 50 light as single full-width lines at their heights, and the green columns step left to right at full height.

The column time (`hub75GpioColumnTimeNs`) is calibrated on the three 6-bit rows above, which the shift bounds: it predicts 681, 649 and 619 Hz against 682, 647 to 649 and 620 fps.

The sections below were read on the moonpi build, 2026-11-15 to 2026-11-20, whose refresh thread encoded each frame before dumping it.

### First light on the sign

The GPIO output has run on the Pi 4 sign: the output ran, the status was ok, and `shown` read 1,030 to 1,082 fps at 6-bit, but with multiplexing normal the picture was a washed, shearing blend, and BENCH-V3's Stripe (a row-only permutation) came out stretched, sheared and vertically banded: the wrong shape. The mapping is fixed (stretch factor 2) and the dump walks rows outer, planes inner with the address set in the dark, and BENCH-V5 verified both on the sign ([Stripe mapping](#stripe-mapping)); a rotating wireframe renders clean, sharp, with no spreading, tearing or ghosting.

### Stripe mapping

BENCH-V5 verified Stripe on the Pi 4 sign: the [test pattern](hub75-test-pattern.md) renders right, red rows top to bottom, green columns left to right, held row 0 at top.

### Dump order

The order matters on slow-decay outdoor panels: dumped plane-major, the v4 bench (BENCH-V4) showed both axes spreading on the sign (one column filled every column, one row spread over a dozen) with the mapping already fixed, because the address moved every 13 us while the rows were still lit.

### Column-time calibration

The moonpi build's model counted each slowdown delay loop's first GPLEV0 read at 107 ns and further reads in the loop at 6.4 ns each, so the loops dominated the column, and it carried the encode at the share the BENCH-V6 calibration saw, a fresh frame's with no carry, so its prediction was the rate with `carryBits` 0. This output's model is the one above (`hub75GpioColumnTimeNs`), with no encode share.

### Refresh, predicted against measured

The moonpi build's model predicted 612, 584 and 558 Hz on the sign at 6-bit, true slowdowns 1, 2 and 3, 160 ns LSB, against 608 to 616, 584 to 585 and 555 to 560 fps read with no dither (BENCH-V6), within 3%. At 8-bit and true slowdown 2 it predicted 438 Hz at a 160 ns LSB and 432 Hz at 306 ns, where the top plane's lit time outlasts the shift, and the sign read 447 to 450 fps at 306 ns, 3 to 4% above it. A dark ramp ran smooth to black at 8-bit.

### Pulse length

On the sign at 8-bit and brightness 255 (2026-11-15, binary 7898dd44), 3000 ns read 144 fps against 417 at 424 ns.

### The carry's price

On the sign (96x64, true slowdown 2, a 433 ns LSB, the triangle gray map with moving content; 2026-11-15, binary 24bc16c9), `shown` read 433 fps at 8 bits and brightness 33 with the carry off and 328 with 2 bits, and 548 against 455 at 6 bits and brightness 255: about 0.74 ms more a refresh at 8 bits and 0.37 ms at 6. The remainder memory is 18 KB on the sign. A pixel's pattern repeats within 2^bits refreshes, so at 2 bits a quarter of the refresh rate is the slowest repeat, 82 Hz and 114 Hz in those two cases.

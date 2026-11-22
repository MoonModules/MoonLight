# HUB75 bench record

What the benches measured on the Raspberry Pi 5 output of the [HUB75 driver](../../moonmodules/light/drivers.md#hub75): `shown` rates, the conditions of each reading, and predictions against readings. Setup and settings are in [Driving a HUB75 panel](hub75-panels.md#raspberry-pi-5-rp1-pio). `shown` is the driver's measured refresh. BENCH-V2 and BENCH-V8 name the bench rounds of 2026-11-13.

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

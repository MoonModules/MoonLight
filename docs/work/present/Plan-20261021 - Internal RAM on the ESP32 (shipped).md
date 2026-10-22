# Plan: internal RAM on the ESP32

Plan of 2026-10-21, every step done 2026-10-22. An ESP32-S3 driving 3,840 lights through a pin expander ran with 63 KB of internal RAM free and a 30 KB largest block, enough for a TLS handshake to fail and a UI refresh to crash it. The goal: as much internal RAM free as the board allows, never less than 100 KB free with a 50 KB largest block.

**Result**: the same board holds 150 to 155 KB free with a 108 KB block, the UI open, at 102 to 107 fps where it ran 75.

## Where it lives now

| What | Home |
|---|---|
| The rule: what must be internal and why, the floor, the sends' back-pressure, a board without PSRAM | [MoonCore, Internal RAM](../../explanation/architecture/mooncore.md#internal-ram) |
| How deep the LCD-MM ring is, and its live counters | [LED drivers, Sizing the MoonI80 ring](../../reference/hardware/led-drivers.md#sizing-the-mooni80-ring) |
| Each build option and its reason | `esp32/sdkconfig.defaults` |
| The measurements: the S3 step by step, the ring's depth and encode cost, the P4, the classic, network receive | [performance.md, Internal RAM](../../reference/performance.md#internal-ram) |
| The floor as a test, and the `browser` step | `scenario_RAM_floor`, [testing.md, Performance contracts](../../reference/testing.md#performance-contracts-contracttarget) |
| What a user notices | [MIGRATING](../../reference/MIGRATING.md#lcd-mms-ring-is-smaller-and-refuses-psram), [troubleshooting](../../reference/troubleshooting.md#free-memory-is-low-with-the-interface-open) |
| What it taught | [lessons](../past/lessons.md#lessons-from-the-internal-ram-plan-2026-10-22) |
| Left open | the ring's refill jitter ([backlog-light](../future/backlog-light.md#the-rings-refill-jitter-2026-10-22-from-the-internal-ram-plan)), the 3.0.0 update-connection symptom ([backlog-core](../future/backlog-core.md#an-updates-connection-that-would-not-open-until-a-reset-2026-10-22-from-the-internal-ram-plan)) |

## What was done

1. WiFi, lwIP and TLS buffers in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`).
2. WiFi's code in flash (`CONFIG_ESP_WIFI_IRAM_OPT`, `_RX_IRAM_OPT` off); network receive measured unchanged.
3. ESP-IDF's 32 KB DMA reserve pool off (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=0`), which capped the largest block at its size.
4. Every plain `malloc` asks PSRAM first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`), after what a cache-safe interrupt reads went internal by `platform::InternalRamObject`: the bus states, then `ParallelLedDriver` and `MoonI80Peripheral` once a flash write on the other core was seen to cut PSRAM off mid-refill.
5. Stacks trimmed where measured: `mmEncode` 8 to 5 KB, `improv` 6 to 4 KB; WiFi's NVS config cache off (4.9 KB).
6. The audio transform's twiddle table built in and freed with the microphone (16 KB).
7. The LCD-MM ring: its settings on the driver, so a peripheral switch keeps them and a fresh driver reports their defaults; the encode faster than the wire (emit 5,800 to 3,650 cycles a row, gather 1,850 to 1,300); `ringAuto` sized by lead with 3-row buffers (28 buffers to 21, 112 KB to 37 KB); the prime's core-0 helper `mmSnap` removed (+12 fps); the ring refusing PSRAM.
8. The HTTP server's sends wait while internal RAM is short: embedded files stream across ticks (the LEDs no longer freeze for a slow client), and the file, state and preview sends hold back below half the floor.
9. `scenario_RAM_floor` and the live runner's `free_internal`, `min_stack_free` and `browser` step.
10. Found on the way: the Devices card saved the network config to flash on every multicast verdict, and the once-a-second status line at log level Info stalls the render loop on the S3 (troubleshooting).

Decided against: Control following only when viewed (150 µs a second), a tick figure without interruptions (the preemption it served was `mmSnap`), fewer WiFi receive buffers and a smaller TCP send buffer (Art-Net's burst headroom, every board's throughput), `kInternalFreeFloor` as a refusal in every module's `prepare()`, ring buffers and the snapshot in PSRAM (the cache race), ESP-IDF's heap task tracking (it crashed; the standalone heap trace and a low-point sampler did the job).

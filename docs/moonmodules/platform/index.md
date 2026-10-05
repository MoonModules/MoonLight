# Platform

The one interface every module reaches hardware through. Core and the light domain call these names and never a vendor SDK, so the same source drives an ESP32, a Teensy, a Raspberry Pi and a desktop.

Why the boundary is drawn here, and what is allowed to cross it, is [MoonCore's platform abstraction](../../explanation/architecture/mooncore.md#platform-abstraction). This page is the reference half: what the interface offers, and where each target implements it.

`platform.h` declares the interface; `desktop/` and `esp32/` implement it. A module that needs something the interface does not offer gets a new function here rather than an `#ifdef` at the call site, which is what keeps the domains target-blind.

## Time

`millis`, `micros`, `delayMs`, `delayUs`. The clock every animation is written against, and the only sleep a module may take.

Detail: [technical](moxygen/platform.md)

## Memory

`alloc`, `free`, `allocInternal`, `freeHeap`, `freeInternalHeap`, plus the accounting (`allocatedBytes`, `allocatedCount`, `allocatedPeak`) that MoonStats reports. Internal versus PSRAM is a platform question, so a module asks for what it needs and the layer decides where it lands.

Detail: [technical](moxygen/platform.md)

## Executable memory

`allocExec`, `writeExec`, `freeExec`. Memory a CPU will fetch instructions from, which is IRAM on an ESP32 and an mmap page on desktop. [MoonLive](../light/moonlive.md) compiles into it; nothing else does.

Detail: [technical](moxygen/platform.md)

## Filesystem

`fsMount`, `fsRead`, `fsWrite`, `fsList`, `fsExists`, `fsMkdir`. LittleFS on a device and `std::filesystem` on desktop, behind one set of names, so a config file and a script are read the same way on both.

Detail: [technical](moxygen/platform.md)

## GPIO and ADC

`gpioRead`, `gpioWrite`, `gpioInputBegin`, `adcRead`, `adcReadMv`, `adcMaxCount`, and the capability introspection PinsModule renders. A pin's existence is a per-chip fact, so the layer answers it rather than each module guessing.

Detail: [technical](moxygen/platform.md)

## Networking

Sockets, plus the raw-L2 ethernet path (`ethInit`, `ethSendRaw`, `ethLinkUp`, `ethGetIPv4`). Art-Net and sACN reach the wire through this, which is why a driver never opens a socket itself.

Detail: [technical](moxygen/platform.md)

## Tasks and workers

RTOS task introspection for TasksModule, and the pinned worker with wake notification that carries the render and encode split across cores. On desktop these are threads; the contract is identical.

Detail: [technical](moxygen/platform.md)

## Video output

NDI and HLS. Present where the target can carry them and absent elsewhere, which a module discovers by asking rather than by target.

Detail: [technical](moxygen/platform.md)

## Two implementations, one contract

`src/platform/desktop/` and `src/platform/esp32/` implement the same declarations. The desktop one exists so the whole system runs, is tested and is developed without hardware, which is what makes the desktop-first rule possible.

The MoonLive assemblers live here too, one per instruction set, because emitting machine code is the most target-specific thing the system does. The engine and its lowering stay in core; only the encoding is per target.

| Assembler | Target |
|---|---|
| [moonlive_asm_xtensa](moxygen/moonlive_asm_xtensa.md) | classic ESP32 and S3 |
| [moonlive_asm_riscv](moxygen/moonlive_asm_riscv.md) | ESP32-P4 |
| [moonlive_asm_host](moxygen/moonlive_asm_host.md) | desktop arm64 and x86-64 |

Each satisfies the assembler contract [the lowering](../light/moonlive.md) names, so adding an instruction set is a new file behind an unchanged IR. Per-target build settings are in each half's own `platform_config`: [desktop](moxygen/desktop_platform_config.md) and [ESP32](moxygen/esp32_platform_config.md), the latter carrying the per-chip capability table.

## What does not belong here

A vendor SDK call in core or the light domain. A `#ifdef ESP32` outside this folder. A module that reads a chip register directly. Each is the same mistake: a target fact escaping the layer that exists to hold it, and [the code report](../../reference/metrics/code.md) counts it, at zero.

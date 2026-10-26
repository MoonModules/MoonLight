# Issues in other projects

The bugs and fixes MoonLight waits on in the projects it builds on: what each one is, what our tree does meanwhile, where it stands, and what changes once the fix ships.
A workaround's reason stays beside it in the code, which links here; the tables below hold its status, so a workaround goes in the release its reason goes.
Prior art the source cites is credited in [Attributions](attributions.md) instead.

The Status column holds what each tracker shows on 2026-10-26.

## ESP-IDF

| Issue | Filed by | Submitted | What | Our side | Status | Once it ships |
|---|---|---|---|---|---|---|
| [#19308](https://github.com/espressif/esp-idf/issues/19308) | us | 2026-10-25 | `pxPortGetCoprocArea` sits in flash. An interrupt entry calls it to save the hardware-loop registers of the task it interrupted, so one landing during a flash write panics with a Cache error, on the P4 and the S31 | `CONFIG_FREERTOS_IN_IRAM=y` in [the P4 fragment](https://github.com/MoonModules/MoonLight/blob/main/esp32/sdkconfig.defaults.esp32p4rev1-eth) and the S31's, 14 KB of internal RAM | Open. Espressif moves the function to IRAM in an internal MR. Without the workaround our code survives 60 restores and 186 stressed flash writes on a P4, so the fix is unconfirmed here | Drop the line, which returns the 14 KB |
| [#19025](https://github.com/espressif/esp-idf/issues/19025) | us | 2026-08-28 | In every release the P4's hardware-loop state erratum workaround is gated on a misnamed macro, so it is always on | Nothing | Closed on 2026-10-26, fixed on master (86837ab6, its tests b341f304), in no release | Nothing on the rev 1 builds; the rev 3 builds lose the workaround, which their silicon does not need |
| [#18759](https://github.com/espressif/esp-idf/issues/18759) | another user | 2026-06-24 | `sleep_clock_icg_startup_init` runs out of retention memory at boot on the P4 with esp_hosted, a boot loop | `CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n` in [the P4 WiFi fragment](https://github.com/MoonModules/MoonLight/blob/main/esp32/sdkconfig.defaults.esp32p4rev1-eth-wifi) | Open | Drop the line |

## esp-dsp

| Issue | Filed by | Submitted | What | Our side | Status | Once it ships |
|---|---|---|---|---|---|---|
| [#119](https://github.com/espressif/esp-dsp/issues/119), [#102](https://github.com/espressif/esp-dsp/issues/102) | other users | 2026-06-18 and 2025-03-24 | Some assembly kernels break the P4's hardware-loop constraints (loop boundaries 4-byte aligned, no coprocessor instruction last), so an interrupt on a loop boundary corrupts the loop counter: the FFT crash behind #19025 | The optimized kernels ship (`CONFIG_DSP_OPTIMIZED=y`); `CONFIG_DSP_ANSI=y` in [the P4 fragment](https://github.com/MoonModules/MoonLight/blob/main/esp32/sdkconfig.defaults.esp32p4rev1-eth) is the fallback, off | Open. Espressif commits in #19025 to the alignment fix in an esp-dsp release | Run the FFT and HLS soak on a rev 1 P4 |

## teensy_loader_cli (PJRC)

| Issue | Filed by | Submitted | What | Our side | Status | Once it ships |
|---|---|---|---|---|---|---|
| [PR #96](https://github.com/PaulStoffregen/teensy_loader_cli/pull/96) | us | 2026-10-13 | macOS: match only PJRC's devices in the HID manager, fixing its #60 | [teensy_loader_cli-match-own-devices.patch](https://github.com/MoonModules/MoonLight/blob/main/teensy/patches/teensy_loader_cli-match-own-devices.patch) | Open, no reply since 2026-10-13 | Drop the patch |

## QNEthernet

| Issue | Filed by | Submitted | What | Our side | Status | Once it ships |
|---|---|---|---|---|---|---|
| Not filed yet | us | | Four changes: bounded waits in the W5500 driver, an overridable TCP send buffer, the receive window reopened on read, and mDNS NSEC answers | The four `QNEthernet-*.patch` files in [teensy/patches](https://github.com/MoonModules/MoonLight/tree/main/teensy/patches) | To file, as [the Teensy plan](../work/present/Plan-20261010%20-%20Teensy%204.1%20%283%20of%206%29.md) lists | Drop each patch as its change merges |

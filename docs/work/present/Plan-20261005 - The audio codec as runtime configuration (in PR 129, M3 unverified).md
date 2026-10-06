# The audio codec as runtime configuration

A board with an ES8311 codec in front of its microphone is a `deviceModels.json` entry on an existing firmware, not a firmware of its own.

## Why

Two community PRs add a P4 board each with an onboard ES8311: #128 (Waveshare ESP32-P4-ETH) and #55 (Guition JC-ESP32P4-M3-DEV).
Both added a firmware variant, because the driver (`platform_esp32_es8311.cpp`) is built only for the S31 and the codec type and pins are compile-time constants in `platform_config.h`.
The two boards share their codec wiring, so a variant per board would multiply firmwares for what is one setting.

## Steps

1. **Take the two codec fixes from #128**, credited to TouchMyLight: pass the I2C address pre-shifted where `esp_codec_dev` expects the 8-bit form, and set the `data_if` that `esp_codec_dev_new` requires. Both affect the S31 as well.
2. **Build `esp_codec_dev` for the P4** as well as the S31, in `esp32/main/idf_component.yml`; measure the flash it adds on `esp32p4rev1-eth` with the flash split.
3. **Make the codec runtime configuration**: AudioService gains `codec` (none or ES8311) and `codecAddr`, the address shown only when a codec is chosen. The bus is the board's: the I2C scan module becomes I2cBusModule, which opens it once on its pins for every device on it. None is the default, so a P4 board without one never probes the bus. `audioCodecInit` takes them from AudioService; `audioCodecType` and `audioCodecPins` leave `platform_config.h`.
4. **The S31 CoreBoard entry** names its codec in `deviceModels.json` (SDA 51, SCL 50, MCLK 52, address 0x18), since the firmware leaves the codec to the device model.
5. **Two catalog entries** on `esp32p4rev1-eth`, as proposed on the PRs: Waveshare ESP32-P4-ETH and Guition JC-ESP32P4-M3-DEV, both SDA 7, SCL 8, 0x18, MCLK 13, BCLK 12, WS 10 and Ethernet `P4-NANO`, with mic data on 11 (Waveshare, its wiki and a bench test) and 48 (Guition, its schematic and #55's hardware test).

## Tests

- AudioService, on the host: no codec is asked for until one is named with both bus pins, the request carries the bus and the 7-bit address, and every codec setting re-initializes the microphone.
- The controls' visibility and the address shift live behind `hasI2sMic` and in the ESP32 driver, which a host test does not reach; the S31 bench run proves both.
- `check_devices` validates the three entries.

## Verification

- S31 on the bench: the mic reports level and onsets through the codec, which also proves the two fixes.
- The MM testbench P4 (direct I2S mic, no codec) keeps its audio unchanged with `codec` at none.
- The two P4-ETH boards are verified by their PR authors on their hardware.

## What comes out

The compile-time codec constants, and the need for #128's and #55's firmware variants; both PRs close or rebase onto this.

## Docs

The AudioService card describes the codec controls; MIGRATING notes that an S31 configured before this needs its device model applied again, or the codec set by hand.

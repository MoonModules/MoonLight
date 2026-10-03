# Waveshare ESP32-P4-ETH hardware reference

Pin maps and onboard features for the **Waveshare ESP32-P4-ETH**, read from the official schematic so MoonLight work (Ethernet, audio) reads this instead of re-scraping the PDF. The board is `esp32p4rev1-eth-es8311` in firmware, `"Waveshare ESP32-P4-ETH"` in the device catalog.

**Sources**
- Product page: <https://www.waveshare.com/esp32-p4-eth.htm>
- Wiki: <http://www.waveshare.com/wiki/ESP32-P4-ETH>
- Schematic (local): `datasheet/ESP32-P4-ETH/ESP32-P4-ETH-datasheet.pdf`

Same family as the Waveshare ESP32-P4-NANO (see [gpio-usage.md § ESP32-P4](gpio-usage.md#esp32-p4) for its pin map): ESP32-P4 (RISC-V dual-core), same onboard IP101 Ethernet PHY wiring, same CSI/DSI/I2C accessory-bus convention. This board adds an onboard **ES8311** audio codec feeding an SMD mic + a 2-pin JST speaker header, which the P4-NANO lacks. The [MHC-WLED ESP32-P4 shield](mhc-wled-esp32-p4-shield.md) is a different, unrelated P4 carrier board (RS-485/line-in, no onboard codec).

## Audio (ES8311 codec)

The onboard SMD mic and speaker header connect through an **ES8311 mono codec**. The ESP is the I2S master (it drives MCLK).

| Signal | GPIO | Direction / role |
|---|---|---|
| I2S MCLK | 13 | master clock → codec (ESP is I2S master; MCLK = 256 × sample_rate) |
| I2S SCLK (BCLK) | 12 | bit clock |
| I2S LRCK (WS) | 10 | word select |
| I2S ASDOUT | 11 | **mic / ADC data: codec → ESP** (the record path) |
| I2S DSDIN | 9 | playback / DAC data: ESP → codec (speaker path, **not wired up** — out of scope, mic-only) |
| **ESP_I2C_SDA** | **7** | codec control bus (shared with the CSI/DSI accessory headers) |
| **ESP_I2C_SCL** | **8** | codec control bus |
| PA_Ctrl | 53 | speaker amp enable (not wired up — speaker is out of scope) |

- **ES8311 I2C address: `0x18`** (the hardware default; no address-select strap).
- Driven by Espressif's **`esp_codec_dev`** managed component (same driver as the ESP32-S31's ES8311), gated behind the `CONFIG_MM_P4_ES8311` Kconfig option (set only by the `esp32p4rev1-eth-es8311` firmware fragment `sdkconfig.defaults.esp32p4-es8311`) — most other P4 boards share this chip target but have no codec on the I2C bus, so the codec bring-up can't be a chip-wide default: `audioCodecType`/`audioCodecPins` are compile-time constants, and once non-`None`, `AudioService::reinit()` unconditionally probes the codec over I2C and fails the whole mic path if nothing answers.
- The codec needs **MCLK running before it answers I2C**; `AudioService::reinit()` brings up the I2S channel (which drives MCLK) before the codec I2C config, same ordering as the S31.
- **Bench-verified end to end** 2026-10-03: Ethernet link + DHCP, `I2cScanModule` ACKs the codec at 0x18 on GPIO7/8, and `AudioService` reports live mic data (non-zero level RMS, onsets, peak Hz) with `mode="local audio"`, `micMode="I2S"`, `sckPin=12`, `wsPin=10`, `sdPin=11` (the catalog entry's defaults).
- **Two bugs found and fixed in `platform_esp32_es8311.cpp`** during that bench pass (both apply to the S31's codec path too, same file, not P4-ETH-specific):
  1. `esp_codec_dev`'s I2C control layer right-shifts the address it's given by one bit before handing it to the IDF `i2c_master` driver (`audio_codec_ctrl_i2c.c`: `.device_address = (i2c_cfg->addr >> 1)`) — it wants the 8-bit write-address form, not the bare 7-bit address a scan/probe uses. Passing `0x18` straight through silently targeted `0x0C`: every register write NACK'd (`ES8311: Open fail`) even though a raw `i2c_master_probe` at `0x18` ACKs cleanly (that path doesn't shift). Fixed by shifting at the one call site that needs it; `AudioCodecPins.i2cAddr` itself stays the natural 7-bit value everywhere else.
  2. `esp_codec_dev_new` requires a non-null `data_if`, which the code never set (by design — `audioMicInit` owns the real I2S RX channel, not `esp_codec_dev`). Fixed with a `data_if` whose rx/tx handles stay null on purpose; its `set_fmt`/`enable` calls log harmless `NULL` warnings each re-init (`i2s_channel_get_info: input parameter 'handle' is NULL`, etc.) since `esp_codec_dev_open` discards their return value — expected noise, not a fault.
- **Mic-only path** (audio-reactive input): all pins above confirmed independently from the schematic netlist *and* the Waveshare wiki (which separately publishes the same I2C/I2S pin set). The speaker path (DSDIN + PA_Ctrl) is a separate capability, not implemented here.

## Ethernet (IP101 PHY, RMII)

On-chip EMAC → **IP101GRI** PHY → RJ45, **RMII** with a 25 MHz crystal, externally-fed 50 MHz reference clock. Same reference design as the Waveshare P4-NANO — confirmed pin-for-pin identical:

| Signal | GPIO | Source |
|---|---|---|
| MDC | 31 | schematic netlist (unambiguous two-pin net, matches the P4 pin table) |
| MDIO | 52 | schematic netlist (same) |
| PHY reset | 51 | direct schematic inspection (the flattened PDF text extraction was ambiguous here; confirmed by eye against the drawing) |
| RMII ref clock (external in) | 50 | carried over from the P4-NANO preset, consistent with the other three now-confirmed pins |

Catalog entry: `NetworkModule.ethBoard = "ESP32-P4-ETH"`, its own `kEthPresets` row in `NetworkModule.h` (not a silent alias of `"P4-NANO"`) — the pins read identical today, but a future correction to one board's preset shouldn't silently move the other.

## Other onboard features (not wired up by this branch)

From the Waveshare wiki: MIPI-CSI (2-lane, OV5647-compatible), MIPI-DSI (2-lane, 5"/7"/8"/10.1" panels), SDIO 3.0 microSD slot, USB-C (native + UART bridge), BOOT/RESET buttons. None of these conflict with the audio (GPIO 7-13) or Ethernet (GPIO 28-31/49-52) pins above.

## Free GPIO for user peripherals (LED strips)

Following the P4-NANO's own reference set (`ParallelLedDriver` default `pins="20,21,22,23,24,25,26,27"`), with this board's audio pins added to the committed set:

| Peripheral | GPIOs |
|---|---|
| Ethernet RMII (fixed silicon pads) | 28-31, 49-52 |
| ES8311 audio (I²S + I²C) | 7-13 |
| CSI/DSI accessory headers | share the I2C bus (7-8) plus their own dedicated MIPI diff-pairs |

**Clear, following the P4-NANO's documented set**: 20-27, 32-33, 39-48 (the catalog's `ParallelLedDriver` default is `pins="20,21,22,23,24,25,26,27"`, same as the P4-NANO). GPIO 20/21 also appear as spare IO on this board's CSI header (same situation as the P4-NANO, which treats them as free LED pins regardless) — only relevant if a camera module is actually plugged in. See [gpio-usage.md § ESP32-P4](gpio-usage.md#esp32-p4) for the general P4 pin map this board extends.

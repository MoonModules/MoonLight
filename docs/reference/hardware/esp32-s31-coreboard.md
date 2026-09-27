# ESP32-S31 Function-CoreBoard-1 hardware reference

Pin maps and onboard features for the Espressif **ESP32-S31 Function-CoreBoard-1**, read from the official schematic so MoonLight work (Ethernet, audio, SD, USB-host) reads this instead of re-scraping the PDF. The board is the bench S31 (`esp32s31` firmware).

**Sources**
- Schematic (rev C, 2026-05-13): <https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-schematics.pdf>
- PCB layout: <https://dl.espressif.com/schematics/esp32-s31-function-coreboard-1-pcb-layout.pdf>
- Datasheet (chip): <https://documentation.espressif.com/esp32-s31_datasheet_en.pdf>
- Module (WROOM-3): <https://documentation.espressif.com/esp32-s31-wroom-3_datasheet_en.pdf>
- User guide: <https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html>

The module is **ESP32-S31-WROOM-3**: RISC-V dual-core (≤300 MHz), Wi-Fi 6, BT, IEEE 802.15.4, on-chip 1 Gbps EMAC. It shares the MoonLive RISC-V backend with the ESP32-P4.

## Audio (ES8311 codec)

The onboard electret mic (J6) and speaker connect through an **ES8311 mono codec** (U7) + an
**NS4150B 3 W class-D amplifier** (U9). The ESP is the I2S master (it drives MCLK).

| Signal | GPIO | Direction / role |
|---|---|---|
| I2S_MCLK | 52 | master clock → codec (ESP is I2S master; MCLK = 256 × sample_rate) |
| I2S_SCLK (BCLK) | 53 | bit clock |
| I2S_LRCK (WS) | 55 | word select |
| I2S_ASDOUT | 54 | **mic / ADC data: codec → ESP** (the record path) |
| I2S_DSDIN | 56 | playback / DAC data: ESP → codec (speaker path) |
| **ESP_I2C_SDA** | **51** | codec control bus: **SDA is GPIO51, SCL is GPIO50** |
| **ESP_I2C_SCL** | **50** | codec control bus |
| PA_CTRL | 57 | NS4150B amplifier enable |

> **SDA/SCL are GPIO51/GPIO50**, the *opposite* of what the schematic's `ESP_I2C_SDA` /
> `ESP_I2C_SCL` net labels suggest. Bench-confirmed: the [I2cScanModule](../../moonmodules/core/moxygen/I2cScanModule.md)
> (sda=51, scl=50 in the S31 catalog entry) finds the ES8311 ACK at 0x18; with 50/51 nothing
> ACKs. The other audio pins match the schematic + the chip's GPIO table (all of GPIO50–57 are
> plain I/O GPIOs routed through the matrix, no special-function conflict).

- **ES8311 I2C address: `0x18`** (the default; set by the `CE` pin tie).
- Driven by Espressif's **`esp_codec_dev`** managed component (the ES8311 driver). The codec
  needs **MCLK running before it answers I2C**, and `es8311_codec_cfg.mclk_div` must be set (256, the standard ratio) or `open` fails "unable to configure sample rate". So AudioService brings up the I2S channel (which drives MCLK on GPIO52) **before** the codec I2C config.
- **Mic-only path** (audio-reactive input) needs MCLK/SCLK/LRCK + ASDOUT (record) + the I2C bus.
  The speaker path (DSDIN + PA_CTRL) is output, a separate capability.

## Ethernet (YT8531 PHY, RGMII, 1 Gbps)

On-chip EMAC → **YT8531** (Motorcomm) PHY (U8) → RJ45, **RGMII** with a 25 MHz crystal (Y2).

Pin map, **bench-verified** (link + DHCP confirmed on the CoreBoard). The RGMII data + clock GPIOs are the chip's fixed IO_MUX pads (the only ones the EMAC accepts; from IDF's `esp32s31/emac_periph.c`); MDC/MDIO are IDF's S31 SMI defaults (`ETH_ESP32_EMAC_DEFAULT_CONFIG`):

| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| ETH_INTN | 2 | | ETH_TXD3 | 11 |
| PHY_MDC | 5 | | ETH_TX_CTL | 12 |
| PHY_MDIO | 6 | | ETH_TXCLK | 13 |
| ETH_PHY_RST | 7 | | ETH_RX_CLK | 14 |
| ETH_TXD0 | 8 | | ETH_RX_CTL | 15 |
| ETH_TXD1 | 9 | | ETH_RXD3 | 16 |
| ETH_TXD2 | 10 | | ETH_RXD2 | 17 |
| | | | ETH_RXD1 | 18 |
| | | | ETH_RXD0 | 19 |

> **RGMII, not RMII.** MoonLight's classic/P4 Ethernet is RMII (fewer data lines, 50 MHz ref clock);
> the S31's 1 Gbps EMAC is RGMII (4-bit data each way + TX/RX clocks). The shared `ethInitEmac()` in
> `src/platform/esp32/platform_esp32.cpp` drives both: an `#ifdef CONFIG_IDF_TARGET_ESP32S31` block
> selects the RGMII interface and sets the CoreBoard's data/clock pins from the table above, then
> shares the driver-install / netif / DHCP tail with the RMII path. The board's default eth config
> (`ethConfigDefault` in `platform_config.h`) uses PHY type `ethYt8531` (PHY address auto-detected),
> driven by the generic IEEE-802.3 PHY ctor since the YT8531 is a standard-register PHY.
>
> **RGMII 125 MHz Tx clock = APLL.** The EMAC needs a clean 125 MHz Tx clock; the default MPLL is
> owned by PSRAM (400 MHz, no integer path to 125) and CPLL can't hit it on the 40 MHz XTAL grid, so
> `sdkconfig.defaults.esp32s31` sets `CONFIG_ETH_EMAC_RGMII_TX_CLK_SRC_APLL`, the fractional PLL
> synthesises 125 MHz exactly.

## Other onboard features

- **Addressable RGB LED**: WS2812 (D7) on **GPIO60**. *(Driven today: the catalog's S31
  `RmtLedDriver` uses `pins: "60"`, with `loopbackTxPin: 48` / `loopbackRxPin: 47` for the self-test, see [§ Free GPIO](#free-gpio-for-user-peripherals-led-strips-loopback).)*
- **SD card slot**: SD_D0–D3 / SD_CLK / SD_CMD on the module's SDIO pins (GPIO20–25 per the user
  guide). Note: `SOC_SDMMC_SUPPORTED` is **absent** in the S31 soc-caps, so the slot is likely SPI-mode (GPSPI) rather than the SDMMC peripheral, confirm before relying on it.
- **USB-A host**: USB 2.0 high-speed host port (5 V, 0.5 A limit).
- **USB-C ×2**: one is USB Serial/JTAG (native), one is a USB-to-UART bridge (CP2102N default, or
  CH9102X with the alternate BOM). Auto-download via DTR/RTS → EN/BOOT.
- **Buttons**: BOOT (GPIO61), RESET (EN).
- **J5, power/enable jumper** (a 2-pin header next to the 5V→3.3V DC/DC converter U4 and the 3.3V
  power-on LED D11). **Leave it installed.** With J5 removed the board is *half-powered*: the CP2102N runs off USB VBUS so its port still enumerates, but the ESP32-S31's 3.3V/EN rail is incomplete and the chip drives nothing, you get a serial port that opens but zero bytes from the MCU, at any baud, in any reset/download mode (see [lessons.md](../../work/past/lessons.md), which cost an hour of chasing a cable that wasn't the problem).
- **40-pin GPIO header** (J2). Optional 32.768 kHz crystal footprint (Y1, NC by default).

## Free GPIO for user peripherals (LED strips, loopback)

The board's own peripherals claim a large, contiguous low-GPIO block; the **J2 header** exposes the rest. Tallying the maps above, the **taken** pins are:

| Peripheral | GPIOs |
|---|---|
| Ethernet RGMII | 2, 5–19 |
| ES8311 audio (I²S + I²C) | 50–57 |
| SD card (SDIO/SPI) | 20–25 |
| Onboard WS2812 RGB LED | 60 |
| BOOT button | 61 |

**J2 is a 2×20 header**, each column carries two GPIOs, a top-row pin above a bottom-row pin. Reading the silkscreen left→right (top / bottom per column):

| col | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **top** | G | RX0 | 60 | G | 3 | 1 | **48** | 46 | 44 | 42 | 38 | 36 | D0 | D2 | CLK | 4 | C | 3V3 | G | 5V |
| **bottom** | G | TX0 | 61 | 2 | 0 | 49 | **47** | 45 | 43 | 40 | 39 | 37 | 35 | D1 | D3 | CMD | G | 3V3 | G | 5V |

The two pins in **one column are physically stacked**, so a 2-pin jumper cap bridges them with no flying wire, that adjacency is what makes a column a good loopback pair.

**Free on J2 for user I/O** (read off the table above, minus the board peripherals and boot straps): the numbered GPIOs on cols 5–16 (**4, 36, 37, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49**) are plain I/O clear of Ethernet (2, 5–19), audio (50–57), the SD lines (broken out as `D0`–`D3` / `CLK` / `CMD` by function, cols 13–16), the onboard LED (60) and the straps (0, 1, 3, 61). The `C` label at col 17 is a chip-enable, not a GPIO. **The GPIO numbers here are read from the board silkscreen and not yet bench-confirmed**: the S31 reference pin tables have been found off-by-one before (see the [S31 Ethernet lesson](../../work/past/lessons.md)), so probe a pin before committing a design to it.

**Recommended assignment** (what the S31 catalog entry uses):

- **LED strip data:** the onboard WS2812 is on **GPIO 60** (the catalog default). For an *external* strand, use **GPIO 42** as the single-lane pick; a parallel rig (RMT/Parlio) takes the free block (**36–49**, skipping 41 which isn't broken out) for several lanes. **GPIO 4** (col 16 top) also works as an LED data pin and sits one column from the `G` / `3V3` / `5V` power rail (cols 17–20), so a single strip's data + ground + 5 V wires land close together, handy for a tidy 3-wire pigtail. It's a plain I/O with no strap or peripheral tie on this board (the SD lines beside it, D0–D3 / CLK / CMD, are broken out by function name, not GPIO number, so GPIO 4 is *not* one of them; it just neighbors that cluster on the header). The only reason it reads as "distinct" from the rest of the free run is its header position, it's over by the SD/power group rather than in the low-block on cols 8–12.
- **Loopback self-test:** **Tx = GPIO 48, Rx = GPIO 47**, the two pins of **column 7** (48 top, 47 bottom), so a single jumper cap shorts them. A driver transmits a known WS2812 frame out Tx and reads it back on Rx to verify output on real silicon (same pattern as the P4-NANO bench's 32↔33). They sit at the top of the free run, clear of the operational LED pins so the strip wiring and the jumper don't interfere. **Bench-confirmed on the S31 for [RMT](../../moonmodules/light/drivers.md#rmtled) and the [Parallel LED driver](../../moonmodules/light/drivers.md#parallelled) across all three of its peripherals.** The S31 SOC has a real LCD_CAM (`SOC_LCDCAM_I80_LCD_SUPPORTED`), so its `i80` backend is LCD_CAM-driven (not the classic ESP32's I2S-in-i80-mode, which the platform layer excludes on any chip with real LCD_CAM) and can draw its frame from PSRAM. The `peripheral` selector therefore offers all three: **`i80`, `MoonI80`, and `Parlio`**, the RGMII Ethernet does not take the LCD_CAM block. Bench-verified: an 8x8 panel driving on `i80` (data GPIO 60), and Parlio on the free-block pins. Testing several drivers in a row, they all default loopback to GPIO 48, so only one can hold the pin at a time, toggle each driver's `loopbackTest` off before testing the next.

## SoC capabilities (from `components/soc/esp32s31/include/soc/soc_caps.h`)

Wi-Fi 6 · Bluetooth (no separate BLE soc-flag) · IEEE 802.15.4 (Thread/Zigbee) · USB-OTG · GPSPI · TWAI (CAN) · RMT · Parlio · LCD_CAM i80 · on-chip EMAC · PSRAM. RISC-V dual-core.

The S31 catalog entry drives **LEDs** (RMT on GPIO60) and **Wi-Fi 6**, and wires an
**[I2cScanModule](../../moonmodules/core/moxygen/I2cScanModule.md)** on the codec bus (SDA 51 / SCL 50) for
I2C bring-up. The **[AudioService](../../moonmodules/core/moxygen/AudioService.md)** ES8311 path is implemented, the codec seam configures the ES8311 over I2C (codec reachable, ACK at 0x18) and AudioService reads the I2S mic. End-to-end mic validation depends on confirming MCLK at GPIO52; the S31 entry keeps **Audio** under `planned` until that bench check passes, so the installer advertises only what's confirmed working. The other board capabilities (Ethernet, Bluetooth, SD, USB host, …) likewise live in the entry's `planned` list, see the S31 entry in `mooninstaller/deviceModels.json`.

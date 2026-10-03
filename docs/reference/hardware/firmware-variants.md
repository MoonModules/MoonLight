# Firmware variants and memory

What each firmware build contains, and what each class of device can run. Facts to look up; the reasoning behind them is in [the architecture](../../explanation/architecture/index.md).

## Firmware variants

Selected by `build_esp32.py --firmware <key>`, reported by `SystemModule.firmware`, and the contract target key in scenarios. Each chip's firmware carries the Ethernet driver it can host (RMII EMAC for classic and P4, W5500 SPI for S3); which PHY and pins a device model uses is runtime configuration.

| Variant | Chip | Network |
|---|---|---|
| `esp32` | classic | WiFi and RMII Ethernet in one binary; Ethernet comes up only when a PHY is present |
| `esp32-eth` | classic | Ethernet only, WiFi excluded |
| `esp32-16mb` | classic, 16 MB flash | WiFi and Ethernet |
| `esp32s3-n16r8`, `esp32s3-n8r8` | S3 | WiFi and W5500 SPI Ethernet |
| `esp32p4rev1-eth` | P4 rev 1 (Waveshare ESP32-P4-NANO) | Ethernet only |
| `esp32p4rev1-eth-wifi` | P4 rev 1 | Ethernet, plus WiFi via the on-board ESP32-C6 over esp_hosted |
| `esp32p4rev1-eth-es8311` | P4 rev 1 (Waveshare ESP32-P4-ETH) | Ethernet only, plus the board's onboard ES8311 audio codec (see [esp32-p4-eth.md](esp32-p4-eth.md)) |
| `esp32p4rev3-eth`, `esp32p4rev3-eth-wifi` | P4 rev 3 silicon | As rev 1. Rev 3 is not binary-compatible with rev 1; untested, no v3 board on the bench |

## Scaling to available memory

| Device | Memory | Typical capability |
|---|---|---|
| ESP32 + OPI PSRAM | 2-8 MB | Many layers, 10K+ LEDs |
| ESP32, no PSRAM | ~320 KB internal | Full pipeline: double buffering, mapping, blending, parallelism. Proven up to 16K lights (128x128 measured live on an Olimex Gateway) |
| Teensy 4.x | 1 MB internal, no PSRAM | Comfortable headroom for several layers; DMA-based LED output (OctoWS2811). Ethernet built-in on 4.1 |
| Desktop / RPi | Abundant | No constraints |

## Degradation cascade

When memory is short the pipeline steps down, best to worst. Each step is observable via `lutSkipped()` and reported per module in `/api/system`.

1. **Full pipeline**: LUT plus driver output buffer. Modifiers applied, clean separation.
2. **Skip LUT and driver buffer**: modifiers not applied, forced 1:1 mapping. A LUT without a driver buffer to map into is useless, so they go together.
3. **Reduce layer dimensions**: halve width and height until the buffer fits, minimum 8x8.

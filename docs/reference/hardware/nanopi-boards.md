# NanoPi boards

The NanoPi line from FriendlyElec runs MoonLight's Linux build: the same aarch64 package or Docker image as any other arm64 board, described in [Running MoonLight on a Linux machine](../installing-on-linux.md). Every model below is 64-bit, serves the same web UI and runs the same effects; what differs for an installation is the Ethernet, because a HUB75 receiving card wants its own link, and at 100 Mbit that link saturates and stutters; [Driving LED panels with a receiving card](panel-cards.md) explains why. Always put the card on a Gigabit port.

Specs are from FriendlyElec's store and wiki; prices are bare-board USD approximations checked 2026-11-06, not quotes. No eMMC is fine: MoonLight writes to disk only when settings change, so an SD card carries it comfortably.

| Model | SoC | CPU | RAM | Ethernet | Other | Price (~) | For MoonLight |
|---|---|---|---|---|---|---|---|
| R28S | RK3528A | 4× Cortex-A53 @ 2.0 GHz | 1 GB | 2× Gigabit | WiFi 6, USB-C console, 5 cm board | $20 | The value pick: dual Gigabit for house network + card link; renders 128×128 at 215+ fps |
| R2S | RK3528 | 4× A53 @ 1.8 GHz | 1 GB | 1× Gigabit | USB 3.0 | $25–35 | Older single-NIC sibling of the R28S |
| R3S / R3S-LTS | RK3566 | 4× A55 @ 1.8 GHz | 1–2 GB | 2× Gigabit (native + PCIe) | HDMI 2.0, speaker | $30 | Compact router board with HDMI; 1 GB, no eMMC runs fine from SD |
| R4S | RK3399 | 2× A72 + 4× A53 | 4 GB | 1× Gigabit | M.2 NVMe | $60–75 | Retail listings paused over a MAC-chip shortage |
| R5S / R5S-LTS | RK3568 | 4× A55 @ 2.0 GHz | 4–8 GB | 2× 2.5 GbE | M.2 NVMe, HDMI 4K | $45–65 | Sweet spot for NAS/router plus MoonLight, 2.5 GbE headroom |
| R6S | RK3588S | 4× A76 + 4× A55 @ 2.4/1.8 GHz | up to 8 GB | 2× 2.5 GbE | PCIe 3.0 M.2, HDMI 4K, 6 TOPS NPU | $60–85 | Performance king of the line |
| R76S | RK3576 | 8-core (A72 + A53) | up to 16 GB LPDDR5 | 2× 2.5 GbE | HDMI 2.0, M.2 WiFi/BT slot, 58×58 mm | $50–70 | Newest compact router/NAS board (2025) |
| M5 | RK3576 | 8-core | 4–8 GB | 2× 1 GbE | M.2 NVMe, 6 TOPS NPU | $55–75 | Larger M-series with NVMe |
| M6 | RK3588 | 8-core A76 + A55 | 8–16 GB | 2× 2.5 GbE | dual M.2, HDMI 8K | $100+ | Full-size flagship |

## Which one

- **R28S** when the job is one panel card plus the house network: two Gigabit ports, the cheapest board that does it.
- **R5S** when you want NVMe storage and 2.5 GbE spare, or several cards.
- **R6S or R76S** when the box renders several 128×128 outputs or takes video in.

All of them install and update the same way: [Install MoonLight](../../moonwalk.md#install) with Linux or Docker picked, and the container update is one command in [the update step](../../moonwalk.md#install). Older NEO and M4 models are legacy; skip them.

# Driving a HUB75 panel

A HUB75 LED panel can be driven straight from the board's pins with the [HUB75 driver](../../moonmodules/light/drivers.md#hub75), or over Ethernet through a receiving card with [Panel Card](../../moonmodules/light/drivers.md#panelcard). Board, panel and settings are below, in that order, and the readings behind the Raspberry Pi 5 figures are in the [HUB75 bench record](hub75-bench.md).

## Which of the two paths

A receiving card earns its place above roughly **16,384 pixels**. Below that, a card is a Windows tool to configure and a dedicated Ethernet link to run, for a panel the board could drive itself. The HUB75 driver is the small end of that split; for a card, see [Driving LED panels with a receiving card](panel-cards.md).

A [MoonHub75](https://moonmodules.org/projects/hardware/#moonhub75-pcb) drives a 64x64 1/32-scan panel at 6-bit and 1,508 Hz. Larger walls and other panel families wait on reports: the `refresh` reading plus your geometry makes one useful.

## Which board

The chip needs an LCD_CAM or a Parlio block, which the S3, the P4 and the S31 carry and the classic ESP32 lacks. Its 13 usable output GPIOs are all a 1/16 port needs, so a strand, a button or a microphone has no pin left.

A Raspberry Pi 5 drives panels through its RP1 PIO from the 40-pin header instead: [Raspberry Pi 5](#raspberry-pi-5-rp1-pio).

Three boards have published pin maps, which the `board` select fills in:

- **MoonHub75**, a Lilygo T7-S3 on a passive adapter. It also carries an INMP441 microphone socket on IO10/11/12, so an audio-reactive effect and a panel run together.
- **Adafruit MatrixPortal S3.** Three of its lines sit on GPIO 35 to 37, which the octal-PSRAM images hold, so it runs the `esp32s3-n8r2` image, built for its quad-PSRAM N8R2 part. It has no microphone; one wires to the free header pins, A0 to A4 and TX/RX, which its [pinout](../../assets/deviceModels/esp32-s3-n8r2-adafruit-matrixportal-pinout.jpg) shows, and takes an Audio service set to those pins.
- **Waveshare ESP32-S3 RGB Matrix**, SKU 34422. The Waveshare ESP32-S3-Matrix is a different product, with an onboard 8x8 matrix and no HUB75 connector.

A published map hides the pin rows, since those lines are soldered; they stay bound and still drive the panel. The generic per-chip sets and Custom show them. A line on a pin the chip wires to flash or PSRAM is refused before init, with the line named. Free pins per chip: [GPIO usage](gpio-usage.md).

!!! tip "The one naming trap"
    A panel's ribbon names its color lines R1/G1/B1 (upper half) and R2/G2/B2 (lower half). Some board docs call the same pairs R0/G0/B0 and R1/G1/B1. A panel that lights every pixel in the wrong hue has its color lines in another order: swap the pins, not the panel.

## Which panel

The driver speaks to plain shift-register driver chips, such as the MBI5124, where the board makes the gray levels as bit planes and the latch takes one clock. An S-PWM chip (the ICN1065, the DP3264S and their relatives) makes its own gray and expects a register setup the driver does not send, so it stays dark, and a chip that wants several clocks under the latch, such as the ICN2038S, is not driven either. The driver chip's marking tells the families apart: the advertised gray depth, refresh rate, pitch, size and scan rate do not, so ask the seller for the marking. [ledwall](https://github.com/siegeld/ledwall) drives both families from a Colorlight receiving card's FPGA, and its panel notes explain the difference.

## Settings the panel cannot tell you

Steps: [Set up a driver or service for the hardware](../../moonwalk.md#driver-setup), its HUB75 choice.

- **`scanRate`** is read off the panel's label or listing, not calculated: two panels of the same size can scan differently. A 1/32-scan panel needs the `e` line; on 1/16 that pin is free.
- **`peripheral`** matters on a P4, which has one LCD_CAM and one Parlio: a board already driving LED strips from one needs the panel on the other.
- **`clockEdge`**: every pixel one column over means the other edge.

## Depth, refresh and brightness

Every bit plane is a full scan, so `bitDepth` costs refresh and memory in proportion to the planes on a wider row. A row of 2^(`bitDepth`−2) clocks or fewer, 16 at 6-bit, is stretched with hold words, which cost more of both ([hold words](hub75-refresh.md#plane-weights-and-hold-words)). One 64x64 panel at 1/32 scan is 16,384 bytes a frame at 4-bit and 24,576 at 6-bit. Brightness on a HUB75 panel is time, not amplitude, so lowering it shortens each plane's lit time and keeps every color level. The price is top brightness: full white lights about a third of the time at 6-bit.

On the bench (2026-10-06, the MoonHub75's panel on LCD_CAM) 4-bit refreshes at 2,157 Hz, against the 2,441 Hz [ceiling](hub75-refresh.md#how-drivers-set-the-lsb) its shift clock predicts. What a longer chain or deeper color costs: [HUB75 refresh rates](hub75-refresh.md). 6-bit costs about 2 ms more render tick, about 57 against 64 fps.

If a panel flickers, `refresh` is the measured rate to report. Flicker only in the dark areas, while bright ones hold still, is a refresh rate too low: a dim pixel is lit for one short pulse a refresh, and the eye catches the short pulse first.

**A panel takes its own 5 V supply on its power connector.** A lit row draws all its columns at once, so the peak current does not fall with brightness: a 64x64 panel at full red and blue pulls about 2.6 A. A supply that sags under it, such as a controller board's USB, drops the blue and green LEDs first, which need about 3 V where red needs 2, so a color with much red in it shows as red or orange. A 128x64 panel measured about 4 A at full white, and panels differ, so a supply is sized from each panel's rated maximum current times the panel count, and the voltage is checked at the far panel's connector under a white frame. A picture that holds on dark content and breaks up on bright content is a power fault, whatever it looks like.

## Raspberry Pi 5 (RP1 PIO)

A Pi 5 drives HUB75 panels from its 40-pin header through one RP1 PIO state machine, which clocks all three lanes in lockstep from one 32-bit word per pixel clock. The engine drives the PIO through the kernel's own interface, `/dev/pio0`, with no library in between, so one program carries everything.

### Which output

The HUB75 driver takes its output from the machine: an ESP32 takes the bus output, LCD_CAM or Parlio by chip, and a Pi 5 takes the PIO output. The PIO output stays selected when the PIO is refused, and its status gives the reason. A Pi 4 or earlier shows the Pi card with a refusal in its status.

The machine comes from the kernel's device tree, `/proc/device-tree/compatible`. A container that masks that file (Docker does by default) falls to the `Model` line of `/proc/cpuinfo`, so a Pi 5 in a container is still a Pi 5; the device tree wins where both can be read. The container also needs `/dev/pio0` passed through, for example `docker run --device /dev/pio0`.

### Wiring

The board is the active 3-channel one, wired as hzeller's `regular` mapping (see [attributions](../attributions.md#hzeller)). The wiring is soldered, so the card carries no pin controls. Three lanes of HUB75 data lines fan out from shared address, clock, latch and output-enable lines, and each lane takes up to 32 panels in series.

| Signal | GPIO | Header pin |
|--------|------|-----------|
| OE (active low) | 18 | 12 |
| CLK | 17 | 11 |
| STB (latch) | 4 | 7 |
| A | 22 | 15 |
| B | 23 | 16 |
| C | 24 | 18 |
| D | 25 | 22 |
| E (1/32 scan) | 15 | 10 |

Each lane's color lines, as GPIO with the header pin in parentheses. R1, G1 and B1 feed the upper half of the panel, R2, G2 and B2 the lower half.

| Lane | R1 | G1 | B1 | R2 | G2 | B2 |
|------|----|----|----|----|----|----|
| 1 | 11 (23) | 27 (13) | 7 (26) | 8 (24) | 9 (21) | 10 (19) |
| 2 | 12 (32) | 5 (29) | 6 (31) | 19 (35) | 13 (33) | 20 (38) |
| 3 | 14 (8) | 2 (3) | 3 (5) | 26 (37) | 16 (36) | 21 (40) |

E carries row address bit 4, so a 1/32-scan panel needs it, on HUB75 pin 4 or 8 depending on the panel; the board has a jumper for it.

### Geometry

`rows`, `cols` and `panelsPerLane` are one panel's size and the panels in series on one lane. The layout's lights, cut into whole panels, are the panels, and the lanes driven at once follow, up to the board's three. The defaults are a 256x192 wall: 128x64 panels, 2 to a lane, 3 lanes, stacked to 256x192 at 1/32 scan and 6-bit, clocked at 10 MHz. The scan follows the rows, `rows` / 2, so a 64-row panel scans 1/32 on its own. Only a `rows` value of 16, 32 or 64 opens; any other is refused at open, with the status naming the rows that work. A geometry that does not cable into the lanes is refused with the way out, and a layout of another count, or off plain rows, shows a warning. The Pi card's settings live on the driver, so a saved card keeps them through a boot where the PIO refuses.

### Pins the kernel may own

The engine muxes GPIO 2 to 27 to the PIO, and the board uses every one of them. A kernel driver owns some of them when it is enabled, takes them back from the panels, and corrupts the picture. Free them in `/boot/firmware/config.txt` before wiring a panel; the driver cannot see these lines.

| Pins | Owner when enabled | Free them with |
|------|--------------------|----------------|
| GPIO 2, 3 | I2C1 | `dtparam=i2c_arm=off` |
| GPIO 7 to 11 | SPI0 (lane 1's color pins) | `dtparam=spi=off` |
| GPIO 14, 15 | UART0 (lane 3's R1 and the E line) | `enable_uart=0` |
| GPIO 4 | 1-wire (the strobe) | remove `dtoverlay=w1-gpio` |
| GPIO 18 to 21 | I2S (18 is OE); an I2S microphone is what an audio-reactive install adds | remove `dtparam=i2s=on` |
| GPIO 12, 13, 18, 19 | the PWM overlays: `pwm` (pin 18 by default), `pwm-2chan` (pins 18 and 19 by default) and `audremap-pi5` or `audremap` (pins 12 and 13 by default, `audremap` also 18 and 19) | remove the `dtoverlay=` lines of those overlays |

### Access to the node

The engine opens `/dev/pio0` for reading and writing. On the bench Pi the node reads `crw-rw---- root gpio`, so the user MoonLight runs as joins the group that owns it, `gpio` there. The Debian package's install does this for the `moonlight` service user: it reads the group from the node and adds the user to it. A failed open names the node's mode, owner and group, so the status line gives the group the machine uses. A node owned by `root` and group `root` takes a udev rule that gives it a group (`SUBSYSTEM=="*-pio", GROUP="gpio", MODE="0660"`), then a reboot. The package's install skips a node whose group is `root`, so after the rule the service user joins the group by installing the package again, which runs the install step again, or by `sudo usermod -a -G gpio moonlight`, and the service restarts with `sudo systemctl restart moonlight`.

### Kernels

The output opens where `/dev/pio0` exists and the kernel serves a PIO. What the frame submit returns depends on the kernel:

- **6.12 (rpi-6.12.y):** the submit returns when every chunk has reached the state machine. A wait for a free buffer lasts up to 1 second; when it expires the kernel logs "DMA bounce timed out" and ends the copy, so the call returns 0 with the frame cut short and no error, after about 2 seconds in all. The completion wait that follows lasts up to 1 second per chunk and answers a timeout (-ETIMEDOUT) when it expires: the engine counts a failed submit, `shown` falls to 0, the status says the PIO is failing, and the engine retries after a pause. The engine's lowest pixel clock, 50 kHz, keeps one 64 KiB chunk inside that second.
- **6.18.39 and later:** the submit returns once the last chunk is queued and blocks only while all four DMA buffers are busy. A state machine that stops draining blocks the call for good, and the engine's shutdown reports it.
- **6.18.39 up to 6.18.59:** a PIO reopen stalls. The output refuses to open on these kernels and the status gives the reason. 6.18.60 and later serve PIO.

### Pixel clock and refresh

The frame is `bitDepth` x scan rows x (lane width + the encoder's hold words) pixel clocks, so one state machine sets the refresh by the clock: `pixelClock` runs from 1 to 40 MHz. A lane of at most 2^(`bitDepth` - 2) words, 16 at 6-bit and 64 at 8-bit, carries hold words so that its lowest plane still lights; a wider one, such as the wall's 256, carries 0. `latchBlank` blanks 0 to 8 of the words before each latch, for a panel that ghosts: the frame keeps its length, and only the lit time shortens. The card quotes the computed wire rate, 203.5 Hz on the default wall at 10 MHz and 6-bit, and `shown` is the measured rate, 169 to 170 fps on the bench on kernel 6.18 and 198 on 6.12.

The gap is the DMA feed, which falls behind the wire as the clock rises: a frame took 1.2 times its wire time at 10 MHz and 1.8 times at 20 MHz on 6.18 ([bench record](hub75-bench.md#refresh-at-6-bit-and-8-bit)), and about 1.0 and 1.7 times on 6.12 ([the kernel-interface backend](hub75-bench.md#the-kernel-interface-backend-2026-11-20)). When the feed runs behind, the state machine stalls on an empty FIFO inside the frame, and a stall lengthens the word it stops on, which shifts plane weights. A pixel clock above 20 MHz, the highest the bench ran, shows a warning in the status.

### 8-bit color

The card offers 2 to 8 planes, starting at 6; 8 is set on the card. A frame at 8-bit is 8/6 the words of 6-bit, and the bench readings scale by 6/8 ([bench record](hub75-bench.md#refresh-at-6-bit-and-8-bit)). A narrow row leads with hold words, which cost refresh: the rule is `hub75HoldWords` in [`Hub75Bus.h`](https://github.com/MoonModules/MoonLight/blob/main/src/platform/Hub75Bus.h), and [hold words](hub75-refresh.md#plane-weights-and-hold-words) shows the effect. The brightness below which the lowest plane drops out is in [`Hub75Encode.h`](https://github.com/MoonModules/MoonLight/blob/main/src/platform/Hub75Encode.h): 64 on the wall's 256-word rows at 8-bit, where the picture reads as 7-bit.

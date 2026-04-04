# MoonBase Overview

<img width="170" src="https://github.com/user-attachments/assets/87ed3c78-7a4e-4331-b453-47762cce64fa" />

MoonBase is the IoT foundation that MoonLight is built on. It runs independently of lights — you can use MoonBase on its own to build custom ESP32 applications with WiFi, MQTT, OTA, file management, task monitoring, and hardware I/O, all with a reactive SvelteKit web interface.

---

## What's inside MoonBase

| Module | What it does |
|---|---|
| [File Manager](https://moonmodules.org/MoonLight/moonbase/filemanager/) | Browse, create, edit, upload and download files on the device filesystem |
| [Devices](https://moonmodules.org/MoonLight/moonbase/devices/) | Discover and control MoonLight and WLED devices on your local network |
| [Tasks](https://moonmodules.org/MoonLight/moonbase/tasks/) | Live view of FreeRTOS tasks — CPU usage, state, stack, priority |
| [IO](https://moonmodules.org/MoonLight/moonbase/inputoutput/) | Board presets, pin assignment, power budget, ethernet, I2C peripherals |

---

## File Manager

The File Manager gives you direct access to the ESP32's LittleFS filesystem through the browser. You can:

- Navigate folders and create new files or directories
- Edit text files (`.json`, `.sc`, live scripts) directly in the browser
- Upload files from your computer (e.g. live script `.sc` files)
- Toggle visibility of hidden files (names starting with `.`)

Live scripts live in `/livescripts/` on the device. The File Manager is the easiest way to deploy or edit them without rebuilding firmware.

---

## Devices

The Devices module discovers all MoonLight and WLED devices on your local network using mDNS and the SuperSync broadcast protocol.

For each device you can see:

| Column | Description |
|---|---|
| **Name** | Device hostname — click to open its web UI via mDNS |
| **Lights On** | Toggle the device on/off |
| **Brightness** | Adjust brightness |
| **Palette** | Active palette |
| **Preset** | Active preset |
| **Last Sync** | How long ago a heartbeat was received (MoonLight: every 10 s; WLED: every 30 s) |
| **IP** | Click to open the device UI by IP address |
| **Version** | Firmware version |
| **Build** | Firmware build date |
| **Uptime** | Time since last boot |

### SuperSync

MoonLight devices broadcast their state every 10 seconds (and on every control change). The Devices module listens for these broadcasts and updates the table in real time.

Devices can be **grouped by hostname** using hyphens — for example `studio-left-1` and `studio-left-2` belong to the same group. When a grouped device receives a control update, it forwards the change to other devices in its group, keeping them in sync without manual coordination.

Distributed effects and synchronised clocks are planned for a future release.

---

## Tasks

The Tasks module shows a live snapshot of all FreeRTOS tasks running on the device — useful for diagnosing performance issues or runaway CPU usage.

| Column | Description |
|---|---|
| **Core** | Which core the task is pinned to (Core 0 or Core 1) |
| **Name** | Task name |
| **State** | 🧍 Ready · 🏃 Running · 🚧 Blocked · ⏸️ Suspended · 🗑️ Deleted |
| **CPU** | Percentage of CPU time consumed (per core) |
| **Priority** | FreeRTOS task priority |
| **Stack left** | Remaining stack space in bytes |
| **Runtime** | Total CPU cycles consumed |

MoonLight's three main tasks:

| Task | Core | Role |
|---|---|---|
| **AppEffects** | Core 0 | Runs effect nodes, writes `virtualChannels` |
| **AppDrivers** | Core 1 | Reads `channelsD`, sends data to LEDs/ArtNet/DMX |
| **Sveltekit** | Core 1 | HTTP server, WebSocket, module `loop()` |

---

## IO

The IO module defines the hardware identity of your ESP32 — which board it is, what each pin does, and how much power the LED driver is allowed to consume. All other nodes (LED drivers, audio, ethernet, I2C) read their pin assignments from here.

### Board Presets

Select your board from the dropdown and all pins, power budget, ethernet type, and switches are configured automatically. Currently supported boards include QuinLED (Dig-2-Go, Dig-Next-2, Dig-Uno, Dig-Quad, Dig-Octa), SE16, LightCrafter16, MHC P4 Nano Shield, Olimex ESP32-POE, Atom S3R, and more.

If your board is not listed, select the default preset and assign pins manually — the **modded** flag is set automatically so future preset changes won't overwrite your configuration.

### Pin Types

| Category | Pin types |
|---|---|
| **LED outputs** | LED data (WS2812B / SK6812), CW/WW PWM, R/G/B PWM |
| **Audio (I2S)** | SD, WS, SCK, MCLK — supports INMP441 (standard I2S) and PDM microphones |
| **I2C** | SDA, SCL — auto-scans for peripherals when both are assigned |
| **Buttons & relays** | Push button, toggle switch, LightOn button/relay |
| **Energy monitoring** | Voltage, current, battery ADC inputs |
| **Ethernet** | RMII data pins, SPI (W5500), MDC/MDIO, clock, power |
| **Communication** | Infrared receiver, RS-485 TX/RX/DE, UART serial |
| **Sensors** | PIR motion sensor |

See [IO Module](https://moonmodules.org/MoonLight/moonbase/inputoutput/) for the full pin reference, ethernet type selection, board-specific wiring notes, and I2C peripheral scanning.

---

## Status Bar

<img width="306" src="https://github.com/user-attachments/assets/5568bcdf-8d12-430e-9801-3c851f4204b5" />
<img width="362" src="https://github.com/user-attachments/assets/2379c5c4-0b85-4810-aac3-d4e6c650a12b" />

The status bar at the top of every page shows live device state at a glance:

| Icon | Meaning |
|---|---|
| 🛡️ | **Safe mode** — device crashed and restarted with reduced functionality. Click to restart. Once the crash cause is gone, the device boots normally. In safe mode: no layouts > 1024 lights, no LED drivers, no live scripts. |
| 🔄 | **Restart needed** — a change (e.g. new driver) requires a reboot to take effect. Click to restart. |
| 💾 / ↻ | **Save / Cancel** — unsaved changes are pending. Save applies them permanently; Cancel discards and restores the last saved state. |
| ☀️ / 🌙 | **Theme toggle** — switch between Light, Dark, and Auto. |
| ⚡️ | **Energy** — charging voltage and current (only on boards with energy monitoring, e.g. SE16). |
| 🔔 | **New firmware** — a newer firmware version is available for download. |
| 📶 | **WiFi** — connection status (see [ESP32-SvelteKit](https://github.com/theelims/ESP32-sveltekit)). |

---

## Reporting Issues

Found a bug or want to request a feature? See the [reporting issues guide](../moonlight/overview.md#reporting-issues) for guidelines on writing a good bug report, including how to attach the module API output.

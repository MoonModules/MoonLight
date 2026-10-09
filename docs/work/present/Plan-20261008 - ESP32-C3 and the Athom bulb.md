# Plan: ESP32-C3 and the Athom bulb

MoonLight runs on the ESP32-C3, starting with a C3 board on the bench and then an IoTorero-branded Athom 12W RGBCW bulb that runs WLED today.
The bulb has no USB in reach, so it moves from WLED to MoonLight over WiFi: one WLED firmware update installs a small image that rewrites the partition table and puts MoonBase in place, and MoonBase installs MoonLight.

## What the bulb is today

Read from `http://192.168.1.230` (`/json/info`, `/cfg.json`, `/json/state`, `/presets.json`) on 2026-10-08.
WLED never returns passwords, so only their lengths are known.

| What | Value |
|---|---|
| Product | Athom_12W_Bulb (www.athom.tech), sold as IoTorero |
| Chip | ESP32-C3 at 160 MHz, 4 MB flash in DIO mode, no PSRAM |
| Firmware | WLED 0.15.0 (build 2412100, "Kōsen"), release name `ESP32-C3`, built 2025-09-01 on Arduino core with ESP-IDF v4.4.4 |
| Free heap under WLED | 132 KB |
| MAC | 10:00:3B:DB:96:00 |
| Name, mDNS | `Bulb1`, `wled-DB9600` |
| WiFi | SSID `MoonModules` (password 9 characters), DHCP, channel 11, RSSI -52 dBm, TX power 19.5 dB, sleep off |
| Access point | `IoTorero_DB9600` (password 8 characters), channel 1, 4.3.2.1, opens when no connection |
| Light | one light, 5-channel PWM (WLED type 45, RGB plus warm and cold white) |
| PWM pins | red GPIO6, green GPIO7, blue GPIO5, warm white GPIO4, cold white GPIO3 |
| PWM frequency | 9765 Hz |
| Power limit | 850 mA on, LED current per light 0 (WLED's PWM default) |
| Color handling | gamma 2.8 on color, white from RGB mode 255 (per-bus setting 0), CCT blending 0 |
| Defaults at boot | on, brightness 255, no preset |
| Transition | 700 ms |
| Current color | primary [41, 76, 255, 86] (RGBW), effect 0 (solid) |
| Buttons | button 1 on GPIO0 with type 0 (disabled); no IR, no relay, no I2C, no SPI |
| Realtime | E1.31 on, universe 1, DMX address 1, mode 4 (multi RGB), timeout 2.5 s |
| Sync | WLED UDP on 21324, receive on (brightness, color, effect, palette), send off |
| Off | MQTT, Hue, Alexa, NTP, ESP-NOW |
| OTA | not locked, ArduinoOTA on, so `/update` takes an image |
| Presets | none |
| Filesystem | 12 KB used of 983 KB |

Flash layout, from `getfreeflash` (the other app slot, 1.5 MB) and the filesystem size (960 KB), which together match WLED's `WLED_ESP32_4MB_1MB_FS.csv`:

| Partition | Offset | Size |
|---|---|---|
| bootloader (ESP-IDF v4.4.4) | 0x0 | |
| partition table | 0x8000 | |
| nvs | 0x9000 | 20 KB |
| otadata | 0xE000 | 8 KB |
| app0 | 0x10000 | 1.5 MB |
| app1 | 0x190000 | 1.5 MB |
| spiffs (LittleFS) | 0x310000 | 960 KB |

MoonLight is 2.2 MB on the classic ESP32 and does not fit a 1.5 MB slot; MoonBase is 0.8 MB and does.

## Why the partitions can change without USB

An OTA update writes an app, never the partition table, but the app it writes can.
WLED 0.15.0's `/update` takes any valid C3 image up to the slot size (to confirm on the bench, step 4).
ESP-IDF documents that a bootloader boots apps built with a newer ESP-IDF, so the bulb keeps its v4.4.4 bootloader and runs an ESP-IDF 6.1 app ([bootloader compatibility](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/bootloader.html#bootloader-compatibility)).

The image WLED installs is a **migration image**: an ESP-IDF app carrying MoonBase for the C3 and MoonLight's 4 MB partition table, about 0.9 MB in all.
It runs once:

1. **Out of the way.** It needs WLED's app0 free, since MoonBase goes to 0x10000. Running from app0, it copies itself to app1, points otadata at app1 and restarts.
2. **MoonBase.** Writes MoonBase to 0x10000 and checks its SHA-256 against the embedded copy.
3. **The table.** Writes MoonLight's partition table to 0x8000 and reads it back.
4. **The choice of app.** Erases otadata, so the bootloader starts the factory app, which is MoonBase.
5. **Restart.** MoonBase starts on the network WLED was on, carried over from WLED's `wsec.json`, and installs MoonLight by URL. Without a readable `wsec.json` it opens its access point, and joining it from a phone provisions the network.

What a power cut does at each step:
- before step 3, the old table and otadata still start the migration image, which starts over;
- after step 3, the new table's otadata names no valid app (or none at all), so the bootloader starts the factory app, MoonBase;
- **during step 3**, a 4 KB sector erase and write of about 50 ms, the table is gone and the bulb needs serial. This is the one window that bricks, and step 4 rehearses every cut on the bench board.

Writing the table needs `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED`, set in the migration image only.
The C3 bootloader sits at 0x0 (0x1000 on the classic ESP32); the partition table is at 0x8000 on both.

Prior art: Tasmota's Partition Wizard moves a running device to a new layout over OTA in the same way. Going through it would mean installing Tasmota first, then running its wizard, then a further OTA step. The migration image does the job in one OTA step.

## Steps

### 1. The ESP32-C3 firmware ✅

**Done.** Built, shipped in the release (`ships: True`), flashed on the bench C3 and the bulb. MoonLive runs on the C3 with native code from SRAM. Fixes on the way: a task asking for a core the chip lacks runs unpinned, the FPU capability is guarded, the C3 takes the W5500 component, MoonBase compiles its Ethernet handler only where there is a MAC. An RMT strip on the C3 is untested.

- An `esp32c3` variant in `build_esp32.py`, 4 MB with MoonBase, and `moonbase-esp32c3`.
- The 4 MB MoonBase table is chip-neutral, so `esp32dev_moonbase.csv` becomes `4mb_moonbase.csv`, shared by the classic ESP32 and the C3. That rename is this plan's subtraction: one table, not a copy per chip.
- The platform layer on the C3:
  - one core;
  - no PSRAM;
  - two RMT TX channels;
  - no I2S parallel or LCD bus;
  - the console and Improv on USB Serial/JTAG (GPIO18 and GPIO19);
  - 400 KB SRAM.
- MoonLive on the C3: the RISC-V backend emits RV32IMC only (the P4 has more), and code runs from SRAM, which needs ESP-IDF's memory protection off (`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE`). A C3 build without the JIT is the fallback when this resists.
- The release workflow and `check_firmwares` learn the variant; the installer learns the `ESP32-C3` chip name.

### 2. Device models ✅

**Done**, as `ESP32-C3 SuperMini` (its pin map as the image), `ESP32-C3 RGBWW PWM bulb` (the Athom 12W as its example), `QuinLED An-DecaPenta` and `QuinLED An-Penta-Plus` (WiFi supported, PWM outputs planned until their pins are known).

- `ESP32-C3 SuperMini`: chip `ESP32-C3`, firmware `esp32c3`, `RmtLedDriver` on the board's LED pin.
- `ESP32-C3 RGBWW PWM bulb`: chip `ESP32-C3`, firmware `esp32c3`, one light, the PWM driver on the five pins above at 9765 Hz under the `RGBCCT` fixture, the curve left at its default.

### 3. A PWM light driver ✅

**Done.** `PwmLightDriver` with its platform seam, six desktop tests and its card on the drivers page; runs the bulb's five channels, seen working. The white order follows WLED's config on the bulb: cold on GPIO3, warm on GPIO4.

The bulb drives its five channels with PWM through the C3's LEDC peripheral, and MoonLight has no PWM driver yet.
Every ESP32 has LEDC, so the driver serves any PWM light: a bulb, an analog RGB, RGBW or RGBCCT strip, a constant-current controller such as the QuinLED An-Penta boards.

**The spec: PWM Light, a driver card under LED drivers.**

- **One pin per output channel.**
  `pins` lists GPIOs in channel order, and the shared `fixture` profile names what each channel carries, so the driver adds no channel vocabulary of its own.
  The light count is the pin count divided by the profile's channel count: the bulb's 5 pins under `RGBCCT` are 1 light, the An-DecaPenta's 15 are 3.
  Pins past the last whole light idle, and the status says how many.
- **The shared correction applies as on every driver:** `localBrightness`, `curve`, `whiteMode`, `start` and `count`.
- **`frequency`, in Hz**, default 19531, the rate QuinLED recommends.
  The resolution follows from it rather than being a second control: the LEDC clock divided by the frequency bounds the duty steps, so the driver takes the most bits that fit, up to the chip's timer width.
  At 19531 Hz that is 12 bits, and at the bulb's 9765 Hz 13.
- **Duty:** the curve in 16 bits, scaled to the resolution, so the bottom of a fade keeps its steps.
- **Phase:** each channel starts its pulse at an evenly spaced point of the period, so a light at full white does not switch every channel at the same instant, which spreads the current draw.
- **Limits, from the SDK's capabilities:** one LEDC timer per driver, so one frequency per driver and up to four PWM drivers; the chip's channel count across its speed modes (16 on the classic ESP32, 6 on the C3). More pins than channels is an error naming the limit.
- **The hot path** writes a channel's duty only when it changed, a register write that does not block.
- **The platform seam:** `pwmStart(frequency)` returning the resolution, `pwmAttach(channel, pin, phase)`, `pwmWrite(channel, duty)` and `pwmStop()`, with a desktop stub that records the duties, so the mapping is tested on the desktop.
- **Pins** are claimed through the pins registry, as the LED drivers claim theirs.
- **Tests:** lights times channels to duties through the stub, the resolution from the frequency, a partial last light, more pins than channels, the phase spacing.
- **Catalog:**
  - The Athom bulb: `pins` "6,7,5,3,4" (red, green, blue, cold, warm, the order `RGBCCT` names), `fixture` `RGBCCT`, `frequency` 9765.
  - The An-Penta boards get their pins once QuinLED's pinout guide gives them.

### 4. The bench C3, then the rehearsal ✅

**Done.** The whole path over WiFi alone on the bench, a power cut at each of the five pauses and the move-aside path all came back without a cable; the network is written before the table, so a cut after the table still finds it. The image checks the board before writing anything and restarts WLED when it cannot finish, re-verified on the normal and move-aside paths; the refusals themselves are untested on hardware. The image is `moonbase/migrate/`, shipped as `shared-migrate-esp32c3.bin`, with the how-to `docs/how-to/migrating-over-the-air.md`.

On `/dev/cu.usbmodem202134311`, after a repower (it was wedged on the last probe, MAC 08:92:72:85:B3:60):
1. Identify the chip and flash size.
2. Flash MoonLight `esp32c3` over USB; verify WiFi, the UI, MoonLive, and an RMT strip.
3. Flash it back to the bulb's exact state: WLED 0.15.0's ESP32-C3 release, its v4.4.4 bootloader and the `WLED_ESP32_4MB_1MB_FS` table.
4. Run the bulb's path over WiFi only: WLED `/update` with the migration image, then MoonBase on the carried-over WiFi, then MoonLight by URL.
5. Cut the power at each step with a build that pauses between steps; every cut outside step 3's window has to come back.

### 5. The bulb ✅, Glow 🚧

**Done** on 2026-10-08: WLED took the image, MoonBase came up on the WiFi in 6 seconds, MoonLight installed and runs the bulb's catalog entry. **In progress:** Glow, iterated on the bulb to follow the bass, waits for your verdict before it moves into `moonlive/effects/`.

- Before anything is written: every file WLED's `/edit?list=/` lists, downloaded for reference into the appendix below. WLED hides `wsec.json` from the listing and from HTTP, so only the migration image reads it.
- With step 4 green and your go-ahead: the same `/update` on 192.168.1.230.
- MoonBase on the carried-over WiFi, MoonLight by URL, then the device model `Athom 12W RGBCCT bulb`.
- An audio-reactive effect for the bulb: `glow.mle`, the whole rig as one light pumping with the bass, iterated on the bulb and copied into `moonlive/effects/` once settled.

### 6. Apply a device model from the device's own UI ⏳

**Next**, a commit of its own.

A catalog entry in `deviceModels.json` reaches a device two ways: the web installer sends it over USB at install time, and MoonDeck sends it over the network when a model is picked.
A device that came to MoonLight over WiFi has neither, so the Athom bulb's PWM pins, fixture and grid were set through MoonDeck; the same holds for any device a user sets up later.
- **The catalog:** the device's UI fetches the `deviceModels.json` the web installer publishes on moonmodules.org, as it fetches the release list for a firmware update.
- **The picker:** on the System card's `deviceModel`, showing the boards of the device's own chip.
- **Applying:** each container's `state` through `PATCH /api/state`, Drivers last, as MoonDeck does.
- **A confirmation** that names what the entry replaces, since its containers carry `"$patch":"replace"`.
- **Offline:** without internet the catalog does not load, and the picker says so.
- **Tests:** a JS test for the chip filter and the document order, beside the ones pinning MoonDeck's and the installer's.

## If the bulb has to be opened

The bulb runs on mains through a non-isolated supply: unscrew it from the socket and leave it unpowered for a minute before opening it, and never connect anything to it while it is in a socket.

- **Opening.** The diffuser dome is pressed and glued into the body. Warm the seam with a hair dryer for a minute, then twist and pull with a rubber grip, or work a guitar pick or plastic spudger around the seam.
- **Inside.** A round aluminum LED board, with the C3 module on it or on a small upright board in the neck. Expect no USB connector.
- **Pads to look for:**
  - 3V3, GND, TX (GPIO21), RX (GPIO20);
  - IO9, the boot strap: held low at power-up, it starts the ROM's download mode;
  - sometimes EN;
  - sometimes GPIO18 and GPIO19, the C3's native USB D- and D+.
- **Flashing through them.** Either a 3.3 V USB-serial adapter on TX, RX, GND and 3V3 with IO9 to GND at power-up, or a USB cable soldered to GPIO18 (D-), GPIO19 (D+) and GND with 3.3 V from a regulator. Never put 5 V on 3V3.
- **Before you go further.** A photo of both sides of the board settles which pads exist.

## Decisions

- **The PWM driver is part of this plan**, since without it the bulb runs MoonLight dark.
- **The migration image is its own ESP-IDF project in `moonbase/migrate/`**: ESP-IDF builds one app per project, and the image exists only to install MoonBase. `build_esp32.py` builds it after MoonBase, whose binary it embeds.
- **WiFi crosses the move**: the migration image reads the network from WLED's `wsec.json` on the old filesystem and hands it to MoonBase, so the bulb comes back on the network by itself. No credential enters the repo or a build, any WLED device moves the same way, and the bench rehearsal runs end to end without anyone joining an access point.

## Verification

- ✅ The bench C3 runs MoonLight from a USB flash.
- ✅ The bench C3 makes the WLED-to-MoonLight move over WiFi alone, and survives a power cut at every step outside the table write.
- ✅ The bulb runs MoonLight with its five channels driven, verified by your eyes.

## Appendix: the bulb's filesystem

`/edit?list=/` on 2026-10-08 lists two files, downloaded unchanged.

`/cfg.json` (2275 bytes):

```json
{
 "rev": [
  1,
  0
 ],
 "vid": 2412100,
 "id": {
  "mdns": "wled-DB9600",
  "name": "Bulb1",
  "inv": "Light",
  "sui": false
 },
 "nw": {
  "espnow": false,
  "linked_remote": "",
  "ins": [
   {
    "ssid": "MoonModules",
    "pskl": 9,
    "ip": [
     0,
     0,
     0,
     0
    ],
    "gw": [
     0,
     0,
     0,
     0
    ],
    "sn": [
     255,
     255,
     255,
     0
    ]
   }
  ],
  "dns": [
   8,
   8,
   8,
   8
  ]
 },
 "ap": {
  "ssid": "IoTorero_DB9600",
  "pskl": 8,
  "chan": 1,
  "hide": 0,
  "behav": 0,
  "il": true,
  "ip": [
   4,
   3,
   2,
   1
  ]
 },
 "wifi": {
  "sleep": false,
  "phy": false,
  "txpwr": 78
 },
 "hw": {
  "led": {
   "total": 1,
   "maxpwr": 850,
   "ledma": 0,
   "cct": false,
   "cr": false,
   "ic": false,
   "cb": 0,
   "fps": 42,
   "rgbwm": 255,
   "ld": true,
   "ins": [
    {
     "start": 0,
     "len": 1,
     "pin": [
      6,
      7,
      5,
      4,
      3
     ],
     "order": 1,
     "rev": false,
     "skip": 0,
     "type": 45,
     "ref": false,
     "rgbwm": 0,
     "freq": 9765,
     "maxpwr": 0,
     "ledma": 0
    }
   ]
  },
  "com": [],
  "btn": {
   "max": 4,
   "pull": true,
   "ins": [
    {
     "type": 2,
     "pin": [
      -1
     ],
     "macros": [
      0,
      0,
      0
     ]
    },
    {
     "type": 0,
     "pin": [
      0
     ],
     "macros": [
      0,
      0,
      0
     ]
    },
    {
     "type": 0,
     "pin": [
      -1
     ],
     "macros": [
      0,
      0,
      0
     ]
    },
    {
     "type": 0,
     "pin": [
      -1
     ],
     "macros": [
      0,
      0,
      0
     ]
    }
   ],
   "tt": 32,
   "mqtt": false
  },
  "ir": {
   "sel": true
  },
  "relay": {
   "pin": -1,
   "rev": false,
   "odrain": false
  },
  "baud": 1152,
  "if": {
   "i2c-pin": [
    -1,
    -1
   ],
   "spi-pin": [
    -1,
    -1,
    -1
   ]
  }
 },
 "light": {
  "scale-bri": 100,
  "pal-mode": 0,
  "aseg": false,
  "gc": {
   "bri": 1,
   "col": 2.8,
   "val": 2.8
  },
  "tr": {
   "mode": true,
   "fx": true,
   "dur": 7,
   "pal": false,
   "rpc": 5,
   "hrp": true
  },
  "nl": {
   "mode": 1,
   "dur": 60,
   "tbri": 0,
   "macro": 0
  }
 },
 "def": {
  "ps": 0,
  "on": true,
  "bri": 255
 },
 "if": {
  "sync": {
   "port0": 21324,
   "port1": 65506,
   "espnow": false,
   "recv": {
    "bri": true,
    "col": true,
    "fx": true,
    "pal": true,
    "grp": 1,
    "seg": false,
    "sb": false
   },
   "send": {
    "en": false,
    "dir": false,
    "btn": false,
    "va": false,
    "hue": true,
    "grp": 1,
    "ret": 0
   }
  },
  "nodes": {
   "list": true,
   "bcast": true
  },
  "live": {
   "en": true,
   "mso": false,
   "rlm": true,
   "port": 5568,
   "mc": false,
   "dmx": {
    "uni": 1,
    "seqskip": false,
    "e131prio": 0,
    "addr": 1,
    "dss": 0,
    "mode": 4
   },
   "timeout": 25,
   "maxbri": false,
   "no-gc": true,
   "offset": 0
  },
  "va": {
   "alexa": false,
   "macros": [
    0,
    0
   ],
   "p": 0
  },
  "mqtt": {
   "en": false,
   "broker": "",
   "port": 1883,
   "user": "",
   "pskl": 0,
   "cid": "WLED-DB9600",
   "rtn": false,
   "topics": {
    "device": "wled/DB9600",
    "group": "wled/all"
   }
  },
  "hue": {
   "en": false,
   "id": 1,
   "iv": 25,
   "recv": {
    "on": true,
    "bri": true,
    "col": true
   },
   "ip": [
    192,
    168,
    1,
    0
   ]
  },
  "ntp": {
   "en": false,
   "host": "0.wled.pool.ntp.org",
   "tz": 0,
   "offset": 0,
   "ampm": false,
   "ln": 0,
   "lt": 0
  }
 },
 "ol": {
  "clock": 0,
  "cntdwn": false,
  "min": 0,
  "max": 29,
  "o12pix": 0,
  "o5m": false,
  "osec": false,
  "osb": false
 },
 "timers": {
  "cntdwn": {
   "goal": [
    20,
    1,
    1,
    0,
    0,
    0
   ],
   "macro": 0
  },
  "ins": []
 },
 "ota": {
  "lock": false,
  "lock-wifi": false,
  "pskl": 7,
  "aota": true
 },
 "um": {}
}
```

`/presets.json` (8 bytes):

```json
{"0":{}}
```

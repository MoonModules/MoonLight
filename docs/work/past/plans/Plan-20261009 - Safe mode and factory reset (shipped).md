# Plan: Safe mode and factory reset

A device that a setting or a script keeps crashing, or that a weak supply browns out, comes up in safe mode by itself, reachable and with its lights off, so the setting can be fixed.
A device that cannot be reached gets its access point back by switching it on and off four times, each within a few seconds, with its setup kept; a factory reset on the System card brings it back as installed.
Both read one record of how the last boots went, kept in flash from the very start of boot.

## Where it starts

- `opens` on the Access Point card has `first setup only` in place of `never`, which is what `never` did: open until a network or Ethernet is configured, closed after. Done, uncommitted.
- Four quick power-ons forget the known networks, counted by `platform::quickPowerCycles()` in NVS and cleared by a one-shot timer after 5 seconds of uptime, read in `NetworkModule::setup`. Done, uncommitted, and step 2 replaces what it resets and where.
- The old ewowi/MoonLight had a safe mode: any abnormal restart (a crash, a watchdog, a brownout) started the next boot without LED drivers, live scripts or more than 1,024 lights, with a 🛡️ in the status bar that restarted normally when clicked.

## Steps

### 1. One record of how the last boots went

`platform::quickPowerCycles()` becomes `platform::bootRecord()`: how many power-ons and how many abnormal restarts (a panic, a watchdog, a brownout) came in a row, each without 5 seconds of uptime between them.
Counted once per boot in NVS, and cleared by the one-shot timer once the device has stayed up, so the write stays off the render loop.
A restart from the UI, an update or a deep sleep counts as neither.

About 40 lines across the platform and its desktop seam: 1 hour.

### 2. Access back: four quick power-ons

At the start of `mm_main`, four power-ons in a row open the access point for this boot, whatever `opens` says, and change nothing else: the known networks, the light setup and the presets all stay.
It is the way back for a device whose router changed while its access point is set to `first setup only`: join the access point, set the new network on the WiFi card, and the device carries on as it was.
The System card says the access point was opened by switching, and the next ordinary boot follows `opens` again.
The network module loses its own reset (`forgetAll`, `resetByCycling_`), which forgot the networks.

A device in a brownout loop takes the gesture too: each switch-on is a power-on, counted before any light is driven, and the brownout restarts in between neither count nor clear it. Safe mode (step 4) is what gets it running again.

About 30 lines: 1 hour.

### 3. Factory reset: a button on the System card

`factory reset`, pressed twice as Restore is, deletes `/.config` (every module setting and the presets) and restarts, so the device starts as freshly installed with its access point open.
Scripts and other files stay, since they are content rather than settings; the installer's erase is the way to clear everything.
It is reached through the UI, from the home network or from the access point step 2 opened, so no gesture of its own is needed.

About 30 lines: 1 to 2 hours.

### 4. Safe mode: abnormal restarts in a row

Two abnormal restarts in a row start the next boot in safe mode, which keeps the network and the UI and leaves out what can crash a device or overload its supply:
- the LED drivers do not start, the preview does;
- MoonLive scripts are not compiled or run;
- a layout is capped at 1,024 lights.

The System card says why, with the reset reason the UI already shows, and a restart button that starts normally, since the record clears once the device stayed up.
A single crash starts normally, so one random crash in a museum night does not turn the lights off.

About 80 lines across core, the drivers, MoonLive and the layouts: half a day.

### 5. Tests and docs

- Unit tests through the desktop seam: four quick power-ons open the access point with every setting kept, three do not; the factory reset deletes `/.config` and keeps a script; two abnormal restarts start safe mode with no driver, no script and the cap, one does not; the status names each.
- On the bench C3: the gesture by switching its hub port, and the factory reset button.
- On the StadBeest legs (LightCrafter 16): safe mode by a real brownout, since that board browns out at full brightness. Brightness to the maximum, and after two brownouts it must come up in safe mode, its lights off and its UI reachable.
- Docs: the System card (safe mode, factory reset), troubleshooting (a device that crashes, a device you cannot reach), MIGRATING (the gesture opens the access point, the button resets the config).

Tests and docs: half a day.

## Estimate

| Step | Size | Work |
|---|---|---|
| 1. Boot record | ~40 lines | 1 hour |
| 2. Access back by switching | ~30 lines | 1 hour |
| 3. Factory reset button | ~30 lines | 1 to 2 hours |
| 4. Safe mode | ~80 lines | half a day |
| 5. Tests and docs | | half a day |
| **Total** | **~180 lines plus tests** | **about 1.5 days**, plus your testing on a board |

## Decisions

- Does the access point the gesture opens carry the first known network's password, or is it open for that boot? Switching the device four times is proof of physical access, and the owner of a changed router may no longer know the old password.
- Two abnormal restarts for safe mode, or three? Two catches a brownout loop sooner; three tolerates a pair of unrelated crashes.
- Does safe mode also start at a low brightness, or is leaving the LED drivers off enough? Off is the stronger guarantee against a brownout.

## What it removes

The network module's own power-cycle reset, which forgot the networks: step 2 opens the access point instead and forgets nothing.

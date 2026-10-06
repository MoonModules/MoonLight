# Open work in the unarchived plans

What is left in each plan, audited against the tree on 2026-10-02. One line per item, pointing rather than restating: the plan itself is the description, this is the worklist. A plan leaves this file when its last line here is struck.

## Desktop: closable in a sitting

| plan | what is left |
|---|---|
| **Input mapping and scripted sensors** | `addControl` still pulls the light header into the service builtins (`MoonLiveBuiltins_service.h`). |

## Bench-gated: needs hardware, not keyboard time

| plan | what is left |
|---|---|
| **One way to send to many receivers** | Step 7: discovery per mode, names, OSC, audio sync and the fallback with the S3 and the desktop; E1.31 multicast needs a second receiver. `run_network_live.py` writes `hosts` and reads each sender's `fixture` now, unrun since. |
| **Gamepads and MIDI desks** | The X-Touch check and the preset check of step 2; scribble strips and Learn need a desk. |
| **Input mapping and scripted sensors** | Step 3 (analog, the expression pedal) is host-verified only; the `.mls` picker and "new script" template are unconfirmed. |
| **RTSP video out** | Verification 2 to 5 on the P4, the glass-to-glass delay against HLS above all, since no factor is claimed until it is measured. |
| **MoonLight, v5.0.0 to the rename** | Verification 1, 2, 5 and 6 (upgrade, OTA across the move, the redirect, backup to restore), and the two fidelity bench questions: the audio level scale and the reconstructed logic. |

## Larger, and its own effort

| plan | what is left |
|---|---|
| **Gamepads and MIDI desks** | USB host on S3 and P4 (step 4), RTP-MIDI (step 5), a native Linux source (step 6). |
| **Input mapping and scripted sensors** | Steps 4, 4b, 5 and 6, none started: I2C sensors, the VL53L8CX zone grid, pulse timing with `EncoderService`, PIR. |
| **Documentation sweep** | The prose and docgen debt, now counted in [prose.md](../../reference/metrics/prose.md) and [docgen.md](../../reference/metrics/docgen.md) and ratcheted; the sweep itself is the backlog's [prose sweep](../future/backlog-core.md) item. |

## Struck since 2026-09-07

- **MoonLive palettes**: `scenario_Palettes_a_script_drives_the_effects` writes a palette script, picks it by name, turns its control and removes it, in-process and live.
- **OSC control ingest**: the scenario promise is dropped, since a scenario drives the pipeline over HTTP; `unit_OscPacket`, `unit_OscModule` and the bench cover OSC.
- **Two-way control surfaces**: the surface tests live in `unit_ControlModule`, and the seam is documented in [supporting](../../moonmodules/core/supporting.md).
- **Config backup and restore**: the installer's erase note says to back up first.
- **MoonLight migration**: closed by the rename plan's decision that the migration is finished.
- **Input mapping, step 2 and the MoonLiveService card**: bench-verified on the Dig-2-Go on 2026-09-02, and the card is in [services](../../moonmodules/core/services.md).
- **Input mapping, continuous writes**: saving is debounced and bounded by a ceiling ([FilesystemModule](../../../src/core/system/FilesystemModule.h)).
- **Input mapping, modules exchanging values over the network**: OSC shared controls answer it ([multi-device runtime](../../explanation/architecture/mooncore.md#multi-device-runtime)).

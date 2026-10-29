# Plan: Home Assistant wall controller

Plan of 2026-10-25, a Feature, for approval. A MoonLight panel on the wall shows Home Assistant's state (room temperatures, lights, heating) and its buttons, later its touchscreen, control Home Assistant. The first device is the Waveshare 64×64 HUB75 panel; the M5Stack Tab5 (ESP32-P4, 5" MIPI-DSI, GT911 touch) follows. Home Assistant sees the device's control desk as entities over MQTT, the way it sees a Zigbee2MQTT remote or a Tasmota plug, so no token lives on the device.

## What exists

- **MqttModule** is MoonLight's own MQTT 3.1.1 client. It takes on/off, brightness and HSV on `MoonLight/<mac>/…/set`, and with `haDiscovery` on it announces a `light` and an `update` entity, one discovery topic each. Every inbound command goes through the shared control path. Inbound packets are capped at 512 bytes (`kMqttMaxPacket`).
- **ControlModule** is the device's desk: 8 switches, 8 encoders, 8 faders and 64 preset pads. Each slot drives whatever `Module.control` it is assigned and reads that control back, so the desk follows what it drives. Surfaces (`ControlSurface`: OSC, MIDI) mirror the desk through `sendValue` and `sendDisplay`.
- **Inputs** share one binding table, `InputMapping`: a row is a source (a pin, an IR code, a gamepad input) plus an `InputAction` aimed at a desk slot (toggle, set while held, nudge). ButtonService, InfraredService, GamepadService and AnalogService each add only the source half.
- **I2cBusModule** opens the board's one bus; a device module claims its address through `i2cDevices()`, the way AudioService claims the ES8311.
- **TextEffect** draws multi-line text in a 4×6 or 6×8 font, 128 characters at most. There is no display driver for an LCD panel, and the P4 HDMI and MIPI-DSI output is step 10 of [Video in, LED walls out](Plan-20261024%20-%20Video%20in%2C%20LED%20walls%20out%20%280%20of%2010%29.md).
- **The Tab5** is a local wall preview or touch console, which needs a `platform::` MIPI-DSI seam; a touch input is a Service under `Services`, as the button and infrared rows are.
- **Proven on the bench (2026-10-25):** a live `PATCH /api/state?live=1` from a script on the Mac put the HA states on the Waveshare as one TextEffect, using the 6×8 font, 10 characters by 8 lines.

## The rule this plan follows

Home Assistant is a control surface, like an OSC or MIDI desk: it drives the device's slots and follows them back. MQTT stays a transport onto the shared control path. The layout lives on the device as a preset, and Home Assistant pushes only values into slots, as openHASP and ESPHome do. No Home Assistant concept enters the light domain, and no effect does I/O.

## Is this the textbook Home Assistant way

Yes, for a device that is not ESPHome. HA's [MQTT discovery](https://www.home-assistant.io/integrations/mqtt/) is the standard way for third-party firmware (Zigbee2MQTT, Tasmota) to bring entities into HA, and each entity type has one purpose:

| Desk slot | HA entity | Why this one |
|---|---|---|
| switch | `switch` | state both ways: HA can show the light's state on the switch, and a press toggles it |
| fader, encoder | `number` | a value the device owns and HA sets |
| text (new) | `text` | HA's documented "remote LCD screen" entity: an automation calls `text.set_value` with a template; 255 characters at most |

Pads are not exposed: they are presets, which already reach HA as the light's effect list. `button` sends from HA to the device only, so it cannot report a press; `notify` keeps no state; `event` and `device_automation` fit inputs that have no state, which a switch slot does have.

Two alternatives were rejected:

- **ESPHome's native API.** HA prefers it for ESPHome devices, but it is ESPHome's own protocol, and MoonLight already speaks MQTT.
- **Pushing a whole state document over MQTT.** No HA-side standard produces one; openHASP and NSPanel each needed a custom component for that.

## Design

### The desk becomes HA entities

MqttModule announces one entity per desk slot. All of them carry the same `device` block, so HA shows a single device with the light, the update and the desk together. A slot's entity uses `MoonLight/<mac>/<slot>/set` and `/get`, for example `switch2/set`, routed onto the slot through the control path. A slot publishes on change, using the same follow pass the OSC and MIDI surfaces use. The name HA shows is the slot's target ("Woonkamer licht" once a label exists, `switch2` until then). Each entity's `unique_id` is the device's MAC suffix and the slot, such as `563cfe_switch2`, so renaming a target or the device keeps its entity and the automations on it. A fader or encoder is a `number` with the slot's byte range: min 0, max 255, step 1.

### A text bank on the desk

ControlModule gains text slots, assigned like the others to a `Module.control` of text type, such as `RoomText.text`. A text slot holds no buffer: it reads and writes the control it targets. `ControlModule::setValue` carries a byte, so text has a path of its own: a text slot kind and a write that takes the payload as UTF-8, refuses a target that is not a text control, and holds to the target's length without splitting a character. HA sets it through a `text` entity, whose maximum is the target's size. One slot per TextEffect means a look can color each room differently.

### Touch is an input service

`TouchService` is a core Service. It reads a GT911 on the board's I2C bus and claims its address through `i2cDevices()`. Its rows are a rectangle in percent of the panel, the same 0 to 100 as RegionModifier, plus an `InputAction`, so a touch zone is a virtual button. A finger down is a press and a lift is the release, which lets set-while-held work. Because the rectangle is in percent, a touch zone and the layer band under it match on any resolution. The GT911 reports in the panel's native axes, portrait on the Tab5, so TouchService turns a touch through the same rotation the display driver applies, an axis swap and an inversion, before it tests the rectangles. On the desktop, a click on the web Preview feeds the same rows, so the whole touch path is tested without a board.

### The Tab5 screen is a driver

A MIPI-DSI driver in `Drivers` sends the rendered grid to the panel through a `platform::` display seam, scaling it up by a whole-number factor (a 160×90 grid becomes 1280×720). NightDriver does the same on its Tab5 target. The seam is the one the Video plan's HDMI out needs, so it is built once, by whichever plan gets there first.

## Steps

1. ⏳ **The desk as HA entities.** Table-driven discovery in MqttModule for the light, the update and every switch, fader and encoder slot, one topic per entity; the slot `set` and `get` topics; tests through `feedForTest` and the send capture, asserting each config and each routed command.
2. ⏳ **Text slots.** A text bank in ControlModule, assigned and followed like the other banks; its HA `text` entity; tests for assigning a slot, writing through it, reading it back, refusing a target that is not text, and a text at and past the target's length, cut whole at a multi-byte character.
3. ⏳ **The wall look.** An Effects preset for the Waveshare (four rooms, text slots assigned) and the HA automations (`text.set_value` from templates; `switch2` both ways with `light.woonkamer_woonkamer`). Bench: the product owner watches the panel follow HA.
4. ⏳ **Physical buttons.** ButtonService rows on the Waveshare aimed at `Control.switch2…`; a press toggles the HA light, and the light's state comes back to the switch. Bench.
5. ⏳ **TouchService.** The GT911 through a platform seam, rows of percent rectangles with an `InputAction`, press and release; Preview clicks on the desktop as the test source; unit tests for hit-testing, overlapping zones, release, and a corner touch landing in the right corner in both orientations, which Preview clicks do not exercise.
6. ⏳ **MIPI-DSI out.** The platform display seam and a driver with a whole-number upscale, shared with the Video plan's step 10. Bench on the Tab5.
7. ⏳ **The Tab5 board.** Its catalog entry in `deviceModels.json`: I2C pins, the touch rows, the DSI driver and a starter wall look. Bench.
8. ⏳ **Document it.** home-automation.md gets the desk-as-entities section, a how-to covers the wall controller, MqttModule and ControlModule get card updates, and MoonWalk gets a process "Build a Home Assistant wall controller".

## Decisions (recommended)

- **Discovery shape: one topic per entity, all sharing one `device` block.** HA's newer device-based discovery (one `cmps` payload, since 2024.11) needs about 5 KB for 26 components at once. One entity at a time keeps the buffer at one config's size, which a board without PSRAM can afford, and HA groups the result into the same device.
- **Gate: one new switch, `haDesk`, beside `haDiscovery`.** `haDiscovery` stays off by default because the WLED integration already lists the light. The desk entities have no WLED counterpart, so they need their own opt-in, and turning it on announces the desk without listing the light twice.
- **Text slots: 4.** That is one per room on a 64×64 panel. The count is a constant, and a slot costs only the target name, since it holds no text of its own.
- **GT911 access: a generic `platform::i2cTransfer` seam**, rather than `touchInit`/`touchRead`. The next I2C sensor then reuses it, and the GT911 register protocol becomes a tested header in core.

## Extensions

- A tile effect: a rectangle with a label, colored by the state of the control it is bound to, so a touch zone shows whether it is on.
- Pages: a `select` slot that switches presets, so one panel holds several screens.
- An `event` entity for long press and double press, once an input row can tell them apart.

## Impact and challenges

- **Discovery count:** 30 retained discovery topics per device with `haDesk` on (the light, the update, 24 desk slots and 4 text slots), re-announced on connect, as the light already is. Each retract is a known empty retained payload.
- **The 512-byte inbound cap** is ample for one text slot (at most 255 characters). The plan keeps the cap and documents it next to the topic.
- **The broker:** HA runs MQTT for Zigbee2MQTT. Step 3 needs the broker's address and a login for the panel, which MqttModule already keeps: its `username`, and its `password` stored obfuscated like the WiFi one.
- **Tab5:** its panel is portrait natively (720×1280), so the driver needs a rotation, and the DSI timing comes from M5Stack's BSP.

## Estimate

| Step | Time |
|---|---|
| 1. The desk as HA entities | 2 days |
| 2. Text slots | 1 day |
| 3. The wall look | 0.5 day, plus bench time |
| 4. Physical buttons | 0.5 day, plus bench time |
| 5. TouchService | 2 days |
| 6. MIPI-DSI out | 3 days (shared with the Video plan) |
| 7. The Tab5 board | 1 day, plus bench time |
| 8. Document it | 1 day |

## What it removes

- The light's and the update's own discovery publishers become two rows of the one table-driven announcer, so three discovery code paths become one.
- No token handling: `getJson` stays without headers, and the device stores no HA token; the broker login it needs is the one MqttModule already keeps.
- The REST push from a script on the Mac, used for the first bench test, is not carried forward.

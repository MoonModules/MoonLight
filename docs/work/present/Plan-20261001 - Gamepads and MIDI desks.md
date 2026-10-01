# Plan: play the games with a gamepad, and drive the desk with a MIDI controller

## Why

The arcade effects (Pong, Breakout, Space Invaders) play themselves. With a controller in hand they become games, which is what makes them worth standing in front of at an event. A motorized MIDI desk such as the Behringer X-Touch or the iCON QCon Pro G2 is the same wish from the performer's side: physical faders and knobs that drive the show, and that move by themselves when the show changes.

## What already exists

Everything this needs on the receiving side is built. The plan adds transports and device profiles, not a new control system.

- **ControlModule** is the surface: 8 switches, 8 encoders, 8 faders and a pad grid, each assignable to any `Module.control`, driven through `Scheduler::setControl` and following its target back.
- **The input services** map a physical input onto the surface, or onto any control directly, with the shared `InputMapping` rows (Toggle, Set, Delta): `ButtonService` for GPIO buttons, `AnalogService` for potentiometers, `InfraredService` for remotes (with a `learn` field: the next code received binds to the row), `OscModule` for OSC.
- **`ControlSurface`** is the two-way interface a desk attaches through (`sendValue`, `sendRing`, `sendColor`, `sendLabel`), and `setTouched` tells the surface a hand is on a control. It was written for MIDI hardware, and OSC is so far its only transport ([backlog-core § MIDI as a control surface](../future/backlog-core.md)).
- **[Control surfaces](../../reference/hardware/control-surfaces.md)** records the bench desks: the X-Touch and the QCon both speak Mackie Control (MCU) over MIDI, neither speaks OSC, the X-Touch adds RTP-MIDI on its Ethernet port, and the QCon is USB only.
- **`setControl` calls `onControlChanged`** on every write, from any of those writers, and `ControlDescriptor::live` marks a control as state that is never written to flash.

## Done on this branch

**Step 0: the games have player controls.** They are ordinary controls, so every input above reaches them today.

- Pong: `player1` and `player2`, the paddle positions, 0 to 255.
- Breakout: `player`, the paddle position.
- Space Invaders: `player`, the cannon position, and `fire`, one shot in the air at a time as in the arcade.
- All are live controls. While the game plays itself it writes its own positions into them, so an assigned fader or encoder follows the paddle.
- Any write through `setControl` hands that player to a person; ten seconds without one hands it back, so an installation never stands idle. The rule lives once, in `PlayerSeat`.
- Wiring with what exists: a ControlModule fader assigned to `PongEffect.player1`, an `AnalogService` potentiometer onto that fader (a pot is the 1972 paddle), two `ButtonService` rows with Delta ±16 for left and right, a Set row onto `SpaceInvadersEffect.fire`.
- A DIY controller needs nothing more: a gamepad's buttons wired to GPIOs are `ButtonService` rows, and a stick is two `AnalogService` channels.

Also on this branch, from the same session: the Breakout effect (self-playing, the arcade's speed-ups and paddle halving, `descend` for a creeping wall, an audio-reactive wall that glows per band), a Pong score with first-to-11, independent paddles and `size` shown only with `spriteBall`, the beat-driven balls removed from Pong and Breakout, and Tetrix's stack drawn again with a per-run seed and staggered starts.

## The shape: two services, each with a source select

**A gamepad and a MIDI desk are different kinds of device, so they are two services.**

- A **gamepad** is input only. It is an `InputMapping` client like `ButtonService`: a button acts like a press, a stick like an `AnalogService` reading, and each row names a surface control or any `Module.control`.
- A **MIDI desk** is a two-way surface. Its faders, knobs and buttons are inputs, and its motors, LED rings, button lights and scribble strips want the current state back. That is exactly `ControlSurface`, so a `MidiService` attaches as one, the way OSC does. Notes as performance data (which keys are held, how hard) are a second consumer of the same transport, already scoped in the backlog as a `MidiFrame` beside `AudioFrame`.

From the user's side both look the same: add the service under Services, pick where the device is connected, pick its profile, map what it does.

**Each service has one `source` select**, the way `AudioService` picks between a local mic, the network and simulation. The mapping half is identical for every source, so a service per source would be copies of one job.

| | Browser | USB | Network |
|---|---|---|---|
| Gamepad | the W3C Gamepad API, any pad plugged into or paired over Bluetooth with the computer showing the UI | USB host plus HID on S3 and P4 (the S31 to be confirmed); Xbox pads speak GIP over USB, not HID | none |
| MIDI desk | the Web MIDI API in Chrome, Edge and Firefox (Safari has none), Bluetooth MIDI included once the computer has paired it; SysEx needs the user's permission | USB host plus the USB MIDI class on S3 and P4: the QCon's only route without a computer | RTP-MIDI (RFC 6295): the X-Touch's Ethernet port |
| Limit | works only while a browser tab is open next to the device | the device plugged into the board | the X-Touch only |

**Bluetooth stays off on the board.** No build enables it (the `sdkconfig.defaults` files leave `CONFIG_BT` at IDF's default, off), and three costs keep it that way. A BLE host stack takes roughly 100 to 200 KB of flash with NimBLE and more with Bluedroid, a real slice of a 4 MB classic board's 1.75 MB app slots. Its controller and host take tens of KB of internal RAM, the scarcest kind on a classic. And WiFi and Bluetooth share one 2.4 GHz radio by taking turns, so Art-Net, DDP and the UI lose airtime and gain latency whenever Bluetooth is active, while its controller interrupts run at high priority on core 0 beside the network stack. A Bluetooth pad reaches the device through the browser instead, paired with the computer showing the UI. If a wireless pad with no computer is ever needed, it is an opt-in firmware variant, never the default build.

**The browser source comes first for both**, because it reaches every chip and the desktop with no firmware driver, and because it serves the bench hardware at once: the Xbox pad over USB or Bluetooth on the computer, and both desks over USB on the computer.

## The motorized desks: no OSC needed

The X-Touch and the QCon need no OSC bridge on this route. `MidiService` with the browser source and an **MCU profile** reaches both:

- fader positions arrive as pitch bend, 14-bit, one channel per fader, onto the surface's faders;
- the touch notes (0x68 to 0x70) feed `setTouched`, so a fader under a hand is not fought by the motor;
- the encoders send relative CCs, which is exactly `applyEncoderDelta`;
- back out, `sendValue` becomes pitch bend that drives the motors, `sendRing` the ring CCs, button notes light the buttons, and `sendLabel` writes the scribble strips through SysEx.

So a preset change moves the faders and relabels the strips, which is what makes these desks worth owning. OSC stays what it is for: TouchOSC, Open Stage Control and the wider app ecosystem. The direct routes come later: RTP-MIDI in firmware for the X-Touch over Ethernet, and USB MIDI host on S3 or P4 for the QCon.

## The vocabulary: standard layouts and profiles

**A gamepad row names a standard button, not a raw byte.** The W3C Gamepad API defines a standard layout (A, B, X, Y, the D-pad, shoulders, triggers, two sticks), and SDL's community GameControllerDB maps hundreds of real controllers onto it. So rows read "D-pad left: Delta −16 onto encoder 1", and one set of rows works with any pad.

**Presets live one layer down, as device profiles.** The browser source needs none for a gamepad: Chrome reports Xbox and PlayStation pads in the standard layout. The USB source needs a translation per controller, raw HID to standard, seeded from GameControllerDB rather than written by hand. A MIDI desk gets a profile per protocol rather than per desk: one MCU profile covers the X-Touch, the QCon and every other Mackie desk.

**Every class-compliant MIDI desk works through `MidiService`, profile or not.** An Akai desk (APC mini, APC40, MIDImix) is one of them, over the browser or over board USB like the others. What a desk without a profile lacks is feedback, not input:

- **With a profile**, the desk works out of the box, both ways: the MCU profile drives the X-Touch's and the QCon's motors, rings and strips.
- **Without one**, learn binds each knob, fader and pad to a row, so the desk drives the show at once; it cannot light its pad LEDs or rings, because only a profile knows which message lights which.
- **Akai desks do not speak MCU.** Each model has its own note and CC layout, so an Akai gets its own profile, written when one is on a bench to verify it against.

**A default mapping per game** on top of that makes a pad playable out of the box: the stick or D-pad onto `player`, A onto `fire`.

**Learn**, as `InfraredService` already does: arm a row, move a control on the device, and it binds. MIDI learn is the norm in every lighting and VJ tool, and it is what lets a desk without a profile work at all.

## Where a row points: the surface, by default

A row can point at a surface control (fader 1, encoder 3) or straight at a game control (`PongEffect.player1`). Both work; the default rows point at the surface, for three reasons. The surface is the one place that shows what every input does. A motorized desk sees the same faders the gamepad moves. Re-pointing a game is one assignment on the Control card rather than an edit in every input service. The direct form stays available for a one-off.

## Is this the industry standard?

Checked against the tools a lighting or VJ operator already knows. The shape matches; nothing here is bespoke.

| this plan | the established pattern |
|---|---|
| one service per device kind, with a transport select | QLC+ input plugins (MIDI, HID, OSC, Art-Net) feeding one input universe; Resolume and Chataigne the same split of transport from meaning |
| device profiles | QLC+ input profiles per desk, the MCU protocol shared by every Mackie desk, SDL's GameControllerDB for gamepads |
| mapping rows onto a fixed surface | the MIDI-mapping tables of Resolume, Ableton and QLC+, and MoonLight's own two-step model in [input-mapping-analysis](../future/input-mapping-analysis.md) |
| learn | MIDI learn, universal in DAWs, VJ and lighting software |
| feedback to motors, rings and LEDs | MCU feedback, QLC+ input feedback, Ableton's control-surface scripts |
| the standard gamepad layout | the W3C Gamepad API and SDL's GameController API |
| a browser as the host for USB devices | Web MIDI and the Gamepad API, the standard browser device APIs, used by vendors' own web editors |

## Steps

### 1. GamepadService, browser source

- The service with a `source` select, its rows on `InputMapping`, the standard layout as the row vocabulary, learn, and a default row set per game pointing at the surface.
- The UI gains a browser input bridge: it reads `navigator.getGamepads()` while a tab is open and sends the pad's state to the service over the existing WebSocket, on change only, so the rate is a person's rather than a frame rate.
- The service applies the rows through `setControl`, which keeps the hot path untouched.
- Each game's hand-back becomes a control, `holdSeconds`, defaulting to 10, in place of `PlayerSeat`'s constant.
- Verified with the product owner's Xbox Series pad in Chrome, over USB and over Bluetooth, playing Pong, Breakout and Space Invaders.

### 2. MidiService, browser source, with the MCU profile

- The service attaches as a `ControlSurface`; the MCU profile maps pitch bend, touch notes, relative encoders and buttons in, and motors, rings, button lights and scribble strips out.
- The same browser bridge gains Web MIDI in both directions, SysEx included.
- Learn for a desk without a profile, and the OpenLamp convention as the generic fallback mapping where it fits.
- Verified with the X-Touch and the QCon: a fader moves a control, a preset change moves the faders, a touched fader is not fought.

### 3. USB host on S3 and P4

- USB host bring-up in the platform layer, behind a seam.
- USB MIDI first, since it is one class with no per-vendor parsing: the QCon without a computer.
- HID gamepads next, with controller translations seeded from GameControllerDB. Xbox pads over USB need a GIP driver on top, and only if wanted, since the browser already reaches them over USB or Bluetooth on the computer.
- The S31 joins if its datasheet confirms a USB host.

### 4. RTP-MIDI for the X-Touch

The X-Touch over Ethernet with no computer: RFC 6295's session handshake and journal, then the same MCU profile as step 2.

The desktop needs no native step: the browser source covers it. A native OS gamepad or CoreMIDI path earns its place only for a desktop running headless.

## Decided

- **The pad is an Xbox Series X|S controller** (it has the Share button). The browser reaches it as a standard gamepad, over USB or Bluetooth on the computer; USB on the board needs GIP, so that route waits.
- **No Bluetooth on the board**, for its flash, RAM and shared-radio cost; Bluetooth devices come in through the browser.
- **Akai desks are in scope, through learn.** They work through `MidiService` like every MIDI desk; a dedicated Akai profile, for pad LEDs and rings, waits until an Akai is on the bench. The MCU profile is verified with the X-Touch and the QCon.
- **The hand-back stays ten seconds, and becomes a control.**

## Open for the product owner

- **Default rows point at the surface** is the recommendation above, waiting for a yes.

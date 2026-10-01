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
| Limit | works only while a browser tab is open next to the device, on a secure origin (below) | the device plugged into the board | the X-Touch only |

**Bluetooth stays off on the board.** No build enables it (the `sdkconfig.defaults` files leave `CONFIG_BT` at IDF's default, off), and three costs keep it that way. A BLE host stack takes roughly 100 to 200 KB of flash with NimBLE and more with Bluedroid, a real slice of a 4 MB classic board's 1.75 MB app slots. Its controller and host take tens of KB of internal RAM, the scarcest kind on a classic. And WiFi and Bluetooth share one 2.4 GHz radio by taking turns, so Art-Net, DDP and the UI lose airtime and gain latency whenever Bluetooth is active, while its controller interrupts run at high priority on core 0 beside the network stack. A Bluetooth pad reaches the device through the browser instead, paired with the computer showing the UI. If a wireless pad with no computer is ever needed, it is an opt-in firmware variant, never the default build.

**The browser source needs a secure origin.** Chrome, Edge and Firefox expose the Gamepad API and Web MIDI only to a page served over HTTPS or from `localhost`; on a device's plain `http://<ip>` page they report no pad and no MIDI port, which looks exactly like nothing being connected. Safari exposes the Gamepad API on any page but has no Web MIDI at all. Per browser:

| Browser | Gamepad on a device's `http://` page | Web MIDI |
|---|---|---|
| Safari on macOS | works: no secure-origin rule | not implemented |
| any browser on iPhone or iPad (all run on Safari's engine) | works, as in Safari | not implemented |
| Chromium browsers: Chrome, Edge, Brave, Opera, Vivaldi, Arc, Chrome on Android | blocked; the `unsafely-treat-insecure-origin-as-secure` flag lifts it per address | secure origin only |
| Firefox | blocked; `about:config` has a list of hosts to treat as secure (`dom.securecontext.allowlist`), to verify on a real install | secure origin only |

No browser lifts the rule for MIDI: the Web MIDI standard requires a secure origin, and Safari does not implement the API. For gamepads, a phone or tablet in Safari is a laptop-free path that works on any board's page today.

So the supported paths are:

- **the desktop app's page on `localhost`**, a secure origin in every browser, for both gamepads and MIDI;
- **Safari on a device's page**, for gamepads only;
- **Chrome with the device's address listed under `chrome://flags/#unsafely-treat-insecure-origin-as-secure`**, a per-browser setting that makes one device's page a secure origin, for both;
- **the desktop app as a hub** (next section), which reaches every board with no browser workaround at all;
- **HTTPS served by the device itself**, which is the real fix for a laptop-free browser and has its own cost. ESP-IDF's `esp_https_server` on mbedTLS can serve it, AES running in hardware; the firmware grows by roughly 100 KB or more and each TLS connection holds about 30 to 40 KB for its buffers, which S3 and P4 can place in PSRAM and a classic ESP32 can afford once or twice. The harder half is the certificate: a device on a LAN address or a `.local` name cannot get a publicly trusted one, so it is self-signed and the browser warns until the user clicks through, and whether a clicked-through page counts as a secure context for these APIs needs a bench check. Avoiding the warning means a home-made root certificate installed on every computer, or Plex-style infrastructure issuing a real certificate per device. It is not in this plan; it earns a step if a laptop-free browser setup becomes a real need.

## The desktop app as the hub, verified

The desktop app's page on `localhost` is a secure origin, so a pad or a desk works there in every browser, and the app forwards to the boards over OSC with no new code:

```text
pad / desk → browser (localhost) → desktop GamepadService → desktop surface → OscModule → board's OscModule → board's surface → its target
```

`OscModule` sends every surface value as `/mm/fader/N`, `/mm/switch/N` and `/mm/encoder/N` to its `feedbackTo` address once `listen` and `feedback` are on, and a receiving `OscModule` routes those into its own surface. Verified on the bench (2026-10-01): with the desktop's `feedbackTo` set to the P4 and `feedbackPort` to its listening port, 9000, moving the desktop's fader 1 to 200 and then 60 set the P4's fader 1 within a second each time, and the P4 reported the Mac as its peer. A value the desktop's own game writes travels the same way, so a board's surface also follows a self-playing paddle. One laptop serves every controller for every board in the rig, and no board needs TLS.

The one setting that is easy to miss: `feedbackPort` defaults to 9001, the port apps such as TouchOSC listen on, while a board listens on 9000.

## A Linux box as a computer-free hub

A headless Linux board such as a NanoPi runs the desktop build but has no browser of its own, and a phone browsing to it over plain HTTP is not a secure origin, so the browser source does not help there. Linux does offer native input, more readily than any other platform:

- **gamepads** come through the kernel: the `xpad` driver handles Xbox pads over USB, their GIP protocol included, BlueZ handles Bluetooth, and a native source reads either from `/dev/input` with no HID parsing;
- **MIDI desks** appear as ALSA MIDI ports, the X-Touch and the QCon included.

So a Linux-only source behind the same `GamepadService` and `MidiService` turns such a box into a rig controller with the pad and the desks plugged into it: it forwards over OSC exactly as the desktop does, or drives LEDs itself. This is the headless case the step list names.

The bridge needs no socket of its own: it writes through the same `/api/control` request every other control write uses, so it lives on whatever origin the page does.

**The browser source comes first for both**, because it reaches every chip and the desktop with no firmware driver, and because it serves the bench hardware at once: the Xbox pad over USB or Bluetooth on the computer, and both desks over USB on the computer.

## The motorized desks: no OSC needed

The X-Touch and the QCon need no OSC bridge on this route. `MidiService` with the browser source and an **MCU profile** reaches both:

- fader positions arrive as pitch bend, 14-bit, one channel per fader, onto the surface's faders;
- the touch notes (0x68 to 0x70) feed `setTouched`, so a fader under a hand is not fought by the motor;
- the encoders send relative CCs, which is exactly `applyEncoderDelta`;
- back out, `sendValue` becomes pitch bend that drives the motors, `sendRing` the ring CCs, button notes light the buttons, and `sendLabel` writes the scribble strips through SysEx.

**What is replayed when the desk attaches.** `ControlModule::addSurface` calls `followTargets` and then `resendTo`, which sends every switch, encoder and fader value, so the motors move to the current positions and the button lights match the switches. Rings follow their encoder's value through the same call. The scribble-strip labels and the pad colors have no source today: step 2 adds one, the label being the name of what each control drives (`player1`, `brightness`) and the color the target's palette entry, and extends `resendTo` to send both on attach and on every reassignment. Until then a preset change moves the faders, and the strips keep whatever they showed.

So a preset change moves the faders, which is what makes these desks worth owning. OSC stays what it is for: TouchOSC, Open Stage Control and the wider app ecosystem. The direct routes come later: RTP-MIDI in firmware for the X-Touch over Ethernet, and USB MIDI host on S3 or P4 for the QCon.

## The vocabulary: standard layouts and profiles

**A gamepad row names a standard button, not a raw byte.** The W3C Gamepad API defines a standard layout (A, B, X, Y, the D-pad, shoulders, triggers, two sticks) and reports `mapping` as `"standard"` for every pad the browser knows how to remap onto it; SDL's community GameControllerDB maps hundreds of real controllers onto the same layout. So rows read "D-pad left: Delta −16 onto encoder 1", and one set of rows works with any pad.

One set of rows therefore works for every pad whose `mapping` is `"standard"`. A pad the browser does not recognize reports its own index order, so the names no longer match its buttons; learn binds it all the same, since learn records whichever input moved.

**Presets live one layer down, as device profiles.** The browser source needs none for a recognized gamepad: Chrome and Safari report Xbox and PlayStation pads with the standard mapping. The USB source needs a translation per controller, raw HID to standard, seeded from GameControllerDB rather than written by hand. A MIDI desk gets a profile per protocol rather than per desk: one MCU profile covers the X-Touch, the QCon and every other Mackie desk.

**Every class-compliant MIDI desk works through `MidiService`, profile or not.** An Akai desk (APC mini, APC40, MIDImix) is one of them, over the browser or over board USB like the others. What a desk without a profile lacks is feedback, not input:

- **With a profile**, the desk works out of the box, both ways: the MCU profile drives the X-Touch's and the QCon's motors, rings and strips.
- **Without one**, learn binds each knob, fader and pad to a row, so the desk drives the show at once; it cannot light its pad LEDs or rings, because only a profile knows which message lights which.
- **Akai desks do not speak MCU.** Each model has its own note and CC layout, so an Akai gets its own profile, written when one is on a bench to verify it against.

**A default mapping per game** on top of that makes a pad playable out of the box: the stick or D-pad onto `player`, A onto `fire`.

**Learn**, as `InfraredService` already does: arm a row, move a control on the device, and it binds. MIDI learn is the norm in every lighting and VJ tool, and it is what lets a desk without a profile work at all.

## Where a row points: the surface

A row points at a surface control (fader 1, encoder 3, switch 2), and the Control card points that at a game control (`PongEffect.player1`), for three reasons. The surface is the one place that shows what every input does. A motorized desk sees the same faders the gamepad moves. Re-pointing a game is one assignment on the Control card rather than an edit in every input service.

**Surface only, for every input service.** The editor already offers only surface targets, and `InputMapping` keeps a direct `Module.control` target as an escape hatch through the interface. Closing it makes the surface the one way an input reaches a control. Two conditions come with it. The surface's banks (8 switches, 8 encoders, 8 faders) are the capacity limit, so when a rig outgrows them the banks grow, rather than the hatch reopening. And a switch driving a button control fires on the press alone, since a button takes every write as a press; that fix shipped with step 1.

**The indirection costs microseconds, not a perceptible delay.** The extra hop is a second `setControl`: a name lookup, a small parse and a write, tens of microseconds on an ESP32. The input lag a player feels comes from the rest of the chain: the browser polls the pad once per animation frame (up to about 16 ms), WiFi adds a few to ten milliseconds, the device handles an inbound write on its 20 ms network tick, and the game reads the value on its next frame. That totals roughly 20 to 50 ms, the range of a television's input lag. One cost is worth measuring rather than assuming: each surface drive re-reads all 24 surface targets.

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

- ✅ `GamepadService`: its rows on `InputMapping`, SDL's GameController names as the row vocabulary, learn, a first state taken as the baseline, sticks written only past a deadband.
- ✅ The default rows point at the surface: the left stick's Y onto fader 1, the right stick's Y onto fader 2, the left stick's X onto fader 3, A onto switch 1. The Control card assigns those to a game.
- ✅ The UI's browser bridge reads `navigator.getGamepads()` while a tab is open on a secure origin and writes the pad's state into the service's hidden, live `pad` control through the ordinary control write, on change only and at most 30 times a second. No new message type was needed.
- ✅ Each write runs the rows at once, through the ordinary control write, which keeps the hot path untouched.
- ✅ A switch driving a button control fires on the press alone.
- The `source` select arrives with the second source; with the browser as the only one, it would be a select of one.
- ✅ Verified with the product owner's Xbox Series pad over Bluetooth in Safari on the desktop app's page, driving Pong through the surface.
- ✅ Verified in Chrome on `localhost` too. It feels immediate from pad to lights; a measured input-to-frame time is not worth its code for now.

### 2. MidiService, browser source, with the MCU profile

- ✅ Inbound: the MCU profile maps fader pitch bend onto the surface's faders, touch notes onto `setTouched`, relative knob turns onto `applyEncoderDelta`, and SELECT onto the switches.
- ✅ The browser bridge gains Web MIDI inbound, on a secure origin; each write is decoded as it lands.
- ✅ Verified with the QCon in Chrome on `localhost`: its faders drive the surface and, through it, Pong's paddles.
- Outbound: the service attaches as a `ControlSurface`, and motors, rings, button lights and scribble strips go back to the desk through the bridge, SysEx included.
- Learn for a desk without a profile, and the OpenLamp convention as the generic fallback mapping where it fits.
- Verify the outbound half with the X-Touch and the QCon: a preset change moves the faders, and a touched fader is not fought.

### 3. Surface-only input targets

- `ButtonService`, `AnalogService`, `InfraredService`, `GamepadService` and OSC's `/mm/control/<Module>/<control>` path lose their direct `Module.control` targets; every input reaches a control through the surface.
- A persisted row with a direct target is reported rather than silently dropped, and MIGRATING records the change.

### 4. USB host on S3 and P4

- USB host bring-up in the platform layer, behind a seam.
- USB MIDI first, since it is one class with no per-vendor parsing: the QCon without a computer.
- HID gamepads next, with controller translations seeded from GameControllerDB. Xbox pads over USB need a GIP driver on top, and only if wanted, since the browser already reaches them over USB or Bluetooth on the computer.
- The S31 joins if its datasheet confirms a USB host.

### 5. RTP-MIDI for the X-Touch

The X-Touch over Ethernet with no computer: RFC 6295's session handshake and journal, then the same MCU profile as step 2.

### 6. A native Linux source

`/dev/input` for gamepads and ALSA for MIDI desks, so a headless Linux box such as a NanoPi is a rig controller with no browser. A desktop with a screen needs no native step, since the browser on `localhost` covers it.

## Decided

- **The pad is an Xbox Series X|S controller** (it has the Share button). The browser reaches it as a standard gamepad, over USB or Bluetooth on the computer; USB on the board needs GIP, so that route waits.
- **No Bluetooth on the board**, for its flash, RAM and shared-radio cost; Bluetooth devices come in through the browser.
- **Akai desks are in scope, through learn.** They work through `MidiService` like every MIDI desk; a dedicated Akai profile, for pad LEDs and rings, waits until an Akai is on the bench. The MCU profile is verified with the X-Touch and the QCon.
- **The hand-back is a fixed ten seconds.** A control for it was built and removed again: nobody tunes it, and it cost a control on three games.
- **Rows point at the surface**, and direct targets go for every input service (step 3).

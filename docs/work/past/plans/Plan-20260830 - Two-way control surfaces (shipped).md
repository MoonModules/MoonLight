# Plan: Two-way control surfaces

## Context

OSC ingest shipped: a fader in Open Stage Control writes `ControlModule`'s surface, and the device
reacts. What it cannot do is answer. Open Stage Control shows a stale slider the moment anything
else moves the value (the web UI, a preset recall, an audio-reactive effect), and two clients
disagreeing about one value is the split-brain the ingest design deliberately avoided on the way in.

The reason to fix it now rather than later is not Open Stage Control, which merely redraws. It is
that **a motorised desk's motors only move because the host sends positions back**. Feedback is not
a nicety there; it is the reason to own the hardware. A feedback path built only for OSC would
assume absolute values, no touch state and no labels, and all three are wrong for the X-Touch and
QCon Pro G2 already on the bench. That retrofit is the cost this plan exists to avoid.

## What the research settled, including a negative result

A survey of dedicated lighting surfaces (MA grandMA2/3 wings, ETC Eos fader wings, ChamSys MagicQ
wings, Obsidian NX, Avolites Titan) returned a clear negative worth recording, because it stops a
plausible feature from being planned twice:

- **There is no Mackie Control equivalent in lighting.** Every vendor wing is a closed proprietary
  USB peripheral that talks only to its own software, undocumented and not third-party
  implementable. This is a licensing position, not an oversight: the wing is what unlocks the
  parameter count.
- **OSC is the only open door, and there is zero convergence.** ETC Eos `/eos/fader/<bank>/<n>`,
  grandMA3 `/gma3/Page1/Fader201`, ChamSys `/pb/<n>`, ONYX `/Mx/fader/<n>`; Avolites has no OSC at
  all. Four vendors, four namespaces, four feedback models.
- **A professional expects an LED/DMX controller to be a SINK, not a console.** They point a desk at
  it over sACN or Art-Net, which `NetworkReceiveEffect` already does. Nobody expects to plug a fader
  wing into a third-party controller.

So an X-Touch driving MoonLight is a genuinely novel capability, and this plan does not pretend it
meets an industry expectation, because there is not one. What IS industry standard is the shape:
one internal surface model with swappable transports, which is how every host that speaks to both a
Mackie desk and an OSC app is built.

## Scope

**In:** a `ControlSurface` seam; `ControlModule` as the owner of surface state; OSC as the first
implementation, both directions.

**Out, with reasons:**

- **Lighting-vendor OSC profiles** (Eos, grandMA3, MagicQ, ONYX address maps). They would let
  MoonLight DRIVE a console, which is backwards: a console already drives us over sACN. Each needs
  its own hardware to test, and Avolites cannot be reached at all. Revisit only if a user asks.
- **RTP-MIDI and the MCU semantic layer.** The seam is designed for it and the test strategy
  rehearses it, but the transport is its own plan.
- **MIDI transport of any kind, and per-device profiles.** Steps 2 to 5 of the delivery order. The
  seam must not make them harder, which is what its four verbs are for; building them is not this
  plan. Named targets when they come: Akai APC mini and APC40 mk2 (the PO's), Launchpad, Launch
  Control XL, nanoKONTROL2, X-Touch MINI/Compact, and the full X-Touch.
- **Scribble strips over OSC.** The seam carries labels; the OSC implementation ignores them,
  because no OSC client we target renders them.

## What the hardware actually demands

A second survey, of the non-motorised controllers people actually own (Akai APC mini and APC40 mk2,
Novation Launchpad and Launch Control XL, Korg nanoKONTROL2, Behringer X-Touch MINI and Compact,
Arturia), corrected the first assumption this plan was built on.

**Mackie Control is the minority case, not the target.** Every Akai APC and every Launchpad speaks
plain notes/CC plus a vendor SysEx dialect, and never MCU. Only the X-Touch MINI/Compact,
nanoKONTROL2 and Arturia offer MCU at all, and each only as a boot-selected secondary mode. The
APC40 mk2's own protocol document lists Generic, Ableton Live and Alternate Ableton Live: no Mackie.

**Feedback comes in three shapes, and only the first is what a motorised desk needs:**

1. **Position** to a fixed protocol address (MCU: fader as pitch-bend, ring as a packed CC).
2. **Echo** of whatever address the control itself used, with a brightness or on/off value. Covers
   nanoKONTROL2, MIDImix, Launch Control XL, X-Touch in Standard mode.
3. **Colour**, via a per-vendor SysEx dialect with no shared structure (`F0 47` Akai,
   `F0 00 20 29` Novation), different palettes and different bulk framing.

The third is where all the RGB lives, and it is the one a lighting controller cares about most: an
APC mini's 64 pads have no meaning until the host lights them. That device is a dumb 8x8 output
surface with buttons under it, and our pads already carry a colour (`roleHue`, derived from what a
preset captures), so the grid can show what the web UI shows rather than needing a new model.

So the seam's requirements are:

1. **Feedback is the display, not an echo.** On a motorised desk it keeps hardware in sync; on an
   APC it IS the output. Either way it cannot be an afterthought.
2. **Encoders are RELATIVE on MCU and absolute elsewhere.** Both forms, or every encoder needs
   unpicking later.
3. **Touch state matters** on the surfaces that report it (MCU notes 0x68-0x70). Sending a position
   to a held fader fights the user.
4. **Three feedback verbs, not one.** A value, a ring position with a STYLE (both the APC40 mk2 and
   the X-Touch MINI carry style on a separate CC, with near-identical enums), and a colour.
5. **Bulk colour updates.** Per-pad note-on for 64 pads at frame rate is 192 bytes a frame over
   USB-MIDI and will choke. Both vendors publish a range/bulk SysEx precisely for this.

## Design

**`ControlModule` owns the surface state; a surface is a transport that mirrors it.** That is the
existing rule ("the control module should be the only interface") carried to feedback, and it is
what lets two surfaces attach at once. A phone running Open Stage Control and an X-Touch on the desk
both stay correct because each is a view of one state, not a peer syncing with the other.

**The seam** (`src/core/util/ControlSurface.h`), implemented by `OscModule` today and an MCU module later:

```cpp
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };
/// What an LED ring should DRAW, not just where. The APC40 mk2 (CC 0x18/0x38) and the X-Touch MINI
/// (CC 1-8) both carry this on a separate CC from the value, with near-identical enums.
enum class RingStyle : uint8_t { Single, Volume, Pan, Fan, Off };

class ControlSurface {
public:
    virtual ~ControlSurface() = default;
    /// A position: a motor, a widget, an LED bar. 0..255 in the surface's own units.
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;
    /// A ring: position AND what it draws. Default forwards to sendValue, so a transport with no
    /// rings needs no code and one with rings cannot forget the style.
    virtual void sendRing(uint8_t index, uint8_t value, RingStyle style) {
        sendValue(SurfaceControl::Encoder, index, value);
    }
    /// A colour, for an RGB grid. Default does nothing: most transports have no colour.
    virtual void sendColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {}
    /// Colours changed together. Default loops sendColor; a transport with a bulk SysEx overrides
    /// it, which is the difference between one message a frame and 192 bytes of them.
    virtual void sendColors(const uint8_t* rgb, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) sendColor(i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
    /// Text, where the transport can show it (a scribble strip). Default does nothing.
    virtual void sendLabel(uint8_t index, const char* text) {}
};
```

Four verbs rather than one, because the hardware genuinely has four kinds of feedback. Every verb
past the first has a default, so the OSC implementation overrides one method and a future MCU or
APC transport overrides what it can actually drive. A transport never has to pretend.

Inbound is not a virtual: a surface calls `ControlModule`'s existing write path, which is already
the one chokepoint. What the seam adds inbound is two facts a transport knows and the module cannot
infer:

```cpp
/// A turn, not a position: `delta` is signed detents. An absolute transport never calls this.
void applyEncoderDelta(uint8_t index, int8_t delta);
/// A hand is on this fader. Feedback to it is suppressed until released.
void setTouched(SurfaceControl kind, uint8_t index, bool held);
```

**When feedback is sent: `tick1s`, on change.** The same shape `HttpServerModule` uses to patch the
web UI, and it is chosen for a specific reason: sending from the write path would fire on the render
thread for every control write, including the ones the surface itself just made. Sampling instead
means no hot-path cost, no hook in `Scheduler::setControl`, and the echo problem mostly dissolves,
since a value that came from the surface already matches what we would send back.

1 Hz is right for Open Stage Control, which redraws. It is likely too slow for a motorised fader to
feel connected, so the cadence is a control (`feedbackHz`, default 1) rather than a constant, and
the honest measurement happens on the desk.

**Echo suppression, three layers**, because one is not enough:

- Send only on CHANGE against the last value sent per control.
- Skip a control whose `touched` flag is set.
- A short per-control mute after an inbound write, so a value arriving between two samples cannot
  bounce.

**Where OSC sends.** OSC has no discovery. Learn the peer from inbound packets (the address a
`/mm/fader/N` arrived from), with a `feedbackTo` control as an override for a fixed receiver. Zero
configuration for the case we actually have.

## Delivery order

The survey's own conclusion, and it is ordered by value per unit of work rather than by architecture:

1. **The seam + OSC feedback.** Proves the loop, and Open Stage Control is the test rig for
   everything after it. This plan.
2. **MIDI learn.** Works on every controller ever made, input only, and costs the user five seconds
   where a profile costs us a day. This is what makes an APC or a nanoKONTROL useful at all, and it
   is the industry's actual answer for generic gear (Resolume, QLC+, Ableton all do it).
3. **Generic echo feedback.** Nearly free once learn exists: send back the address that was learned.
   Lights the LEDs on nanoKONTROL2, MIDImix, Launch Control XL and X-Touch in Standard mode.
4. **MCU transport.** One implementation covers the full X-Touch, the MINI/Compact in MC mode, the
   nanoKONTROL2 in DAW mode and Arturia. Motors and touch.
5. **Per-device colour profiles.** Only the handful with RGB grids (APC mini mk2, APC40 mk2,
   Launchpad). Small, self-contained, and the highest visual payoff: a 64-pad grid showing the
   layout's own colours is the reason lighting people own those devices.

**A GPIO surface belongs early, not late.** It needs no transport, no USB and no network; `addPin`
and `PinsModule` already exist; and it is the only entry that works on an ESP32 with no new platform
capability. Slot it beside step 2 rather than after step 5. It is also the honest answer for a
purpose-built MoonLight box, where a MIDI controller is the wrong shape entirely.

Steps 2 and 3 together make every controller on the survey usable. That is a better second step than
MCU, which serves fewer devices for more work.

**On profile format, when step 5 arrives:** QLC+ ships 42 input profiles as flat XML that names and
types a numeric channel space, and treats "the same device in MCU mode" as a SEPARATE profile rather
than a protocol layer. That is the shape to copy rather than invent, and it is a reason not to
design a profile format now.

## What can physically reach us, and on which platform

The seam is transport-neutral; the hardware is not. Worth stating before anything implies parity we
cannot deliver.

**Almost every controller in the survey is USB-only** (APC mini, APC40 mk2, Launchpad, nanoKONTROL2,
Launch Control XL). Plugging one into a device means the ESP32 acting as a **USB host**, and there is
no USB host support in this firmware today. On desktop the OS hands us the port and it is easy.

That inverts an assumption in the delivery order: **the X-Touch over its RJ45 (RTP-MIDI) needs no USB
at all**, so on ESP32 the motorised desk may be REACHABLE SOONER than a cheap USB pad grid.

| transport | desktop | ESP32 |
|---|---|---|
| OSC over UDP | works today | works today |
| RTP-MIDI (X-Touch's Ethernet port) | network | network, no USB needed |
| USB MIDI (every APC, Launchpad, nanoKONTROL) | OS gives us the port | needs USB host + a MIDI class driver |
| GPIO (below) | not applicable | native |
| DMX-512 in over RS-485 | needs a USB-DMX widget | needs the transceiver already backlogged for DMX out |

**GPIO is the surface with no protocol, and it may be the most valuable one.** A dedicated MoonLight
box with four knobs, a few switches and a foot switch needs no MIDI, no USB and no network. It is
also the case the existing infrastructure already fits: `addPin` declares the pins and `PinsModule`
handles claiming and conflicts, so a GPIO surface is a small module implementing `ControlSurface`
with no new platform work.

- **Switches** to `switch1..8`: a debounced digital read. Latching or momentary is a control.
- **Rotary encoders** to `encoder1..8`: quadrature, and it produces exactly the RELATIVE deltas the
  seam already carries for MCU. The same `applyEncoderDelta` serves both.
- **Analog pots** to `fader1..8` on an ADC, where the board has one free.
- **A foot switch** is one GPIO and is genuinely used live: the best value per unit of work on any
  of these lists.
- **Feedback**: an LED per switch is `sendValue` driving a GPIO. A WS2812 strip beside the buttons
  would even serve `sendColor`, using the driver we already ship.

**A phone is already done**: Open Stage Control runs in a browser, so a phone is a supported surface
today rather than future work.

**Not planned, with reasons:** a generic gamepad (novelty, no lighting precedent) and Stream Deck
(proprietary USB HID, and a screen-button device is what our own web UI already is).

## Files

1. **New `src/core/util/ControlSurface.h`** — the seam above. Pure interface, no transport.
2. **`src/core/system/ControlModule.h`** — register/unregister a surface; hold `touched` state; notify
   surfaces on change from `tick1s`; `applyEncoderDelta`.
3. **`src/core/util/OscPacket.h`** — an `encode()` beside the existing `parse()`, same constants.
4. **`src/core/services/OscModule.h`** — implement `ControlSurface`; `feedback` (default off), `feedbackTo`,
   `feedbackHz` controls; learn the peer address.
5. **Docs** — the OSC card gains the feedback controls; `control-surfaces.md` gains the seam and the
   negative lighting result.

## Tests

- **`unit_ControlSurface`**: a mock surface receives exactly the changed controls and nothing else;
  a touched control is skipped; an encoder delta accumulates and clamps at both ends.
- **`unit_OscPacket`**: encode round-trips through the existing parse, including the padding
  boundary that the ingest bug lived in, and a golden byte vector for one outbound message.
- **`unit_OscModule`**: an inbound write does not immediately echo; a change from elsewhere does
  send; the learned peer address is used when `feedbackTo` is empty.
- **Scenario**: set a control via the HTTP API, assert the surface was notified.

## Verification: Open Stage Control as a hardware simulator

The test strategy is the part worth stating, because it de-risks the transport we cannot yet test.

**Open Stage Control simulates the desk.** A layout mimicking an X-Touch (8 faders, 8 encoders, a
channel-button row, text widgets standing in for scribble strips) exercises the WHOLE seam: feedback
moves a widget where it would move a motor, touch can be simulated, relative encoders can be sent.
Only the MIDI framing stays untested. That turns "does the closed loop work" from a bench question
into a desktop one, repeatable and visible.

**The X-Touch layout is written BEFORE the MCU transport**, as its acceptance test. When the MIDI
work starts, the target behavior is already defined and demonstrable.

**What it cannot prove**, stated plainly: MIDI pitch-bend encoding, the RTP-MIDI session handshake,
and whether a real motor fights a real hand. The last mile still needs the desk.

Bench order: desktop with Open Stage Control, then an ESP32 (the module is platform-neutral, but the
1 Hz sample runs on the render thread and should be measured there), then the X-Touch when its
transport exists.

## Risks

- **Feedback cadence is a guess.** 1 Hz suits a redrawing client and probably not a motor. It is a
  control, and the number comes from the bench rather than from this document.
- **Echo suppression is where this class of feature usually fails.** Three layers, and the
  mute-after-write window is the one with an arbitrary constant in it.
- **`tick1s` sampling costs a comparison per surface control per second** (24 today). Negligible,
  but it is on the render thread and the hot-path check will report it.
- **Two surfaces attached at once is designed for but untested** until a second transport exists.
- **The seam could still be wrong for MCU.** It is derived from written protocol references rather
  than from a working implementation. The Open Stage Control rehearsal is what shrinks this risk,
  and it does not eliminate it. The first revision of this plan assumed MCU was the target and had
  ONE feedback verb; the hardware survey showed most controllers are not MCU at all and need three.
  A third correction is possible.
- **A gotcha to document when MIDI lands:** the nanoKONTROL2 ignores all LED feedback until the user
  flips `LED Mode` from Internal to External in Korg's editor and writes it to the device. A
  persistent hardware setting no host can send, and it will read as our bug.

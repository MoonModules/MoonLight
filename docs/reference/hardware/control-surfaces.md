# Control surfaces: hardware reference

What MoonLight needs to know about the physical desks on the bench, so a control-ingest plan can be written from facts rather than from a product page. A desk here is a candidate source for [ControlModule](../../moonmodules/core/system.md#control)'s pads, encoders and faders, which were laid out to match this class of hardware in the first place.

**The headline, because it contradicts the obvious assumption:** neither desk speaks OSC. Both are
**Mackie Control** surfaces. OSC is the right protocol for the wider ecosystem (Resolume,
TouchDesigner, TouchOSC, DIY Arduino rigs) and is planned on that basis, but it does not reach these two. See [the OSC plan](../../work/past/plans/Plan-20260829%20-%20OSC%20control%20ingest%20(shipped).md).

## Behringer X-Touch (Universal)

The larger of the two, and the only one with a network port.

**Sources:** [product page](https://www.behringer.com/product.html?modelCode=0808-AAF) ·
[Ardour's notes on Behringer in MCU mode](https://manual.ardour.org/using-control-surfaces/mackie-control-protocol/behringer-devices-in-mackielogic-control-mode/)

| | |
|---|---|
| Faders | 9 x 100 mm touch-sensitive **motorized** (8 channel + 1 master) |
| Encoders | 8 rotary push-encoders with LED rings |
| Display | LCD scribble strips per channel |
| Protocols | **Mackie Control (MCU)**, HUI, plain MIDI |
| Connectivity | USB-B (MIDI), 5-pin DIN MIDI in/out, **RJ45 Ethernet for RTP-MIDI** |
| Not supported | OSC |

**The Ethernet port carries RTP-MIDI, not OSC.** RTP-MIDI (RFC 6295, UDP port 5004) is MIDI
tunnelled over a network with a session layer: an invitation handshake, synchronization, and a journal so a dropped packet can be recovered rather than losing a note. It is a real protocol to implement, not a framing detail, which is what separates "reach this desk over the LAN" from "reach it over USB".

## iCON QCon Pro G2

**Sources:** [product page](https://iconproaudio.com/product/qcon-pro-g2/) ·
[user manual (PDF)](https://c3.zzounds.com/media/QCONPX-User-manual-English-7481d0b0d7b94af8a008c7e2634d0f59.pdf)

| | |
|---|---|
| Faders | 9 x touch-sensitive **motorized** (8 channel + 1 master) |
| Encoders | 8 push-encoders with 11-segment LED rings |
| Display | backlit LCD, 12-segment LED level meter per channel |
| Buttons | 78, plus a jog/shuttle wheel and 2 foot-pedal inputs |
| Protocols | **Mackie Control (MCU)**, HUI |
| Connectivity | **USB 2.0 only** (class-compliant) |
| Not supported | OSC, Ethernet |

**No network port at all.** Reaching this desk means a USB-MIDI host, so an ESP32 would need USB
host support and a machine in the rack is the practical answer today.

## What Mackie Control looks like on the wire

Enough to judge the size of the job. MCU is ordinary MIDI carrying agreed meanings, so a parser is small; the work is in the semantics and the feedback, not the bytes.

| element | encoding |
|---|---|
| Fader position | **pitch bend**, one MIDI channel per fader (channels 0-8 for faders 1-9). 14-bit, which is why a motorized desk feels smooth where a 7-bit CC would step |
| Fader touch | note on/off, notes 0x68 (104) to 0x70 (112), so the desk says when a hand is on a fader |
| Encoders | control change, relative (a turn sends a delta, not a position) |
| Encoder LED rings | control change back to the desk; the value's mode field picks dot / boost-cut / wrap / spread |
| Buttons and LEDs | note on/off, the same note number in both directions |
| Scribble strips | SysEx, `0x12` after the header, then the text |

**It is bidirectional by nature, and that is the point of the hardware.** The motors only move
because the host sends fader positions back; the scribble strips only show anything because the host writes them. A MoonLight implementation that only *reads* the desk would work, but would waste what makes these desks worth owning: a preset change should move the faders and relabel the strips.

## What this means for MoonLight

Three routes, in increasing cost:

1. **OSC, for everything else.** Does not reach either desk, reaches the whole app ecosystem and
   any DIY controller. Planned first because it is small and broad.
2. **A bridge.** An existing MCU-to-OSC translator on a laptop turns either desk into an OSC
   source. No firmware work; costs a machine in the rack, which a festival podium may already have.
3. **RTP-MIDI + MCU in firmware.** Reaches the X-Touch over Ethernet with no host machine, and is
   the only route that drives the motors from MoonLight directly. Needs RFC 6295 (session handshake, journalling) plus the MCU semantics above, both directions. Does not help the QCon, which has no network port.

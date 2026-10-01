# Control surfaces: hardware reference

The physical desks on the bench, and Mackie Control as it travels on the wire. A desk here drives [ControlModule](../../moonmodules/core/system.md#control)'s encoders, faders and switches, which were laid out to match this class of hardware. How to connect one: [Connecting a control surface](../../how-to/control-surface.md).

**Neither desk speaks OSC.** Both are **Mackie Control** surfaces over MIDI, which MoonLight reads through its [MIDI service](../../moonmodules/core/services.md#midi).

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

**No network port at all.** It reaches MoonLight through a computer's USB port and the browser.

## What Mackie Control looks like on the wire

MCU is ordinary MIDI carrying agreed meanings, so a parser is small; the work is in the semantics and the feedback, not the bytes.

| element | encoding |
|---|---|
| Fader position | **pitch bend**, one MIDI channel per fader (channels 0-8 for faders 1-9). 14-bit, which is why a motorized desk feels smooth where a 7-bit CC would step |
| Fader touch | note on/off, notes 0x68 (104) to 0x70 (112), so the desk says when a hand is on a fader |
| Encoders | control change, relative (a turn sends a delta, not a position) |
| Encoder LED rings | control change back to the desk; the value's mode field picks dot / boost-cut / wrap / spread |
| Buttons and LEDs | note on/off, the same note number in both directions |
| Scribble strips | SysEx, `0x12` after the header, then the text |

**It is bidirectional by nature, and that is the point of the hardware.** The motors move only because the host sends fader positions back, and the scribble strips show only what the host writes. MoonLight's [MIDI service](../../moonmodules/core/services.md#midi) sends the positions, the button lights and the rings, so a preset change moves the faders.

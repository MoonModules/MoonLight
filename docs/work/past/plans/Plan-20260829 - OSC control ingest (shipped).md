# Plan: OSC control ingest

## Context

MoonLight has a control surface but no way for a physical desk to reach it. `ControlModule` is
already shaped like one: an 8x8 pad grid, `encoder1`..`encoder8`, `fader1`..`fader8`, and a `driveFader()`
that routes a fader to whatever it targets via `Scheduler::setControl`. What is missing is the
transport that turns a knob move on someone's desk into one of those control writes.

**OSC is the right first transport**, and the reason is reach rather than elegance: it is what
Resolume, TouchDesigner, TouchOSC, Chataigne, QLab and most DIY Arduino/Teensy-over-Ethernet rigs
already speak. One implementation opens MoonLight to all of them. It is also small: UDP, a
big-endian address string, a type-tag string, and 32-bit aligned arguments.

**A correction worth recording, because it shaped this plan.** The premise that reached us was
"OSC is the way, I have a Behringer X-Touch". OSC is indeed the way for the ecosystem, but **the
X-Touch does not speak it**, and neither does the QCon Pro G2 that `control.md` also names. Both
are **Mackie Control** surfaces ([control surfaces reference](../../../reference/hardware/control-surfaces.md)).
So OSC does not connect the desks we own, and this plan deliberately does not pretend
otherwise. Driving those needs RTP-MIDI plus the MCU semantic layer, including motor feedback,
which is a much larger job and is scoped separately at the end.

## Scope

**In:** receive OSC over UDP and write it onto `ControlModule`'s existing surface.

**Out (this plan):** sending OSC, RTP-MIDI/Mackie, OSC bundles, TCP/SLIP framing, address-pattern
wildcards, and any new control surface. Each is noted at the end with why.

## The wire format (authoritative: OSC 1.0, opensoundcontrol.stanford.edu)

An OSC message is three parts, every one padded to a 4-byte boundary, all values **big-endian**:

| part | encoding |
|---|---|
| address pattern | OSC-string starting `/`, null-terminated, padded to 4 with nulls |
| type tag string | OSC-string starting `,`, then one char per argument (`i` int32, `f` float32, `s` string, `b` blob) |
| arguments | in tag order, each 32-bit aligned |

That is the whole format we need. A packet beginning `#bundle` is a bundle; we ignore those in
this pass (see Not doing).

## Design

**One new module: `OscModule`** (core, a Services child beside AudioService). It owns a UDP socket,
parses inbound messages, and writes onto `ControlModule`. It does NOT own a second control surface:
the pads, encoders and faders already exist, and duplicating them would be the split-brain the
architecture forbids.

**Address scheme.** Map onto the surface that exists, with the module's own name as the root so the
namespace stays MoonLight's:

```
/mm/fader/1   f 0.0..1.0     ->  ControlModule fader1   (or i 0..255)
/mm/encoder/3     f 0.0..1.0     ->  ControlModule encoder3
/mm/pad/12    i 1            ->  apply preset in slot 12 (nonzero = press)
```

A second, deliberately narrow form reaches any control directly, which is what makes MoonLight
useful to TouchDesigner without waiting for a surface binding:

```
/mm/control/Drivers/brightness  f|i  ->  Scheduler::setControl("Drivers", "brightness", value)
```

Both land in `Scheduler::setControl`, the same entry point the HTTP API and the UI already use, so
OSC gains no privilege of its own and every validator still runs.

**Value conversion, stated once.** OSC apps overwhelmingly send `f` in 0..1 (TouchOSC, Resolume);
hardware bridges often send `i`. Accept both: a float is scaled by 255 and clamped, an int is
clamped. A float outside 0..1 is clamped rather than rejected, because a controller sending 0..127
would otherwise appear dead.

**Where it runs.** Drain the socket in `tick()` with the bounded non-blocking loop
`AudioService::syncReceive` already uses (N datagrams per tick, `recvFrom` returning -1 ends it).
No allocation, no blocking, so the hot-path rule holds. A control write per datagram is fine at
fader rates (a desk moves a handful of controls per frame, not thousands).

## Files

1. **New `src/core/util/OscPacket.h`** — the wire format in one place, the `ArtNetPacket.h` /
   `WLEDAudioSyncPacket.h` convention: constants plus inline `parse`, unit-tested against a golden
   byte vector. Pure logic, no socket, so it is host-testable.
2. **New `src/core/services/OscModule.h`** — the module: socket lifecycle, the drain loop, address routing.
3. **`src/main.cpp`** — register the type.
4. **Docs** — a card in `docs/moonmodules/core/services.md`, and the address table.

## Tests

- **`unit_OscPacket`**: golden vector for a real TouchOSC message; padding at every length (an
  address of 3, 4, 5 characters exercises the pad boundary); a float and an int argument;
  rejection of a truncated packet, a missing type tag, a non-`/` first byte, and a `#bundle`.
  The parse must never read past the buffer, which is the security-relevant property here since
  the input is an unauthenticated datagram from the LAN.
- **`unit_OscModule`**: `/mm/fader/1` at 1.0 sets brightness to 255; at 0.5 to 127; an int form
  gives the same result; an out-of-range address (`/mm/fader/99`) is ignored rather than writing
  anywhere; `/mm/control/...` reaches an arbitrary module.
- **Scenario**: an OSC datagram changes a control and the change is visible in `/api/state`.

## Verification

Desktop first, driven by a Python OSC sender in `moondeck/` (no new dependency: the format is
~20 lines to emit). Then a real client: TouchOSC on a phone against the desktop build, which is
also the honest test of the 0..1 float convention. Bench ESP32 last, since the module is
platform-neutral.

## Risks

- **An unauthenticated UDP port writes device controls.** Same exposure as the existing ArtNet and
  audio-sync receivers, and the same answer: a LAN-trust protocol, bounded parse, and every write
  goes through `setControl`'s validators. Worth stating in the doc rather than leaving implicit.
  The module is off by default and its port is a control.
- **A malformed datagram is the attack surface.** Mitigated by the parse tests above: length
  checked before every read, no allocation, fixed-size buffers.
- **Address scheme is a public contract.** Once someone builds a TouchOSC layout against it,
  renaming an address breaks their file. Hence the `/mm/` root and a small, boring first set.

## Not doing (scope guards)

- **No OSC send.** Feedback (moving a motorized fader, lighting a pad) is the natural second half,
  but nothing we own consumes it: the desks we have are MCU, not OSC. Add when a device wants it.
- **No bundles.** Controllers send plain messages; a bundle is for timed batches. Parse the
  `#bundle` marker only far enough to ignore it cleanly.
- **No address wildcards** (`*`, `?`, `[]`, `{}`). The spec has them; controllers do not send them.
  Exact match until something needs more.
- **No TCP/SLIP OSC.** UDP is what the ecosystem uses.
- **No new control surface.** OSC maps onto ControlModule's existing pads/encoders/faders.

## Afterwards: the Mackie desks

This plan does not connect the X-Touch or the QCon. Two routes, to judge once OSC lands:

1. **A bridge**: an existing MCU-to-OSC translator on a laptop. Zero firmware work, costs a machine
   in the rack.
2. **RTP-MIDI + MCU in firmware**: an RFC 6295 session (invitation handshake, journalling) plus the
   MCU semantics (faders as per-channel pitch-bend, and **motor positions sent back**, which makes
   it bidirectional by nature). Much larger, and only pays off for MCU-class hardware. The
   bidirectional half is the real argument for it: on a motorized desk, a preset change should move
   the faders.

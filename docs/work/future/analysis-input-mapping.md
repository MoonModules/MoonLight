# Input mapping: how a physical control reaches a module control

Analysis, not a decision. Written 2026-09-01 after `ButtonService` shipped with a single button and
a single target, which does not scale to a board with four buttons or a remote with twelve keys.

## The two-step interface

The product owner's framing: **everything goes through ControlModule**. An input service learns to
map its physical events onto the surface (switches, encoders, faders, pads); the surface maps onto
whatever those drive. Two steps, each simple:

```text
physical event            step 1: input service      step 2: ControlModule
  button on GPIO 0    ->  switch1                ->  Drivers.on
  IR code 0x40BF      ->  switch1                ->  Drivers.on
  encoder A turned    ->  encoder3               ->  (whatever encoder3 targets)
  OSC /mm/pad/3       ->  pad3                   ->  preset 3
```

**Why two steps rather than the direct `Module.control` that shipped.** A single step is shorter for
one button and wrong for everything else:

- The surface is the **one place** to see what the device's controls do. With direct targets that
  knowledge is scattered across every input service, and two inputs driving the same thing look
  unrelated.
- The surface already exists, is already persisted, and is already what OSC drives. A second wiring
  model beside it is the split brain
  [the OSC plan](../past/plans/Plan-20260829%20-%20OSC%20control%20ingest%20(shipped).md) forbids.
- Feedback needs it. A motorised fader or an LED-ringed encoder has to be *told* the current value;
  that lives on the surface, and an input mapped straight to a module control has nowhere to read it
  back from.
- It makes inputs interchangeable: a rehearsal driven by IR and a show driven by a pedalboard hit
  the same switches, so the mapping behind them is unchanged.

The cost is one indirection for the simple case, and the direct form should stay available:
`target = "Drivers.on"` and `target = "Control.switch1"` are both just strings, so nothing needs a
special case. The surface is the recommended path, not an enforced one.

## What the mapping table looks like

**The trigger vocabulary is worth borrowing.** ESPHome models each input as its own entry (not a
central table, which matches the per-service list here) and its useful contribution is the *event*
set: `on_press`, `on_release`, `on_click` with a length window, `on_double_click`, `on_multi_click`.
A long-press and a double-press are what let three physical buttons drive nine actions, which is the
difference between a usable installation panel and a wall of switches. Start with press / release /
long-press; double and multi-click are the same machinery with more timers.

## Foot pedals: not USB

Worth correcting an assumption recorded earlier in the backlog. Stage pedals are **1/4" jacks**, not
USB, and they come in two kinds that need different handling:

- A **footswitch** is a switch on a TS jack: electrically identical to a wall button, so
  `ButtonService` covers it with no new work. Momentary mode is hold-to-activate; latching is
  press-to-toggle. (Some are latching in hardware, which is why the mode has to be configurable
  rather than assumed.)
- An **expression pedal** is a **potentiometer** on a TRS jack, giving a continuous 0..100%. That is
  an **ADC read**, not a GPIO read, and it maps onto a `faderN`, not a switch. New seam
  (`adcRead`), new service, genuinely different from a button.

So "foot pedal support" is two features: the footswitch is done, and the expression pedal is an
analog-input service that does not exist yet. USB pedals do exist but are a niche (they enumerate as
HID keyboards), and they are the gamepad problem below, not the pedal problem.

## Two kinds of input, and only one of them is a mapping

The two-step model above is right for **discrete** inputs and wrong for **continuous** ones, and
conflating them is the mistake to avoid.

**A discrete input is an event.** A button press, a remote code, a pedal stomp: it happens, it
drives a control, and nothing reads it again until the next one. Its natural rate is human
(a few per second at most), a control write per event costs nothing, and the surface is the right
destination because a person needs to see what it does. This is what the mapping table is for.

**A continuous input is a stream.** A microphone at 22 kHz, an IMU at 50 Hz, a distance sensor an
effect samples every frame: an effect wants *the current value on the hot path*, not a notification.
Routing that through `setControl` would be wrong twice over. It would put a control write, a name
lookup and a persistence dirty-flag on the render path at frame rate, which the
[hot-path rules](../../explanation/architecture/moonmodule.md#hot-path-discipline) forbid. And it would be lossy: a control is
a setting a person edits, where a stream is data an effect reads.

**The pattern for a stream already exists and is already domain-neutral.** `AudioService` publishes
an `AudioFrame` and effects reach it through the static `AudioService::latestFrame()`;
`AudioSpectrumEffect`, `GEQEffect`, `SpectrumEffect` and `NoiseMeterEffect` all consume it that way.
[MoonModule, data exchange](../../explanation/architecture/moonmodule.md#data-exchange-between-modules) states it as the
shared-struct pull: a POD struct the producer overwrites in place each tick, a plain-data header
both sides include, a const getter, no allocation and no subscription. It even names this case:
lock-free "is visually harmless for the gyro/sensor data this carries".

So the rule is:

| input | rate | how it reaches the light domain |
|---|---|---|
| button, remote code, footswitch, PIR | an event, human rate | mapping table -> `setControl` -> the surface |
| encoder detent | an event, human rate | mapping table -> `setControl` (a relative nudge) |
| microphone | continuous, per tick | `AudioService::latestFrame()`, shipped |
| IMU, distance, light level | continuous, per tick or per 20 ms | a published frame, the same pull |
| expression pedal | either | a fader (a setting) or a frame (a stream), by intent |

**The expression pedal is the interesting boundary**, and it shows the split is about *use*, not
about the sensor: swelling brightness is a setting a person is adjusting, so a fader; driving an
effect's parameter per frame is a stream. A pedal could reasonably do both, which argues the two
paths should be selectable per input rather than fixed by the device type.

**What this means for the modules.** A sensor service does both jobs, and they are cheap to combine:
it publishes its frame for effects to pull, *and* it may carry mapping rows for thresholds a person
cares about ("closer than 50 cm" drives a switch). One module, two outputs, matching what the sensor
is: a stream, with events derivable from it.

### A third shape: a sensor that produces an image

The VL53L8CX (and its VL53L5CX predecessor) is not a distance sensor with extra decimals. It reports
an **8x8 grid of zones at up to 60 Hz**, over I2C or SPI at 3 MHz: a low-resolution depth image at
render rate. That is a third shape beside event and stream; what it changes is step 4b of [the plan](../present/Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20%287%20of%2012%29.md).

**It carries an 84 KB firmware blob**, measured from ST's own driver
([stm32duino/VL53L8CX](https://github.com/stm32duino/VL53L8CX), `vl53l8cx_buffers.h`):

| | |
|---|---|
| firmware uploaded to the sensor at every boot | **86,017 bytes** (plus 972 B config, 776 B crosstalk) |
| upload time | 1.94 s at I2C 400 kHz, 0.77 s at 1 MHz, **0.23 s at SPI 3 MHz** |
| driver RAM | ~2.3 KB (`VL53L8CX_Configuration`) |
| result struct | ~1 KB per frame |

**No prior art to lean on, checked rather than assumed.** `troyhacks/WLED` (the tracked friend repo)
carries `usermods/VL53L0X_gestures` on all three of its P4 branches, but that is upstream WLED's
usermod for the *simple* VL53L0X, present identically in `wled/WLED`, and a code search for
`VL53L8` across both that fork and the MoonModules org returns nothing. So the blob upload, the SPI
path and the zone-frame publishing are ours to work out, with ST's ULD driver as the only reference.

**Where it lands in the plan:** the only ToF on the bench, so it is both the second I2C device and
the hard case. See step 4b.

## Bus sensors: I2C and I2S

A GPIO carries one bit. The sensors an installation actually wants mostly do not: a motion sensor
worth having reports distance or an occupancy zone, not a level, and that arrives over a bus.

**I2C is the common case, and its seam is proposed rather than shipped**: the unmerged commit `11f8eb76` adds `i2cInit`, `i2cWriteReg` and `i2cReadRegs`, and step 4 of [the plan](../present/Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20%287%20of%2012%29.md) merges it.

What it unlocks, in the order an installation asks for it: a **ToF distance sensor** (VL53L0X: a
hand's distance drives a parameter, the well-behaved version of "motion"), a **light-level sensor**
(BH1750: the piece dims itself to the room), an **IMU** (the MPU6050 that commit would add), and
a **gesture/proximity sensor** (APDS-9960). All four are the same shape: init the bus, poll a
register set on `tick20ms` or `tick1s`, publish a value.

**Motion specifically is three different sensors**, and calling them all "motion" is what makes the
category confusing:

- **PIR** is a digital pin. Not a bus device at all, so it needs nothing beyond the shipped GPIO
  seam: presence as a level, which is how MoonLight models it (`pin_PIR`, "HIGH = lights on").
- **Ultrasonic (HC-SR04)** is a trigger pulse plus an echo whose *width* is the distance. Neither
  I2C nor a plain read: it needs a **pulse-timing seam** that does not exist. The textbook answer on
  ESP32 is the same PCNT/RMT machinery an encoder wants, so the two features share a dependency.
- **ToF** is I2C, so it rides the layer above. A **mmWave radar** such as the HLK-LD2450 is UART, so it needs the UART seam the [audio-reactive plan](../present/Plan-20261013%20-%20Audio%20reactive%20live%20performance%20%280%20of%207%29.md) names.

## MoonLive: scripted sensors and mappings

The direction is that MoonLive gains driver scripts whose hello-world is "read from GPIO, write to
GPIO", with a precompiled sibling always available. Input mapping is where that pays off twice,
because the two halves want different things from a script.

**Half one: a script AS a sensor.** A sensor nobody has written a module for is exactly what a
script is good at, since the module is small and the value is in the register knowledge, which is
data rather than architecture. A VL53L0X script is then a `defineControls` for the address and pins, a `setup` that writes the
init registers, and a `tick20ms` that reads two bytes. That is a page of code, and a user with a
datasheet can write it without a toolchain. **This is the strongest argument for scripted services**:
the long tail of sensors is unbounded, and no firmware can carry all of them, but a script per
sensor costs nothing to ship and nothing to maintain.

**Half two: a script AS the mapping.** Once a script can call `setControl`, the mapping table itself
becomes optional for anything unusual: "if distance < 50 cm then switch1 on, else off" is three
lines, and expresses a rule no fixed table column could. The table stays the answer for the common
case (a button drives a switch), because a table is inspectable and a script is not.

## What the rules say

- **[CLAUDE.md, industry standards](../../CLAUDE.md)**: take the textbook construct. Quadrature via
  PCNT rather than a hand-rolled edge decoder; a HID descriptor parser rather than a per-model byte
  table.
- **[CLAUDE.md, minimalism](../../CLAUDE.md)**: every fact has one home. The surface is that home
  for "what does this control do", which is the argument for the two-step model over direct targets.
- **[MoonModule, live reconfiguration](../../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame)**:
  a mapping must be editable on a running device. A compiled-in action table fails this, which is
  the concrete defect in `IrService` today.
- **[MoonCore, services](../../explanation/architecture/index.md)**: "Direction is per-module, not a role: a
  service may read (gyro), write (relay), or both." Input services are already the sanctioned shape.

# Input mapping: how a physical control reaches a module control

Analysis, not a decision. Written 2026-09-01 after `ButtonService` shipped with a single button and
a single target, which does not scale to a board with four buttons or a remote with twelve keys.

## The problem with what shipped

`ButtonService` maps **one** pin to **one** `Module.control` string, and `IrService` carries a fixed
table of five actions (on/off, brightness up/down, palette next/prev) with a learned code each.
Neither survives contact with real hardware:

- A QuinLED Dig-Next-2 has three buttons; a Penta has three; a stage rig has a pedalboard.
- A remote has twenty keys, and which five are "the" actions is the firmware's opinion, not the
  user's.
- Adding a sixth IR action today means editing `kActions` and reflashing, which is exactly the
  configured-at-build-time model the project exists to avoid
  ([MoonModule, live reconfiguration](../../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame)).

Both modules have the same shape of defect, so they want the same fix.

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

Both services grow **a list of mappings** in place of their single target or fixed action table. The
mechanism exists: `Control::addList` with an editable-row hook (`addListRow`), which
`ControlModule` (presets), `DevicesModule`, `PinsModule` and `TasksModule` already use.

A row is roughly:

| field | button service | IR service |
|---|---|---|
| source | pin, and its `activeLow` / pull | the learned code (hex) |
| event | press / release / click / long-press | (a code is one event) |
| target | `Control.switch1`, or any `Module.control` | same |
| mode | latching / momentary | set / toggle / nudge +N |

**The trigger vocabulary is worth borrowing.** ESPHome models each input as its own entry (not a
central table, which matches the per-service list here) and its useful contribution is the *event*
set: `on_press`, `on_release`, `on_click` with a length window, `on_double_click`, `on_multi_click`.
A long-press and a double-press are what let three physical buttons drive nine actions, which is the
difference between a usable installation panel and a wall of switches. Start with press / release /
long-press; double and multi-click are the same machinery with more timers.

## Rotary encoders

Not a button, and worth being explicit: an encoder emits **quadrature** on two pins (A and B), where
the phase relation gives direction and each detent is a step. So it is a `+1 / -1` stream, not a
level, and it maps naturally onto an `encoderN` (a relative nudge), where a button maps onto a
`switchN`.

Most encoders also carry a **push switch**, which is a plain button on a third pin, so an encoder
row is "two pins for rotation, optionally one for the press". Whether that is one `EncoderService`
with its own list, or an `input type` column in a single service's list, is the open design
question below.

Reading quadrature reliably needs either an interrupt or a fast poll: the ESP32's PCNT (pulse
counter) peripheral does it in hardware and is the textbook answer, with a 20 ms poll of the count.
That is a new platform seam either way, so an encoder is more work than a button, not less.

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
render rate. That is a third shape beside event and stream, and it changes three things.

**It is a frame, not a value.** 64 zones is a *layout* worth of data, and the natural consumers are
an effect that maps zones onto lights (a hand's shape moving across a wall) or a modifier that masks
by depth. A single `distance` control cannot carry it, and neither can a fader.

**The read cost is the constraint, not the bandwidth.** 64 zones at 2 bytes is 128 B/frame, which is
7.7 kB/s at 60 Hz: nothing. But a blocking I2C read of 128 bytes at 400 kHz takes **~2.9 ms**, and
the render tick on a Dig-2-Go is 289 us. So this sensor must not be read on the render tick at all:
either SPI at 3 MHz (which is why the part offers it), a slower ranging rate matched to what the
effect needs, or the read moved off the render core the way the encode split already is
([MoonModule, parallelism](../../explanation/architecture/moonmodule.md#parallelism)). **This is the first sensor whose
platform seam has to be asynchronous**, and that is worth knowing before the synchronous
`i2cReadRegs` shape is treated as sufficient for everything.

**It argues the published frame should be sized by the sensor**, not fixed. A `SensorFrame` with one
`distance` field would be wrong here; an 8x8 array is 128 bytes a consumer reads in place, which the
shared-struct pull handles fine (it is a POD overwritten in place), but the header has to declare
the shape. The honest answer is probably a frame type per sensor family, as `AudioFrame` is specific
to audio rather than a general `SignalFrame`.

**It carries an 84 KB firmware blob**, measured from ST's own driver
([stm32duino/VL53L8CX](https://github.com/stm32duino/VL53L8CX), `vl53l8cx_buffers.h`):

| | |
|---|---|
| firmware uploaded to the sensor at every boot | **86,017 bytes** (plus 972 B config, 776 B crosstalk) |
| upload time | 1.94 s at I2C 400 kHz, 0.77 s at 1 MHz, **0.23 s at SPI 3 MHz** |
| driver RAM | ~2.3 KB (`VL53L8CX_Configuration`) |
| result struct | ~1 KB per frame |

**The boot upload is the bigger problem than the per-frame read**, and it was not the number to
worry about first: a blocking 2-second transfer in `setup()` stalls the device. SPI at 0.23 s is the
answer, which gives a second independent reason to prefer SPI for this part.

**The blob cannot be written ourselves.** It is executable firmware for the sensor's own processor,
not configuration that could be derived: ST publishes no register-level datasheet for this part,
only the ULD API, and the ranging algorithms and SPAD calibration are not documented. Reimplementing
it means reverse-engineering an undocumented DSP.

**So it is compiled in per firmware, not per build.** 84 KB is about 2% of a 4 MB classic ESP32's
flash, and roughly 4.5% of the ~1.8 MB app PARTITION it actually competes for, which is the number
that matters; it is noise on an S3 or P4. This is the first *peripheral* to need
compile-time inclusion gated by the device catalog, the shape `MM_HLS` and `MM_NO_ETH` already
have. A Dig-2-Go never carries it; a P4 installation does.

**MoonLive can still USE it, and that is the distinction to keep.** A script cannot upload 84 KB and
should never try, but it does not need to: the compiled module owns the blob and the bus, and the
script calls a builtin (`tofZone(x, y)`) exactly as it calls `beat()` or `paletteRGB()` without
implementing a beat or a palette. The power-function pattern is what makes a heavyweight sensor
scriptable. What a script cannot do is *be* the driver for such a sensor, which is the honest
boundary of the scripted-sensor claim: a script suits a sensor whose init is a handful of register
writes (MPU6050, BH1750, VL53L0X), and a blob sensor is a compiled module by necessity.

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

**I2C is the common case, and the seam is already written.** An unmerged commit
(`11f8eb76`, "Add GyroDriver (MPU6050) + generic platform I2C layer") adds exactly the three
functions a sensor module needs, with the register knowledge staying in the module:

```cpp
bool i2cInit(uint8_t sdaPin, uint8_t sclPin);
bool i2cWriteReg(uint8_t devAddr, uint8_t reg, uint8_t value);
bool i2cReadRegs(uint8_t devAddr, uint8_t reg, uint8_t* buf, size_t len);
```

This branch has only `i2cScan` (which addresses ACK, the `i2cdetect` operation) and therefore
cannot read a register at all. So **merging that layer is the prerequisite** for every I2C sensor,
and it is prior art from a contributor rather than something to design.

What it unlocks, in the order an installation asks for it: a **ToF distance sensor** (VL53L0X: a
hand's distance drives a parameter, the well-behaved version of "motion"), a **light-level sensor**
(BH1750: the piece dims itself to the room), an **IMU** (the MPU6050 the commit already drives), and
a **gesture/proximity sensor** (APDS-9960). All four are the same shape: init the bus, poll a
register set on `tick20ms` or `tick1s`, publish a value.

**Motion specifically is three different sensors**, and calling them all "motion" is what makes the
category confusing:

- **PIR** is a digital pin. Not a bus device at all, so it needs nothing beyond the shipped GPIO
  seam: presence as a level, which is how MoonLight models it (`pin_PIR`, "HIGH = lights on").
- **Ultrasonic (HC-SR04)** is a trigger pulse plus an echo whose *width* is the distance. Neither
  I2C nor a plain read: it needs a **pulse-timing seam** that does not exist. The textbook answer on
  ESP32 is the same PCNT/RMT machinery an encoder wants, so the two features share a dependency.
- **ToF** is I2C, so it rides the layer above. A **mmWave radar** such as the HLK-LD2450 is UART, so it needs the UART seam the [audio-reactive plan](../present/Plan-20261013%20-%20Audio%20reactive%20live%20performance.md) names.

**I2S is audio-shaped, and should stay that way.** The platform's I2S surface is `audioMicInit` /
`audioMicRead`, deliberately named for what it does rather than for the peripheral, and
`AudioService` is its only consumer. A few non-audio sensors do speak I2S (some ToF and radar
modules stream samples that way), but generalising the audio seam to serve them would trade a clear
contract for a vague one. If such a sensor is ever wanted, it gets its own seam named for its own
job, exactly as `audioMic*` is. **No action: I2S is done, for audio.**

The other reason to be careful: I2S is the mic's bus, and a second I2S consumer on the same board
competes for a peripheral instance. That is a real constraint on a classic ESP32 (two I2S
controllers), not a theoretical one.

## MoonLive: scripted sensors and mappings

The direction is that MoonLive gains driver scripts whose hello-world is "read from GPIO, write to
GPIO", with a precompiled sibling always available. Input mapping is where that pays off twice,
because the two halves want different things from a script.

**Half one: a script AS a sensor.** A sensor nobody has written a module for is exactly what a
script is good at, since the module is small and the value is in the register knowledge, which is
data rather than architecture. Builtins, matching the seams above:

```c
int  gpioRead(int pin);                    // shipped seam
void gpioWrite(int pin, int on);           // shipped seam
int  i2cRead(int addr, int reg);           // needs 11f8eb76 merged
void i2cWrite(int addr, int reg, int val);
```

A VL53L0X script is then a `defineControls` for the address and pins, a `setup` that writes the
init registers, and a `tick20ms` that reads two bytes. That is a page of code, and a user with a
datasheet can write it without a toolchain. **This is the strongest argument for scripted services**:
the long tail of sensors is unbounded, and no firmware can carry all of them, but a script per
sensor costs nothing to ship and nothing to maintain.

**Half two: a script AS the mapping.** Once a script can call `setControl`, the mapping table itself
becomes optional for anything unusual: "if distance < 50 cm then switch1 on, else off" is three
lines, and expresses a rule no fixed table column could. The table stays the answer for the common
case (a button drives a switch), because a table is inspectable and a script is not.

**The `setControl` decision is the gate**, and it is bigger here than it looked when first raised. A
script that can write any control on any module can also write a driver's pin list or a layout's
geometry. Options, in increasing order of restriction: expose it plainly and trust the author (a
script is already native code on the device, so this is not a sandbox boundary); restrict it to the
`Control` module, which makes the surface the script's only reach and matches the two-step model
above; or give a scripted service a declared list of targets it may write, which is the most work
and the most inspectable. **The middle option is the one that fits this analysis**: a script drives
the surface, the surface drives everything, and the same two steps hold whether the mapping came
from a table or a script.

**What it needs beyond builtins:** a `MoonLiveService` host module, the service twin of
`MoonLiveEffect`, carrying a `script` control and the compile/status path. Small, mostly a copy of
the existing binding, and it is what makes a scripted sensor addable from the picker like any other
module.

## USB game controllers

Unchanged from the earlier entry, and confirmed as its own project: a HID **report descriptor
parser** is the work (each controller lays its bytes out differently; the descriptor is a nested
tag/value blob that must be parsed to learn which bit is which button), plus USB Host bring-up.
S3/P4 only, since the classic ESP32 has no USB Host peripheral at all. The *mapping* half is free
once reports arrive: buttons to switches or pads, axes to encoders and faders, through the same
list.

DIY is the alternative worth naming: a gamepad's buttons wired to GPIOs is `ButtonService` with a
longer list, and an analog stick is two ADC channels. For an installation where the enclosure is
custom anyway, that may be the better answer than parsing a commercial controller.

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

## Open questions for the product owner

1. **One service or several?** `InputService` with a `type` column per row (button / encoder / IR /
   analog), or `ButtonService`, `EncoderService`, `IrService` each with their own list? One service
   is fewer modules and one table to learn; several keep each module's controls honest (an encoder's
   two pins do not belong on a button row). The catalog leans toward several, since a board declares
   what it has.
2. **Does `IrService` keep its action table** as a convenience default, or become purely
   code -> surface mappings? Purely mappings is cleaner and loses the out-of-the-box remote.
3. **What happens to the shipped `ButtonService.target`?** A single row is the same thing as a
   one-row list, so the migration is mechanical, but it is a breaking change to a control that has
   shipped (in this branch only, so far).
4. **How far into the trigger vocabulary** to go in the first pass: press/release only, or
   long-press too? Long-press is what multiplies a small panel's reach.

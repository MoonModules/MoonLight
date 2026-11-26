# Plan: input mapping and scripted sensors

Turns [the input-mapping analysis](../design/input-mapping.md) into steps. Two threads
run together throughout, deliberately:

- **Compiled modules** for the inputs a board ships with, declared in the device catalog.
- **MoonLive** as the flexible half, so a sensor nobody wrote a module for is a script a user writes
  with a datasheet.

They are not sequential phases. Every step that adds a platform seam exposes it as a MoonLive
builtin **in the same step**, because a builtin is one table row plus a host function
(`MoonLiveBuiltins_light.h`, 53 of them today) and because the script is how the seam gets tested
before any module depends on it. A step is done when both halves work on the bench.

## Background: what a driver is, where inputs live, and the GPIO seam

Merged in from the separate GPIO-seam plan (2026-09-02), whose steps are now either shipped or
carried into the step list below. This is the analysis that decided the shape, kept because nothing
else records it.

## What a driver is: the contradiction, and the correction

Two definitions are in the repo today and they do not agree.

- [`docs/moonmodules/light/drivers.md:3`](../moonmodules/light/drivers.md): "A driver sends
  lights somewhere." Output-only, light-specific.
- [`docs/explanation/architecture/index.md:143`](../explanation/architecture/index.md): "producers vs consumers: producers generate
  data, consumers process and output it. Effects are producers, drivers are consumers." A role in a
  dataflow, said of the light domain.

The product owner's definition is broader than both: **a driver communicates with hardware or the
network**, which explicitly includes talking to GPIOs. That is the definition this plan adopts, and
it is the better one: it describes what the code *is* (the boundary between the device and the
physical world) rather than what today's instances happen to do.

**The concrete contradiction in code.** `DriverBase` declares `virtual void setSourceBuffer(Buffer*)
= 0`: every driver is *structurally required* to consume a light buffer. So the light domain's
`DriverBase` is not "anything that talks to hardware"; it is specifically "a consumer of the light
buffer that outputs it". Every one of the sixteen drivers under `src/light/drivers/` does exactly
that. The narrow wording in `drivers.md` is therefore an accurate description of `DriverBase`, and
the disagreement is that it claims the word "driver" while describing only the light-domain
specialization of it.

**The resolution, and it is a rename of concepts rather than of code.** There are two distinct
things, and the repo needs both words:

| | what it is | where it lives | contract |
|---|---|---|---|
| **Driver** (light domain) | consumes the light buffer and outputs it | `Drivers` container, `DriverBase` | `setSourceBuffer` |
| **Service** (core domain) | a capability bridge the device provides or consumes | `Services` container, `ModuleRole::Service` | none imposed |

`services.md` already states this precisely: Services are "capability bridges the device provides or
consumes ... the core-domain twin of the light domain's Effects/Drivers". Audio and IR already live
there. Both talk to hardware; neither touches the light buffer.

So under the PO's broader definition, a light Driver and a core Service are **both** drivers in the
general sense: two families of the same idea, split by whether the light buffer is involved. The
documentation should say that, and `drivers.md` should stop implying its own definition is the only
one.

**Action:** amend `drivers.md:3` to scope its claim ("A *light* driver sends lights somewhere"), and
add a line to `architecture.md` naming the general sense and the two families. No code changes.

## Where the new inputs go: Services

A button, a foot pedal and a game controller are capability bridges the device **consumes**. They
produce no lights and never touch the light buffer, so they are Services, beside Audio and IR. This
is not a new container or a new role: `Services::acceptsChildRoles()` already returns `"service"`,
and `ModuleRole::Service` already exists.

## The GPIO seam

`platform.h` already carries a well-built GPIO vocabulary, and it is the industry-standard shape:
`gpioCapability(gpio)` (static truth: valid / output-capable / RTC / strap / reserved, from the
IDF's own `GPIO_IS_VALID_GPIO`, `GPIO_IS_VALID_OUTPUT_GPIO`, `rtc_gpio_is_valid_gpio`) and
`gpioLiveState(gpio)` (what a pin is doing now). `PinsModule` is the ownership map.

What is missing is an **input role**: `gpioLiveState` reads the pad, but it is a diagnostic sampled
on tick1s, not an input path. Two functions close it:

```cpp
// Configure one GPIO as an input with an optional internal pull. Idempotent; re-configuring a pin
// the caller already owns is not an error. Returns false on a pin gpioCapability rejects.
bool gpioInputBegin(uint8_t gpio, GpioPull pull);
// Read one configured input. Cheap enough for a 20 ms poll; no allocation, no blocking.
bool gpioRead(uint8_t gpio);
```

Plus `gpioWrite(uint8_t, bool)` for the output half, which the Dig-2-Go's relay needs and which
MoonLive's hello-world requires (below). Desktop implements all three against the existing test-seam
pattern (`setTestGpioLiveState`), so button logic is host-testable without hardware.

**Debouncing belongs in the module, not the seam.** The seam reports the pad; a bouncing contact is
a property of the switch, and the module owns the time constant as a control. This keeps the
platform layer a thin, faithful boundary, which is what the platform rule asks for.

## USB game controllers: why this is its own plan

A USB gamepad is not a GPIO. It needs **USB Host**, which on our targets means:

- **Classic ESP32 cannot do it at all** (no USB Host peripheral; it has no USB OTG). So this feature
  is S3/P4-only, and the Dig-2-Go in front of us could never use it.
- **S3/P4** have USB-OTG, and the IDF ships a USB Host stack plus a HID class driver. A gamepad is a
  HID device with a report descriptor that must be parsed to know which byte is which button/axis:
  that parsing is the real work, and it is why "support gamepads" is not a small item.
- **Desktop** would use the OS gamepad API, a completely different implementation behind the same
  seam.

The mapping half is easy once reports arrive (buttons -> pads, axes -> encoders/faders, through the
same `setControl`); the transport half is a genuine project. Scope it separately, after the GPIO
inputs land, and expect it to be S3/P4-only from the start.

## The sensors this plan is tested against

Agreed with the product owner, and all on the bench. **One per seam**: each proves a different
platform primitive, and each one that works unlocks the class of devices that share it. Nothing
redundant, because a second I2C sensor proves nothing the first did not.

| sensor | seam it proves | maps to | step |
|---|---|---|---|
| **Button** (the Dig-2-Go's, GPIO 0) | GPIO digital in | `switchN` | shipped, rebuilt in 1 |
| **PIR** (HC-SR501) | the same GPIO, as a *level* rather than a press | `switchN` | 6 |
| **IMU** (MPU6050) | I2C register read, a multi-register burst | `encoderN` / `faderN` | 4 |
| **ToF distance** (VL53L0X, on order) | I2C register read, the simple case | `faderN` / any value control | 4 |
| **ToF zone grid** (VL53L8CX) | I2C at a size the render tick cannot absorb | an effect or modifier, per zone | 4b |
| **Rotary encoder** (KY-040) | pulse timing / PCNT quadrature | `encoderN` + `switchN` for the push | 5 |

**Two on I2C is deliberate, and is not the redundancy the rule warns about**, because the two ask
opposite questions of the same bus. The MPU6050 is the sensor the unmerged `GyroDriver` commit
already drives, so it is the *regression* check that the merged seam still does what its author
built it for: a 14-byte burst, comfortably inside a render tick. The VL53L8CX is the *stress* case:
128 bytes at up to 60 Hz, which a blocking read cannot absorb on the render path at all. The first
proves the seam works; the second proves where it stops working, which is the more valuable of the
two and the reason it is not optional here.

**A VL53L0X is on order (2026-09-01), arriving in about two weeks.** It restores the gentle step:
one distance from one register read, so the I2C seam is exercised by a second, easy device before
the zone grid tests where that seam stops working. Until it arrives, step 4 proves the seam with the
IMU alone; when it lands it slots in as step 4's second sensor, which also removes the ambiguity
noted in 4b (a failure there being either the merged seam or that sensor's own demands).

**Every one is tested twice**, which is the point of running the two threads together: once as a
compiled module, once as a MoonLive script doing the same job. A sensor that works only one way
means the seam is not as domain-neutral as it claims.

Not on the list, and why: an **ultrasonic** (HC-SR04) shares the encoder's pulse-timing seam, so it
adds bench time without proving a new primitive; a **light sensor** (BH1750) is a third I2C device;
a **USB gamepad** needs hardware none of the boards here have.

## Two paths into the light domain, not one

Steps 1 and 2 build the mapping table, which is right for **events**: a press, a remote code, an
encoder detent. Steps 4 and 5 add **streams**, and those must not go through the same path. A
distance an effect samples every frame is not a control a person edits, and routing it through
`setControl` would put a name lookup and a persistence dirty-flag on the render path at frame rate.

The pattern for a stream exists and is already domain-neutral: `AudioService` publishes an
`AudioFrame` that effects pull through a static `latestFrame()`, which
[the architecture](../explanation/architecture/moonmodule.md#data-exchange-between-modules) states as the shared-struct
pull. A sensor service does the same, and may *also* carry mapping rows for thresholds a person
cares about ("closer than 50 cm" drives a switch). One module, two outputs, because that is what a
sensor is: a stream, with events derivable from it.

So every sensor step below has two acceptance criteria, and both are on the bench: **an effect reads
the value per frame through the published frame**, and **a threshold drives a control through the
table**. A sensor that only does one is half-built. The full reasoning is in
[the input-mapping analysis](../design/input-mapping.md).

## Step 0: the decisions ✅

One service per input kind, infrared as pure mappings, a script holding the logic and reaching the surface only: [ButtonService](../../src/core/services/ButtonService.h), [InfraredService](../../src/core/services/InfraredService.h), [InputMapping](../../src/core/util/InputMapping.h), [the design](../design/input-mapping.md). One `MoonLiveService` holds one script, so one per sensor or one for several is the user's choice.

## Step 0b: the two open items carried in from the GPIO-seam plan ✅ (`/mm/pad/N` shipped; the driver/service line is drawn in mooncore.md, and `DriverBase.h` says a driver consumes the light output)

The driver and service line is drawn in [MoonCore](../explanation/architecture/mooncore.md); `/mm/pad/N` is in [the control surface](../reference/control-surface.md#the-osc-addresses).

## Step 1: rebuild Infrared and Button around a mapping list ✅

[ButtonService](../../src/core/services/ButtonService.h) and [InfraredService](../../src/core/services/InfraredService.h) hold rows on [InputMapping](../../src/core/util/InputMapping.h): kinds, momentary and latching, debounce, learn.

## Step 1b: an encoder that shows what it selects 🚧 (the Open Stage Control session has no strip widget; its label and pad-matrix gaps stay on [the backlog](#backlog))

Small, and it follows step 1 rather than joining it, so the rebuild is not also carrying a UI change.

Infrared's palette actions need a destination. There is no `LightsControl` module and there will not
be one: `palette` stays on `Drivers` because
that is where it is consumed, and the surface reaches into it, exactly as `fader1` already targets
`Drivers.brightness` and `switch1` targets `Drivers.on`.

- **`encoder1` targets `Drivers.palette`**, hardcoded for now. That matches the existing pair and the
  fact that per-control assignment is a later UI plus persistence job.
- **Wrap or clamp follows the bound control's type**: a palette ring wraps, a brightness clamps.

A `uint8_t` encoder covers 256 options, comfortably past the palette count. Worth stating as the
boundary, since a preset encoder would reach 64 slots and a script list could one day exceed 255.

### The display strip

Turning a knob through palettes has to read "Rainbow", not "37", and the place to show it is **one
shared display row, not a label per encoder**. That is how the hardware this surface mirrors works:
a strip of small text displays across the top, showing whatever was last touched.

- **One row, above the switches.** Declaration order is render order in `defineControls`, so the
  strip is one `addControl` before the switch loop. Above them because a channel reads
  display, then buttons, then knob, then fader on the desks this mirrors, and because it then sits
  with the numeric readouts rather than floating between banks.
- **Any control that has text can drive it.** Not an encoder feature: a select's option name, a
  preset's name, a script's status. The rule is "a surface control with a text form writes the
  strip", so a new bound control needs no display code.
- **It shows the LAST thing set.** No per-control cell to keep in sync, no question of what a stale
  cell means, and it matches what a performer wants: the thing they just touched.

**Hardware and Open Stage Control.** Some surfaces have these displays (the X-Touch's scribble
strips) and some do not, so the strip is published as a control like any other and a surface that
cannot show text simply ignores it. The shipped
[Open Stage Control session](../reference/examples/open-stage-control.json) gains a text widget
bound to it, which is also worth doing because that session already has known gaps (its widget labels
do not render and its pad matrix draws nothing, both recorded in
[the backlog](#backlog)): adding the strip is the moment to fix the session
by building one widget of each kind in its own editor and copying the shape it produces.

**Test:** the infrared remote's palette next/prev rows driving `Control.encoder1`, with the palette
NAME appearing in the strip and the lights following. That is the whole two-step model in one
observation: a remote code reaches the surface, the surface reaches Drivers, and the strip says what
happened.

## Step 2: MoonLive at the pins ✅

Builtins in [MoonLive](../moonmodules/light/moonlive.md) and [the platform page](../moonmodules/platform/index.md); the service is [MoonLiveService](../../src/core/services/MoonLiveService.h); the bench figures are in [performance](../reference/performance.md#moonlive-scripted-effect-tick-and-memory).

## Step 2b: encoders send deltas ✅

Superseded by 2c: a delta exists only at `turn()` ([ControlModule](../../src/core/system/ControlModule.h)), and `/mm/encoder/N` stays absolute ([the control surface](../reference/control-surface.md#the-osc-addresses)).

## Step 2c: the surface's own bindings become assignable ✅

Assign mode, what a surface drives and live positions: [ControlModule](../../src/core/system/ControlModule.h), [the control surface](../reference/control-surface.md), [Control.h](../../src/core/module/Control.h).

### Measured: the 1 Hz state push stays, and instant feedback needs a different mechanism ✅

The 1 Hz push stays, and a watched value moves through the targeted push: [HttpServerModule](../../src/core/system/HttpServerModule.h).

### ✅ Resolved: `addControl` is in the neutral builtins

`addControl` lives in the neutral builtins: [MoonLiveBuiltins_common.h](../../src/core/moonlive/MoonLiveBuiltins_common.h).

### A control written continuously ✅

Persistence of a control written every frame is bounded: [Persistence](../explanation/architecture/moonmodule.md#persistence), [FilesystemModule](../../src/core/system/FilesystemModule.h).

## Step 3: analog in, and the expression pedal 🚧 (bench: the ADC reads live on the Dig-2-Go and a script reads volts on four boards; a pot's full travel through `AnalogService` is untested)

An ADC read is the smallest new seam and unlocks a whole input class.

- Seam: `bool adcRead(uint8_t gpio, uint16_t& raw)`, plus the calibration ESP32 needs to turn a raw
  count into millivolts.
- Compiled: `AnalogService`, a list of rows mapping an ADC pin to a `faderN` with min/max/invert,
  because a pedal's usable travel is never the full range.
- MoonLive: `int adcRead(int pin)`.

**Test:** host tests with an injected value. Bench: a potentiometer on an ADC pin driving
`Control.fader1`, first through `AnalogService` and then through a three-line script, so both halves
are proven on the same hardware.

### SHIPPED (2026-09-02): host-verified, NOT bench-verified

- **Seam**: `adcRead(gpio, raw)` + `adcMaxCount()`, raw counts only. No millivolts: every consumer
  maps a travel to a range anyway, so a calibrated voltage would add per-chip machinery to serve a
  conversion the caller immediately undoes. ESP32 uses one kept ADC1 oneshot handle; **ADC1 only**,
  because ADC2 is shared with the WiFi radio and would read fine on the bench then fail once the
  device joined a network. Desktop reports an injected value (`setTestAdcValue`).
- **`AnalogService`**: rows of pin + `inMin`/`inMax`/`invert`, an exponential filter (`smoothing`)
  and a `deadband`, driving a target through the new `runInputLevel`.
- **`runInputLevel`** is separate from `runInputAction` rather than a parameter on it: an event
  carries no number and a level's value IS the reading, and a toggle or a delta driven at 50 Hz is
  not something a user can mean. It rescales 0..255 into the control's own bounds, so one pedal
  configuration works on any target.
- **MoonLive**: `adcRead(pin)` and `adcMax()`, so a script normalizes without a magic number.

**A bug the tests caught**: the integer exponential filter stops converging once the step truncates
to zero, so a pedal pushed fully down settled at 253 rather than 255 and full brightness was
unreachable. It now takes the remaining distance whole once the step rounds to nothing.

**Still open**: the bench half. A pot on an ADC pin, both paths on the same hardware. The SE16 and
LightCrafter sense pins are the natural rig and are now free ([the backlog](#backlog)), but the scale
factor per board needs the schematic.

## Step 4: I2C sensors 🚧 (only the shared bus and its scan, `I2cBusModule`; no register read, sensor module, published frame or `i2cRead` builtin)

The prerequisite is not new work: an unmerged commit (`11f8eb76`, "Add GyroDriver (MPU6050) +
generic platform I2C layer") already carries the seam, and this branch has only `i2cScan`, which
cannot read a register.

- **Merge that layer first**, verifying it against the current tree rather than assuming it applies:
  `i2cInit`, `i2cWriteReg`, `i2cReadRegs`. The commit also carries a working **MPU6050** module,
  which is on the bench, so the merge has its own regression test built in: if the gyro still reads,
  the seam survived the merge.
- Its `GyroDriver` moves from `Drivers` to `Services` while it lands. It reads a sensor and outputs
  no lights, so `Services` is its home under the split this analysis rests on, and `DriverBase`'s
  pure `setSourceBuffer` would oblige it to consume a light buffer it has no use for.
- MoonLive: `int i2cRead(int addr, int reg)`, `void i2cWrite(int addr, int reg, int val)`.
- The second device that would prove the seam generalizes is **step 4b**, since the only ToF on the
  bench is the zone grid. So this step ends with one sensor and one script, and the generalization
  claim is not yet made.

Each publishes a frame for effects to pull, following `AudioService`: a POD struct in a plain-data
header, a static accessor, overwritten in place. A `SensorFrame` shared by both is the first question
to answer when building it, since a distance and a tilt are different shapes; the alternative is one
per sensor, which is more headers and less coupling.

**MoonLive needs to read that stream too**, or a script can drive a pin but not react to a sensor.
The builtin is a read of the published frame (`distance()`, `tilt()`, or a general
`sensor(int which)`), which is the same shape as the audio builtins scripts already have.

**Test:** host tests for the register decode against a faked bus. Bench, on the Dig-2-Go's exposed
I2C pins (21/22): the MPU6050 reporting tilt, which is the regression check that the merged seam
still does what its author built it for. Both acceptance criteria: **an effect reacting to tilt every
frame** through the published frame, and **a threshold row** driving a switch past a tilt angle.
**Then the same sensor as a script** (init registers in `setup`, the burst read in `tick20ms`, the
value published for an effect to pull), which is the proof of the scripted-sensor claim: a user with
a datasheet and no toolchain gets a working sensor an effect can use.

## Step 4b: the sensor that produces an image ⏳

**Not optional, and not a follow-up.** The VL53L8CX is the only ToF on the bench, so it is both the
second I2C device (proving the seam generalizes) and the case that proves the seam is not enough.
Those were meant to be two sensors and are now one, which makes this step harder than it reads:
a failure here is ambiguous between "the merged seam is wrong" and "this sensor needs more than a
synchronous read". Establish the MPU6050 works first (step 4), so anything that breaks here belongs
to this sensor.

It reports an **8x8 zone grid at up to 60 Hz**, so it is a depth image rather than a distance. The
constraint is not bandwidth (128 B/frame is 7.7 kB/s) but the **blocking read**: 128 bytes over I2C
at 400 kHz is ~2.9 ms, against a 289 us render tick on the Dig-2-Go. So the synchronous
`i2cReadRegs` from step 4 is not sufficient here, and this step is where that becomes visible.

Three ways out, to be chosen with the measurement in hand: **SPI at 3 MHz** (which is why the part
offers it), a **ranging rate matched to what the effect needs** rather than the sensor's maximum, or
the read **moved off the render core** as the encode split already is. Measure first: the plan should
not guess which.

It also carries an **84 KB firmware blob** uploaded to the sensor at every boot (measured from ST's
driver: 86,017 bytes, 1.94 s over I2C at 400 kHz, 0.23 s over SPI at 3 MHz). That blob cannot be
written ourselves, since it is executable firmware for the sensor's own processor with no
register-level datasheet published. Two consequences: **SPI is preferred** for the boot upload as
well as the per-frame read, and the module is **compiled in per firmware** rather than into every
image, gated by the device catalog the way `MM_HLS` already is. 84 KB is roughly 4.5% of the ~1.8 MB app PARTITION (about 2% of a 4 MB chip's flash) and
matters on a 4 MB classic; it is noise on an S3 or P4.

The consumer is an effect or a modifier that maps zones onto lights, so the published frame is an
array rather than a value. That argues a frame type per sensor family (as `AudioFrame` is specific
to audio) rather than one general struct.

**MoonLive is in scope for this sensor, through a power function.** A script cannot upload the blob
and never should, but it does not need to: the compiled module owns the blob, the bus and the frame,
and the script reads a zone through a builtin the way it already calls `beat()` or `paletteRGB()`
without implementing either. So this step ships a builtin alongside the module:

```c
int tofZone(int x, int y);   // distance in mm for one of the 8x8 zones, 0 = no target
```

A script that lights the column nearest a hand is then a few lines, and the heavyweight sensor is as
scriptable as the light ones. The boundary the blob draws is narrower than it first looked: a script
cannot *be* the driver for such a sensor, but it can *use* one fully.

## Step 5: pulse timing, and what it unlocks ⏳

Two wanted inputs share one missing seam: an ultrasonic sensor's echo *width*, and a rotary
encoder's quadrature. Both are edge timing, and the textbook ESP32 answer for both is a hardware
peripheral (PCNT for the encoder, RMT or a timed capture for the echo) rather than a poll.

- Seam: pulse measurement, shaped by whichever of the two is built first.
- Compiled: `EncoderService` (a row is two pins plus an optional push pin, mapping to `encoderN`),
  and an ultrasonic distance module.
- MoonLive: `int pulseIn(int pin, int level, int timeoutUs)`, the Arduino name because it is the one
  every datasheet example uses.

**Test:** an encoder on the bench driving `Control.encoder1` through the module, and an HC-SR04
reporting distance through a script.

## Step 6: PIR, and the trigger vocabulary ✅

A PIR is a `set` row, which holds a level: [ButtonService](../../src/core/services/ButtonService.h), [the design](../design/input-mapping.md).

## Worth an analysis of its own: modules exchanging values over the network

**The question (product owner, 2026-09-02): AudioService syncs its frame between devices, so can any
module send and receive its values the same way?**

Worth taking seriously, and deliberately NOT folded into a step here, because it is a network
question rather than an input one. Audio sync is a single hardcoded packet (`parseWledAudioSync`, a
44-byte WLED v2 frame) carrying one struct between devices that both know what it means. Generalizing
that means a module publishing arbitrary state to a peer group and another subscribing to it, which
needs answers this plan has no reason to hold opinions about: what identifies a value across devices,
what happens when two devices publish the same one, how a subscriber behaves when the peer goes away,
and whether it is a broadcast or a subscription.

It also overlaps two things that already exist: `DevicesModule` already discovers peers and knows
what modules they run, and OSC and MQTT already carry control values off the device. So the honest
first question is whether this is a new transport at all, or a matter of pointing the existing ones
at each other.

It touches inputs at exactly one point, which is why it came up here: a sensor on one device driving
lights on another is the obvious use, and the two-step model already makes that expressible (a remote
device's surface is just another target). That is enough to note; the design belongs in its own
analysis alongside the backlog's existing network entries.

## Backlog

- **SE16 and LightCrafter power monitoring.** Both boards carry voltage and current sensors: the SE 16 V1's on GPIO 8 (voltage) and 9 (current), the LightCrafter's on [its page](../reference/hardware/lightcrafter-16.md#power-sensing). Open per board: `voltagePin` and `currentPin` in the two device definitions, "Power monitoring" from `planned` to `supported`, the SE16's scale factors (its divider ratio and shunt, from the schematic), and a reading checked against a meter. It gives the ADC seam a real bench rig, better than a hand-wired potentiometer.
- **The Open Stage Control session's labels.** The shipped session ([open-stage-control.json](../reference/examples/open-stage-control.json)) works for every fader, encoder and switch, but draws no widget names and an empty pad matrix. A session Open Stage Control saves itself carries no `label` key on a fader, so the reliable fix is to build one widget of each kind in its editor, save, and copy the shape. Cosmetic: only the labels and the pad grid are missing.
- **Double-click and multi-click on a button**, from ESPHome's trigger vocabulary: the same debounce timers with more states, so one button does several things. Built when a panel needs more actions than it has buttons.

## Not in this plan

- **USB game controllers**: a HID report-descriptor parser plus USB Host bring-up, S3/P4 only.
  Its own plan, and the mapping half is free once reports arrive.
- **I2S sensors**: the platform's I2S surface is `audioMicInit`/`audioMicRead`, named for its job.
  A non-audio I2S sensor would get its own seam named for its own job rather than generalizing that
  one. There is also a hardware limit: a classic ESP32 has two I2S controllers and the mic holds one.
- **Rewriting OSC**: it already drives the surface, which is the model this plan extends to physical
  inputs. Its one gap, the missing `/mm/pad/N` handler, is worth doing alongside step 1 since every
  input wants to fire a pad.

## Why the order

Each step is shippable alone and each unlocks the next. 1 fixes a defect in what already shipped.
2 makes scripts real, and every later step then has two implementations to test against. 3 is the
smallest new seam. 4 has its prerequisite already written by someone else. 5 is the most work
(hardware peripherals) so it comes after the pattern is established. 6 is nearly free.

The MoonLive thread is not a phase at the end: a builtin lands with the seam that makes it possible,
so a script is always the second implementation proving the seam is genuinely domain-neutral. If a
seam is awkward to expose as a builtin, that is a signal the seam is wrong.

# Plan: input mapping and scripted sensors

Turns [input-mapping-analysis.md](../future/input-mapping-analysis.md) into steps. Two threads
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

- [`docs/moonmodules/light/drivers.md:3`](../../moonmodules/light/drivers.md): "A driver sends
  lights somewhere." Output-only, light-specific.
- [`docs/explanation/architecture/index.md:143`](../../explanation/architecture/index.md): "producers vs consumers: producers generate
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
[the architecture](../../explanation/architecture/moonmodule.md#data-exchange-between-modules) states as the shared-struct
pull. A sensor service does the same, and may *also* carry mapping rows for thresholds a person
cares about ("closer than 50 cm" drives a switch). One module, two outputs, because that is what a
sensor is: a stream, with events derivable from it.

So every sensor step below has two acceptance criteria, and both are on the bench: **an effect reads
the value per frame through the published frame**, and **a threshold drives a control through the
table**. A sensor that only does one is half-built. The full reasoning is in
[input-mapping-analysis.md](../future/input-mapping-analysis.md).

## Step 0: the decisions

**Decided (2026-09-01), so step 1 is unblocked:**

1. **A service per input kind.** `ButtonService`, `InfraredService`, later `EncoderService` and
   `AnalogService`, each with its own mapping list whose columns all apply. A board's catalog entry
   adds the ones it has. More modules than one `InputService` with a `type` column, but every column
   on a card is real for that input, and the card can validate what it shows.
2. **The infrared service is pure mappings.** No compiled-in actions. The five familiar ones
   (on/off, brightness up/down, palette next/prev) ship as **default rows in the device catalog**, so
   a remote still works out of the box while nothing is fixed in firmware and a user can delete or
   rewrite any of them.

3. **A script defines input hardware, and a script is where the LOGIC goes** (product owner,
   2026-09-02). Once a script can read a pin and call `setControl`, a script IS an input module, and
   the relationship to `ButtonService` is the one effects already have: the compiled module is the
   fast, shipped path, the script is the flexible one for hardware nobody wrote a module for.

   **Not a JSON.** A JSON can express a MAPPING (this pin drives that control) and that is exactly
   what the mapping lists in step 1 already are. It cannot express a RULE ("if the distance sensor
   reads under 50 cm, run preset 3"), and a JSON that grows conditionals, comparisons and sequencing
   becomes a programming language with none of the tools of one. So the declarative half stays the
   lists, and MoonLive is where an `if` belongs. The two compose: a script writes the surface, the
   surface is what the lists already target.

4. **One script per sensor, or one script for all of them: both, with no new mechanism.** A
   `MoonLiveService` instance holds one script, so one instance per sensor type is the tidy default
   and one instance driving several is a script with more members. This is the same choice a user
   already has with effects, and it needs no `type` column or registry to support.

**Still open, and it gates step 2 rather than step 1:**

5. **What `setControl` may reach from a script.** Recommendation: **the `Control` module only**. A
   script drives the surface, the surface drives everything, which is the same two-step model the
   tables use. Unrestricted is simpler and lets a script rewrite a driver's pin list.

## Step 0b: the two open items carried in from the GPIO-seam plan

That plan had seven steps. Five shipped (the GPIO seam, ButtonService, the Dig-2-Go catalog entry,
the MoonLive GPIO builtins and MoonLiveService). Two did not, and they are small enough to state
rather than schedule:

- **The driver definition in the docs.** `drivers.md:3` still reads "A driver sends lights
  somewhere", the output-only wording the analysis above set out to correct. The correction is two
  edits and no code: scope that line to a LIGHT driver, and name the general sense plus its two
  families in `architecture.md`. It is cheap, and every other document defers to it.
- **`/mm/pad/N` in `OscModule`.** Every input can fire a pad now (the pad target resolves through the
  generic pad-grid list), but OSC has no path for one, so a surface cannot. Decide what a press means
  on an empty slot and whether a nonzero value is press-versus-hold.

## Step 1: rebuild Infrared and Button around a mapping list

**This is the first work, and it is a rebuild rather than an extension.** Both modules are wrong in
the same way, and both are new enough on this branch to change without a migration: `ButtonService`
maps one pin to one target, and `IrService` carries five compiled-in actions whose identity is the
firmware's opinion. Neither survives a board with three buttons or a remote with twenty keys.

**`IrService` is renamed `InfraredService`.** "IR" is an abbreviation the code does not need: the
project's own standard is the textbook name a new contributor reads without expanding it
([CLAUDE.md, industry standards](../../../CLAUDE.md)), and every sibling is already spelled out
(`AudioService`, `ButtonService`, `FilesystemModule`). The rename touches the class, the file, the
factory registration, the catalog entries that add it per board, and its spec anchor. Cheap now, and
it stops the abbreviation spreading to the doc, the UI card and the device catalog.

Both then grow **a list of mappings** via `Control::addList` + `addListRow`, the mechanism presets,
devices, pins and tasks already use:

- A **button row**, as built: `pin`, `activeLow`, and the shared action (`target`, `kind`, `value`).
  `kind` is toggle / set / delta, where `set` is the momentary one (writes while held, clears on
  release) and covers a foot pedal. No long-press: it needs a held-timer state machine
  `ButtonService::pollRow` does not have, so it is step 6's trigger vocabulary rather than step 1's.
  A board with three buttons is three rows, which is the whole point.
- An **infrared row**: `code` (learned), plus the same shared action. Learning binds the
  next received code to the row being learned, so a remote's twenty keys are twenty rows a user adds
  rather than five the firmware chose.
- `target` is a `Module.control` string, so `Control.switch1` (the recommended two-step path) and
  `Drivers.on` (direct) are the same mechanism with no special case.

The single-target controls that shipped (`ButtonService.target`, `IrService`'s five `code …`
controls) are **removed, not deprecated**: a one-row list is the same thing, and this branch is the
only place they have ever existed.

**MoonLive in this step:** nothing new. The GPIO builtins arrive in step 2; here a script cannot yet
read a pin, so the table is the only path. Stated so the step is not held up waiting for it.

**Test:** host tests for the row parsing and the debounce state machine,
and a test that the rename left no `IrService` behind. Bench: the Dig-2-Go's GPIO 0 button on one row
and the infrared remote on another, both driving `Control.switch1`, so two inputs reaching one target
is proven rather than assumed.

## Step 1b: an encoder that shows what it selects

Small, and it follows step 1 rather than joining it, so the rebuild is not also carrying a UI change.

Infrared's palette actions need a destination. There is no `LightsControl` module and there will not
be one ([backlog-mixed.md](../future/backlog-mixed.md)): `palette` stays on `Drivers` because
that is where it is consumed, and the surface reaches into it, exactly as `fader1` already targets
`Drivers.brightness` and `switch1` targets `Drivers.on`.

- **`encoder1` targets `Drivers.palette`**, hardcoded for now. That matches the existing pair and the
  backlog's own note that per-control assignment is a later UI plus persistence job
  ([power-functions-analysis-top-down.md](../future/power-functions-analysis-top-down.md)).
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
[Open Stage Control session](../../reference/examples/open-stage-control.json) gains a text widget
bound to it, which is also worth doing because that session already has known gaps (its widget labels
do not render and its pad matrix draws nothing, both recorded in
[backlog-core.md](../future/backlog-core.md)): adding the strip is the moment to fix the session
by building one widget of each kind in its own editor and copying the shape it produces.

**Test:** the infrared remote's palette next/prev rows driving `Control.encoder1`, with the palette
NAME appearing in the strip and the lights following. That is the whole two-step model in one
observation: a remote code reaches the surface, the surface reaches Drivers, and the strip says what
happened.

## Step 2: MoonLive at the pins

The GPIO seam shipped (`gpioInputBegin` / `gpioRead` / `gpioWrite`). This exposes it to scripts and
adds the host module that makes a scripted service addable.

- Builtins: `int gpioRead(int pin)`, `void gpioWrite(int pin, int on)`. One row each in
  `MoonLiveBuiltins_light.h` plus a host function.
- `setControl` as a builtin, scoped per step 0's decision.
- **`MoonLiveService`**: the service twin of `MoonLiveEffect`. A `script` control, the compile and
  status path, picker integration. Mostly a copy of the existing binding.

**The hello-world**, which is also the test:

```c
class ButtonScript {
  int pin = 0;
  int last = 0;
  void defineControls() { addControl("pin", pin, 0, 48); }
  void tick20ms() {
    int now = gpioRead(pin);
    if (now != last) { last = now; setControl("Control", "switch1", now); }
  }
}
```

**Test:** host tests for the builtins through the desktop seam (`setTestGpioLevel` injects a level,
the script reads it). Bench: the script above on the Dig-2-Go toggling the lights from the physical
button, doing in eight lines what `ButtonService` does in a module. That comparison is the point:
**the compiled module and the script are interchangeable**, which is the relationship effects
already have.

**And the rule, not just the mapping.** The step is only finished when a script can express what a
list cannot, which is the reason scripts are in this plan at all (step 0, decision 3). The second
hello-world is a CONDITION driving an action:

```c
class NearTrigger {
  int pin = 0;
  int threshold = 50;
  int wasNear = 0;
  int near = 0;
  void defineControls() {
    addControl("pin", pin, 0, 48);
    addControl("threshold", threshold, 0, 255);
  }
  void tick20ms() {
    int distance = gpioRead(pin) * 100;   // step 4 replaces this with an I2C distance read
    near = 0;
    if (distance < threshold) { near = 1; }
    if (near != wasNear) { wasNear = near; if (near) setControl("Control", "pad3", 1); }
  }
}
```

A mapping row cannot say "under 50 cm", cannot hold the edge state that stops it firing every tick,
and cannot pick a different pad by time of day. That is the whole argument for a script over a
richer JSON, and this test is what proves it rather than asserting it. Until step 4 lands the I2C
read, the same shape is tested with the button standing in for the sensor: `gpioRead` gives 0 or 1
and the script scales it, so the comparison against `threshold` is the one the real sensor will use
and only the source of the number changes.

### Step 2 status (2026-09-02): built and host-verified, NOT bench-verified

Shipped: the `gpioRead` / `gpioWrite` / `setControl` builtins, a `.mls` extension with its template
and catalog entry, `MoonLiveService` registered under Services, and `unit_MoonLiveService.cpp`
pinning the whole path (a script reads an injected pin level and drives `switch1`, declares its own
controls, and survives a missing or broken script).

**BENCH-VERIFIED (2026-09-02, QuinLED Dig-2-Go, 300 LEDs on RmtLed).** `sweep.mls` compiled to 752
bytes of Xtensa, published its declared `bpm` control, and swept faders 5-8 continuously with no
boot loop and no crash. Measured on the board:

| | desktop | Dig-2-Go |
|---|---|---|
| script tick | 58 us | **966 us** |
| for scale | | Drivers 6887 us, Effects 209 us |
| frame rate | | **67-85 fps with and without the script** |

The tick is 17x the host's, which is the number worth knowing before a script does anything heavier.
It is still only about 12% of what the LED driver costs on the same frame, and the frame rate moved
no more with the script running than it does between two samples without it. So a scripted service
is affordable at this size on a classic ESP32.

Still open, and the step is not done until they are:
- **The `.mls` picker.** The extension map and syntax highlighting are wired; nobody has confirmed
  the picker offers `.mls` files or that "new script" writes `kServiceTemplate`.
- **The catalog card.** `services.md` has no MoonLiveService section, and `@card
  MoonLiveService.png` names a file that does not exist.

Two things this step found, which the text above predates:

- **The hello-world in this plan does not compile as written.** MoonLive has no local variables, so
  `int now = gpioRead(pin)` is a parse error; the value has to be a member. And `setControl` takes
  two arguments (`"switch1"`, value), not three: the module is fixed to `Control` by step 0's
  decision, so naming it at the call site would offer a choice that does not exist. The shipped
  `kServiceTemplate` is the corrected form.
- **`MoonLive::run()` refuses a call with no light buffer** (`!buf || nLights == 0 || cpl < 3`),
  which is right for an effect and wrong as the engine's only entry: a service paints nothing, and
  the guard rejected it SILENTLY, so the script compiled, reported its size and never ran a line.
  `MoonLiveService` calls `runValue` instead, which invokes the same emitted block and asks only for
  the arena. Correct but oblique: the main entry still carries a light-domain assumption, and the
  next non-light binding meets it the same way. Worth splitting the guard out of `run()`, or naming
  the two entries for what they are.

## Step 2b: encoders send DELTAS, the way an encoder actually works

A rotary encoder has no end stops. It reports MOVEMENT, not position: one detent clockwise is "+1",
and the device it is plugged into owns the value and decides what +1 means. That is what every
control surface does, and it is what Mackie Control (the protocol this surface already follows)
sends: a signed-bit delta per detent, not a position. The informal name is an ENDLESS encoder; the
formal one is continuous rotation, and the message it sends is a RELATIVE (or incremental) value,
against a potentiometer's ABSOLUTE one.

**Ours are absolute today, and that is the problem.** `encoderN` is a `uint8_t` 0..255 in
`ControlModule`, and the surface accumulates detents into it (`nudgeEncoder`). So the encoder holds
a SECOND copy of whatever it targets, and the second copy has to be kept in step with the first:
`followTargets` pulls every encoder from its target once a second, `mirrorOne` pushes changes back
out, and `sentEncoders_` remembers what each attached surface was last told.

That mirroring is not free, and it is visibly lossy at the edges:

- **A wide target does not fit.** `getControl` answers in surface units, so a Uint16 holding 300
  reads back as 255. The encoder's copy is then wrong, and the next turn computes from the wrong
  base. `getControlWide` fixed the ACTION path; the mirror still clamps because a byte is what the
  surface speaks.
- **Wrap versus clamp belongs to the target, not the knob.** A palette ring should wrap from the
  last palette to the first; brightness should stop at 255. With the value living on the encoder,
  the surface has to know which, for a target it is only loosely bound to.
- **Nothing to sync is simpler than syncing.** `followTargets`, `pullTarget`, `mirrorOne` and
  `sentEncoders_` all exist to keep two numbers equal. A relative encoder has one number.

**The change:** an encoder carries no value. A detent applies `+1` or `-1` to whatever it targets,
through the same `runInputAction` delta path a mapping row already uses, and the target's own type
and bounds decide the result. The surface stops holding a copy, so there is nothing to pull, nothing
to mirror, and no clamp.

- `encoderN` becomes a control the transports WRITE a delta to rather than a value: a positive
  number steps up, a negative one steps down, and reading it back answers nothing meaningful.
- The seven-segment readout under each encoder shows the TARGET's value, not the encoder's, which is
  what a desk shows and what a user is actually asking about.
- The display strip is unaffected: it already names the target and its value.

**Two transports send absolute positions today** and have to send deltas instead: OSC's
`/mm/encoder/N` and the WLED bridge, plus whatever a MIDI surface would send (Mackie's signed bit is
the encoding to follow). Both are small, and both get simpler: a delta needs no scaling from the
target's range into 0..255.

**Test:** an encoder stepping a Uint16 target past 255 (which the absolute form could not do), a
palette ring wrapping while brightness clamps at the same detent, and the readout following the
target rather than the knob. Bench: the Dig-2-Go's encoder1 on the palette, turned a full revolution
in each direction, with the strip naming each palette as it passes.

## Step 2c: the surface's own bindings become assignable

`fader1` drives `Drivers.brightness`, `switch1` drives `Drivers.on` and `encoder1` drives
`Drivers.palette`, hardcoded in `ControlModule::surfaceTarget` and its siblings. Everything around
them is already general: the write goes through `Scheduler::setControl`, the read-back through
`getControl`, and `followTargets` keeps the surface showing what its targets actually hold. Only the
NAMES are fixed, and this replaces them with an assignment a user makes.

**The target is the string the inputs already speak.** `"Module.control"`, the same form a button or
infrared row stores, resolved by the same `composeTarget` / `decomposeTarget` helpers. A surface
control and a mapping row then name a target one way rather than two, and a target typed into either
means the same thing.

**Any control the REST API can set** (product owner, 2026-09-02). Not a curated allow-list per
module: the dropdown is generated from the LIVE module tree, so it can only ever offer what exists,
and a control added tomorrow is assignable without anyone remembering to list it. The pairing is
exactly what `/api/control` takes, which is the point: there is no second vocabulary to keep in step
with the first.

**Two-way, which is not new work.** `Scheduler::getControl` is the documented mirror of `setControl`,
and `followTargets` already pulls each surface control from its target once a second so a change made
from the web UI, MQTT or OSC moves the fader. A user-assigned target uses the identical path. (Step
2b removed that pull for ENCODERS on the grounds that an endless knob has no position to correct.
With assignable targets the surface has to show what its knob drives, so the pull comes back and step
2b's premise is worth revisiting: see the note below.)

**Persisted with the Control module**, as configuration, in its own JSON and restored at boot, like
every other control value. Deliberately NOT captured in light presets: a pad that silently re-maps
the whole desk mid-set is a surprise a performer cannot recover from, and a preset is about a LOOK.

### The UI

The popup already exists and is already reachable on a touch screen: `attachTargetPopup` opens on
right-click and on long-press, and today it shows one read-only line ("drives Drivers.palette"). The
change is what that line becomes:

- **Two dropdowns**, module then control, not a typed string. A typo in `"Drivres.palette"` is
  invisible until the fader silently does nothing, which is the same reasoning that made the infrared
  row's target a dropdown rather than a text box.
- **The control list follows the module choice**, filtered to what a surface can actually drive: a
  fader cannot meaningfully move a filename, and offering one produces a control that looks assigned
  and does nothing.
- **A clear button**, because unassigning has to be as easy as assigning.
- **ONE way in: an assign MODE**, which is what Ableton, Bitwig, Reaper, TouchOSC and Open Stage
  Control all do. An `assign` button on the surface's card turns it on; every fader, knob and switch
  outlines in the accent color; a single tap on one opens its picker; the button reads `done` to
  turn it off.

**One mechanism, not several** (product owner, 2026-09-02). The alternatives were tried and each
fails somewhere: right-click has no touch equivalent, long-press has no mouse equivalent, and
double-click COLLIDES with the control itself, since double-clicking a fader also moves it. A mode
has no such conflict, because while it is on a tap means exactly one thing, and it is identical on a
mouse and a touch screen.

The mode also has to suppress the control's own gestures while it is on: the knob's drag and wheel,
and the fader's range input, are muted so the tap that opens a picker cannot also move the value it
is about to re-target.

**Test:** assigning `fader2` to a second module's control and driving it; a target changed from the
web UI moving the surface control within a second (the follow path); an assignment surviving a
reboot; and clearing an assignment leaving the control inert rather than still driving its old
target. Bench: the Dig-2-Go, with a fader assigned to something other than brightness.

### The encoder question this reopens

The product owner's framing (2026-09-02): **the HARDWARE sends deltas, and everything we display is
the absolute result.** A rotary encoder, an X-Touch, a Mackie surface: each reports movement, and the
device turns that into a value once, at the boundary. `applyEncoderDelta` is exactly that boundary
and already works.

Step 2b went further and made the SOFTWARE knob relative too, which put the delta on the wire between
the browser and the device, where both sides hold the absolute value and neither needs one. The cost
shows up as a UI knob that can read 106 while its target reads 59. Worth revisiting once this step
lands, because assignable targets need the surface to show what its control drives, which is the
mirroring step 2b removed.

### Measured: the 1 Hz state push stays, and instant feedback needs a different mechanism

The surface shows a value changed by something else (a script, OSC, another client) up to a second
late, because the UI learns it from the 1 Hz state patch. The obvious fix is a faster push, and the
measurement says no (desktop, 2026-09-02, a full pipeline plus a sweeping script):

- **460 us average** per patch, **8.5 ms worst case**, ~600 bytes, 14 leaves changed.

At 1 Hz that is 0.05% of a core. At 50 Hz the average alone is 23 ms of render budget per second,
and the 8.5 ms worst case is most of what a 20 ms tick has left once the light pipeline has run, so
a spike lands as a visible hitch rather than as headroom. On an ESP32 both numbers are several times
larger, which is exactly the LED stutter that made this 1 Hz in the first place
(HttpServerModule.h, the state-push note).

The 34 KB full serialize that originally forced the rate IS gone, replaced by the value-diff. What
costs now is the diff itself: it walks every leaf, serializes it and hashes it, so the cost scales
with the number of controls rather than with what changed.

**So instant feedback is a targeted push, not a faster one** (product owner, 2026-09-02: push the
change rather than poll for it). A surface control that changes sends its own leaf immediately,
which is one value rather than a walk of all of them, and the periodic patch stays at 1 Hz as the
catch-up for everything else.

The mechanism to copy is already there: `MoonModule::setSchemaChangedHook` is a static hook the
HTTP server installs, and a value hook takes the same shape. What makes it a feature rather than a
line:

- **Coalescing.** A script sweeping a fader at 50 Hz must not become 50 pushes a second: the whole
  point of the measurement above is that the render thread cannot afford that. A pending-set drained
  on tick20ms is the shape, so a burst collapses to one push per 20 ms.
- **Baseline bookkeeping.** The periodic patch compares against a cached hash per leaf. A pushed
  leaf has to update that baseline, or the next patch sends the same value again.
- **Which controls.** Only the ones a person watches move (the surface), not every control on the
  device: a driver's telemetry changing 50 times a second is exactly what the 1 Hz sampling is for.

A user's own action already feels instant, since the UI updates locally on send. What this buys is
the case where something ELSE moved the control: a script, OSC, a second browser.

### ✅ Open: `addControl` is still in the light header

Resolved 2026-10-06: `addControl`, its sink and `runDefineControls` live in `MoonLiveBuiltins_common.h`, registered once for both tables, and the per-thread slot claim is `core/util/ThreadSlot.h`, which the light sinks share.


The domain-neutral builtins moved to `core/moonlive/MoonLiveBuiltins_common.h` (2026-09-02): the
math, the waveforms, noise, randomness and print. `addControl` did NOT, so the service table still
includes the light header for that one function, which leaves core depending on a domain for the
sake of declaring a setting.

It stayed behind because it is not a pure function: it writes through an `AddControlSink`, a
per-thread slot table that also serves `addLight` and the draw canvas, and moving that machinery is
a bigger job than moving thirteen pure functions was. `smin` went back to the light table for the
same class of reason, and correctly: it wraps `draw::smin`, so it IS a light idea.

Worth finishing when the sink table is touched for another reason. The include is honest about why
it is there.

### Open: a control written continuously should not be persisted at that rate

Found while testing a scripted sweep (2026-09-02): a `.mls` service running on tick20ms and writing
four faders makes 200 `setControl` calls a second, and every one of them ends in `markDirty()` plus
`noteDirty()`, because that is what the generic control-change reaction does for any writer.

**Nothing is written to flash, and that is an accident rather than a design.** The debounce waits
two seconds after the LAST dirty mark, so a 50 Hz writer re-stamps the timer before it ever expires.
Measured: with the sweep running, `ControlModule.json` sat untouched for 35 minutes; the moment the
sweep stopped and one fader moved, it was written within seconds.

So there is no flash churn today, but the shape is wrong in both directions. A continuously written
control **never persists at all**, so a power cut loses it (the reboot handler's `flushPending()` is
the only thing that saves it, and a power cut does not call it). And the protection depends on the
write rate staying above the debounce: a script writing every three seconds would rewrite the file
every three seconds, forever.

The cost measured on the host is small (~30us per tick, about 0.15% of a core at 50 Hz).

**Measured on an ESP32-P4 (2026-09-02, MM-P4 at .139, esp32p4rev1-eth).** The scripted sweep costs
**58us per tick** for its four `setControl` calls, so **14.5us per call** including
`rebuildControls()` + `markDirty()` + `noteDirty()`: 2.9 ms/s, **0.29% of one core** at 50 Hz, about
2x the host figure. The render tick was unaffected (fps 482, tick 2071us, unchanged from baseline).

**The starvation is confirmed on hardware, not just inferred.** With the sweep running, `lastSaved`
climbed 20s, 40s, 1m, 2m, 3m, 4m across a three-minute sample and never reset: the file is never
written at all while a 50 Hz writer runs.

So the cost is NOT the problem and no optimization is called for; the shape is. What needs fixing is
that a continuously written control never persists (a power cut loses it) while a slower writer would
rewrite the file forever.

**The product owner's framing** (2026-09-02): a script should not be responsible for the pace, the
system should manage it. Two things follow, and both belong in the persistence layer rather than in
every script:

- **Rate-limit the persistence, not the value.** A surface control moving at 50 Hz needs its value
  applied 50 times a second and saved at most once. The dirty mark is what wants a ceiling.
- **Ask whether a swept value is configuration at all.** A fader position a script is driving is live
  state, closer to a sensor reading than to a setting a user chose. Something declared that way would
  not mark dirty in the first place, which also settles the question above.

### SHIPPED (2026-09-02): both halves, and the second one was necessary

BOTH were needed, and finding out why is the useful part of this entry.

**`ControlDescriptor::live`** answers the second question: a surface position MIRRORS whatever it is
assigned to, and that target persists in its own module, so saving the position stored the same fact
twice and let the two disagree on load. The three position banks (switches, encoders, faders) are
declared live; the ASSIGNMENTS still persist, and an unassigned control starts at 0, which is what it
means. Live controls are excluded from the saved file and never mark their module dirty.

**That alone did not fix the starvation**, which is the finding. Marking a surface control live stops
IT from marking dirty, but the moment a user assigns it to something (a fader driving
`Drivers.brightness`, two clicks in the UI) the write lands on an ordinary persisted control and the
50 Hz stream of dirty marks resumes ONE MODULE DOWNSTREAM. Measured on the P4: an unrelated setting
changed while a sweep ran was still unsaved 56 seconds later. So the first question needed answering
too, in the mechanism rather than per control.

**`FilesystemModule::MAX_DEFER_MS` (10 s)** is that ceiling: `noteDirty` stamps the ceiling clock
only on the FIRST mark of a pending save, so later marks move the debounce without extending the
wait without end. The debounce still coalesces a burst; it can no longer be starved. Verified on the
P4 with a swept fader assigned to `Drivers.brightness`: `lastSaved` now cycles 0-7 s where it
previously climbed past a minute.

Cost measured before the fix: 14.5us per `setControl` on the P4, 0.29% of a core at 50 Hz, render
tick unaffected. Small enough that no optimization was called for, which is why the work went into
the shape rather than the speed.

## Step 3: analog in, and the expression pedal

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
LightCrafter sense pins are the natural rig and are now free (see `backlog-light.md`), but the scale
factor per board needs the schematic.

## Step 4: I2C sensors

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

## Step 4b: the sensor that produces an image

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
and the script reads a zone through a builtin the way it already calls `beat()` or `paletteR()`
without implementing either. So this step ships a builtin alongside the module:

```c
int tofZone(int x, int y);   // distance in mm for one of the 8x8 zones, 0 = no target
```

A script that lights the column nearest a hand is then a few lines, and the heavyweight sensor is as
scriptable as the light ones. The boundary the blob draws is narrower than it first looked: a script
cannot *be* the driver for such a sensor, but it can *use* one fully.

## Step 5: pulse timing, and what it unlocks

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

## Step 6: PIR, and the trigger vocabulary

The cheap finish. A PIR is a digital pin, so it needs no seam at all: it is a `ButtonService` row
whose event is a *level* rather than a press. MoonLight models it exactly so (`pin_PIR`,
"HIGH = lights on"). Add `level` to the event set, and PIR is done.

If the trigger vocabulary is going further (double-click, multi-click, from ESPHome's set), this is
where it lands: the same timers, more states, and it multiplies what a three-button panel can do.

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

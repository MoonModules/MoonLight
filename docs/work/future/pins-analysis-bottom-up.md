# Pin manager — bottom-up analysis

A design study for a MoonLight **PinsModule**, built the bottom-up way: survey how the field solves GPIO assignment, ownership, conflict, MCU-validity, and display — then extract the *ideas* to write our own fresh against MoonLight's `ControlType::Pin` model, per [*Industry standards, our own code*](../../CLAUDE.md#principles) (learn from the prior art, credit it here, don't depend on or trace any one tool). The top-down companion (design from the goal, written after and not consulting this) will follow at `pins-analysis-top-down.md`.

The question the module answers: **which module owns each GPIO, for what role, and is that assignment valid + conflict-free** — the *ownership/coordination* map. A separate, optional *live-state* view ("is GPIO18 high right now") is a different axis (see GPIOViewer below); this study is primarily about ownership, with live-state noted as a distinct future layer.

## The field

### MoonLight — `ModuleIO.h` (the direct lineage, the richest prior art)

MoonModules' own predecessor ([MoonLight/src/MoonBase/Modules/ModuleIO.h](https://github.com/MoonModules/projectMM/blob/main/src/MoonBase/Modules/ModuleIO.h)) — the closest prior art by far, and the one to study hardest since MoonLight descends from it. It is **board-preset-centric**, not runtime-allocation-centric, and it already unifies *both* axes this study separates (ownership + live state) in one module:

- **A pin is a row in a JSON pin table**, one per physical GPIO (`0..GPIO_PIN_COUNT-1`, all pre-populated), with fields `{GPIO, usage, index, summary, Level, DriveCap}`. Not a C++ struct — a `JsonObject`. The **`usage`** field is an `enum IO_PinUsageEnum` (~56 values: `pin_LED`, `pin_I2C_SDA`, `pin_ETH_MDC`, `pin_PIR`, `pin_Relay_LightsOn`, …) — a **central role vocabulary** that both names what the pin does *and* implicitly encodes its owner (one usage = one feature). Multiple pins may share a usage (`pin_LED` ×N); `PinAssigner` auto-increments the `index` field to disambiguate them.
- **Ownership = the `usage` enum**, read back into cached members (`_pinI2CSDA = pinObject["GPIO"]` when `usage == pin_I2C_SDA`). No separate owner-tag table — the role vocabulary *is* the ownership model.
- **No runtime conflict detection.** Safety comes from **curated board presets** (`setBoardPresetDefaults(boardName)` for 20+ boards: QuinLED, Serg, MHC, SE, Atom, …) that assign non-overlapping pins; `PinAssigner` tracks only `lastUsage`/`argCounter` for index bumping, not conflicts. A `modded` flag records whether the user hand-edited (true) vs. runs the clean preset (false); clearing it reloads the preset.
- **Rich live-state telemetry (the second axis, already built):** `readPins()` + `loop1s()` read `gpio_get_level()` → "HIGH"/"LOW", `gpio_get_drive_capability()` → "WEAK…STRONGEST", validity via `GPIO_IS_VALID_GPIO`/`GPIO_IS_VALID_OUTPUT_GPIO`/`rtc_gpio_is_valid_gpio`, and **ADC** (`analogReadMilliVolts` with dynamic attenuation for battery/voltage/current). Capability emojis: ✅ valid, 💡 output, ⏰ RTC, 🔌 I2C.
- **`readPins()` is also a hardware-init hub** — it brings up I2C (`Wire.begin`), Ethernet (`ETH.begin` via a settings service, with `ethernetType`/`ethPhyAddr`/`ethClkMode`), RS485 (`uart_*`), and ADC from the assigned pins. So the module *applies* the pin map, not just displays it.
- **UI:** a **board-preset selector** (`selectFile`), the **editable pin table** (`rows`: GPIO read-only, `usage` dropdown, `index`, and read-only `summary`/`Level`/`DriveCap`), an **I2C bus-scanner** table, and `switch1`/`switch2` toggles for alternate configs (e.g. Ethernet-vs-IR on a shared pin).
- **Deferred-apply pattern:** a UI edit sets `_newBoardPresetPending`; `loop20ms()` applies it next tick (avoids reentrancy) — the same off-the-edit-path discipline MoonLight uses.

**Take — the most reusable thing is WHAT ModuleIO reports per pin.** Independent of its central-manager structure, ModuleIO's **per-pin report is a ready-made field list for MoonLight's map** — the columns worth surfacing for every GPIO:

| ModuleIO field | What it reports | Source call |
|---|---|---|
| **GPIO** | the physical pin number (the row key) | — |
| **usage** | the role/owner (LED, I2C_SDA, ETH_MDC, …) | `IO_PinUsageEnum` |
| **Level** | live logic level, "HIGH" / "LOW" | `gpio_get_level()` |
| **DriveCap** | output drive strength, "WEAK…STRONGEST" | `gpio_get_drive_capability()` |
| **summary** | a human one-liner combining the above + capability flags | composed |
| **capability flags** | ✅ valid GPIO · 💡 output-capable · ⏰ RTC · 🔌 I2C | `GPIO_IS_VALID_GPIO` / `GPIO_IS_VALID_OUTPUT_GPIO` / `rtc_gpio_is_valid_gpio` |
| **ADC (mV)** | analog reading for sense pins (battery/voltage/current) | `analogReadMilliVolts()` + dynamic attenuation |

That set — **role + live level + drive strength + per-pin capability flags + optional ADC** — is exactly what a MoonLight pin map should show per GPIO, and it's the concrete lesson to carry (the *validity/capability flags* especially: they'd have named the GPIO-46-is-a-strap loopback problem instantly). ModuleIO's other traits — a **central JSON pin table**, `assignPin`/**board-preset assignment**, **hardware-init from the map** (I2C/Eth/RS485/ADC brought up in `readPins()`), and a `modded`/user-edited flag — are the *central-manager* mechanism, and ModuleIO has **no runtime conflict detection** (safety rests on preset curation). **Whether MoonLight adopts that central-manager structure or inverts it (each module owning its own pins, a central module only coordinating) is the top-down's call, not this survey's** — flagged as the key design fork, since MoonLight's `ControlType::Pin` controls + deviceModel catalog already model pins and presets differently.

### WLED — `PinManager` (the runtime-authority prior art)

WLED (and WLED-MM, [mm.kno.wled.ge/usermods/pinmanager](https://mm.kno.wled.ge/usermods/pinmanager/); [pin_manager.cpp](https://github.com/Aircoookie/WLED/blob/main/wled00/pin_manager.cpp)) has a **centralized runtime authority**, not just per-field config — the axis ModuleIO leaves to presets:

- **`allocatePin(pin, output, ownerTag)` / `allocateMultiplePins(...)` / `deallocatePin(pin, ownerTag)`.** Every consumer (LED output, a usermod, the button, I2C/SPI) *requests* a pin from the manager and *releases* it. The manager is the single source of truth for "who has what."
- **Owner tags.** Each allocation records *which* subsystem owns the pin (an enum: `PinOwner::Button`, `PinOwner::UM_*`, …). A conflict message names the owner: "GPIO N already allocated by owner X."
- **MCU-validity gate.** `isPinOk(pin, output)` rejects a pin that can't do the requested direction on this chip (input-only pins for output, flash pins, etc.) *before* allocation.
- **Boot-time conflict report** to Serial: an overview of allocated pins + conflicts after boot.
- **Known weak spot (instructive):** usermod-allocated pins weren't reflected in the UI's GPIO dropdowns — pins allocated by the manager still appeared "free" to pick ([#4070](https://github.com/Aircoookie/WLED/issues/4070), fixed in [#4071](https://github.com/wled/WLED/pull/4071)). *Lesson: the allocation authority and the UI pin-picker must read the same source, or the UI offers pins that are already taken.*

**Take:** the allocate/deallocate + owner-tag + validity-gate triad is the right *runtime* model. Its weak spot warns us: **one source of truth the UI also consults.**

### Tasmota — template-driven, component-per-pin

Tasmota ([Templates](https://tasmota.github.io/docs/Templates/), [Components](https://tasmota.github.io/docs/Components/)) is **declarative config, not a runtime allocator**:

- A **template** is a fixed-length array: one slot per physical GPIO, each slot holding a **component code** (Relay1, Button2, DHT11, …). Assignment is "this GPIO *is* this component."
- The web UI is **a dropdown per GPIO** — pick the component role for each pin from the full component list (`Gpios 255` lists them).
- **Conflict handling is structural**: one component code per GPIO slot (you can't put two components on one pin — the array holds one), and *sequential numbering* enforced per type (Button1, Button2, no gaps).
- **Base + override**: you pick a base module (#18 = generic) and override per-pin.

**Take:** the **GPIO-indexed view** (a row *per physical pin*, showing what it does) is the display model users recognise — Device-Manager-like. And the **role-per-pin** (not just "used/free" but "used *as a relay*/*as a button*") is richer than a boolean. Tasmota can't have two owners per pin *by construction* (one array slot) — a cleaner conflict story than a runtime allocator, but only because it's static.

### ESPHome — pin schema + reuse validation

ESPHome ([pin schema](https://esphome.io/guides/configuration-types/), [pin-reuse validation](https://iot-devices.com.ua/en/about-changes-in-esphome-configuration-validation-pin-reuse-validation-en/)) validates **at config-compile time**:

- A **`pin:` schema** each consumer includes: `number`, `inverted`, `mode` (input/output/pullup), plus escape hatches. Pins are declared *by the component that uses them*, with a rich per-pin config, not a central array.
- **Pin-reuse validation** (v2023.12+): using the same pin in two components is a **compile error** by default — the killer feature. `allow_other_uses: true` is the explicit opt-out for a legitimately-shared pin (and it auto-disables interrupts to make sharing safe).
- **Reserved-pin validation**: flash/PSRAM/strap pins raise an error; `ignore_pin_validation_error: true` overrides for a board that frees a normally-reserved pin.
- **Shared-pin semantics**: when a pin *is* shared (opt-in), the framework changes behaviour (polling not interrupts) to make it work.

**Take:** three ideas worth stealing — (1) **conflict is an error by default, with an explicit opt-out** (not silent, not a hard block — `allow_other_uses`); (2) **reserved/strap validation with a per-board override** (`ignore_pin_validation_error`) — exactly our gpio-usage.md knowledge, made actionable; (3) **the pin carries config beyond its number** (inverted, mode) — a pin is a small struct, not just an int.

### GPIOViewer — the live-state axis (a different question)

[GPIOViewer](https://github.com/thelastoutpostworkshop/gpio_viewer) answers *"what is each pin doing electrically right now"*, not *"who owns it"*:

- **Polls** pin state (~100 ms, configurable) — Digital / Analog / PWM / ADC — and animates it on an **interactive board diagram** matching the ESP32 model (with a generic fallback).
- **Skips peripheral-owned pins** (I2C/SPI/UART) by default — an *implicit* ownership notion (it knows a bus owns some pins) but doesn't surface *which module* owns them.
- ~50 KB; web assets loaded externally.

**Take:** this is the **dynamic view**, orthogonal to ownership. Two ideas: (1) the **board-diagram visualisation** (pin position on the physical header, not just a list) is the most intuitive display when it matters; (2) polling live state is cheap and off-hot-path. But it doesn't answer ownership, and its external-asset + board-image-per-model model is heavy — reverse-engineer the *idea* (live per-pin state), write our own.

**Live pin state is a TESTING tool, not just a UI toy — this is the MoonLight-specific reframe.** MoonLight already leans hard on hardware self-verification, and live pin state feeds directly into two existing mechanisms:

- **HAL / loopback driver tests.** The LED drivers already do an on-board RMT-RX loopback to prove the output byte-stream (see [backlog-light § LED drivers](backlog-light.md)); a live pin-state view is the *human-facing* counterpart — watch a GPIO actually toggle while a driver runs, confirm the lane is wired where the config says, catch a dead/mis-wired lane a green unit test can't. The [leddriver top-down](../../work/future/leddriver-analysis-top-down.md) argues the real flicker/correctness proof is watching the *actual pin* under load; live pin state is that, surfaced.
- **The mic-health diagnostic** (shipped today: "no samples" = clocks dead / "data line silent" = SD dead). That diagnosis is *inferred* from the sample stream. Live pin state on the mic's SCK/WS/SD would let a user *see* which line is toggling — the exact "which wire is at fault" answer, made direct rather than inferred. The mic debug that cost an afternoon (a strap-pin misread) would have been a glance: SD not toggling → wrong pin.

So live pin state is **shared infrastructure for the test framework**, not a cosmetic layer. It's still a *distinct axis* from the ownership map (different question, different data source), and still deferred to its own effort — but it earns its place as a testing/bring-up tool, which raises its priority above "optional polish." The top-down should treat it as a first-class (if separately-built) sibling of the ownership map, wired into the HAL-test story.

### Native APIs (the floor)

Arduino-ESP32 / ESP-IDF give **no central pin registry** — `pinMode`/`gpio_config` claim a pin with no arbitration; last-writer-wins, conflicts are silent. This is *why* WLED built PinManager and ESPHome built reuse-validation on top: the platform doesn't coordinate, so the framework must.

## What MoonLight already has

- **`ControlType::Pin`** ([Control.h](../../src/core/module/Control.h)) — a pin *is* its own control type, clamped to the chip's real GPIO ceiling (`MM_MAX_GPIO`, build-injected per target from `CONFIG_SOC_GPIO_PIN_COUNT`). So MoonLight already models a pin as a first-class, per-chip-bounded value — ahead of "just an int."
- **[gpio-usage.md](../../reference/hardware/gpio-usage.md)** — the per-MCU reserved / strap / role-conflict knowledge, hand-curated. The *data* ESPHome's reserved-pin validation would need; not yet wired to any check.
- **The pin-uniqueness backlog item** ([backlog-core § Pin-uniqueness](backlog-core.md#pin-uniqueness-check-across-modules-prevents-conflicts-replaces-a-singleton-hack)) — already specs *enumerate every `Pin` control, a value seen twice is a conflict*. That's the WLED allocate-check re-expressed against our control model.
- **No central authority today** — no `allocatePin`, no owner tracking, no conflict gate, no reserved-pin check. Each module sets its `Pin` controls independently; nothing arbitrates. **This is the gap.**

## Ideas extracted (for the top-down to design against)

| Idea | Source | MoonLight shape |
|---|---|---|
| **A role vocabulary per pin** (named `usage`, owner-implied) | MoonLight ModuleIO `IO_PinUsageEnum` | Derive the role from the owning control's *name* (`sckPin`→BCLK, `pins`→LED lane) — the same "one usage names role + owner" idea, but read from our controls, no separate enum table. |
| **Live-state telemetry is worth building** (level, drive-cap, validity, ADC) | MoonLight ModuleIO `readPins`/`loop1s` | The live-state axis (§6 of the top-down): `gpio_get_level`, `gpio_get_drive_capability`, `GPIO_IS_VALID_*`, ADC — behind a `platform::` seam. ModuleIO proves it's mainstream, not gold-plating. |
| **The pin module can double as the hardware-init hub** | MoonLight ModuleIO `readPins()` brings up I2C/Eth/RS485/ADC | Possible later convergence: MoonLight inits per-module today, but a map that *applies* claims (not just shows them) is the ModuleIO end-state to weigh. |
| **Board presets over runtime allocation** (curated non-conflicting sets) | MoonLight ModuleIO `setBoardPresetDefaults` + `modded` flag | MoonLight's deviceModel **catalog** already IS the preset; the `modded`/preset-reload idea maps to "catalog inject vs. user edits." |
| **Single source of truth for pin ownership** | WLED PinManager | Enumerate the tree's `ControlType::Pin` values — the controls *are* the registry; no parallel allocation table. |
| **UI pin-picker reads the same source** | WLED bug #4070 | The pin dropdown/validation and the ownership map must both read the live `Pin` controls — never diverge. |
| **Owner + role, not just used/free** | Tasmota component-per-pin, WLED owner tag | Each claimed pin shows *owning module* + *role* (from the control's name: `sckPin`→BCLK, `pins`→LED lane). |
| **GPIO-indexed display** (row per physical pin) | Tasmota, GPIOViewer diagram | The map is keyed by GPIO number (what Device Manager shows), not by module — "GPIO18 → RmtLed lane 0". |
| **Conflict = flagged by default, explicit opt-out** | ESPHome `allow_other_uses` | Reject/warn a double-claim; a deliberate shared-pin opt-out for the rare legitimate case. |
| **Reserved/strap validation + per-board override** | ESPHome `ignore_pin_validation_error` | Cross-check a claim against gpio-usage.md; flag a strap/reserved pin, board can override. |
| **A pin is a small struct** (mode/inverted), not an int | ESPHome pin schema | Future: `ControlType::Pin` could carry mode/invert if a consumer needs it; today number-only is enough. |
| **Live electrical state — a HAL-testing tool** | GPIOViewer | A separate axis from ownership, but *not* mere polish: feeds the loopback/HAL driver tests and the mic-health diagnostic (see-the-wire-toggle). Poll + optional board diagram, built as its own effort, wired into the test framework. |
| **The platform won't coordinate — the framework must** | native APIs | MoonLight owns arbitration; don't expect the ESP-IDF to. |

## Scope signal for the top-down

The convergent core across every serious tool is **one authority that knows who owns each pin, validates it against the chip, and flags conflicts — and that the UI pin-picker consults the same authority.** MoonLight is well-placed: the `Pin` controls already *are* the registry (no parallel table to keep in sync — the WLED bug can't happen if the map reads the controls directly), and gpio-usage.md is the validity data. The module is therefore mostly a **reader + validator over the existing `Pin` controls**, plus the conflict gate the pin-uniqueness item already scoped — not a new allocation subsystem. The live-state (GPIOViewer-style) view is explicitly a separate, later, optional axis.

The top-down study will design from the goal — *coordinate GPIO assignment across a live, user-editable module tree* — and decide the reader/validator/authority shape and the conflict UX (reject-on-add vs soft-flag-on-edit, already an open question in the pin-uniqueness item). It should treat **live pin state as a first-class sibling axis** (built separately, but designed in) because of its testing value: it plugs into the HAL/loopback driver tests and the mic-health diagnostic as a *see-the-wire* verification tool, not just a display. Two axes, one module theme: *what owns each pin* (ownership map) and *what is each pin doing* (live state, for testing + bring-up).

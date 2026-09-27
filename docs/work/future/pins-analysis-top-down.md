# Pin manager — top-down analysis

The design counterpart to [pins-analysis-bottom-up.md](pins-analysis-bottom-up.md) (the field survey). The bottom-up ended with a scope signal; this designs from the goal — **coordinate GPIO assignment across a live, user-editable module tree** — down to the module shape, the conflict UX, and the two-axis split. It is a study, not a plan: the shippable increments and their order are named at the end, each getting its own `/plan` when picked.

> **Status (2026-07-09): the pin manager is shipped and complete.** Increments #1–#4 landed (read-only ownership map, reserved/strap flagging, conflict soft-flag, live-state dir/level/drive), plus the [release-on-disable](../../work/past/lessons.md) hardware half. #5 (a reassignment broker) is **obviated** by the soft-flag choice — live pin swaps already work without it (see §8.5). The one open remainder is an optional strict reject-on-add mode, backlogged separately if wanted. This study is kept as the design record.

## TL;DR

- **The pin registry already exists** — it's the tree's `ControlType::Pin` controls. A `PinsModule` is a **reader + validator** over them, not a new allocation subsystem (no parallel table, so WLED bug #4070 — picker and allocator diverging — is structurally impossible here).
- **Two axes, one theme.** *Ownership* (who claims each GPIO, for what role) and *live state* (what each GPIO is doing right now). The ownership map is the read-only phase-1 module; live state is a separate, later, testing-focused axis designed in from the start.
- **The authority is the pin-uniqueness check**, promoted from "scattered across two HTTP sites" to "one thing the map owns and both the add-path and the UI picker consult." That's the phase-2 increment, not phase-1.
- **Validity data is `gpio-usage.md`** — the reserved/strap/role-conflict knowledge, wired to a check for the first time.
- **Phase-1 is genuinely small**: enumerate `Pin` controls → group by GPIO → render a `ListSource` map, refreshed on `loop1s`. The [System-Modules shape](system-modules.md) (TasksModule template) already exists to hang it on.

## 1. The goal, stated precisely

A running MoonLight device has GPIO claims declared **all over its module tree** — `RmtLedDriver.pins`, `AudioService.sckPin/wsPin/sdPin`, `NetworkModule.ethMdcGpio/…`, `I2cScanModule.sda/scl`, `IrService.pin` — each set independently, nothing arbitrating. The goal is a single surface that answers three questions a bench operator and the firmware both need:

1. **Who owns GPIO N?** — the ownership map (which module, which role).
2. **Is any GPIO claimed twice?** — the conflict gate (two modules, one pin = broken output or driver-error spam).
3. **What is GPIO N physically doing?** — live electrical state (high / PWM / analog), for HAL/loopback test verification and mic-health bring-up.

(1) and (2) are the *ownership* axis; (3) is the *live-state* axis. The design keeps them separate but sibling.

## 1a. The inversion — the load-bearing design decision (vs. MoonLight's ModuleIO)

The bottom-up's richest prior art is MoonLight's [`ModuleIO.h`](pins-analysis-bottom-up.md) — MoonModules' own predecessor — and its structure is the one thing MoonLight most deliberately **does NOT copy**. ModuleIO is a **central pin manager**: one module owns a JSON pin table, *assigns* GPIOs to features via board presets, and *brings up the hardware* (I2C/Eth/RS485/ADC) from that table. Ownership flows **outward** — from one authority to the features.

**MoonLight inverts this. Each module owns and manages its own pins**, and the central module only *coordinates*:

- **The `AudioService` owns** its `sckPin`/`wsPin`/`sdPin` and inits its own I²S; **the `RmtLedDriver` owns** its `pins` and inits its own RMT channels; **the `NetworkModule` owns** its `ethMdcGpio`/… and brings up its own PHY. Each module is the single authority over — and the initialiser of — its own hardware, via its own `ControlType::Pin` controls. Ownership flows **inward**: the modules hold it, the coordinator reads it.
- **The PinsModule is a coordinator/observer, never an owner.** It *reads* the pins the modules already declare, *validates* them (chip-validity, strap/reserved, cross-module conflicts), and *renders* the coordinated map. It does not assign a pin, does not hold a pin table, does not init any hardware. It is a **lens over distributed ownership**.

This inversion is *why* the module is small and why the earlier "starts ahead" points hold: there is **no parallel pin table to keep in sync** (the modules' `Pin` controls already ARE the registry — so ModuleIO's central JSON array, `PinAssigner`, and preset-assignment machinery are all unnecessary), and **no hardware-init-from-the-map** (each module already inits itself; the deviceModel catalog already carries the board's pin values, ModuleIO's "board preset" equivalent). What MoonLight *adds* over ModuleIO is the one thing ModuleIO lacks — a **conflict/validity gate** — but even that only *validates* the distributed claims; it never centralises ownership of them.

So from ModuleIO MoonLight **carries the ideas, rejects the structure**: adopt the role vocabulary (derived from each control's *name*, not a central `usage` enum), the live-state telemetry (§6), and the deferred-apply discipline; reject the central pin table, `assignPin`/preset-assignment, and init-from-map — all of which assume the central-manager model this inverts. Everything below designs against the *coordinator-over-distributed-ownership* model, not a central manager.

## 2. Why MoonLight starts ahead

The bottom-up survey's convergent core — *one authority that knows who owns each pin, validates it against the chip, and flags conflicts, and the UI pin-picker consults the same authority* — is where every serious tool (WLED PinManager, Tasmota templates, ESPHome pin schema) lands. MoonLight already has the pieces the others had to build:

| The tool's mechanism | MoonLight's existing equivalent |
|---|---|
| WLED `PinManager` allocation table | The `ControlType::Pin` controls **are** the registry — no parallel table to sync. |
| A pin clamped to the chip's GPIO count | `ControlType::Pin` is already clamped to `MM_MAX_GPIO` (build-injected per target from `CONFIG_SOC_GPIO_PIN_COUNT`). |
| ESPHome reserved/strap validation data | [`gpio-usage.md`](../../reference/hardware/gpio-usage.md) — hand-curated per-MCU reserved/strap/role-conflict knowledge. |
| The pin-conflict check itself | Already specced: the [pin-uniqueness item](backlog-core.md#pin-uniqueness-check-across-modules-prevents-conflicts-replaces-a-singleton-hack) — *enumerate every `Pin` control, a value seen twice is a conflict.* |

So the module is **mostly a view + a validator over data that already exists**, which is why phase 1 is small. The one thing missing entirely is the *authority* — no `allocatePin`, no owner tracking, no conflict gate wired in. That's the gap this closes, in stages.

## 3. The ownership map (phase 1 — read-only)

### Shape

A **fixed System module** ([the Tasks/I2cScan pattern](system-modules.md)): a read-only `ControlType::List` via the `ListSource` adapter, refreshed on `loop1s()` (a periodic sample, never the hot path), always present, wired-by-code — same as [TasksModule](../../moonmodules/core/system.md#tasks). It reads the live tree, so it needs no state of its own.

### The enumeration

Walk the module tree; for every control of `ControlType::Pin` (and the list-pin text controls like `RmtLedDriver.pins` = `"18,19,20"`), record a claim: **`{gpio, owningModule, controlName}`**. A `Pin` value of `-1` is *unused* (the standard sentinel) and skipped; any other value is a claim. This is the exact mechanism the pin-uniqueness item already needs — the map and the check read the same thing.

### The row = a GPIO (Device-Manager keying)

Key the map by **physical GPIO number**, not by module — the row is `GPIO18 → RmtLed (lane 0)`, matching what an OS Device Manager / Tasmota template / GPIOViewer diagram shows. The per-row field set is taken almost directly from **MoonLight ModuleIO's per-pin report** (the bottom-up's reusable lesson) — the columns a pin map should show for each GPIO:

- the **GPIO number** (row key),
- the **owning module** (`RmtLed`, `Audio`, `Network`, …) — from the enumeration; ModuleIO's owner, but read from our controls, not a central table,
- the **role**, derived from the *control name* (`sckPin`→BCLK, `wsPin`→WS, `sdPin`→data, `pins`→LED lane, `ethMdcGpio`→MDC, `sda`/`scl`→I²C) — MoonLight's equivalent of ModuleIO's `usage` enum, but name-derived instead of a central vocabulary,
- **capability flags** (ModuleIO's ✅/💡/⏰/🔌) — *valid GPIO / output-capable / RTC / I2C-capable*, from `GPIO_IS_VALID_GPIO` / `GPIO_IS_VALID_OUTPUT_GPIO` / `rtc_gpio_is_valid_gpio` behind the platform seam. **These are high-value**: an output role claimed on an input-only pin, or a driven role on a strap, is visible at a glance — the exact class of bug the GPIO-46 loopback corruption was,
- a **flag** if the pin is reserved/strap per `gpio-usage.md` (see §5), or **claimed twice** (see §4).

The **live-state columns** (Level, DriveCap, ADC — the rest of ModuleIO's report) are the §6 axis, added when the `platform::gpioRead` seam lands; phase 1 shows the static columns (owner/role/capability) that need no per-tick read.

Unclaimed GPIOs need not be listed (or a compact "free: …" summary), the way Tasmota shows assigned rows, not all 40.

### What phase 1 is NOT

No arbitration, no reassignment, no writing. It *shows* the picture the pin-uniqueness check computes; it does not own or enforce it. That authority is phase 2. Keeping phase 1 read-only is what makes it a small, safe first increment (the same staging the [PinsModule backlog entry](backlog-core.md#pinsmodule-strict-reject-on-add-mode-the-one-remaining-increment) already draws).

## 4. The conflict authority (phase 2)

Today two modules can claim the same GPIO and nothing stops it (`RmtLedDriver.pins="18"` twice, or two mics on the same `wsPin`) — at best garbage output, at worst endless `i2s_new_channel` driver-error spam. **The same collision happens *within* one module too:** the i80 `LcdLedDriver`'s WR (`clockPin`, default 10) and DC (`dcPin`, default 11) must be distinct from its 8 data lanes, but a data-lane list like `pins="18,5,6,7,8,9,10,11"` silently overlaps the defaults — lanes on 10/11 then carry the bus's clock/DC toggling instead of LED data, and nothing flags it (observed on the S3 bench, 2026-07-12). The check must enumerate a driver's own `clockPin`/`dcPin` alongside its `pins` list, so an intra-module overlap is caught the same way a cross-module one is. The [pin-uniqueness check](backlog-core.md#pin-uniqueness-check-across-modules-prevents-conflicts-replaces-a-singleton-hack) currently scatters this logic across `POST /api/modules` + `POST /api/control`. Phase 2 promotes the PinsModule to **the one authority both consult**: it owns the live pin map (which it already computes for the display), so the add-path and the control-set path ask *it* "is GPIO N free?" instead of each recomputing.

**Conflict UX** (an open question the pin-uniqueness item already flags — resolve here): two shapes, and the design should pick per the *robustness-to-any-input* principle:

- **reject-on-add** — the API refuses a module-add / control-set that double-claims. Clean, but can wedge a legitimate live *swap* (two drivers exchanging pins needs a transient double-claim through a free intermediate).
- **soft-flag-on-edit** — the claim lands, the map flags it red, output for the conflicting pins is suppressed until resolved. Never wedges; matches "degraded is acceptable, crashed is not."

**Recommendation: soft-flag as the default** (robustness), with the map making the conflict loud, plus an ESPHome-style **explicit shared-pin opt-out** (`allow_other_uses`) for the rare legitimate case (two consumers reading one input). Reject-on-add stays available for the installer/catalog path where a clean tree is wanted.

**Later still**: live pin *reassignment* — the "swap two drivers' pins" case the uniqueness item flags as needing a free intermediate; a coordinator can broker the swap. And it pairs with the shipped "disabling releases resources" work so a disabled module's pins show as freed.

## 5. Validity: wiring `gpio-usage.md` to a check

`gpio-usage.md` holds the per-MCU reserved (flash/PSRAM), strap (boot-mode), and role-conflict (JTAG/UART0) pins — the data ESPHome's reserved-pin validation needs, **not yet wired to anything**. Phase 1 can already *flag* a claim on a reserved/strap pin in the map (advisory, non-blocking — the [ESPHome `ignore_pin_validation_error`](pins-analysis-bottom-up.md) posture: warn by default, board can override). This is exactly the class of bug the recent bench work surfaced live — a loopback jumper on GPIO 46 (a P4 boot strap) that *silently corrupts the signal*; a strap-pin flag in the map would have named it immediately. The data-side task: give `gpio-usage.md` a machine-readable form (or a generated table) the module can load, rather than re-hardcoding the strap lists in C++.

## 6. Live state — the second axis (separate, later, testing-first)

*What is GPIO N doing right now* is a **different question** from ownership, and the bottom-up is emphatic it's **not mere polish** — it's a HAL-testing tool. The two prior-art sources combine here: **MoonLight ModuleIO** reports the *static per-pin electrical facts*, **GPIOViewer** reports the *live per-pin activity*, and MoonLight's version should surface **both, unified per GPIO**.

### The full per-pin info set (ModuleIO ⊕ GPIOViewer)

| Field | From | What it tells a bench operator / a test | HAL-testing use |
|---|---|---|---|
| **level** (HIGH/LOW) | ModuleIO `gpio_get_level` + GPIOViewer live | is the pin actually toggling, or stuck? | the see-the-wire check: a driver's output pin must toggle when it renders; a mic clock (SCK/WS) must toggle when the mic runs |
| **drive capability** (WEAK…STRONGEST) | ModuleIO `gpio_get_drive_capability` | is the pin driving hard enough for the wire/level-shifter? | explains signal-integrity failures — a WEAK pin into a long strip or a shield's series resistor under-drives |
| **valid GPIO / output-capable** (✅/💡) | ModuleIO `GPIO_IS_VALID_*` | can this pin *be* an output at all? | catches an output role on an input-only pin *before* the driver silently fails |
| **RTC / I2C-capable** (⏰/🔌) | ModuleIO `rtc_gpio_is_valid_gpio` | special-function capability | picking pins for I²C / low-power roles |
| **ADC (mV)** | ModuleIO `analogReadMilliVolts` | analog reading on sense pins | battery/current sense, and reading an *analog* loopback level rather than a digital one |
| **continuity probe** (drive tx, read rx) | MoonLight loopback `loopbackJumperOk` + GPIOViewer-style live read | is a jumper actually bridging two pins, and does the signal reach? | **the exact tool that cracked the P4-shield loopback** — see below |
| **live activity** (toggle rate / recent edges) | GPIOViewer | is the pin *active* (PWM-ing, clocking) vs. idle? | a running LED driver's pin shows activity; a dead one doesn't — separates firmware-idle from wire-fault |

### Why this set is HAL-testing, not decoration — a worked example

The live-pin data *is* the HAL diagnostic. Concrete case from the bench (2026-07-09): the P4 loopback self-test kept failing on the MHC-WLED shield's 4x-In/Out header. A **continuity probe** (drive Tx GPIO high, read Rx GPIO — exactly the field above) logged **`hi=0 lo=0` on 47↔48, 46↔47, and 48↔47** — the Rx pin never saw the driven level *in any direction*, with the jumper physically seated. That single per-pin reading proved the header is **not a direct-GPIO path** (series resistors / a level-shifter's unidirectional output side), not a firmware bug — turning a multi-hour guess into a one-line fact. A pin map that surfaced *level + drive-cap + a continuity probe* would have shown this immediately: "pin 47 driven high, pin 48 reads low → no continuity → this header can't loopback." This is the whole argument for the live axis: **it converts invisible wire/board faults into visible per-pin readings**, feeding:

- the **loopback / HAL driver self-tests** (the loopback proves the *peripheral* emits correct bits; the live-pin view proves the *pin/wire* actually carries them — complementary, and the continuity probe is a first-class member of the live axis),
- the **mic-health diagnostic** — the see-the-wire-toggle check that separates a real I²S signal from a floating/strap pin faking RMS (the exact failure the mic bring-up hit),
- **pin-choice validation** (§9) — "is 47 output-capable and not a strap?" answered from the same reads.

### Shape

Poll pin state behind a `platform::` seam (domain-neutral, per the boundary rule — no direct GPIO read outside `src/platform/`): `gpioRead(level)`, `gpioDriveCap`, the validity/capability queries, and the **continuity probe** already implemented as `loopbackJumperOk`. Surface as live columns on the ownership map (or a sibling live-view module) — plus, later, a board diagram (§9). Built as its **own effort** (the `platform::gpioRead`/`gpioMode`/probe seam doesn't all exist yet), but designed in now so the ownership map and the live view are one module theme, not bolted together.

**Reverse-engineer GPIOViewer, don't depend on it** — study the idea (live GPIO web view, board diagram, polling cadence), then write MoonLight's own against the `Pin`-control + `platform::` model, per [*Industry standards, our own code*](../../CLAUDE.md#principles). Combine it with ModuleIO's static field set (above) so the map reports *both* what a pin **is** (validity/drive/role) and what it's **doing** (level/activity/continuity).

## 7. Testing architecture

- **Host (CI):** the enumeration + role-derivation + conflict detection are pure functions over a module tree — fully host-testable with a fake tree (a couple of modules with `Pin` controls, assert the map rows, the role labels, and that a double-claim flags). The `gpio-usage.md` strap/reserved cross-check is a pure lookup — host-testable against a fixture table. This is where most of the module is proven, no hardware.
- **Scenario:** add/remove/reconfigure pin-holding modules while the map updates — the robustness rule (any add/delete order keeps the map correct), same shape as the existing mutation scenarios.
- **Hardware:** only the *live-state* axis needs a board (the `platform::gpioRead` seam) — the ownership axis is entirely host + scenario. This keeps the bulk of the work CI-proof and the hardware surface minimal.

## 7a. Stretch goal — per-board pinout logic that *drives decisions*

A **stretched-goal** axis, flagged because it's the natural end-state of everything above, not the near-term work. Today's per-pin knowledge is generic-per-chip (`gpio-usage.md`: this MCU's straps/flash/reserved). But every session's real pin work has been **per-board** and **decision-driving**: *where do we put the mic on the Olimex Gateway?* (answer required knowing the Gateway's CON1 exposes 5,6,7,8,9,10,11,16,17,32-39, that 6-11 are the flash bus, that 34-39 are input-only) and *where does the loopback Rx/Tx go on the P4 shield?* (required knowing the shield's 4x-In/Out header is 46/47/2/48, that 46 is a strap, and — the punchline — that the header isn't even a direct-GPIO path). That analysis was done **by hand, from the board's pinout diagram**, every time.

**The idea: encode the pinout-diagram *logic* per board** — a machine-readable board pin map (exposed headers, which GPIO on which physical pad, what each pin is committed to on *this* board: flash, PSRAM, PHY, codec, LED bank, level-shifter, strap) — so the module can *answer the placement question* instead of a human re-deriving it from a JPEG. Concretely it would let the map (and a future picker) say:

- **"a mic needs SCK+WS output pins + an SD input; on this Olimex the free output-capable exposed pins are 16/32/33, SD can go on input-only 34-39"** — the exact reasoning done manually for the Olimex mic.
- **"a loopback needs a driven Tx and a readable Rx, both direct-GPIO; on this P4 shield the 4x-In/Out header isn't direct (proven `hi=0 lo=0`), so use base-board pins X/Y"** — the exact reasoning for the P4 loopback.
- **"you're about to claim GPIO 46 as a driven output, but it's a strap on this board — pick another"** — proactive, board-aware.

**Where the data comes from** (each a level of ambition):
1. **The deviceModel catalog already encodes some of it** — the board's `pins`/`ethMdcGpio`/mic pins *are* "what's committed on this board." The map could infer "committed" from the catalog + the live tree, for free.
2. **`gpio-usage.md` per-board sections** — extend the per-MCU tables with per-board committed/exposed sets (the Olimex CON1 list, the P4-NANO clear set, the shield header quirks) in machine-readable form. This is the hand-knowledge that already lives in prose + the bench-setup memory.
3. **The board photo/pinout as structured data** — the true stretch: a per-board pin table derived from (or checked against) the pinout diagram, so "exposed on which header, direct vs. level-shifted, physical position" is queryable. This is what would let the module *place* things, and it's where the board images the installer already carries could gain a data twin.

**Why it's a stretch, not a phase:** it needs a per-board data model that doesn't exist (the catalog is close but not complete — it says what's *used*, not what's *free* or *exposed-but-committed*), and the "direct-GPIO vs. buffered-header" fact (the thing that actually decided the P4 loopback) isn't in any datasheet — it's board-schematic knowledge. So it's a **direction**, seeded by the two axes above: once the map knows a pin's *capability* (§6) and *ownership* (§3), adding *per-board exposure + commitment* is the third layer that turns "show me the pins" into "tell me where to put the next thing." Design the ownership/live axes so this can bolt on (a per-board data source the map consults), don't build it yet.

## 8. Increments (each its own `/plan` when picked)

1. **Ownership map (read-only)** — the phase-1 module: enumerate `Pin` controls → GPIO-keyed `ListSource` map with owner + name-derived role. Host + scenario tested. The concrete-first foundation; ships alone.
2. **Reserved/strap flagging** — machine-readable `gpio-usage.md` + the advisory flag in the map. Small, high-value (catches the strap-pin class of bug, e.g. the GPIO-46 loopback corruption).
3. **Conflict authority** — the pin-uniqueness check moves into the module; both the add-path and the UI picker consult it. Soft-flag default + explicit shared-pin opt-out. This is where the [pin-uniqueness backlog item](backlog-core.md#pin-uniqueness-check-across-modules-prevents-conflicts-replaces-a-singleton-hack) lands.
4. **Live-state view** — the second axis: `platform::gpioRead`/`gpioMode` seam + a live column / board diagram, wired into the HAL/loopback tests and the mic-health diagnostic. Its own effort (needs the new platform seam + hardware proof).
5. **Live reassignment / broker** — ~~swap two drivers' pins through a free intermediate; pairs with disabling-releases-resources~~. **Obviated by the phase-3 soft-flag choice — not built.** The "free intermediate / broker" only exists to work around **reject-on-add** (where the API refuses a transient double-claim, so A↔B can't swap directly). Since phase 3 chose **soft-flag** (a pin change always lands; the map flags a transient conflict red), a user can already swap two drivers' pins live with no broker: set A→B's pin (transient red), then B→A's pin (red clears). Proven on hardware (S3, 2026-07-09: RmtLed 18↔Audio 21 swapped live, no reboot). Together the read-only map (#1–#4) + soft-flag (#3) + [release-on-disable](../../work/past/lessons.md) *are* live reassignment. The only non-redundant remainder is an **optional reject-on-add mode** for the installer/catalog path (a strict "clean tree" — add-path *validation*, a different feature than reassignment); backlog that separately if a strict mode is wanted.

## Scope guard

Phase 1 is a **reader over existing `Pin` controls** — resist making it an allocation subsystem (the controls are already the registry). Keep the two axes separate: *ownership* (what owns each pin) is the small read-only start; *live state* (what each pin is doing) is the testing-first sibling built later. Don't wire the conflict authority into phase 1 — it's phase 3, and forcing it early turns a small map into a policy engine. The module theme is [System Modules](system-modules.md) (Tasks/Memory/Pins), so it inherits that shape rather than inventing one.

# System modules — how System is organised, and the System / Services split

A design note (not a plan) settling how the fixed modules that hang under **System** — **Tasks** (shipped), **Memory** and **Pins** (proposed) — fit together, and the **System → System + Services** split they force. No code follows from this note directly; it's the shared frame the individual specs/plans are written against, so Memory/Pins land consistent with Tasks instead of three bespoke modules that merely look alike.

These are System's **child modules**: fixed, always present, each inspecting (and, over time, *managing* — pin reassignment, task relocation) one facet of the device. Not "views" — they may act — and not "Services" (that's the separate user-added container defined below); they're the modules *of* System.

## The industry reference

They are MoonLight's **Task Manager / Activity Monitor / Device Manager** — the OS system-management surfaces. Those aren't read-only either (Task Manager ends a process, Device Manager disables a device), which is why "views" undersells them. The mapping (*Industry standards, our own code* — study the tools, build our own):

| Windows Task Manager / macOS Activity Monitor / Device Manager | MoonLight | State |
|---|---|---|
| Processes / Details (per-process CPU, memory) | **Tasks** — RTOS tasks, modules nested, per-module cost | shipped |
| Performance → Memory (`free`, `vmstat`: used/free by type, largest block) | **Memory** — internal vs PSRAM, used/free/largest, per-module `dynamicBytes` | proposed |
| Device Manager (what hardware is present + how it's wired) | **Pins** — the GPIO map, who owns each pin | [backlogged](backlog-core.md#pinsmodule-strict-reject-on-add-mode-the-one-remaining-increment) |

The load-bearing lesson from those tools: **they sample existing OS accounting cheaply and always-on; the heavy per-event tracking (UMDH, `malloc_history`, Valgrind, ESP-IDF `heap_trace`) is a separate opt-in profiler.** MoonLight already learned this on TasksModule (CPU% is ~5% tick → build-flag opt-in; the task list is a cheap sample → always-on). The same tier applies to Memory (below).

## The shared pattern (so the System modules are one shape, not three)

Every member is the **same recognizable shape** — the [TasksModule](../../moonmodules/core/system.md#tasks) template, reusing primitives that already exist:

- a **read-only** module presenting a `ControlType::List` via the `ListSource` adapter (DevicesModule/I2cScan shape);
- refreshed on `loop1s()` (a periodic *sample*, never the hot path);
- reading MoonLight's **existing self-report** (`loopTimeUs`/`classSize`/`dynamicBytes`) + **existing platform getters** (`freeHeap`/`freeInternalHeap`/`maxAllocBlock`/`totalHeap`, and per-module seams behind the boundary like `platform::taskSnapshot`);
- **zero hot-path cost**, cross-platform (desktop shows what it can, stubs the rest).

Memory and Pins **must** follow this shape. If a proposed feature can't fit it (e.g. "route every alloc through the module" — see below), that's the signal it's a different, heavier thing that needs its own opt-in mechanism, not a System Module.

## The System → System + Services split

The System-vs-Services distinction forces a conceptual line that was already muddled. Today `ModuleRole::Peripheral` conflates two different things, both parented under System:

- **Capability bridges the user adds** (user-managed) — Audio, IR: optional, per-board, come and go. These are the real "services." (MQTT is always-there network infra, Devices is fleet-scope — both stay code-wired under Network, not user-added; see the Devices note below.)
- **Fixed things that carry the `Peripheral` role only incidentally** — Tasks was given `Peripheral`+delete only to render the UI delete button; it's actually a fixed System Module, not a user-added capability.

(**FileManager is a standalone top-level concern** — the OS-standard pattern: Finder / Explorer / Nautilus are their own thing, not part of Activity Monitor / Task Manager / Device Manager, and not a user-added capability either. It stays top-level and fixed; it just shouldn't wear the capability role.)

The clean model, matching your framing that *"System is really to view/manage the hardware a device has"*:

- **System** = the device's **fixed hardware + its inspection**: identity (deviceName, chip, mac), live vitals (uptime, fps, tick), reboot — **and the System Modules hang here: Tasks, Memory, Pins, I2cScan.** Always present, not user-added. These are the device's **inspection / bring-up toolkit** — Tasks (what runs), Memory (what's allocated), Pins (what's assigned), I2cScan (what's on the I²C bus). All fixed, all "inspect *this* device."
- **Services** (new top-level container) = the **user-added capability modules**: Audio, IR — this-device bridges to the outside world. Optional, per-board.

**DevicesModule is deliberately NOT in either bucket** — it is **fleet-scope** (discovers/lists *other* devices, drives Hue, the seed of future multi-device features: groups, sync, orchestration), so it's neither a this-device System Module nor a this-device Service Module. It stays a wired-by-code child of Network for now; its eventual home is a **later decision** — a standalone top-level module, or a "Fleet"/"Devices" top-level *container* once a second fleet module exists to justify one (don't build a container for one child). Flagged here so the distinction isn't lost.

This is a principled boundary — *observe the fixed hardware* vs *add an optional capability* — not just decluttering.

### The name: **Services** (decided)

Container = **Services**, and `ModuleRole::Peripheral` is **renamed to `ModuleRole::Service`** so there is one concept, one word (the container name and the role name agree). Chosen on the merits, not on what already exists:

- **"Service" is the more accurate word — and applies ONLY to the `Services` container's user-added children** (today Audio and IR). It is deliberately **not** carried by the always-there network bridges (MQTT, Devices, Hue): those are wired-by-code children of Network and stay **roleless (`Generic`)**, so no container offers to add/delete them and the `Service` role stays unambiguous — one role, one container. "Peripheral" was a hardware term (a thing attached at the periphery), too narrow once the container also holds a non-hardware capability; "Service" = an ongoing capability the device provides or consumes, the word HA / systemd / Android use for the same idea.
- **The rename cost is not an argument to avoid it.** "We already have `Peripheral`" is a precedent-defense the principles reject ([*don't store derivable, no precedent-defense*](../../CLAUDE.md#principles); best solution over technical-debt-carried-forward). The rename is mechanical (role enum + `roleName` + `acceptsChildRoles` + docs) and one-time.
- **The "services" overload (WiFi/HTTP are also 'services')** is weaker than it looks: those are *always-there infrastructure the device runs on*, not *user-added capabilities* — they stay in System. The line "user-added optional capability (Services)" vs "always-there infrastructure (System)" is clear.

So: **Services** container, **`ModuleRole::Service`** role, and the split plan carries the peripheral→service rename.

### Fixed vs user-managed — the clean rule (decided)

The split maps onto one rule, matching how OS system managers behave (Task Manager / Activity Monitor are *always available*, never added or deleted):

- **Everything under `System` is FIXED** — always present, **no add/delete**, wired-by-code. That's System's own vitals **and** the System Modules (Tasks, I2cScan; Memory, Pins later) **and** the always-there infrastructure (Network, Firmware, Improv). You don't delete a System Module any more than you delete Task Manager.
- **Everything under `Services` is USER-MANAGED** — add/delete/replace, `ModuleRole::Service`. Audio, IR. (MQTT stays code-wired under Network as always-there infra; Devices is fleet-scope, see its note.)

**`Services` is the existing container shape, applied to the core domain** — the strongest justification (*Common patterns first*). "A top-level container holding the user-added children of one role" is a pattern the codebase already runs on; `Services` reuses it rather than inventing a second arrangement. So the split adds no new concept: it recognises that Audio and IR are user-added children of one role, and that is what a container of that shape is for.

This settles the role question: **Services children = `ModuleRole::Service` (user-managed); System's fixed children carry no user-editable affordance** (wired-by-code, no delete). It **fixes TasksModule's current stopgap** — it was given `Peripheral`+delete only to render the delete button, which is now the *wrong* answer: Tasks is a fixed System child, so it should be wired-by-code with no delete, like ImprovProvisioning under Network.

### Docs follow the split: `services.md` → `system.md` + `services.md`

The single [core/services.md](../../moonmodules/core/services.md) summary page splits to mirror the two containers (one home per module, *Documentation model*):

- **`system.md`** — System + its fixed children: the System Modules (Tasks, Memory, Pins) and the always-there infra (Network, Firmware, Improv, System vitals).
- **`services.md`** — the user-added Service modules: Audio, IR.

The `main.cpp` `registerType` docPaths update accordingly (System children → `system.md#…`, Services → `services.md#…`); `check_specs.py` validates each resolves. Part of the split plan.

## Memory module — scope (read-only sampler)

A Tasks sibling. **In scope** (cheap, always-on, from existing accounting):

- **By heap type:** internal RAM and PSRAM each — total / used / free / largest contiguous block (`freeHeap`/`freeInternalHeap`/`totalHeap`/`maxAllocBlock`/`maxInternalAllocBlock`, `platform::hasPsram`).
- **Per-module footprint:** each module's `classSize` (static) + `dynamicBytes` (heap from `onBuildState`), largest first — the "which module holds the big buffers" view. This is MoonLight's **self-report**, already collected.
- **Notable large allocations shown explicitly:** the big contiguous buffers (the layer buffer, LED DMA buffers, ArtNet handoff) — surfaced from the owning module's self-report, optionally tagged, so a 48 KB buffer at 128×128 is visible as itself, not just folded into a module total.

**Explicitly out of scope — "route every alloc/free through the module":** this is per-event heap tracking, which (1) fights the hot-path *alloc-once, no per-frame allocation* rule — by design there is no stream of allocs to intercept in the render loop; (2) taxes *every* system allocation with hook overhead; (3) is how a leak-hunt profiler works, not a dashboard. OSes keep it as an opt-in tool (ESP-IDF `heap_trace`, UMDH), never always-on. MoonLight's model is **self-report, not intercept**: a module *reports* its big buffers; malloc is not funneled through a UI module. If leak-hunting is ever wanted, that's a **separate, build-flag-gated `heap_trace` mode** (the CPU%-style opt-in), not part of this read-only Memory view.

## Open decisions (for the specs/plans that follow)

1. **Split container name + role** — DECIDED: `Services` container + `ModuleRole::Service` (see above); the split plan carries the peripheral→service rename.
2. **What moves to the new container** — only the user-added capability bridges: **Audio, IR**. **MQTT stays code-wired under Network** (always-there network infra, not user-added — see the split above), **Devices stays under Network** (fleet-scope), **FileManager stays standalone** (its own top-level concern, OS-standard — just correct its `Peripheral` role so it isn't tagged a capability). **Network, Firmware, Improv** stay System (always-there infrastructure).
3. **System Modules are FIXED** (decided) — wired-by-code, no delete; applies to Tasks/Memory/Pins, and fixes TasksModule's stopgap `Peripheral`+delete.
4. **Catalog + persistence migration** — every deviceModel entry parenting Audio/etc. under `System` moves to the new container; persisted device configs need the same reparent. Sized in the split's plan.
5. **Build order** — the split is a prerequisite-ish for a tidy System, but Memory can ship under System today and reparent with the rest later; don't block Memory on the split.

This note is the frame. Each of Memory, the System/Services split, and Pins gets its own spec + `/plan` referencing it, built concrete-first, one piece at a time.

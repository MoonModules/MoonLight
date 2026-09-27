# UI extensibility (app.js) — bottom-up analysis

## The governing statement (the invariant to achieve)

> **`app.js` is completely module-independent.** It supports only the **generic** UI: module cards, the generic control widgets (number, text, pin, bool, select, list, …), creating / deleting / replacing / reordering cards, the tick / memory / status emoji header — the self-describing-controls machinery. **No module specifics live in it — none.** A core module's UI specifics live in **core** (next to that module), a light-domain module's UI specifics live in **light**. app.js knows *control types*, never *module names*.

This is a **rule, not a direction** — it's enforceable (grep app.js for a module name; there should be zero) and it's the acceptance test for the refactor. This study surveys how the field achieves it, measures MoonLight's current distance from it, and extracts the mechanism. The top-down companion (design from the goal, not consulting this) follows.

The trigger: app.js has grown to **~3,200 lines** with module-specific content baked in (a hardcoded `renderFileManager` and `if (mod.type === …)` branches), and while building TasksModule we **hit the wall** — the generic list-detail renderer couldn't display a new module's data shape, forcing a workaround. That's the smell; the statement above is the fix.

## The goal (what "generic" means here)

MoonLight's core UI strength is that it renders **any** module's controls generically from the `/api/state` control list — one code path draws an effect, a driver, or a system service (the same self-describing-controls model the architecture is built on). A module that needs *more* than the 9 generic control types (a file tree + editor, a task table, a memory-by-type chart, a pin board diagram) should **plug that in** without app.js knowing the module exists by name. Generic core, module-owned specifics.

## Where MoonLight is today — a split personality

The codebase already does this **two opposite ways**, which is the tell:

- **✅ Extracted (the good pattern):** the light-domain 3D preview lives in its own file, `src/ui/preview3d.js`, `import`ed into app.js as `preview`. So there *is* precedent for a domain/module owning its UI in a separate module.
- **❌ Inlined (the anti-pattern):** FileManager's UI is **357 lines of `renderFileManager` inside app.js** (folder tree, inline editor, drag-drop, upload), reached by a **hardcoded type-check** — `if (mod.type === "FileManagerModule") { renderFileManager(...) }` (app.js:595). The generic renderer has a specific module's name compiled into it.

And the **generic core that should stay** is clean and worth preserving: a `switch (ctrl.type)` over the 9 `ControlType`s (app.js:961) — pin / bool / select / list / … — the self-describing-controls renderer. That's *control-type* dispatch (generic, keep it); the violation is *module-name* dispatch (specific, remove it). The two must not be confused: `ctrl.type === "bool"` stays; `mod.type === "FileManagerModule"` goes.

### The exact distance to the invariant (what must leave app.js)

Every module-name reference in app.js today — each of these must move to its module's domain for the statement to hold:

| app.js today | What it is | Moves to |
|---|---|---|
| `if (mod.type === "FileManagerModule")` + `renderFileManager` (357 lines) + ~8 call sites | the file tree / editor / drag-drop UI | **core** — FileManager's own UI file |
| `if (mod.type === "FirmwareUpdateModule")` (the install-picker mount) | the firmware install picker | **core** — Firmware's own UI file |
| `state.modules.find(m => m.type === "SystemModule")` (app.js:2421) | a System-specific lookup | **core** — System's UI hook, or a generic mechanism |
| `find(m => m.type === "FirmwareUpdateModule")` (app.js:2570) | Firmware lookup | **core** — with the above |

The `ctrl.type === "bool"/"text"/…` checks (app.js:2029-2030) are **generic control types, not module names** — they stay.

**Home placement — the rule extends to preview3d too.** Today all UI lives in `src/ui/` (app.js, preview3d.js, install-picker.js). The statement says *light specifics in light, core specifics in core* — so even the already-extracted **preview3d.js is in the wrong home**: it's a light-domain widget sitting in the shared UI dir. Achieving the invariant means UI assets live **with their domain's source** — `src/light/…/preview3d.js`, `src/core/FileManagerModule.ui.js`. So the refactor is two-part: (1) *extract* module UI out of app.js into per-module files, and (2) *relocate* those files (and preview3d) into their domain tree.

**Build-step consequence (a real cost to name).** `embed_ui.cmake` today gzips + hex-embeds a **hardcoded filename list from one `UI_DIR` (`src/ui`)** — `gzip_file_hex("app.js")`, `("preview3d.js")`, … one explicit line per file, single directory. It is *not* a glob and *not* multi-directory. So relocating widgets into `src/light`/`src/core` requires teaching the embed step to **gather UI files across the domain tree** (a glob over `src/**/*.ui.js` or a manifest each module contributes to), and the served-file router (`/preview3d.js` etc.) to map the new paths. That's the concrete build-side work the invariant implies — small, but it exists; the top-down should scope it, not hand-wave it.

**The live evidence this matters:** building TasksModule, its nested per-module detail is an *array of objects*, which `fillListDetail` (app.js:1537) can't render (`if (typeof v === "object" && !isScalarArray) continue` — it silently skips objects). The fix shipped was to **flatten each module to a scalar string** — a workaround in the *module* because the *generic renderer* couldn't be extended cleanly. A Tasks-owned UI hook would have rendered a real sub-table. The System Modules (Memory's by-type bars, Pins' board map) will each hit this same wall.

## The field

### Home Assistant — Lovelace custom cards (the direct analog)

HA is exactly MoonLight's situation: a generic dashboard rendering entities, with an extension path for custom UI ([developers.home-assistant.io/docs/frontend/custom-ui/custom-card](https://developers.home-assistant.io/docs/frontend/custom-ui/custom-card/)):

- **A card is a Custom Element** — `customElements.define('my-card', MyCard)`. The dashboard doesn't know the card's internals; it instantiates the tag.
- **A lifecycle contract** the core calls: `setConfig(config)` on setup/config-change (throw → the core renders an error card), and it **sets the `hass` property** on every state change (the card re-renders from it). One-way data in, event out.
- **`config-changed` event** — the card dispatches config changes back up; the core doesn't reach into the card.
- **A registry for discovery** — `window.customCards` (an array of `{type, name}`) so the picker can list custom cards without hardcoding them.

**Take — this is the target pattern.** Generic core + custom element per module + a lifecycle contract (data-in via a property, changes-out via an event) + a registry for discovery. HA proves it scales to thousands of third-party cards with the core knowing none of them by name.

### Web Components / Custom Elements (the platform primitive)

The browser-native mechanism HA builds on ([MDN Web Components](https://developer.mozilla.org/en-US/docs/Web/API/Web_components)):

- **`customElements.define(name, class)`** registers a tag; the core writes `<mm-filemanager>` and the browser instantiates the class. Encapsulated, no framework.
- **Lifecycle callbacks** (`connectedCallback`, `attributeChangedCallback`, `disconnectedCallback`) — the element manages its own mount/update/teardown, so the core doesn't.
- **Scoped registries** (emerging) let independently-built widgets avoid tag-name collisions — relevant only if third-party UI is ever a goal; overkill for first-party modules.
- **Manual vs auto registration** — a module can *export* its element class and let the app register it, or self-register on import. Either fits.

**Take:** Custom Elements are the **zero-framework, standards-based** substrate — matches MoonLight's no-build-step, no-framework UI (plain ES modules today). No dependency added; it's a browser API.

### The registry/dispatch idea (generalised)

Across HA, VS Code (contribution points), Grafana (panel plugins), the shape is identical: **a core that dispatches by a key to a registered handler, never a switch over known names.** The core holds a `Map<key, renderer>`; a module registers `registry.set('FileManagerModule', FileManagerWidget)`; the core does `registry.get(mod.type)?.(mod, host)` with a generic fallback. Adding a module's UI = registering an entry, touching zero core code.

## What MoonLight already has going for it

- **Self-describing controls** — the generic `switch (ctrl.type)` renderer is the 90% path and already works; most modules need *no* custom UI. The extension point is only for the few that exceed the 9 control types.
- **ES modules, no build step** — `import { preview } from "/preview3d.js"` already works; Custom Elements are a browser primitive needing no tooling. The substrate is present.
- **A clean control-value contract** — modules already emit their state as JSON controls; a custom widget receives the same `mod` object the generic renderer gets. The data-in side is solved.
- **One real extraction already done** (preview3d) — a proof the split works; FileManager is the one to follow it, and the pattern generalises to Tasks/Memory/Pins.

## Ideas extracted (for the top-down to design against)

| Idea | Source | MoonLight shape |
|---|---|---|
| **Registry dispatch, not a name switch** | HA `customCards`, VS Code, Grafana | app.js holds `Map<moduleType, renderer>`; `registry.get(mod.type)` with the generic fallback. No `if (mod.type === "X")` in core. |
| **Custom Element per module widget** | HA cards, Web Components | Each special module ships `<mm-tasks>` etc. as a Custom Element in its own file; app.js instantiates the tag, knows nothing inside. |
| **Lifecycle contract: data-in property, changes-out event** | HA `setConfig`/`hass`/`config-changed` | The widget gets the `mod`/state via a property/method the core sets each refresh; it dispatches control changes via the existing `/api/control` path (or an event the core forwards). |
| **Module owns its file** | preview3d.js (already done), HA | `src/ui/modules/filemanager.js`, `.../tasks.js`, … — one file per custom widget; `renderFileManager`'s 357 lines move out of app.js. |
| **Domain grouping** | preview3d = light domain | Light-domain widgets under one folder/entry (per your note "light domain contains its own hook-ins"); core widgets (System Modules) under another. |
| **Generic renderer stays the default** | HA renders standard entities generically | The `switch (ctrl.type)` core is untouched and remains the path for every module without a registered widget — subtraction, not addition, for the 90%. |
| **Extend the generic list-detail too** | the TasksModule wall we hit | Separately: teach `fillListDetail` to render nested object-arrays (a sub-table) so a module doesn't need a *full* custom widget just for richer list detail — a middle tier between "generic" and "full custom element." |

## Scope signal for the top-down

The convergent answer is **a small registry + a per-module widget contract, both dead-standard (Custom Elements + a `Map` dispatch)** — MoonLight needs no framework, no build step, and already has the substrate (ES modules) and one worked example (preview3d). The work is: (1) define the widget contract (how a module widget receives state + emits changes), (2) a registry app.js consults instead of the hardcoded branches, (3) migrate FileManager out of app.js as the first citizen (proving the contract on the hardest existing case), (4) a middle tier — richer generic list-detail — so not every custom need forces a full widget. Then Tasks/Memory/Pins each add a file + a registry entry, and app.js stops growing per-module.

There are **three tiers** the top-down should name, so a module reaches for the lightest that fits (minimalism): (a) **generic controls** — no custom UI, the default; (b) **generic-with-richer-list-detail** — a module whose need is just nested/tabular detail; (c) **full custom widget** — a Custom Element for genuinely bespoke UI (file tree, board diagram). FileManager is (c); TasksModule today is (a)-with-a-workaround that (b) would fix; Memory/Pins are likely (b) or (c).

This is directly tied to the [System Modules design](system-modules.md): Tasks/Memory/Pins are the *next* modules that will want tier (b)/(c) UI, so the extension architecture should land before (or alongside) building Memory and Pins — otherwise each repeats FileManager's inlined-in-app.js mistake.

## The acceptance test (from the governing statement)

The refactor is done when **`grep` of `app.js` finds zero module names** — no `FileManagerModule`, `FirmwareUpdateModule`, `SystemModule`, no `renderFileManager`. Only control types (`bool`, `pin`, `list`, …) and generic card/emoji/CRUD machinery remain. Concretely, the invariant holds when:

1. app.js dispatches custom UI via a **registry** (`Map<moduleType, widget>`), never a name switch — and the registry's entries are *registered by the modules*, so app.js's own source contains no module name.
2. FileManager's 357 lines + the Firmware picker live in **core** UI files; preview3d + any light widget live in **light** — each embedded from its domain tree.
3. A new module needing custom UI adds *one file + one registration* and touches app.js **not at all**.
4. `check_specs.py` (or a new lint) could even *enforce* the invariant: fail if app.js contains a known module type name — the rule made a gate.

That last point is the strongest form: the statement isn't just an aspiration in a doc, it's a **checkable rule** the build can hold app.js to.

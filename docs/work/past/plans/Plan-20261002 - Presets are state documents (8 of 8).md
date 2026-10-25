# Plan: presets are state documents

Plan of 2026-10-02, every step done 2026-10-25. A preset became any part of the device's state, one hierarchical JSON document, and that document became the one shape for everything that holds state: a REST call, a preset file, a card's `{ }`, a gallery entry, a config file, a device model and a scenario fixture.

## Why

- **A preset was one whole container.** It captured Layouts, Effects, Drivers or Services, so a palette could only be saved with the driver's pins and brightness.
- **Adding an effect with its controls took several requests**, `POST /api/modules` and then one `POST /api/control` per control, which a user reported.
- **Six shapes described one tree**, each with its own parser: flat dotted keys with positional children in config and preset files, per-module REST calls, Improv's add and set ops, `deviceModels.json`'s module list, scenario `add_module` steps, and the card's diagnostic dump.

WLED works this way: a preset is a stored `/json/state` body. Here the state is a tree of modules, so a document also creates and removes them.

## Steps

1. ✅ **The apply engine in core**: JSON Merge Patch (RFC 7386) over the module tree, `"$patch": "replace"`, `null` removing, one prepare at the end ([Setting everything at once](../../../reference/integrating.md#setting-everything-at-once)).
2. ✅ **`PATCH /api/state`**, adding an effect with its controls in one request ([the REST API](../../../reference/api.md)).
3. ✅ **Presets as documents**, the role following from the containers touched, a palette-only preset possible ([Presets](../../../reference/presets.md)).
4. ✅ **A card's `{ }` shows its document**, with copy, the diagnostic dump a link beside it ([UI](../../../moonmodules/core/ui.md)).
5. ✅ **A save-as-preset button** on each module card ([Presets](../../../moonwalk.md#presets)).
6. ✅ **The gallery in the UI**, installed with a click, the browser fetching ([Install from the gallery](../../../moonwalk.md#gallery-install)).
7. ✅ **A gallery that holds a hundred entries**: thumbnails made at acceptance, a paged grid, search, filter and sort, a detail view.
8. ✅ **Config files, `deviceModels.json` and scenario fixtures as documents**, each removing its old reader:
   - 8a. One create path in core: every add, replace and delete applies a one-member document.
   - 8b. Config files as documents, applied leniently as a stored file ([MoonModule, Persistence](../../../explanation/architecture/moonmodule.md#persistence), [MIGRATING](../../../reference/MIGRATING.md#config-files-are-state-documents)).
   - 8c. `deviceModels.json` entries and Improv's APPLY_OP as documents ([MoonInstaller](../../../explanation/architecture/mooninstaller.md)).
   - 8d. Scenario fixtures through `apply_state` documents ([testing.md](../../../reference/testing.md)).
   - Verified on the desktop, the Olimex and two S3s; provisioning over Improv and through the web installer on a P4; a flat backup restored on the P4 by the product owner; documents of up to 30 KB on an S3; a palette-only preset over an effects preset (2026-10-25). The restore found a write refused while its file was read, fixed in the HTTP server, and a P4 panic on flash writes, fixed by placing FreeRTOS in IRAM.

## End states weighed

| | What uses documents | Gains | Loses |
|---|---|---|---|
| A | REST and presets | the two user-visible needs, the smallest change | config files keep the flat format: two shapes for one subtree |
| **B** (built) | A, the card's `{ }` and the gallery, then config files, `deviceModels.json` and scenario fixtures | one shape everywhere; the positional key code and three list formats went | more steps, and config files changed shape, which MIGRATING records |
| C | A plus `deviceModels.json` | the installer's tree in the preset shape | the firmware's own duplication stays |

## Decided

- **One JSON for everything that holds state** (the product owner, 2026-10-04), so B.
- **The directive is `"$patch": "replace"`**, Kubernetes' strategic merge patch spelling for "exactly these children", since Merge Patch has no word for it and a `$` key is one no control can carry.
- **The UI's schema stays its own shape**: `GET /api/state` and the WebSocket carry each control's type, range and options, which a card needs to draw and a document does not hold.
- **C was rejected**: it converts an installer file and keeps the flat config format beside the document engine.
- **Playlists stay out**: a preset listing presets with durations is a later step on this engine.

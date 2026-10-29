# Backlog: UI

Generated from [the Project](https://github.com/users/MoonModules/projects/5) by `moondeck/report/project_mirror.py`: edit the cards, not this file.

## FixtureProfilesModule

### Sparse fixture-profile editor

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131084)

- **Sparse fixture-profile editor.** A FixtureProfiles row currently shows one role Select per channel across the whole `channels` width: including the unmapped `, ` gaps a wide moving head has between its functions. For a fixture you usually only care about the few channels you drive (rgb, pan, tilt). The refinement: show only the *mapped* channels + an "add channel" affordance (pick a role → fills the first gap or grows the fixture), over the unchanged dense `roles[]` storage. A first attempt shipped and was reverted for edit bugs; redo it cleanly (the dense editor is the reliable interim). Prior art: GDTF / QLC+ fixture profiles (a fixture is a sparse `{channel → function}` map, not a dense per-channel array).

## Across modules

### The update overlay's Cancel button does nothing on a plain OTA (2026-09-14)

Kind: Bug · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131157)

`showUpdateOverlay` shows a Cancel button on every firmware install and wires it to
`POST /api/firmware/cancel` (`src/ui/app.js:1479`). Only MoonBase implements that route
(`moonbase/main/moonbase_main.cpp:532`); the application's dispatch table has none, so the request
404s and the `.catch(() => {})` swallows it. The button therefore works during the MoonBase phase of
an install and is inert for a plain app OTA on a device without MoonBase.

A file upload still cancels there, but by a different mechanism: `uploadCtl.abort()` drops the
connection on the line above. So the two install sources behave differently behind one button, and a
URL install on a non-MoonBase device is the case where pressing Cancel does nothing at all.

Two ways out, and the choice is the question. Implement the route in the application, which means an
abort flag the OTA task polls between chunks, the shape MoonBase already uses. Or hide the button
where it cannot act, which is honest but leaves a long download uninterruptible. The first is the
better system and the larger change; the second is a few lines. Found while writing
[Update MoonLight](../../moonwalk.md#update-firmware), which deliberately does not mention Cancel:
documenting a button that half-works is worse than leaving it undocumented until it is one thing or
the other.

### Split app.js by feature, and take the module names out of it (2026-10-15)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131241)

The UI ships as one minified bundle with ETags, so splitting `app.js` costs nothing at runtime, and at 9,200 lines a feature is hard to find. [The rule](../../contributing/coding-standards.md#per-type-behavior-lives-with-the-type) is that `app.js` dispatches on control types and never on module names; it names about fifteen types today, from `renderFileManager` and the firmware install picker to `ALWAYS_SUBSCRIBED`, the System and Network lookups, the MoonStats and MoonTalk consent paths and the Gamepad and MIDI services. The textbook shape is a registry (`Map<moduleType, widget>`) the modules fill from their own files, each widget a Custom Element with data in through a property and changes out through `/api/control`, so a new module's UI is one file and one registration. The File Manager goes first as the hardest case, and `fillListDetail` learns nested object arrays so a tabular module needs no full widget. The field survey behind this shape, Home Assistant cards and VS Code contribution points: [the UI extensibility analysis](../../design/ui-extensibility.md).

### File Manager follow-ups

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131328)

Deferred capabilities of the [File Manager](../../moonmodules/core/system.md#file-manager), each self-contained:

- **Upload, recursive folder tier.** Multi-file / recursive folder drops: client-side `mkdir` + walk via `dataTransfer.items` `webkitGetAsEntry()`.
- **Download, folder as `.zip`.** Per-file download streams (`<a download>` on `/api/file`). A folder download needs the browser to walk `/api/dir` recursively, fetch each file, and build a `.zip` client-side — which means bundling a zip library into `app.js`. That's a permanent app.js size bump, and app.js is embedded in firmware, so it weighs on the flash budget. Gate on real demand; symmetric with the folder-upload tier.
- **Last-modified dates.** Needs a time source (NTP) + LittleFS mtime storage; the tree is name + size until both land. Backlogged with the NTP work.
- **`.ml` syntax highlighting in the editor.** MoonLive source wants *highlighting* (a color layer over the textarea), not the JSON-style reformat — a bigger editor change (a highlight overlay or a small tokenizer), added when MoonLive `.ml` files land on disk. The editor already has an extension seam (`fmPrettify`) for the reformat case; highlighting is the separate, larger tier.
- **Keyboard + screen-reader access for the tree.** The `fm-row` tree entries are mouse-only today (click to select/expand/open). Making them keyboard-operable (focusable, Enter/Space activation) with ARIA tree semantics (`role="treeitem"`, `aria-expanded` from `isOpen`, `aria-level`) is a real accessibility win but adds JS that lands in the firmware-embedded `app.js` (flash cost). Do it when a11y is a stated goal, together across the whole generic UI, not just this panel.

### UI features deferred from v1

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131412)

What v1's frontend had and this UI lacks, each waiting on the engine work named:

- **Health panel**: a `<details>` polling `GET /api/test` for a pass/fail table, which needs a runtime `/api/test` that runs the doctest suite; `ctest` covers this for now.
- **Log panel and log channel**: a ring buffer with severity coloring, stick-to-bottom and `GET /api/log` backfill, fed by a WebSocket `{t:"log",m:"…"}` pushed by the server, which needs an engine-side log producer. Both arrive together, when boot, network and persistence logs interest non-developers.
- **A setup dot before a card's name**: needs `setupOk()` and `health()` on MoonModule with a real failure mode; today both would always be `true` and `""`.
- **Module ID beside the name**: when instances need telling apart, such as two effects of one type under one Layer.
- **Category emoji on the card header**: `ROLE_EMOJI` exists in `app.js` for the picker, so showing it per card is a small step if card scannability needs it.
- **A module `category()`**: a taxonomy beyond `role()` for the picker; derived from `role()` until then.
- **Memory split, heap against PSRAM**: splitting `dynamicBytes` further needs `platform::isPsramPointer(p)` or per-allocation tracking.

Two design questions stay open:

- **Modifier chain visualization**: the `children[]` order is the apply order (modifiers compose as a chain, M₁∘M₂∘…), so a visual that shows the stacking, and that order matters, helps a user reason about a multi-modifier layer.
- **Canvas or node-graph view**: v2 tried it; powerful for complex setups but it doubles the UI surface, so it waits on user demand.

### Double-click and multi-click on a button (2026-10-25)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131477)

From ESPHome's trigger vocabulary: the same debounce
timers with more states, so one button does several things. Built when a panel needs more actions
than it has buttons.

### A desktop build reports firmware "unknown", so the update UI guesses from the browser (2026-09-06)

Kind: Bug · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131545)

Spotted on the container's Firmware card, which showed **Device: macOS arm64** while the image is
Linux amd64. It was not reading the device: `desktopKeyForThisHost()` (`src/ui/install-picker.js`)
and `desktopAssetPrefix()` (`src/ui/app.js`) both sniff `navigator.platform`, which describes the
machine holding the BROWSER. The container makes it visible because there the two always differ, but
the same card lies on any desktop instance opened from another machine.

The download it offers is the failure mode worth naming: on a Mac the picker serves
`MoonLight-macos-arm64-*.dmg`, a real file that installs and runs perfectly, and updates the wrong
computer. Nothing errors, and the device stays on its old build.

**Root cause is one missing build flag.** `MM_FIRMWARE_NAME` already exists for exactly this
(`build_info.h`, surfaced as the `firmware` control): `build_esp32.py` passes the variant key, and
the desktop packaging passes nothing, so every published desktop binary falls through to the
`"unknown"` default. `build_info.h:43` even records the assumption that made this safe, "local
builds aren't published" — the container is a published desktop build, which is what retired it.

**What it would take:** have `package_desktop.py` pass
`-DMM_FIRMWARE_NAME="desktop-<os>-<arch>"` for the target it packages (the keys the picker's
`DESKTOP_LABEL` map already lists), then delete both browser-sniffing functions and read the
`firmware` control instead. `deviceFirmwareInfo()`'s `isDesktop` test (`firmware === "unknown"`)
needs the same treatment, since a named desktop build no longer matches it: test the
`desktop-` prefix. Local unflagged builds keep reporting `unknown`, where a guess is still better
than nothing.

Worth doing with the OTA path rather than alone: a desktop cannot install its own archive anyway, so
the honest end state may be a card that names the platform and links the release rather than
offering a Download button that only ever means "fetch a file and swap it by hand".

### RGBW preview end-to-end

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131627)

The light `Buffer` already holds `channelsPerLight = 4` (RGBW), and the device output drivers handle it, but the **preview only ever sends/draws RGB** — the W channel is invisible in the UI. Once the self-describing header lands, carry the W channel on the wire and render it in the shader (W as a warm-white tint / brightness lift on the disc). Small, but gated on the header so it isn't another bespoke opcode.

### Moving-head preview = peer interpreter

Kind: Feature · Trigger: moving heads land · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287131696)

- **Moving-head preview = peer interpreter.** When moving heads land, the previewer must interpret channel semantics (pan/tilt/RGBW-at-arbitrary-indices) to render a moving fixture: the same fixture-profile model physical drivers use, interpreted to screen. This is *why* the increments named the abstraction "interpret the profile" rather than "apply correction / opt out": so Preview becomes a full peer here without a rename. Its own design plan when moving-head support starts.

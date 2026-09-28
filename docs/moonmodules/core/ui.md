# Web UI

The MoonLight web UI as shipped, the render layer over the MoonModule tree. This page is the UI's own implementation spec (status bar, cards, control rendering, styling, the no-rebuild update contract). The **high-level architecture**, hand-maintained files, MoonModule-driven rendering, the light-domain plug-in points, lives in [MoonCore, the web UI](../../explanation/architecture/mooncore.md#web-ui); the **backend contract** it consumes (every `/api/*` endpoint, the `/ws` frame shape, the control descriptors) is owned by [HttpServerModule](moxygen/HttpServerModule.md); the **emoji legend** the cards and picker render is [the tag emoji legend](../../explanation/architecture/index.md#tag-emoji-legend).
This page covers only what those don't: the browser-side rendering behavior.

## Interaction principles

- **Two timescales for inputs.** Local UI feedback (a slider's value label, a toggle's state)
  updates within ~20 ms of the input event; network sends are debounced (150 ms slider, 500 ms text), so a slider feels instant even when the server is busy.
- **No write-back races.** A `dragTs[moduleId:key]` cooldown of 1 s stops an incoming WS push from
  overwriting a control the user is actively manipulating.
- **No DOM rebuilds on state updates.** The initial render builds the tree; state updates patch values
  in place via `querySelector` by `data-mid` / `data-key` (see [§ no-rebuild contract](#state-updates-the-no-rebuild-contract)).

## Layout

```text
┌─────────────────────────────────────────────────────────────┐
│  Status bar: ☰ · logo · brand · device name · sys stats     │
│              · WS dot · reboot · theme toggle               │
├──────────────┬──────────────────────────────────────────────┤
│  Side nav    │  Sticky preview canvas (shrinks on scroll)   │
│  (root list) ├──────────────────────────────────────────────┤
│              │  Cards for the selected root module          │
│              │    ↳ child cards inline                      │
│  ─────────   │      ↳ grandchild cards inline               │
│  footer:     │                                              │
│  social ©    │                                              │
└──────────────┴──────────────────────────────────────────────┘
```

- Fixed status bar at top (44 px), hamburger, MoonLight logo, then the rest.
- Side nav: a left column listing root modules; the selected root's card subtree fills the main area
  (one root visible at a time).
- Module-card column max-width 500 px, centered (single-column, easier to read on phones). The preview
  canvas is **not** capped, it spans the full content width.
- Preview canvas is sticky just below the status bar and shrinks 0 % → 50 % as the user scrolls
  0 → 300 px.

## Status bar

Fixed top, 44 px, left-to-right:

1. **Hamburger (☰)**, toggles the side nav (`body.nav-open`). See [§ Side navigation](#side-navigation).
2. **MoonLight logo**, 28 px PNG from `/moonmodules-logo.png`; the same image is the page favicon.
3. **Brand wordmark**, "MoonLight".
4. **Device name**, from the `System.deviceName` control.
5. **System stats**, `uptime · NN K free` (uptime `Xd Yh Zm Ws`, free heap KB), read from the
   SystemModule controls in the WS state push; no separate polling endpoint.
6. **WebSocket dot**, green = connected, gray = disconnected. The socket auto-reconnects with
   exponential backoff (500 ms → 5 s) on close, and a page reload covers the rare half-dead socket.
7. **Reboot button (⏻)**, red border via `data-crashed="true"` when `SystemModule.bootReason` is
   PANIC / INT_WDT / TASK_WDT / BROWNOUT. Press-twice to confirm: the first click arms it (solid red), a second sends `POST /api/reboot`. Disarms after 3 s or on pointer-leave, no `confirm()` popup.
8. **Theme toggle (☀/🌙)**, flips `[data-theme]` on `<body>`; preference in `localStorage['mm_theme']`.

## Side navigation

A left column listing the root modules, one entry per top-level MoonModule.

- **One root visible at a time.** Clicking a nav entry sets the selected root; `renderCards()` renders
  only that root's card subtree. The selection persists in `localStorage['mm_selectedRoot']`; the active entry is highlighted.
- **Hamburger toggle.** ☰ toggles `body.nav-open`. On wide screens (≥ 820 px) the nav is a static
  column the hamburger collapses/expands; on narrow screens (< 820 px) it's a slide-in drawer over a dimming overlay (click the overlay or press Esc to close).
- **The nav states its own order** (`NAV_ORDER` in app.js), which is deliberately NOT the order the
  roots run in. `main.cpp` orders by dependency, Filesystem before anything that writes a file, System before the modules that read its identity, and that is load-bearing, so it cannot be reshuffled to suit a menu. It also reads as an implementation detail to a user: it puts System first and the lights last. The nav instead groups by what someone is looking for: **Control**, then the light pipeline in pipeline order (**Layouts → Effects → Drivers**), then the device (**System, File Manager, Network, Services, Firmware**). A root the list does not name still appears, after them, in scheduler order, adding a module never makes it invisible.
- **No root reorder.** The order is fixed in that list; the nav does not drag-reorder.
- **Footer** pinned to the bottom of the nav: social links (GitHub, Discord, Reddit, YouTube, inline
  SVG) and a `© <year> MoonLight` line.

The WS state push carries the whole module tree; only the selected root is rendered, and `updateValues()` patches just the visible cards (non-rendered roots have no DOM nodes, so their data is silently ignored). Filtering per root server-side buys little: the full tree arrives once on connect and each second after that carries only what changed, built through a streaming sink, so tree size is not a buffer-limit concern.

## Module card

Each MoonModule renders as a card, with **child cards nested inside their parent card's box**, the parent's border encloses its children, so the tree shape is visible structurally, not just by indentation. Nesting depth shows as progressively lighter backgrounds and a left-margin indent.

```text
┌─ card ──────────────────────────────────┐
│ [name] [emoji]  [timing · 🧠 mem]  [enabled toggle]  [✎ × ☰]  [? help] [{ } api] │
│ [control rows, one per control]        │
│ ┌─ child card ────────────────────────┐ │
│ │ …                                   │ │
│ └─────────────────────────────────────┘ │
│ [+ add child]                           │
└─────────────────────────────────────────┘
```

- The parent's own controls render **above** its children; `+ add child` renders **below** them.
  Child cards live in a `.card-children` wrapper appended into the parent card's DOM node (not flat siblings); `renderModuleTree` recurses into the parent card, not into `main`.
- **`{ }`** opens `GET /api/modules/{name}` in a new tab, that one module's live JSON, for issue
  reports (see [Log an issue](../../how-to/logging-an-issue.md)). On EVERY card, unlike `✎`/`×` (user-editable children only) and `?` (types that have a doc page).
- **Enabled toggle** in the right-hand action cluster mirrors `MoonModule::enabled()`, a styled
  `<button>` (transparent + muted border, 26×26); state shown by the glyph alone (accent **✓** on, muted **○** off; `data-checked` + `aria-pressed`). Toggling fires `onEnabled()`; the Scheduler skips disabled modules whose `respectsEnabled()` returns true (default).
- **Emoji tags** next to the name show the same set the type picker uses: role + dimensional (both
  UI-derived) + the curated `tags()` from `/api/types`. Identical identity across card and picker; the assignments are [the tag emoji legend](../../explanation/architecture/index.md#tag-emoji-legend).
- **Help link (?)** at the far right of the title row opens the module's doc page in a new tab. The
  path comes from `docPath` in `/api/types` (engine-provided, relative to `docs/moonmodules/`); omitted when the type declares none.
- **Stats line**: `🕒 <timing>` then `🧠 <static>[ + <dynamic>]`. Timing is a tick time
  (µs under 1 ms, ms above), omitted when the module has no measured loop time. Memory is the C++ object size (`classSize`); the `+ <dynamic>` part (`dynamicBytes`, heap) shows only when the module allocated heap. The line shows from expert mode up, and holding it peeks at the rate on every card, the password field's gesture: a tick time inverts to a figure that reads as a frame rate and is not one, so the rate is a press away rather than a mode the cards sit in.
- **Reorder is drag-and-drop** (HTML5 DnD) on the whole card, desktop and mobile. A drag starting on
  an interactive control is canceled in `dragstart` so the control's own gesture wins; a drag is accepted only when source and target share the same `.card-children` container (true siblings).
- **Replace (✎) / Delete (×) / drag-reorder** apply to *user-managed children*, a module whose role a
  container accepts (`acceptsChildRoles`) and that hasn't opted out via `userEditable: false`. Replace swaps for another type at the same position (picker filtered to the module's role; replacement starts at its own defaults). Delete is press-twice confirm. A `userEditable: false` child (e.g.
  PreviewDriver) shows none of these, it's fixed-shape like a code-wired child.
- **`+ add child`** renders in a parent's footer when its type declares a non-empty
  `acceptsChildRoles`; it opens the [type picker](#type-picker) filtered to those roles.

## Control types

Auto-rendered by `controls[].type`, adding a MoonModule with these types needs no UI change. (The descriptors themselves, and their storage shape, are [Control](moxygen/Control.md) / [coding-standards § store values in their native shape](../../contributing/coding-standards.md); this table is how each *renders*.)

| Type | Element | Interaction | Debounce |
|---|---|---|---|
| `slider` (uint8/uint16) | range + numeric display | drag → label instant; value sent debounced; reset-to-default ↺ | 150 ms |
| `toggle` (bool) | switch (pill + thumb; hidden `<input type=checkbox>` is the source of truth) | sends on change | none |
| `select` | dropdown | sends immediately; server may rebuild controls (dynamic `defineControls`) | none |
| `text` | text input | sends debounced | 500 ms |
| `textarea` | resizable multi-line box | sends debounced; the value IS the text, so it rides `/api/control` | 500 ms |
| `filepath` | file picker + inline editor (new/delete buttons) | picker sends on change; the file's CONTENTS save on blur, Ctrl/Cmd+S or the Save button, and a dot marks unsaved work | none |
| `password` | password input | masked; hold-to-peek reveals the stored value | 500 ms |
| `display` (read-only) | static text | WS push updates in place | n/a |
| `display-int` (read-only int + unit) | formatted text (`-58 dBm`) | unit suffix set device-side at `addReadOnlyInt` time, carried in the descriptor's `aux` slot | n/a |
| `time` (read-only seconds) | formatted text (`1d 4h 27m 13s`) | WS push updates | n/a |
| `progress` | bar + numeric `X / max` | WS push updates | n/a |
| `ipv4` | text input (dotted-quad) | server validates (`parseDottedQuad`), 400 on malformed; stored as 4 octets device-side | n/a |
| `button` | clickable button | sends value = 1 on click | none |

- **`filepath` stores a NAME, not a body.** The value is a ~40-byte reference that travels through
  `/api/control` like any text control; the file's contents move over `GET`/`POST /api/file`, because that is the only route allowed to exceed the request buffer (everything else returns 413).
  The module declares where its files live and which to offer (`addFilePath(name, buf, size, pick)`, a {directory, extension, template} triple), so the UI lists a directory, filters it, and seeds a new file without knowing what kind of file it holds. Saving is all it takes: a written file asks the module tree to re-derive, so whatever was built from that file rebuilds itself, with no second request from the browser.
  Editing is the same code the File Manager's modal editor uses, mounted inline instead of in a `<dialog>`, so both share one set of guards (a binary or truncated file loads read-only rather than risking a lossy re-save).

- **Reset-to-default (↺)** appears next to controls whose default is known (captured from a fresh
  probe instance per type, emitted in `/api/types`); dim when value == default, clicking sends the default.
- **Hidden controls.** `hidden: true` on a descriptor → the UI skips rendering it (conditional
  controls like NetworkModule's static-IP fields under DHCP).
- **Password controls are obfuscated, not encrypted.** A `password` control serializes in `/api/state`
  as `{"type":"password","value":"<encoded>"}` where `<encoded>` is the password XOR'd with a fixed key then base64-encoded; the UI decodes it (`decodePassword()`) so the input holds the real value (masked, hold-to-peek). A **first line of defense only**: the XOR key is a shared constant in both the firmware and `app.js`, so it's trivially reversible, it stops the password being plainly readable in a raw `curl /api/state`, not a determined reader.

## Type picker

One picker serves **add** (`+ add child`) and **replace** (the ✎ button), rendered inline in the card (not a modal).

- **Role filter.** Add mode filters to the parent's `acceptsChildRoles` (declared per-type in
  `/api/types`, the UI hardcodes no container→role map); replace mode filters to the target's own role.
- **Emoji tag chips.** A row of toggle chips above the list, one per distinct emoji across the filtered
  types (sources + meanings: [the tag emoji legend](../../explanation/architecture/index.md#tag-emoji-legend)).
  The UI treats `tags` as opaque, splits it into grapheme clusters, one chip each. Toggling narrows with **AND** logic (a type shows only if it carries every active chip).
- **Search box**: substring match on type name; combines with chips (both must match).
- **Keyboard nav**: type to filter, ↓ to enter list, ↑↓ to move, Enter to confirm, Esc to cancel.
- **Confirm / Cancel** at the bottom (confirm reads `create` or `replace`); double-click a row to
  confirm immediately.

## Styling

- **Dark theme (default):** background `#1a1a2e`, foreground `#e0e0e0`, accent `#a78bfa` (purple).
- **Light theme:** `[data-theme="light"]` on `<body>` swaps a curated set of overrides.
- **System UI font stack** (`system-ui, sans-serif`, not monospace); numbers use
  `font-variant-numeric: tabular-nums` so digits don't dance.
- **Module nesting** by progressively lighter card backgrounds (depth 0 / 1 / 2).
- **Responsive breakpoint** at 820 px.
- **Color semantics** consistent across the app: green (`#22c55e`/`#6ee7b7`) = connected/ok/pass; red
  (`#f87171`) = error/fail/crashed/delete; purple (`#a78bfa`/`#c4b5fd`) = accent/brand/active/value; gray (`#6b7280`/`#9ca3af`/`#4b5563`) = secondary/muted.

## Domain preview channel

The UI dedicates a binary slot on its WebSocket, separate from the JSON state updates, for a domain-specific preview frame; the engine pushes one per render and the UI hands it to a domain renderer. Generic shape: `[type-byte] [domain header] [payload]`, the first byte selects the renderer, everything after is the domain's choice; the UI ignores types it doesn't recognize.

- The canvas is sticky just below the status bar, scroll-shrinks 0→50 % over 300 px.
- `width: 100%` + `aspect-ratio: 1 / 1` derives the height from the column width; `max-height: 50vh`
  caps it.
- The last received frame is cached so camera gestures (orbit/pan/zoom) redraw instantly without a new
  frame; `touch-action: none` keeps gestures off native scroll/zoom.
- WebGL clear color `(0, 0, 0, 0)`, a transparent canvas blends into either theme with no per-frame
  color work.

The light-domain renderer (WebGL point cloud, the `0x03`/`0x02` frame format, orbit camera, downsampling) is [MoonCore, the web UI](../../explanation/architecture/mooncore.md#web-ui) / [PreviewDriver](../light/moxygen/PreviewDriver.md).

## State updates, the no-rebuild contract

When a WS state message arrives, the full tree on connect or a patch after it, for each module in the payload:
- If `controls.deviceName` exists, update the status-bar device name and `document.title`.
- For each `controls[key]`: look up the matching `<input>`/`<span>`/`<progress>`/`<select>` by
  `[data-mid][data-key]`; **skip** if `Date.now() - dragTs[mid:key] < 1000` (user interacting); otherwise patch the value in place (sliders update input + `value-display`; toggles set `checked`; selects set `value`). Then update the reset-to-default "at default?" state and the timing display.

The DOM is **never rebuilt** during a state update. A full re-render happens only on (a) initial load and (b) a mutation that changes the tree shape (`/api/control` for a Select that triggers `rebuildControls`; `/api/modules` add/delete/move/replace).

**Object identity across pushes.** Every WS push replaces `state` with a fresh JSON tree, so a
card-render closure holding a `mod` reference goes stale within ~1 s. Any "which index am I now?" lookup must use `findIndex(c => c.name === mod.name)`, not `indexOf(mod)`.

## Communication client behavior

The endpoints, the `/ws` frame shape, and the streaming state sink are owned by [HttpServerModule](moxygen/HttpServerModule.md); `/api/control` is `{module, control, value}`. The
**browser-side** socket behavior the server doesn't dictate:

- URL `ws://<host>/ws` (same port as HTTP); the server sends the full state on connect, then a patch of what changed each second.
- Client sends `"ping"` every 25 s as keepalive (Safari kills idle sockets otherwise).
- Auto-reconnect on close with exponential backoff (500 ms → 5 s ceiling).
- Pause on `document.visibilityState === 'hidden'`, resume on `pageshow` (Safari bfcache survival).

## localStorage keys

```text
mm_selectedRoot     id of the currently-selected root module    (string)
mm_theme            "dark" | "light"                            (default: "dark")
```

No other client state persists; reorder, control values, etc. all live on the device.

## Source

Hand-maintained files: [index.html](../../../../src/ui/index.html) · [app.js](../../../../src/ui/app.js) · [style.css](../../../../src/ui/style.css) · [preview3d.js](../../../../src/ui/preview3d.js) · [install-picker.js](../../../../src/ui/install-picker.js). Embedded into firmware at build time via [embed_ui.cmake](../../../../src/ui/embed_ui.cmake).

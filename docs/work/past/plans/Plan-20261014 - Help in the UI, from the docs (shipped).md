# Plan: help in the UI, from the headers

Plan, for the product owner's approval (2026-10-14). The app explains itself: every module's card shows its own help, per module and per control, as if it were built in, without a large website behind it. The help is written once, in the module's header next to the code, generated into one file and fetched by the browser, so the firmware carries none of it. The module help takes over what the `?` button does, so `?` goes.

## One source: the header

The header is the module's documentation, the textbook "docs next to the code" that Javadoc and Doxygen teach, and nothing else describes the module. Today two sources describe each module, which this plan ends:

- **The hand-written cards** in `docs/moonmodules/*.md`, which `check_specs` only checks exist and `check_docgen` checks for form.
- **The headers' `///` comments**, which Doxygen and moxygen turn into the technical pages the cards link to.

Every control is therefore described twice, on its card and on its member (BeatRipples' `damping`: "how long the water keeps ringing." on the card, "How long the water keeps ringing." in the code). The header is where the code is, so it stays and the cards go.

```
header  ──clang──▶  generator  ──▶  help.json  ──▶  the app's help
                                              └──▶  the website's module catalog
```

**The end state removes** the ~150 hand-written cards, Doxygen, moxygen and `gen_api`, and the card rules in `check_docgen`. The generated technical pages go too: the header on GitHub is the technical reference, so one parser feeds the help and the catalogs, and no Doxygen or Node is in the docs build. What stays as markdown is what no single module owns: architecture, how-tos, tutorials, contributing.

## Read by clang

The generator reads the headers through clang's AST, not a pattern match, because clang already knows what a pattern would guess:

- **A `///` comment belongs to the declaration below it.** Clang attaches each doc comment to its class, method or member, through macros and templates alike.
- **A control's name and its member are the call's arguments.** `controls_.addControl("damping", damping, 0, 255)` is a call whose first argument is the name and second the member, so the name leads to the member and the member to its `///`. A control without a member of its own (a button, a list, a buffer) takes its help from a `///` on its `add…` line.
- **Through libclang**, the LLVM project's Python bindings, on the LLVM `check_clang_query` already requires. It parses `src/module_types.cpp`, which includes every registered module, with the desktop build's `compile_commands.json`: 145 modules in about a second. Its `registerType<Class>("Type", "page.md#anchor")` calls give each type, its class and its card.

### Why clang came only now

Docs v2 (2026-07-02, `424f2e1f8`) piloted four tools for the technical pages. Doxide's parser choked on ordinary C++20 and dropped enum values. cxxdox, a libclang plugin and the closest to this plan, shipped no macOS wheel, was locked to libclang 21, needed a compile context per header and had almost no users. Doxygen with moxygen parsed everything, rendered whole classes and enum tables, and is the long-standing standard, so it won.

That plan also concluded that controls are runtime `controls_.add(...)` calls no static tool sees. clang's syntax tree does see a call inside a method body, which is what `gen_help` reads.

Three things changed since. LLVM joined the toolchain three weeks later (2026-07-28, `5e49d3370`) for `clang-query` and `clang-tidy`, so the generic `libclang` bindings run on the LLVM the checks need, with no plugin wheel or version lock. The compile context became one file, the registry `src/module_types.cpp` that includes every module. And the goal changed: a technical page per header suits Doxygen, while help needs each control's `add…` name tied to the `///` on the member it binds, which only the syntax tree links. A pilot of the plain bindings rather than the plugin would have shown this in July; cxxdox was the only libclang option on the table then.

## What it removes

Measured on 2026-10-15, as the steps land:

- **`gen_api.py`**, 732 lines, with Doxygen and npx moxygen, their installs in the docs job and the 262 pages they generate: the header on GitHub is the technical reference (step 8).
- **`update_module_docs.py`**, 296 lines, which inserts screenshots into the cards' markdown: the header's `@card` names the picture (step 11).
- **The card-table renderer in `mkdocs_hooks.py`**, most of its 653 lines, with the drawn-card bridge: the deck renders every catalog page (step 11). The test-inventory pages stay.
- **The card rules in `check_docgen`**, while its header rules stay and check the same limits on the header (step 11).
- **The ~150 hand-written cards**, one per module as its header takes over (step 9).

`gen_help.py` grows by the deck's static list, so well over a thousand lines go in all.

## Levels of help

Each module's help opens in levels, more on request, the textbook progressive disclosure:

| Level | What it is | In the header as | In the app |
|---|---|---|---|
| 1. One line | what the module does | the class comment's first line | always visible, under the title |
| 2. User info | how to use it, the picture, a line per control | the rest of the class lead, `@card` for the image, each control's `///` | the summary always shows under the line, the picture on the tap |
| 3. More user info | wiring, examples, pitfalls, what each mode is for | the `## ` sections after `@guide` | what the tap opens, with the picture, attribution and links |
| 4. Technical | how it works inside, design choices, prior art | the `@technical` sections, the members' `///`, and the code | the header on GitHub, its Source link |
| 5. Tests | the behavior that is pinned, as sentences | the test names and descriptions | a link to the test file |

- **A control has two levels**: its line, the member's first `///` line, and its more, the member's `/// @guide` line, such as what each option does. The module's own `@guide` never explains a control.
- **`help.json` carries levels 1 to 3** as text; levels 4 and 5 stay links, being long and written for contributors.
- **Structured facts are block tags**, Javadoc's `@author` construct: a lead line opening `@<tag>` is the help field of that name, so the header names what the file carries. `@card` and `@attribution` exist; more move from prose to fields as the pilot finds them.
- **Every control has its line**: `check_docgen` fails a control whose name leads to no `///`.

## Why it works without flash

The device serves the UI; the **browser** fetches the help whenever it has internet:

- **The docs site allows it.** GitHub Pages answers `moonmodules.org/MoonLight/` with `access-control-allow-origin: *`, so a page served from `http://<device>` may read it. Fetching HTTPS from an HTTP page is allowed; only the reverse is blocked.
- **The browser can keep it.** A device's UI is plain HTTP, not a secure context, so it gets no Service Worker and no Cache API. `localStorage` and IndexedDB work in any context, and each device address is its own origin, so each device's UI keeps its own copy.
- **Comments are not compiled.** The help lives in headers, so the firmware is the same size; the UI grows by the fetch, the cache and the rendering, about 2 to 3 KB in `app.js`.

## The help file

```json
{
  "version": "6.1.0-dev.89",
  "modules": {
    "PictureEffect": {
      "line": "The active Picture service's picture, sampled onto the layer, so a picture of any size shows on a layout of any size.",
      "summary": "A modifier rotates, mirrors or tiles it as it does any effect, and a cube shows it on every slice.",
      "image": "https://moonmodules.org/MoonLight/assets/light/effects/PictureEffect.gif",
      "source": "https://github.com/MoonModules/MoonLight/blob/v6.1.0-dev.89/src/light/effects/PictureEffect.h",
      "tests": "https://github.com/MoonModules/MoonLight/blob/v6.1.0-dev.89/test/unit/light/unit_PictureEffect.cpp",
      "controls": {
        "fit": {
          "line": "Keep the aspect with bands or cropped, or stretch it to the grid.",
          "more": "**fit** shows the whole picture, with bands where its aspect leaves the grid empty. ..."
        },
        "sampling": { "line": "One pixel per light, or the mean of the light's rectangle." }
      },
      "doc": "light/effects.md#picture"
    }
  }
}
```

- **Keyed by the registered type**, which `/api/types` already sends, so the app looks a module up with no table of its own; the display name is the app's own `displayName`, so the file does not repeat it.
- **Inline markdown stays in the text**: code, bold and links. The app renders those three after escaping everything else, and every link is absolute.
- **The picture** is named by its URL on the site, loaded when the help opens, so the flash holds no image.
- **Source and tests link to the tagged release**, so a developer reads the code the device runs.
- **One file**, measured at 196 KB and 62 KB gzipped: one request when the app opens rather than ~150, each needing its own cache entry and failure path.

## Where it is published

- **`docs/help/help.json` is committed**, so every docs deploy publishes it at `help/help.json` beside the site, the docs build draws migrated cards from it without LLVM, and a header edit shows in the diff as the help line it changed. `check_docgen` fails when it is stale.
- **Every release** attaches its own `help.json` as an asset, and the Pages deploy, which already restages the last 5 stable and 5 prerelease releases from their assets on every run, copies it to `install/releases/<tag>/help.json`.
- **A device asks for its own version first**, falling back to the latest for a release older than the ten kept or a local build: one more request, and a newer text for an older firmware still helps more than none. The cost is one asset of about 200 KB per release.

## What the app does

- **Once help is on**, opening the app shows the cached help at once and fetches the file in the background; the browser revalidates with the ETag Pages sends, so an unchanged file costs one small request. A changed one replaces the cache.
- **Without internet**, as on a device's own access point, it uses the cache; with no cache either, the cards show as they do today.
- **Help per module** opens from the card's title in its levels: the line, then the summary, the picture and the controls, then the details, then the links to the source and the tests.
- **Help per control** is Material's helper text, one persistent line under the field, never a hover-only tooltip, which Nielsen Norman Group's guidelines warn hides what people need and fails on touch. The pilot compared it with Pickering's ⓘ toggletip and kept the line, since the status bar's ⓘ already shows or hides all help at once. A control with more opens it from its line, behind the module help's arrow.
- **Off until asked**: the ⓘ in the status bar turns help on, and the first time it asks before downloading, since the request shows GitHub, which hosts the docs site, the browser's address. Privacy by default, GDPR article 25's term; nothing is fetched before the yes. The [privacy policy](../../../legal/privacy-policy.md) says so.

## Steps

The change is gradual, the textbook strangler fig: the new path grows module by module around the old one until the old one is gone. One bridge makes that work: for a module whose header does not carry its help yet, the generator reads the module's card instead, so `help.json` covers every module from the first step. The bridge is named as temporary and goes with the last card. The whole setup is built on this branch, and a commit may carry several steps.

1. ✅ **Pilot.** The generator reads headers through clang into `help.json`, with tests: the line, the summary, a control's help from its member and from its `add…` line, a control with none reported, the `@card` image, the reader marker on a section. Picture, FseqPlayer and BeatRipples move over: their cards' text into their headers, their cards deleted, their catalog rows generated from `help.json`. The app shows their help from a locally built file (`build_docs --serve`), the helper line against the ⓘ toggletip compared on a phone and a laptop, and the setting's default chosen. Done in 61c881a7: the helper line won, help is off until the ⓘ asks and the user agrees, and the six catalog decks draw from the same file.
2. ✅ **Modifiers, the first whole catalog.** All twelve modifiers carry their help in their headers, and their cards are cut. The page itself is generated from `help.json`, the end state of step 11 tried on one catalog: its intro from `ModifierBase`, its cards from the headers, its details from `MoonLiveModifier`'s `@guide`, and a modifier's kind read from its overrides. Done in c51a22fb: `@ref` links between modules, the attribution catalog the `@attribution` names resolve against, the Technical link and popup links in the help, and one `///` line per declaration in the modifier headers.
3. ✅ **Layouts, the second catalog.** All seventeen layouts carry their help in their headers, and `layouts.md` is generated from `help.json`: its intro and clip from `LayoutBase` through `@video`, its cards from the headers, and its details from `MoonLiveLayout`'s `@guide`. The layouts credit what they draw on, Boustrophedon for serpentine wiring, polar coordinates for the rings and a conical spiral, through the attribution catalog. A layout is built from others as objects again, its public settings set and placed through `LightEmitter::place`, the relay the Layouts container uses for its children: CarLights and Rings241 from rings, which gain a center, Tubes from columns, Panels from panels and GridBlacks from a grid. Done in 436f37e9.
4. ✅ **Layouts composed in the tree.** A layout built from other layouts in code, as CarLights is from rings, has a runtime twin: the Layouts container placing each child at a position, so a user builds a car's lights from Ring children without code. To analyze: whether the position is the child's own control, such as a ring's center, or the container's per child, and how the bounding box and the light numbering follow. Done in 436f37e9: the position is the child's own control, Ring's `centerX` and `centerY`, so a user places rings side by side under the Layouts container.
5. ✅ **Effects, the third catalog.** All seventy effects carry their help in their headers, and `effects.md` is generated from `help.json`: its intro from `EffectBase`, its cards from the headers, and its details from each effect's `@guide`. Each header gets the step 13 sweeps as it moves, as the layouts did: `@attribution` for its `Author:` and `Prior art:` lines, no predecessor links, one comment kind per declaration, a line per control and a trimmed `@technical`. Done in f2e1b2ee.
6. ✅ **Drivers, the fourth catalog.** All eleven drivers carry their help in their headers, and `drivers.md` is generated from `help.json`: its intro and its two clips from `DriverBase`, its cards from the headers, and the shared controls read through `DriverBase`'s `defineControls`, as the virtual call resolves it. The card's details sections move by who reads them. In the app the user is at the card with a setting in hand, so the help is a glance: what the driver is, what to pick, the one fix when it does not work. On the website the user is sorting something out, which is a how-to page: [choosing and wiring an LED driver](../../../reference/hardware/led-drivers.md), [driving a HUB75 panel](../../../reference/hardware/hub75-panels.md), [watching the output as video](../../../reference/watching-the-output.md) and the panel-card tutorial. So a control's longer help goes to a `/// @guide` line on its member, the choice to a `@guide` section of the module, which never explains a control, tables, steps and reasons to the how-to page the guide links, the mechanism to `@technical`, credit to `@attribution`, and what the code or another page holds is cut. `check_docgen` caps the app help on every generated catalog: 40 words for a control's longer help, 60 for a guide section, 250 for the whole. Done in ef20e444.
7. ✅ **System, Services and Supporting, the last catalogs.** Their modules carry their help in their headers by step 6's rules, and the three pages are generated: Services from its container's class comment, which is then no card, and System and Supporting from their own markdown kept as the intro the deck follows. Supporting's classes that are no module become one linked list. Control's details move into the control-surface, presets and home-automation how-tos, and Audio's into a new audio-input how-to. The separate deck pages go, every catalog page being a deck. Done in 18d069d1.
8. ✅ **The header is the technical reference.** Doxygen, npx moxygen and `gen_api` go, with their installs in the docs job and the 262 pages they generate. The header is what a developer reads, in the repo or on GitHub, so a second rendering of it is a copy to keep in step. Every link into `moxygen/` becomes a link to the header, which the docs build already rewrites to its file on GitHub, and the help's Technical link goes, Source being the same file. `check_docgen` follows: an `@xref` still names a heading in its own file, read as text, and the rules that measured the generated pages (a page nothing links to, an appendix section no page links) go or read the header alone. The supporting lists link headers the same way. Done.
9. ✅ **The migration**, module by module, each card deleted as its header takes over, so the site and the app stayed complete throughout. All 145 modules take their help from their headers, and a card's details section moved as step 6 sets out. The card bridge in `gen_help` is gone, `help.json` coming out byte for byte the same without it. Done.
10. ✅ **The checks.** `check_docgen` checks the help in headers. Done: a stale `help.json` fails it, drawn cards meet the card limits, a control line may not state its default or range, every module and every control has its help line, the app help stays within its caps, a control's guide does not repeat its line, the module guide explains no control, a function carrying a macro such as `MM_NONBLOCKING` has its `///`, and every tag emoji groups at least two modules or factory scripts; `check_specs` counts a module documented when the help reads its header. Reading declarations from the clang AST rather than by pattern is in [the core backlog](../../future/backlog-core.md).
11. ✅ **The end state.** The `?` button is gone, the help's Docs link taking its place. A catalog page writes each module under its deck as plain markdown from `help.json`, its name bold rather than a heading so the table of contents lists no card, which search indexes and a reader without JavaScript reads. `split_blocks`, `fill_cards_from_help`, the table renderer, `update_module_docs.py` and the table styles are gone. `check_docgen`'s card rules read `help.json`: the picture and its format, a control line's width, and a line stating its default or range. `check_specs` counts a module documented when the help reads its header, so its page lookup and drift guards are gone.
12. ✅ **The attribution catalog as a deck.** [The attribution catalog](../../../reference/attributions.md) is a card deck drawn from its own table. The table stays the one home, which search indexes and a reader without JavaScript sees, and the deck script turns its rendered rows into cards. A card shows the entry's picture, name and what it is known for, and its back the modules that name it. Its chips are a Kind column: 🧑 a person, 🏢 a company or shop, 🧰 a project or library, 📜 a standard or protocol, 🧮 an algorithm, 🎨 an art form or culture. 🧰 replaces the planned 📦, which the app's chips use for a compiled module. One card builder and one deck serve both kinds of card.
13. 🚧 **The sweeps**, the last commits before this branch merges: each a mechanical commit of its own, apart from behavioral changes, and under 100 files so a review still reads it whole. In order of size, counted on 2026-10-15:

    | # | Sweep | Status or size |
    |---|---|---|
    | ✅ | The category prefix, links into the predecessor's repo, a line per control, defaults and ranges | 0 left each, defaults and ranges counted over every module in `help.json` |
    | 1 ✅ | `@ref` for links to modules | FseqPlayer's and Picture's links are `@ref`, and `gen_help` reads Doxygen's `@ref Name "text"` |
    | 2 ✅ | `@attribution` for `Prior art:` | 0 left: the peripherals credit through `@attribution`, an origin of MoonLight alone went, and a design pattern stays prose |
    | 3 ✅ | Every attribution attributed | The Attributed by column also reads any source file's `@attribution`, which 18 files outside the modules carry; Verlet became Semi-implicit Euler, and MQTT, GLSL and raster-fonts joined the catalog. RobWomp credits on Hub75Driver; Quindor / QuinLED and WLED-MM have no home in the code yet |
    | 4 ✗ | Display names | dropped: a label beside the derived name gave a type two spellings, and one name everywhere is the simpler rule |
    | 5 ✅ | One comment kind per declaration, and its rule | 561 notes in 92 headers, class and file scope; `check_docgen` reports a `//` on a declaration |
    | 6 ✅ | `@defgroup` goes | 137 files and three generators; a file's lead is its first `///` block before any code |
    | 7 ✅ | `@moreinfo` becomes `@technical` | 273 files: the marker, the generators, `check_docgen` and `check_clang_query`, the standards and the tests; the past plans keep the old name as history |
    | 8 → | `@technical` trimmed, Guides that add | moved to [the processes plan](../../present/Plan-20261015%20-%20Processes%2C%20described%20once%20(17%20of%2023).md), step 18 |
14. ✅ **Publishing.** Each release attaches its `help.json` as `MoonLight-help.json`, which the Pages deploy mirrors beside the release's binaries, and the app reads the copy of the release its device runs, falling back to the site's `help/help.json`. Both answered after the merge's run, and the fallback to the branch's copy is gone.

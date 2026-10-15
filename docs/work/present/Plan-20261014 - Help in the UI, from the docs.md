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

## What it removes

Measured on 2026-10-15, as the steps land:

- **`gen_api.py`**, 732 lines, with Doxygen and npx moxygen, their installs in the docs job and the 262 pages they generate: the header on GitHub is the technical reference (step 8).
- **`update_module_docs.py`**, 296 lines, which inserts screenshots into the cards' markdown: the header's `@card` names the picture (step 12).
- **The card-table renderer in `mkdocs_hooks.py`**, most of its 653 lines, with the drawn-card bridge: the deck renders every catalog page (step 12). The test-inventory pages stay.
- **The card rules in `check_docgen`**, while its header rules stay and check the same limits on the header (step 12).
- **The ~150 hand-written cards**, one per module as its header takes over (step 9).

`gen_help.py` grows by the deck's static list, so well over a thousand lines go in all.

## Levels of help

Each module's help opens in levels, more on request, the textbook progressive disclosure:

| Level | What it is | In the header as | In the app |
|---|---|---|---|
| 1. One line | what the module does | the class comment's first line | always visible, under the title |
| 2. User info | how to use it, the picture, a line per control | the rest of the class lead, `@card` for the image, each control's `///` | the summary always shows under the line, the picture on the tap |
| 3. More user info | wiring, examples, pitfalls, what each mode is for | the `## ` sections after `@guide` | what the tap opens, with the picture, attribution and links |
| 4. Technical | how it works inside, design choices, prior art | the `@moreinfo` sections, the members' `///`, and the code | the header on GitHub, its Source link |
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
- **Off until asked**: the ⓘ in the status bar turns help on, and the first time it asks before downloading, since the request shows GitHub, which hosts the docs site, the browser's address. Privacy by default, GDPR article 25's term; nothing is fetched before the yes. The [privacy policy](../../legal/privacy-policy.md) says so.

## Steps

The change is gradual, the textbook strangler fig: the new path grows module by module around the old one until the old one is gone. One bridge makes that work: for a module whose header does not carry its help yet, the generator reads the module's card instead, so `help.json` covers every module from the first step. The bridge is named as temporary and goes with the last card. The whole setup is built on this branch, and a commit may carry several steps.

1. ✅ **Pilot.** The generator reads headers through clang into `help.json`, with tests: the line, the summary, a control's help from its member and from its `add…` line, a control with none reported, the `@card` image, the reader marker on a section. Picture, FseqPlayer and BeatRipples move over: their cards' text into their headers, their cards deleted, their catalog rows generated from `help.json`. The app shows their help from a locally built file (`build_docs --serve`), the helper line against the ⓘ toggletip compared on a phone and a laptop, and the setting's default chosen. Done in 61c881a7: the helper line won, help is off until the ⓘ asks and the user agrees, and the six catalog decks draw from the same file.
2. ✅ **Modifiers, the first whole catalog.** All twelve modifiers carry their help in their headers, and their cards are cut. The page itself is generated from `help.json`, the end state of step 12 tried on one catalog: its intro from `ModifierBase`, its cards from the headers, its details from `MoonLiveModifier`'s `@guide`, and a modifier's kind read from its overrides. Done in c51a22fb: `@ref` links between modules, the attribution catalog the `@attribution` names resolve against, the Technical link and popup links in the help, and one `///` line per declaration in the modifier headers.
3. ✅ **Layouts, the second catalog.** All seventeen layouts carry their help in their headers, and `layouts.md` is generated from `help.json`: its intro and clip from `LayoutBase` through `@video`, its cards from the headers, and its details from `MoonLiveLayout`'s `@guide`. The layouts credit what they draw on, Boustrophedon for serpentine wiring, polar coordinates for the rings and a conical spiral, through the attribution catalog. A layout is built from others as objects again, its public settings set and placed through `LightEmitter::place`, the relay the Layouts container uses for its children: CarLights and Rings241 from rings, which gain a center, Tubes from columns, Panels from panels and GridBlacks from a grid. Done in 436f37e9.
4. ✅ **Layouts composed in the tree.** A layout built from other layouts in code, as CarLights is from rings, has a runtime twin: the Layouts container placing each child at a position, so a user builds a car's lights from Ring children without code. To analyze: whether the position is the child's own control, such as a ring's center, or the container's per child, and how the bounding box and the light numbering follow. Done in 436f37e9: the position is the child's own control, Ring's `centerX` and `centerY`, so a user places rings side by side under the Layouts container.
5. ✅ **Effects, the third catalog.** All seventy effects carry their help in their headers, and `effects.md` is generated from `help.json`: its intro from `EffectBase`, its cards from the headers, and its details from each effect's `@guide`. Each header gets the step 14 sweeps as it moves, as the layouts did: `@attribution` for its `Author:` and `Prior art:` lines, no predecessor links, one comment kind per declaration, a line per control and a trimmed `@moreinfo`. Done in f2e1b2ee.
6. ✅ **Drivers, the fourth catalog.** All eleven drivers carry their help in their headers, and `drivers.md` is generated from `help.json`: its intro and its two clips from `DriverBase`, its cards from the headers, and the shared controls read through `DriverBase`'s `defineControls`, as the virtual call resolves it. The card's details sections move by who reads them. In the app the user is at the card with a setting in hand, so the help is a glance: what the driver is, what to pick, the one fix when it does not work. On the website the user is sorting something out, which is a how-to page: [choosing and wiring an LED driver](../../how-to/led-drivers.md), [driving a HUB75 panel](../../how-to/hub75-panels.md), [watching the output as video](../../how-to/watching-the-output.md) and the panel-card tutorial. So a control's longer help goes to a `/// @guide` line on its member, the choice to a `@guide` section of the module, which never explains a control, tables, steps and reasons to the how-to page the guide links, the mechanism to `@moreinfo`, credit to `@attribution`, and what the code or another page holds is cut. `check_docgen` caps the app help on every generated catalog: 40 words for a control's longer help, 60 for a guide section, 250 for the whole. Done in ef20e444.
7. ✅ **System, Services and Supporting, the last catalogs.** Their modules carry their help in their headers by step 6's rules, and the three pages are generated: Services from its container's class comment, which is then no card, and System and Supporting from their own markdown kept as the intro the deck follows. Supporting's classes that are no module become one linked list. Control's details move into the control-surface, presets and home-automation how-tos, and Audio's into a new audio-input how-to. The separate deck pages go, every catalog page being a deck. Done in 18d069d1.
8. ✅ **The header is the technical reference.** Doxygen, npx moxygen and `gen_api` go, with their installs in the docs job and the 262 pages they generate. The header is what a developer reads, in the repo or on GitHub, so a second rendering of it is a copy to keep in step. Every link into `moxygen/` becomes a link to the header, which the docs build already rewrites to its file on GitHub, and the help's Technical link goes, Source being the same file. `check_docgen` follows: an `@xref` still names a heading in its own file, read as text, and the rules that measured the generated pages (a page nothing links to, an appendix section no page links) go or read the header alone. The supporting lists link headers the same way. Done.
9. ✅ **The migration**, module by module, each card deleted as its header takes over, so the site and the app stayed complete throughout. All 145 modules take their help from their headers, and a card's details section moved as step 6 sets out. The card bridge in `gen_help` is gone, `help.json` coming out byte for byte the same without it. Done.
10. ✅ **The checks.** `check_docgen` checks the help in headers. Done: a stale `help.json` fails it, drawn cards meet the card limits, a control line may not state its default or range, every module and every control has its help line, the app help stays within its caps, a control's guide does not repeat its line, the module guide explains no control, a function carrying a macro such as `MM_NONBLOCKING` has its `///`, and every tag emoji groups at least two modules or factory scripts; `check_specs` counts a module documented when the help reads its header. Reading declarations from the clang AST rather than by pattern is in [the core backlog](../future/backlog-core.md).
11. 🚧 **Publishing**: the docs deploy and the per-release asset. The docs deploy publishes `help/help.json` once this branch merges, as the file sits in `docs/`. Each release attaches its `help.json` as `MoonLight-help.json`, which the Pages deploy mirrors beside the release's binaries, and the app reads the copy of the release its device runs, falling back to the site's. Until then the app reads the file from this branch on GitHub when the published one is missing, a fallback that goes with the merge.
12. **The end state**: `?` goes, and the docs build's card machinery with the last card. Under each deck the page carries each module's heading, line and controls as plain markdown, written by `generated_catalog` from `help.json`: the site search indexes it and a reader without JavaScript reads it, and the deck replaces it once its script runs (progressive enhancement). That retires `split_blocks`, `fill_cards_from_help`, `_render_catalog_table` and `update_module_docs.py`, and the card rules in `check_docgen` and `check_specs` read `help.json` instead of the drawn card.
13. **The attribution catalog as a deck.** [The attribution catalog](../../reference/attributions.md) becomes a card deck like the module catalogs: a card per entry with its picture, name and what it is known for on the face, and the modules that name it on the back, filtered and searched the same way. To decide where the deck reads its data, the table as it is or a file `gen_help` writes beside `help.json`. Its filter chips are an emoji per kind of entry, a candidate set being 🧑 a person, 🏢 a company or shop, 📦 a project or library, 📜 a standard or protocol, 🧮 an algorithm and 🎨 an art form or culture such as the demoscene, each entry carrying one or more.
14. **The sweeps**, the last commits before this branch merges: each a mechanical commit of its own, apart from behavioral changes, and under 100 files so a review still reads it whole. Counted on 2026-10-15:
    - **`@attribution`**: every `Author:` line and `Prior art:` paragraph becomes the block tag, 106 of them in 95 header and source files counted on 2026-10-16, so every module's attribution is a field rather than a summary paragraph. One that names only MoonLight, or links into the predecessor's repo, goes rather than moves. Each names entries of [the attribution catalog](../../reference/attributions.md), seeded from the README's Credits, and adds the people, projects and algorithms it lacks; an origin of MoonLight alone goes, since the 💫 tag says it. The modifiers already have it.
    - **Every attribution attributed**: each entry of [the attribution catalog](../../reference/attributions.md) is named by at least one module, so its Attributed by column is never empty; 45 of 58 are today. An entry that credits the project as a whole rather than a module, such as WLED or Piper, is named where its idea lives, in the header or source file that carries it. Once none is empty, a check keeps it so.
    - **One comment kind per declaration**: 342 declarations in 96 headers carry a `//` above them, which no page and no help reads; each becomes the one `///` line, merged with any `///` beside it, and a `check_docgen` rule then keeps `//` inside bodies. The modifiers already have it.
    - **`@moreinfo` becomes `@technical`**: the marker takes the name of the help level it feeds, pairing with `@guide` as the developers' half to the users'. 382 markers in 279 files, counted on 2026-10-16, so it lands in commits under 100 files each: the generators, `check_docgen` and the standards accept both names until the last header moves, then the old name goes.
    - **Guides that add**: a control's `@guide` line says only what its line cannot hold, and a module with depth gets a guide about itself, how it works and how to get a look, rather than one walking its controls. `check_docgen` flags a control's guide that opens by repeating its line; the rest is judged in review. The effects already have it.
    - **`@moreinfo` trimmed**: an appendix keeps only what neither the code, the test names, the base class nor the help says, one or two sentences each, with no history of the predecessor. A fact every module of a role shares, such as float maths being fine at rebuild, is said once in the role's base class. The modifiers and layouts already have it.
    - **The category prefix**: 28 class comments open with `Effect: …`. That first line is the module's help line under its title, where the role emoji already names the category, so the prefix goes.
    - **A line per control**: the controls without a `///`, 21 modules' worth today and mostly read-only status values, get one, and step 10's check then blocks a control without help.
    - **Defaults and ranges in member comments**: the control-line rule the catalog cards meet, applied to the headers the drawn cards and the app read.
    - **Links into the predecessor's repo**: the `Origin:` lines and `Author:` comments credit MoonLight with a link into `github.com/ewowi/MoonLight`, 91 of them in 49 files, about 2.5% of `help.json`. Credit says MoonLight without the link; the README's credits keep the one link to the repo. The modifiers already have it.
    - **`@ref` for links to modules**: a header names another module or class as `@ref ClassName`, and the generator links it to the card or the technical page. The modifiers already have it. Two header links are left, FseqPlayer's to Network Receive and Picture's to the Picture service, the second wanting Doxygen's `@ref Name "text"` form for its own wording. The links between catalog cards, about 50, become `@ref` as each card moves into its header.
    - **`@defgroup` goes**: a free-function header leads with `@defgroup` and closes it with `@{` and `@}`, Doxygen's grouping, which nothing reads once step 8 removes Doxygen. 134 headers carry one, so the lead becomes plain `///` lines in commits under 100 files each, and `check_docgen`'s file-lead rule reads the `///` run instead.
    - **Display names**: the app's `typeDisplayName` shows the type name without its role noun, so cards and the picker read "Hls", "Ndi", "ParallelLed" and "NetworkSend" where a user expects "HLS", "NDI", "Parallel LED" and "Network Send". To decide where a module's display name lives, then apply it everywhere a type is shown.

# Plan: presets are state documents

A preset becomes any part of the device's state, written as one hierarchical JSON document, and the REST API applies the same document in one call. A palette alone, brightness plus a palette, or an effect with its controls is then one preset and one request.

**The end goal is one JSON for everything that holds state.** A gallery entry, a preset file, the body of a REST call, what a card's `{ }` button shows, a config file, a device model and a scenario fixture are the same document, so any of them can be copied into any other unchanged. A look found in the gallery is pasted into a pad, a card's state is pasted into a gallery issue, and a curl body is saved as a preset.

A document is always rooted at the top level (`{"Effects":{"Layer":{...}}}`), so whatever the device shows applies as it is, without the reader adding the path.

## Why

- **A preset is limited to one whole container today.** It captures Layouts, Effects, Drivers or Services, so a palette can only be saved together with the driver's pins and brightness, and a palette-only preset cannot exist.
- **Adding an effect with its controls takes several requests.** `POST /api/modules` takes a type, an id and a parent and nothing else, so every control is a separate `POST /api/control` afterwards. A user has reported this.
- **Five formats describe the same tree.** Each one has its own parser and its own gaps:

| Where | Shape |
|---|---|
| Config files and preset files | flat dotted keys with positional children: `"Effects.0.0.type":"BouncingBallsEffect"` |
| REST | `POST /api/modules` per module, then `POST /api/control` per control |
| Improv `APPLY_OP` | one op per frame: `{"op":"add",...}`, `{"op":"set",...}` |
| `deviceModels.json` | a list of `{type, id, parent_id, controls}` |
| Scenario fixtures | `add_module` steps with `props` |
| A card's `{ }` button | `/api/modules/<name>`: control descriptors, values and live telemetry, for issue reports |

**WLED works the way this plan proposes.** A WLED preset is a stored body for `/json/state`: applying it is posting it, it may be partial (`{"bri":40}`), and its save dialog's checkboxes are shortcuts that decide which parts of the current state go in. A playlist is a preset listing other presets with durations. The difference here is that the state is a tree of modules rather than one fixed document, so a document must also be able to create and remove modules.

## The document

The document mirrors the module tree, keyed by module name, which the device already keeps unique:

```json
{ "Drivers": { "palette": "Ocean", "brightness": 40 } }
```

```json
{ "Effects": { "Layer": { "Noise": { "type": "NoiseEffect", "speed": 3, "scale": 40 } } } }
```

The reader exists: `json::parse` in [JsonUtil.h](../../../src/core/util/JsonUtil.h) builds a full tree with members in document order, bounded at depth 64, so the work is the applier, not a parser.

Applying it is **JSON Merge Patch** ([RFC 7386](https://www.rfc-editor.org/rfc/rfc7386)), the standard for exactly this:

- **A scalar or an array sets a control** of the module it sits in, through the same write path `/api/control` uses, so every validator runs and a palette or Select takes a name.
- **An object is a child module.** It is found by name; when it is missing and the object names a `type`, it is created under that parent. A missing module without a `type` is an error the response names.
- **`null` removes a module.**
- **Key order is creation order**, so layers and effects come out in the order the document lists them.
- **One prepare at the end**, not one per module, which is what makes a large document cheap.

A merge leaves everything the document does not mention alone, which is right for "brightness 40" and wrong for "this is the effects stack": an effects preset must not keep the effects that were running before it. So a module object may say its children are exactly the ones listed, which removes the others first. Merge Patch has no word for that, so it takes the directive Kubernetes' strategic merge patch uses for the same gap: `"$patch": "replace"`. A `$` prefix is one no control name can start with.

## REST

`PATCH /api/state` takes a document and answers with what it applied, or with the first error and where it was. `POST` is accepted as well, for clients that cannot send `PATCH`. The existing `/api/modules` and `/api/control` stay, since each is the one-step case of the same engine. The user's complaint becomes one request:

```json
PATCH /api/state
{ "Effects": { "Layer": { "Noise": { "type": "NoiseEffect", "speed": 3 } } } }
```

## Presets

- **A preset file is a document**, plus its own fields: `slot` for its pad, and its name as the file name. The loader takes `slot` off the root before the document reaches the state engine, so it is never read as a module. The File Manager reaches `/.config/presets` with hidden files shown, and its editor edits JSON, so editing a preset by hand needs no new UI.
- **Capturing is a shortcut that writes a document.** "Save layouts / effects / drivers / services" writes that container's subtree with `"$patch": "replace"`. A **save as preset** button on a module's card writes that module's current controls only. Selecting what to include beyond that is a later step.
- **A preset's role follows from the containers it touches.** A preset touching only Effects is an effects preset and shows as the active effects look, as now; one touching several is a mixed preset. The `captures` field goes.
- **Playlists are out of scope.** A WLED playlist runs presets one after another with durations; here that would later be a preset whose document lists presets and times, on top of this engine.

## End states

| | What uses documents | Gains | Loses |
|---|---|---|---|
| **A** | REST and presets | the two user-visible needs, smallest change | config files keep the flat positional format, so two formats describe a subtree |
| **B** (decided) | A, the card's `{ }` and the gallery, then config files, `deviceModels.json` and scenario fixtures | one format everywhere; the positional key code and three list formats go | more steps; config files change shape, which MIGRATING records |
| **C** | A plus `deviceModels.json` | the installer's tree in the same shape as presets | the config-file duplication stays |

One shape stays outside the document, because it carries something a document does not:

- **The UI's schema** (`GET /api/state` and the WebSocket) carries each control's type, range and options, which a page needs to draw a card. It describes controls rather than holding state, so it stays; the values inside it match the document's.

## Steps

1. ✅ **The apply engine in core.** A `StateDocument` applier walking the parsed document against the tree: set, create, remove, `$patch: replace`, one prepare at the end, and a result naming the first failure. Unit tests: each rule, a missing type, an unknown control, `$patch: replace` removing the rest, key order kept, and a malformed document changing nothing.
2. ✅ **`PATCH /api/state`** on the engine, with an HTTP test and a scenario that adds an effect with its controls in one request, in-process and live.
3. ✅ **Presets as documents.** Save writes a document, apply runs the engine, the role follows from the containers touched. A palette-only preset applied over a running effects preset changes the palette and nothing else, pinned by a test and a scenario. `migrate.js` converts an old flat preset file on restore.
4. ✅ **A card's `{ }` shows its document.** The card's module as a document rooted at the top level, from the writer a preset save uses, with a copy button, so it pastes into a preset, a gallery issue or a curl call unchanged. The diagnostic dump with descriptors and telemetry stays reachable at `/api/modules/<name>` for issue reports, as a link beside it.
5. ✅ **A save-as-preset button** on each module card, saving the document step 4 shows.
6. ✅ **The gallery in the UI.** A Gallery tab browses [MoonLight-Gallery](https://github.com/MoonModules/MoonLight-Gallery)'s `index.json`, with each entry's picture or video, and installs one with a click. The browser does the fetching, as the Firmware card's release picker does: GitHub serves the gallery's files to any page (`Access-Control-Allow-Origin: *`), so the device needs no internet access or TLS of its own. A preset is applied through `PATCH /api/state` or saved into `/.config/presets`; a script is written into `/moonlive`. An entry made on a newer MoonLight than the device is marked as such. **Fill the pads with the most liked**: one action installs the top-voted looks onto the empty pads, by `index.json`'s votes, leaving the user's own presets where they are. Only Effects-only entries and palettes qualify, since a preset setting pins or geometry is tied to one rig, and an entry using a MoonLive script installs its script with it. The gallery's own check refuses a flat-format preset and names the conversion, so every accepted entry is a document. Builds on steps 1 to 3, since the gallery's presets are documents.
7. ✅ **A gallery that holds a hundred entries.** The Gallery renders every entry as a card with its full-size picture, so a hundred entries pull about a hundred animated GIFs of around 1 MB each, and a video shows only as a link.
   - **A thumbnail made at acceptance.** `accept.yml` takes one still frame with ffmpeg, from a picture, an uploaded video or a YouTube link's fixed thumbnail, as a WebP of about 15 KB under `thumbs/`, and `index.json` gains `thumb`. A hundred entries then cost about 1.5 MB.
   - **A paged grid of thumbnails**, about 12 to a page with prev and next, in place of one long list of cards.
   - **Search, filter and sort.** A search box over name, description and author, a chip per kind (Preset, each script kind), and most liked or newest.
   - **An entry opens into its detail view**, where the moving preview plays, a video preferred over a GIF since it is about a tenth of the size, with the description and add to pad, try now and install. Prev and next there step through the filtered entries.
   - Tests: a 100-entry index renders one page of thumbnails, no full-size media until an entry opens, and search and a kind chip narrow the list; the gallery's tool test covers the thumbnail field.
8. 🚧 **(B, on `next-iteration`)** Config files, `deviceModels.json` and scenario fixtures as documents, each removing its old reader, in four commits. Today five sequences bring a module to life at runtime (the document applier, HTTP's `applyAddModule`, the config loader's `applyNode`, the scenario runner's `add_module`, and the list walkers over HTTP), and they differ in naming, role checks, lifecycle and notify.
   - ✅ **8a. One create path in core.** The document applier is the only place a module is created, replaced or removed: `POST /api/modules`, `/replace`, `DELETE` and clear children each apply a one-member document through `applyStateAt`, and `applyDeleteModule` and `applyReplaceModule` join `applyAddModule` as transport-free operations the tests drive. A replace is the parent's children listed in order under `"$patch":"replace"`, the replacement in the old one's place. Naming: `POST /api/modules` refuses an `id` some module holds with a 409; without an `id`, and for a replacement's new name, the type's default takes a free `-N` suffix (`Scheduler::freeName`); an APPLY_OP `add` merges, as a document does, so a catalog entry applied twice changes nothing. `replacementName` stays, since a document is keyed by name and the caller chooses it. `AlreadyExists` and the handlers' own create, replace and delete sequences went. Until 8c and 8d, `moondeck.py` and the live scenario runner read a 409 as already there. The Improv `state` op moves to 8c, where the installer first sends one.
   - ✅ **8b. Config files as documents** (verified on the desktop, the S3 testbench and the Olimex). `/.config/Effects.json` holds `{"Effects":{…}}`, written by `writeStateMember` with secrets. The engine takes a `StateSource`: `Stored` applies a file leniently (no up-front check, an unplaceable member skipped and logged, values clamped twice around a `rebuildControls`, an unknown key dropped, a script's key deferred, a code-wired child kept, a name clash taking a free name), and `Boot` does the same before setup with no lifecycle, reactions or dirty marks. Boot's phase 5 sets the deferred controls, so `reapplyValues`, `requestValuesReapply`, the dedupe pass (`ensureUniqueName`, `deduplicateNamesInTree`), `applyNode`, `writeNode`, `saveSubtreeTo`, `applySubtree`, `restoreName` and `$name` went. `migrate.js` converts a flat config on restore with `configToDocument`, naming modules from the live tree so a code-wired child such as `Stats` keeps its name, and `diffRestore` reads documents. MoonBase's `ConfigScrape.h` reads a child's key among its own members. The 6.0 network adoption runs only on a flat file. **Temporary, until the release after next**: the boot load converts a flat file itself (`flatToStateDocument`, naming children from `$name` or the live tree) and saves it as a document, so an update needs no backup, and MoonBase's scraper reads both formats. Desktop: the user's real flat config booted on this build into the same 246-entry tree, every file converted and saved as a document. Boards on 2026-10-08: the S3 testbench (.158) and the Olimex (.207, classic without PSRAM) each converted every file at first boot into the same tree, the largest free block 52 KB and 76 KB; the Olimex's new MoonBase came up on its network from the flat files and again from the documents. A stored file sets only persistable controls, found on the Olimex, whose old file held the pad editor's live inputs.
   - ✅ **8c. `deviceModels.json` as documents** (desktop tests; a board provisioning run waits). Each entry's `modules` list is a `state` document, a container that gains children carrying `"$patch":"replace"`. APPLY_OP carries a state document, one per top-level container (the largest is 236 bytes), applied by the engine, so `applyOp`, `applyClearChildren` and the add/set/clearChildren ops went with `config-ops.js` and `plan_config_ops`. `moondeck.py` pushes one `PATCH /api/state`; `check_devices` validates the document. `"$patch":"replace"` keeps a module main.cpp wires, for every source: the clear-children pass the installer ran deleted WiFi, the access point, MQTT and Devices on the 18 entries listing a Network child. A document sets a list control whole, so a catalog entry can now seed list rows.
   - ✅ **8d. Scenario fixtures as documents** (desktop). A scenario names its top-level containers in `top` and changes the tree only through `apply_state` documents: a member with a `type` adds, a new `type` replaces, `null` removes, `"$patch":"replace"` clears; with `optional` an unknown type is a skip. One table in `module_types.cpp` (`createTopLevel`) builds each container as the device boots it, with the apparatus main.cpp wires into it, and main.cpp and both runners use it. Effects finds the top-level `Layouts` and Drivers the top-level `Effects` by type when they prepare (`Scheduler::topOfType`), so nothing is wired by hand; the setters stay as injection for the unit-test rigs. The live runner snapshots each container's document before a run and applies it back after, which restores modules, types, order and values in one request each. Removed: the `add_module`, `remove_module`, `delete_module`, `clear_children` and `replace_module` ops in both runners with `id`/`parent_id`/`props` steps, `wireModule`, the runner's id map and `purgeSubtree`, and the live runner's tree snapshot, re-create walk and type swap-back. Scheduler setup's phase 1 binds controls from scratch. Some in-process scenarios now carry a container's real apparatus (Preview and FixtureProfiles under Drivers, Audio under Services), so their recorded numbers move once.
   - 🚧 Restore an old backup through the File Manager on this build: a bundle made on a device still on the flat format, restored onto a device on documents, every module, name and value back, and the restore report clean. The conversion is verified in Node on the desktop's real config and the boot conversion on the desktop, the S3 and the Olimex; the clicked-through Restore is not.
   - Verification:
     - ✅ Desktop first: the unit tests, the scenarios, and the user's real config booted from documents.
     - ✅ A board booting from document configs: the Olimex (classic, no PSRAM) at a 76 to 88 KB largest free block, up from 62 KB, and the S3 at 52 KB.
     - ✅ Provisioning a board over Improv with MoonDeck's `improv_provision.py`, which sends the same per-container documents: 5 of 5 on the Olimex, 6 of 6 on the S3 over its native USB.
     - 🚧 The web installer itself (browser, Web Serial) provisioning a board on this build: its JS is unit-tested, not yet run against a board.

## Subtraction

- ✅ The `captures` field, and the capture-role select: the pad editor names a container, any card saves itself.
- ✅ The `{ }` button's diagnostic dump as its first view, kept as a link.
- ✅ A list persisting by default: only a list that restores its rows is written, so the task and pin snapshots no longer reach flash.
- ✅ Under B: HTTP's own create, replace and clear code; the positional dotted-key writer and reader (`saveSubtreeTo`, `applySubtree`, `$name`) and the values reapply; the `deviceModels.json` module list with `config-ops.js`, `plan_config_ops` and the list walkers; and the scenario runner's separate `add_module` props path.

## Verification

- ✅ Desktop: the unit tests and scenarios above; `PATCH /api/state` adding an effect with controls on the desktop app, and on the Olimex.
- ◐ A board: documents applied on the S3, six over Improv and its config files of up to 2.3 KB at boot. 🚧 A deliberately large preset on the S3, the stack and heap case the recursive reader raises, is not yet tried.
- 🚧 The product owner: a palette-only preset on a pad, applied over an effects preset, on the desktop and on a board.

## Decided

- **The directive is `"$patch": "replace"`**, Kubernetes' spelling for "these are exactly the children", so it reads as the known convention.
- **The per-module save button is in the presets branch** (step 4).
- **One JSON for everything that holds state is the end goal** (the product owner, 2026-10-04): B, with the card's `{ }` and the gallery in it.
- **B is the end state.** Steps 1 to 7 leave stored config untouched; step 8 follows on a branch of its own. C was rejected: it converts an installer file and keeps the firmware's own duplication, the flat config format beside the document engine.

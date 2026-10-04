# Plan: presets are state documents

A preset becomes any part of the device's state, written as one hierarchical JSON document, and the REST API applies the same document in one call. A palette alone, brightness plus a palette, or an effect with its controls is then one preset and one request.

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
| **B** (decided) | A, then config files, `deviceModels.json` and scenario fixtures | one format everywhere; the positional key code and three list formats go | more steps; config files change shape, which MIGRATING records |
| **C** | A plus `deviceModels.json` | the installer's tree in the same shape as presets | the config-file duplication stays |

Improv's `APPLY_OP` keeps its one-op frames under every option, since a frame holds 128 bytes and a document does not fit; it is a transport limit, not a second format worth removing.

## Steps

1. 🚧 **The apply engine in core.** A `StateDocument` applier walking the parsed document against the tree: set, create, remove, `$patch: replace`, one prepare at the end, and a result naming the first failure. Unit tests: each rule, a missing type, an unknown control, `$patch: replace` removing the rest, key order kept, and a malformed document changing nothing.
2. 🚧 **`PATCH /api/state`** on the engine, with an HTTP test and a scenario that adds an effect with its controls in one request, in-process and live.
3. 🚧 **Presets as documents.** Save writes a document, apply runs the engine, the role follows from the containers touched. A palette-only preset applied over a running effects preset changes the palette and nothing else, pinned by a test and a scenario. `migrate.js` converts an old flat preset file on restore.
4. 🚧 **A save-as-preset button** on each module card.
5. 🚧 **(B, its own branch)** Config files as documents, then `deviceModels.json` and scenario fixtures, each removing its old reader. Config files change shape on disk and no compatibility code reads the old one, so it ships with a back up, update, restore note, ideally in the release that already asks for a restore.

## Subtraction

- The `captures` field and the capture-role select.
- Under B: the positional dotted-key writer and reader (`saveSubtreeTo`, `applySubtree` and their prefixes), the `deviceModels.json` module list, and the scenario runner's separate `add_module` props path.

## Verification

- Desktop: the unit tests and scenarios above; `curl -X PATCH` adding an effect with controls on the desktop app.
- A board: a document applied on the S3, since the JSON reader is recursive and a large preset is a stack and heap question there.
- The product owner: a palette-only preset on a pad, applied over an effects preset, on the desktop and on a board.

## Decided

- **The directive is `"$patch": "replace"`**, Kubernetes' spelling for "these are exactly the children", so it reads as the known convention.
- **The per-module save button is in the presets branch** (step 4).
- **B is the end state.** Steps 1 to 4 are the presets branch and leave stored config untouched; step 5 follows on a branch of its own. C was rejected: it converts an installer file and keeps the firmware's own duplication, the flat config format beside the document engine.

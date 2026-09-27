# Run files

A run file lists what a person does in the UI, each step carrying what it should produce and how it is narrated. They live in `test/uiscenarios/clips/`, beside the test that performs them; the engine is here in `moondeck/uiscenario/`. That is the same split the pipeline scenarios use: data under `test/`, runner under `moondeck/`. One file, two consumers:

- **`test/uiscenarios/`** performs the steps and checks the `expect` blocks. A failure is a UI bug.
- **`moondeck/uiscenario/uivideo.py`** performs the same steps while Playwright records, overlaying the captions.

The split is the point: a step's truth is written once, and whether it is being asserted or filmed is the caller's business.

## Shape

```json
{
  "name": "build-a-show",
  "description": "The pipeline model in one take.",
  "speed": 2.0,
  "width": 960,
  "steps": [ ... ]
}
```

A run also carries its own settings. `speed` and `width` shape the published clip, and are tunable afterwards without re-recording, since publishing is a separate pass over the raw take. `host` names another MoonLight surface. `requires` names hardware a run needs, resolved against the bench registry rather than written in as an address.

## A step

```json
{
  "action": "set_number",
  "module": "Grid", "control": "width", "value": 32,
  "caption": "A layout says where the lights physically are.",
  "hold": 1.5,
  "expect": {"module": "Grid", "control": "width", "value": 32}
}
```

Only `action` and that action's own arguments are required.

| Key | Used by | Means |
|---|---|---|
| `action` | both | which interaction |
| `caption` | video | text shown over the page while the step runs |
| `hold` | video | extra seconds to dwell afterwards, for the edit |
| `expect` | test | read back over REST; a mismatch fails the run |
| `as` | both | bind the created module's name for later steps |

`as` matters because the DEVICE names a module, not the run file: an added `RipplesEffect` may land as `Ripples-2`. Bind it once, then write `{ripples}` in any later step. A `{name}` nothing bound is an error rather than a literal, so a typo fails loudly instead of hunting a card by that name.

## Actions

| Action | Arguments | The interaction |
|---|---|---|
| `open_card` | `module` | clicks that module's tab |
| `add_module` | `parent`, `type`, `as` | + tab, types in the picker, clicks the row |
| `replace_module` | `module`, `type`, `as` | the card's ✎, then the picker |
| `delete_module` | `module` | the card's ×, twice (it arms, then commits) |
| `clear_children` | `module` | deletes every child, so a container starts empty |
| `set_number` | `module`, `control`, `value` | triple-clicks the box and types |
| `set_text` | `module`, `control`, `value` | selects all and types |
| `drag_slider` | `module`, `control`, `to`, `seconds` | presses and travels the track |
| `choose` | `module`, `control`, `value` | a `<select>` |
| `click_control` | `module`, `control` | a button control |
| `chapter` | `title`, `description`, `seconds` | a title card, changing nothing |
| `wait` | `seconds` | dwell, for a caption to be read |
| `wait_for` | `selector`, `text`, `timeout` | holds until the page shows it, counting a state that already went by |
| `follow_link` | `selector`, `seconds` | opens where a link leads in this page, dwells, and comes back |
| `start_process` | `command`, `name` | runs a command alongside the take, so the interface can be filmed while something else drives it |
| `wait_process` | `name`, `timeout` | holds until that command finishes, and fails the take if it did not succeed |
| `hero` | `seconds` | hides the chrome and fills the frame with the 3D preview |
| `pick_file` | `module`, `control`, `value` | a filepath control's picker |
| `type_script` | `text`, `delay` | types into the MoonLive editor and saves |

Another MoonLight surface has no module tree, so a run there names elements directly. These are the exception, not the default. A new surface earns a contract (data- attributes) the way the device UI has one, rather than growing this list.

| Action | Arguments | The interaction |
|---|---|---|
| `goto` | `url` | opens a page |
| `click` | `selector` | clicks any element |
| `type_into` | `selector`, `text`, `delay` | types into any field |
| `choose_option` | `selector`, `value` | a plain `<select>`, by label (a trailing `*` matches a prefix) |
| `scroll_to` | `selector` | brings an element into view |

`type` is the TYPE name (`RipplesEffect`), not what the picker displays (`Ripples`). The engine looks the display name up and types that, so a run file survives a display-name change.

A run has to be REPEATABLE, because the same file is a test. So a step says what should be true rather than naming something to remove. `clear_children` on a layer leaves it empty whether it held one effect or none. A `delete_module` naming a specific pre-existing module succeeds once and fails on every run after it.

## REST reads, the UI writes

**A hard constraint: REST is for CHECKING a result, never for setting one.**

A step that POSTs its way to the outcome proves nothing about the interface, and on camera it shows an effect with no cause. Every action goes through the affordance a person uses: the `+` tab, the picker, the card's own buttons, the real inputs.

The rule covers the tooling around a run, not only its steps. Cleanup is the place it is easiest to break. Deleting a leftover module with a `DELETE` is quick, and it both sets state behind the interface's back and hides a run whose own delete steps are broken. So neither the video tool nor the test fixture writes: a leftover is reported, which fails the test, and the next run's `clear_children` step puts the pipeline right through the UI.

## Locators

Playwright's guidance is user-facing locators first, test ids where those do not reach, and raw CSS last. This UI carries no `data-testid`, but `data-module`, `data-mid` and `data-tab-mid` are a real contract rather than styling hooks: app.js finds its own elements through them, so they cannot drift without breaking the app.

`set_test_id_attribute` points `get_by_test_id` at `data-module`, which is what names a module's card and its nav button, so looking a module up by name lands in the test-id tier. One attribute, not a list: the comma-separated form the API documents builds a malformed selector in Playwright 1.62. The other two need no test id anyway, because neither is looked up by module name alone: a control is addressed by the `[data-mid][data-key]` PAIR (a key alone collides, since every module has an `on`), and a tab by `data-tab-mid`.

Roles are not usable for the card buttons: they carry `title`, which contributes no accessible name. The picker's rows, which do carry user-facing text, are found by that text, which is the tier Playwright asks for.

One structural fact drives the rest: **only one root's subtree is in the DOM at a time** (`renderCards`: "One root visible at a time"). A module in a closed root is not hidden, it is absent, so the engine opens a step's root before acting on it. A step naming a module in another root does not need to say so.

## Clips and compositions

A **clip** is one topic, standing alone: it opens the card it needs, creates what it uses, and deletes it again. `95-add-a-layer.json` is a clip. Standing alone is what lets it be a test, a doc page's video, and one section of a longer cut without change.

A **composition** stitches clips into a longer video (a get-started, a feature tour) and is where music alignment belongs, since a bar grid only means something across a whole cut. `bpm` and `first_beat` live on the composition, not the clip.

    uv run moondeck/uiscenario/uivideo.py --run test/uiscenarios/clips/95-add-a-layer.json

Sources live under `test/uiscenarios/`, outputs under `media/`:

    test/uiscenarios/clips/<name>.json      a clip: what a person does    (tracked)
    test/uiscenarios/projects/<name>.json   a project: how clips are cut  (tracked)

    media/video/<name>.webm                 the raw take, ignored
    media/video/<project>.mp4               the finished cut, ignored
    docs/assets/uiscenarios/<name>.webm     the published clip, TRACKED

`media/` holds working output, split by kind (`video/`, `audio/` for the music a composition is cut against) and kept out of the repository. The published clip is re-encoded at 2x speed and 960px wide, which is what makes it small enough to track beside the effect GIFs it sits with. Measured on a 49-second take: 1280@1x is 3.3 MB, 960@2x is 872 KB, and speed and scale each take about a third off independently. The codec is nearly irrelevant, because a 3D preview is constantly-changing pixels.

Speeding the clip up changes the VIDEO, not the run: the test and the clip still describe the same interaction, and a UI demo at recording pace is slower than anyone wants to watch. `--speed`, `--width`, `--crf` tune it; `--no-publish` skips it.

## Project files: cutting clips together

A **project** (`test/uiscenarios/projects/<name>.json`) is the edit: which clips, in what order, how long each gets, and the music underneath. It is a video editor's save file kept as JSON, so a cut is reviewable in a diff and re-runnable from scratch rather than a sequence of manual drags nobody can reproduce.

    uv run moondeck/uiscenario/uicompose.py --project test/uiscenarios/projects/getting-started.json

```json
{
  "name": "getting-started",
  "audio": "media/audio/Norse Constellations (Original Mix).mp3",
  "bpm": 112.35, "first_beat": 8.78, "audio_gain": 0.5, "width": 1280,
  "clips": [
    {"clip": "95-add-a-layer", "title": "Stack and blend", "bars": 8},
    {"source": "media/footage/wall.mp4",
     "title": "Twelve thousand lights", "subtitle": "On one board", "bars": 12}
  ]
}
```

`clip` names a published clip; `source` is any video file, which is how footage no run can produce (a camera shot of a real rig) joins the cut. Prefer `clip`: a `source` is outside the tooling, so nothing regenerates or checks it.

**Durations are in BARS, not seconds.** A cut lands on the music or it does not, and
the bar is the unit that makes that true: 8 bars at 112.35 BPM is 17.1 seconds, and changing the track re-times the whole edit from one number. A clip is stretched or compressed to fill its slot rather than truncated, because these are whole interactions and cutting one mid-gesture leaves an action that never completes.

ffmpeg's **concat demuxer** does the joining, which is its standard mechanism for this. There is no richer project format inside ffmpeg (EDL and OTIO belong to other tools), so the JSON generates the listing it consumes.

The per-segment intermediates live in `media/.<name>-work/` and are **deleted once the cut exists**: they are a means rather than an output, several times the size of the finished file, and regenerated by the next run. They survive a FAILED cut, where they are the evidence of what went wrong, and `--keep-work` keeps them deliberately.

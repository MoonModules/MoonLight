# MoonWalk

Step 2 of [Processes, described once](../present/Plan-20261015%20-%20Processes%2C%20described%20once.md): MoonWalk, the model every process is written in and the map that shows them, chosen against [the inventory](processes-inventory.md) and approved on 2026-10-16. The schema is [moonwalk/moonwalk.schema.json](../../../moonwalk/moonwalk.schema.json).

## What it must do

- **One model** holds every process, the end user's and the contributor's, so the whole journey is one map.
- **The map is navigable**: on the website and in the app, zoomed out to the whole model, zoomed in by search and filters, and opened at the place the reader is, a docs page or the card open in the app.
- **A process already done is not offered again**: what the reader has done is known, and the map and the app's suggestions leave it out.
- **The pages named for an activity are generated from it**: building, testing, integrating, troubleshooting, installing on Linux, updating the firmware, logging an issue and the rest of the how-tos and tutorials. So a process carries its prose as well as its steps.
- **Every runner reads the same steps**: the docs, the app's tour, the tests, MoonTube and the agents.

## The three levels, one per textbook

| Level | What it is | Textbook | Zoom |
|---|---|---|---|
| The journey | stages, left to right: read, install, set up, first light, everyday use, advanced use, maintain, report, contribute, review and merge, release | the user story map (Patton) | zoomed out |
| A process | a goal, its actor, the processes it depends on, its steps, its branches | an Arazzo workflow | middle |
| A step | a precondition, an action, an expectation | Given, When, Then (Gherkin) | zoomed in |

**Arazzo-shaped, not Arazzo.** Arazzo requires every step to call an API operation or another workflow (its step object's `operationId`, `operationPath` or `workflowId`), and most of the journey's steps are a click in the app, a command or a person plugging in a board. So the model keeps Arazzo's structure and names (`dependsOn`, `onSuccess`, `onFailure`, a workflow per process, several per document) with its own step shape. A process whose steps are all API calls can be exported to Arazzo later, once the REST API is in OpenAPI, which is where Redocly's Respect and Spectral would come in.

## Files

```
moonwalk/
  moonwalk.schema.json
  01-read.yaml            one file per journey stage: the stage, then its processes
  02-install.yaml
  ...
  11-release.yaml
```

The number orders the backbone, as the MoonTube clips are ordered. **YAML** because a process carries prose: multi-line text, Markdown and comments read well in YAML and badly in JSON strings. MkDocs already depends on PyYAML, so the docs build reads it at no cost. The schema validates YAML and JSON alike.

```yaml
stage: First light
summary: The first effect on the first lights, on the desktop or a board.
processes:
  - id: first-light-desktop
    title: First light on the desktop
    actor: user
    goal: An effect runs on the desktop's preview.
    dependsOn: [install-desktop]
    page: tutorials/first-light-show.md
    intro: |
      Markdown shown above the steps on the generated page.
    steps:
      - id: open-effects
        say: Open the Effects card.
        when: { open: { module: Effects } }
      - id: add-rainbow
        say: Add a Rainbow effect with the + on the Effects card.
        when: { add: { module: Effects, type: RainbowEffect } }
        then:
          - { module: Rainbow, exists: true }
      - id: brighter
        say: Raise the brightness until it looks right.
        when: { set: { module: Layer, control: brightness, value: 200 } }
        then:
          - { module: Layer, control: brightness, equals: 200 }
        detail: |
          Markdown for the page only: why, what to watch for.
    outro: |
      Markdown below the steps: what next, what to read.
```

## A step

- **`say`**: one line, the instruction. It is the docs line, the tour bubble, the film caption and the test's name.
- **`actor`**: who performs the step, when it is not the process's actor; the generator fills in the process's otherwise. BPMN's lane: the commit is the product owner's process, and the checks in it are the agent's.
- **`given`**: preconditions, of four kinds. A process done first (`install-desktop`); an expectation on the device; the change touching a path (`{ changed: ["src/**"] }`, the globs GitHub Actions' `on.push.paths` takes, which the agent and CI read from `git diff --name-only`); and the reader's choice (`{ input: platform, equals: windows }`). The scenarios' `fixture`, `reset` and `top` and a clip's `setup` are all this.
- **`when`**: one action, by intent rather than gesture, so each runner picks its own gesture: the tour points at the card's +, the test applies a state document, MoonTube clicks.

  | Action | Target | Runs in |
  |---|---|---|
  | `open` | a module (its card), a page of the app, or a URL | tour, film, docs |
  | `set` | module, control, value | test, tour, film, docs |
  | `add`, `replace` | parent module, type | test, tour, film, docs |
  | `delete`, `clear` | module | test, tour, film, docs |
  | `apply` | a state document | test, docs |
  | `call` | an HTTP method and path, an OpenAPI operation once step 12 lands | test, docs |
  | `run` | a MoonDeck script with its arguments, or a shell command | agent, CI, docs |
  | `read` | a docs page or a section of one | docs, tour |
  | `process` | another process, run as a subprocess | all |
  | `manual` | free text: plug in a board, look at the strip | docs, tour |

  The runner list is derived from the action, never written down. A runner meeting an action it cannot perform describes it, as the scenario runner now prints SKIP.
- **`then`**: expectations, the step's success criteria. A control's value (`equals`, `notEquals`, the shape `expect_control` has), a module that exists, an HTTP status, a file's content. The same expectation is a test's assertion, the tour's "done, next", and the "already done" check below.
- **`onSuccess`, `onFailure`**: Arazzo's branches, `goto` a step, `end` or `retry`, on what the device shows. Troubleshooting's decision tree is written this way.

**A choice is an input.** What the reader picks, such as the platform, is a process's `inputs`, a JSON Schema as Arazzo's workflow inputs are, and a step runs only for the option its `given` names. The view shows the options as a row of buttons above the steps, defaulting to the reader's own platform, and the plain text names each step's option in bold; the tour asks once, and the app fills in what it knows already, such as running on a desktop.
- **`detail`**: Markdown for the generated page only.
- **`section`**: opens a numbered section of the generated page with this heading, as a tutorial groups its steps into chapters.

Targets use the keys `help.json` uses, a module's type or instance name and a control's name, so the reverse index into the help is a lookup.

## What the device pilot taught (2026-10-16)

Step 4 put the map in the app (🗺️ in the status bar), with done read from the device's state. On the desktop bench:

1. **Done from the state works where a process says what done looks like.** Two of 48 processes carry an expectation the device can answer, joining the WiFi (a known network) and the first light show (a grid, three modules, a brightness), so a device says little yet. It grows as steps gain their `then`, which the migration (step 11) brings.
2. **A list counts as its rows**, so "a network is known" is `{ module: WiFi, control: known, notEquals: 0 }`. Compared as text, an empty list read as done.
3. **The running app proves the install, and nothing says so.** The next list offers installing MoonLight to the device showing it. Decided: the installer vouches for what it installed (step 6 of the plan), since it knows the platform for certain, where a running device cannot tell a desktop from a Linux board or a container. A board run confirmed the rest: on a Waveshare ESP32-S3-RGB-Matrix the map marked joining its WiFi done from the board's own state.
4. **One consent covers the help and MoonWalk**, since both come from the docs site, and `?help=local` reads the local map through the installer preview, as it reads the local help.

## Systems

A process names the MoonLight systems it touches, `systems: [MoonInstaller, MoonCore]`, as the [architecture's Part table](../../explanation/architecture/index.md) names them: that table is the list's one home, and the generator reads it and refuses a name it lacks. The generator adds what the steps imply: a MoonDeck script, the web installer's address, a MoonLive module. On the map, system chips filter, and the columns switch between stages and systems, a process touching several standing in each of their columns.

## Done, without a log

**A step is done when its `then` holds now.** The app reads its own state, so "WiFi is set up" is done the moment `WiFi.ssid` is not empty, however it got there: through the tour, the installer or by hand. This is the onboarding checklist's textbook rule, completion observed rather than recorded, and it needs no storage. A process is done when its last step is.

What the state cannot show, a page read or a tour finished, is marked by the reader and kept in the browser: `localStorage` on the docs site and in the app, as the help's cache already is. Recorded processes, a commit gate or a release, are done on GitHub (step 13).

The website cannot read a device (an https page cannot call a device's http address), so there only the reader's marks count.

## The map

- **One generated file**: `gen_moonwalk.py`, beside `gen_help.py`, validates every stage file against the schema and writes `moonwalk.json`: the stages, the processes, the steps with their derived runners, and two indexes, by module and control and by docs page. It runs in the docs build and is mirrored per release, as `help.json` is.
- **One map, two hosts**: `moonwalk.js` draws `moonwalk.json` with **Cytoscape.js** (MIT, from cdnjs, about 430 KB, so it never enters the firmware). Compound nodes give the three levels: a stage holds its processes, a process holds its steps. The layout is the story map's grid, stages as columns and processes stacked in `dependsOn` order, computed in a few lines rather than by a layout library.
  - **Semantic zoom**: zoomed out shows stages and process titles, zooming in shows the steps and their `say`.
  - **Search and filters**: text over titles, steps and targets; actor; system; hide what is done. Filters by runner (tested, filmed, has a tour) wait for the runners' adapters.
  - **Where you are**: the map opens on the reader's choices, **Here** and **Next**, with the map fitted to them. Here is the processes touching the card in view in the app (the module index) or the page the reader came from on the site (the page index); Next is the next to do. Neither lists what is done. A link naming one process (`?focus=commit`) opens that process.
- **The process view**: `moonwalk.js` renders a process whole in the map's panel, with the help's renderer: its intro, its steps in their numbered sections with their detail and clips, a choice as a row of options, and its outro. One view on the site and in the app, at `moonwalk.html#<process>`; a page MoonWalk takes over forwards there. The MoonWalk page carries every process's prose as plain text for search and readers without JavaScript, hidden once the view draws. The parts of a page that are reference rather than steps (a table of ports, a test strategy) stay on a reference page and are linked.

## What it removes

The step lists duplicated across pages, CLAUDE.md, MoonDeck.md and memory, in time; the scenario's and the clip's separate step vocabularies, which become runners of one; hand-drawn process diagrams. Nothing is removed by this step itself: it adds the schema, and the pilot adds the first files.

## Decided (2026-10-16)

1. **YAML** for the authored files. The generator rejects a boolean where a name belongs, since YAML 1.1 reads an unquoted `on`, `off`, `yes` or `no` as one.
2. **Arazzo-shaped**, with an export to Arazzo for API-only processes later.
3. **Done marks in the browser only**: what the state cannot show is kept in `localStorage`, on the site and in the app.
4. **The pilot is the website's map**; the app's map follows once the pilot has taught what it will.

## What the pilot taught (2026-10-16)

The pilot of step 3 drew 48 processes over 11 stages, three of them with their steps (installing on the desktop, the first light show, the commit). Four findings, each decided and in the model above:

1. **A step has an actor of its own**, defaulting to the process's.
2. **A step can wait on the change**, through `given: { changed: [...] }`.
3. **A choice is a process input**, with `given: { input, equals }` on the steps of each option.
4. **YAML quoting.** Each stage file opens with `# yaml-language-server: $schema=moonwalk.schema.json`, so an editor with the YAML language server validates and completes while typing. A sentence holding a colon, or a question mark inside `{ }`, is quoted or written as a `|` block; the generator names the file and line of a parse error and stops there.

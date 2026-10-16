# Plan: processes, described once

Plan, for the product owner's approval (2026-10-15). MoonLight has many processes, from a first reader to a release, and each is written down in its own place and its own shape. This plan finds one model for all of them, so a process is described once and followed by everyone it concerns: a user in the app, a reader of the docs, a test, a MoonTube film, an AI agent and the product owner.

It is called **MoonWalk**: the model, its stage files under `moonwalk/`, and the map that shows them.

## Why

The same steps live in several places today. Installing on the desktop is a how-to, a getting-started section, a README paragraph and a MoonTube clip. The commit gates are a diagram and prose in CLAUDE.md, the scripts in MoonDeck.md and the agents' memory. A scenario JSON and a MoonTube clip, which is also the UI test, each list steps with an action and an expectation, each in its own format. Every copy drifts on its own, and a stakeholder cannot see the whole journey.

**What it removes**: the duplicated step lists across the docs, CLAUDE.md and the clips, in time the separate step formats once the mapping shows which reduce to one, the hand-written gate results in commit messages once the gates record on GitHub, and `collect_kpi.py`'s own map of which script measures what, once each step declares its metrics.

## The method

Three phases, in order, each approved before the next:

1. **Extract** the model from what exists: the content of every process, not the structure it is stored in.
2. **Define** the textbook solution, chosen against that inventory.
3. **Map** everything that exists onto it, saying per artifact what it becomes, what is generated and what goes.

A defect a phase finds is fixed in that phase, as CLAUDE.md's principle 5 asks: a wrong page, a stale copy, a check that passes what it should not. Only a fix that needs a decision of the product owner waits, named in one line until it is decided.

## The textbook candidates

To decide in phase 2, each named so the choice is explicit:

- **The journey: a user story map** (Jeff Patton). A backbone of activities left to right, the steps of each below it: reading the docs, installing, first light, everyday use, advanced use, sharing in the gallery, reporting an issue, contributing, reviewing and merging, releasing. The roles in CLAUDE.md are its actors, with the end user ahead of them.
- **The process: the Arazzo Specification** (OpenAPI Initiative, 1.0 in 2024). A workflow is an ordered list of steps, each with its inputs, success criteria and outputs, and `onSuccess` and `onFailure` give branches. It is written for docs, test automation and AI agents to read one description. Its steps call API operations, which fits the scenarios; UI, shell and manual steps need its `x-` extensions.
- **The step: Given, When, Then** (Gherkin, from behavior-driven development). A precondition, an action and an expectation, readable as documentation and run as a test. Scenario steps and MoonTube steps already have this shape.
- **Heavier, and not expected to fit**: BPMN 2.0, the process notation of organizations with an XML toolchain, and the CNCF Serverless Workflow, an orchestration runtime.

## Where a process is written

The industry standard for one description serving docs, tests and tools is a data file as the source, validated by a JSON Schema, with the docs generated from it: OpenAPI generates API references, Arazzo workflows drive tests and docs, and GitHub Actions and Backstage describe processes the same way. So a process is one authored file, and a generator compiles them into one `moonwalk.json`, as the headers compile into `help.json`. Published the same way, per release. Phase 2 settles YAML or JSON (Arazzo accepts both; YAML carries comments, today's step files are JSON) and whether `moonwalk.json` stays its own file or joins `help.json`.

## Hooks into the help

The process model builds on [the help in the UI](Plan-20261014%20-%20Help%20in%20the%20UI%2C%20from%20the%20docs.md). A step that acts in the app names its target by the keys `help.json` already uses: the module's type name and the control's name, the address a MoonWalk step needs as well. The hook is written once, on the step, and the generator writes the reverse index into the help, so each module and each control lists the processes that use it and can start one there. Arazzo names API operations as targets, so a module or control as a target is an `x-` extension, which the specification provides for.

## The runners

A runner performs the steps it knows how to and skips or describes the rest:

- **Tests**: the scenario runner and the UI tests run a process's steps and check each expectation.
- **MoonTube** films the same steps with their captions.
- **The app** shows them beside itself as the user performs them: each step opens its screen, points at a control and says what to do.
- **Agents** read the process for the task they are given, the commit gates included.
- **The docs** render a process as a tutorial or a how-to, in Diátaxis' terms, with its flow diagram.

## The APIs a step calls

- **The device's REST API**: 18 routes under `/api`, such as `state`, `control`, `modules`, `types`, `file` and `firmware`. Today it is described in prose in [integrating](../../reference/integrating.md) and in the code. The textbook is an OpenAPI 3.1 description: Arazzo steps then name its operations, the scenario runner checks each request against it, and the reference is generated from it. That is a new file, and the request shapes written by hand in integrating go.
- **The device's WebSocket**: the state stream and the preview. AsyncAPI is the textbook for an event API, a candidate once a process needs to wait on a pushed state.
- **The GitHub REST API**: the releases the app's update check reads, the issues the gallery and the recorded steps use, and the check runs the gates record. Projects are read and written through GitHub's GraphQL API or `gh project`.
- **GitHub Pages**: static files, `help.json`, `moonwalk.json` and each release's mirror, read by the app without a server of ours.
- **MQTT and Home Assistant**: integrations a how-to walks through, which a process names as tool steps.

## The gallery

Three processes, with both kinds of step:

- **Find and install**: the Control card's Gallery reads [MoonLight-Gallery](https://github.com/MoonModules/MoonLight-Gallery) through the browser, and a click adds a preset to a pad, tries it or installs a script. Volatile: it changes the device only.
- **Share**: a card's `{ }` button copies the JSON, and an issue in the gallery repo submits it; its acceptance makes the entry. Recorded, and already an instance on GitHub, the pattern step 14 generalizes.
- **Record a preview**: the preview bar's ⏺ saves a three-second GIF to attach to that issue.

## Existing content, estimated

A first count from the titles and file shapes, which step 1 confirms. Procedural content moves; reference and explanation stay where they are and are linked.

| Content | Count | Fits the model | How |
|---|---|---|---|
| MoonTube clips, also the UI tests | 14 runs, about 250 steps | directly | UI steps with a caption and an `expect` become process steps; the film and the app are runners |
| Scenarios | 32 files, about 620 steps in 12 operations | directly, once the REST API is in OpenAPI | `set_control`, `apply_state` and `expect_http` are API calls; `measure` stays the runner's own step |
| How-tos | 21 | about 15 mostly | step sequences; troubleshooting and the FAQ are decision trees, which branches express |
| Tutorials | 5 | 4 | how-moonlight-works is explanation and stays a page |
| Getting started and the README's first run | 1 page, 1 section | directly | the start of the journey |
| Integrating's HTTP recipes | about 6 sequences | directly | API workflows, the case Arazzo is written for |
| CLAUDE.md | 6 flow diagrams, the commit, merge and release gates | directly | the developer process; the diagrams become generated |
| The agents' memory | the process rules, such as staging and "commit now" | as preconditions | they move to the process, where the product owner sees them |
| Plans in `docs/work/present` | 18 | as instances | each plan is one run of the change process |
| The gallery | 3 processes | directly | above |
| MoonDeck scripts | 83 | as operations | a step calls a script; MoonDeck.md stays their reference |
| GitHub Actions | 13 jobs in two workflows | as they are | already executable; a step names the workflow rather than copying it |

## How big it gets

Neither a Confluence nor a Jira. Confluence is a wiki, and the docs site is ours; Jira tracks work, and GitHub's issues and Projects do. What gets built is a JSON Schema, the process files, one generator of the order of `gen_help`, and an adapter in each runner that exists: the scenario runner, MoonTube, the app and the docs build. Out of scope: a process engine, an editor, a server, and states or permissions beyond GitHub's.

### Where it stands, and how big it grows (measured 2026-10-16, after step 4)

| Measure | Now | Full scope, estimated | Done |
|---|---|---|---|
| Processes on the map | 48, every user and contributor process of the inventory | about 100 to 150, with the gates, the guides' procedures and the gallery's | about 40% |
| Processes with their steps | 4 | nearly all | about 3% |
| Steps | 27 | about 1,000 to 1,500, the scenarios' 622 and the clips' 266 becoming runners of process steps, plus the docs' own | about 2% |
| Pages generated from a process | 0 | the 26 how-to and tutorial pages, getting started, and the pages named for an activity | 0% |
| Machinery | schema, generator, map, device host, done and next | plus page rendering, the runners' adapters, the app's live view, GitHub, the checks and OpenAPI | about 35% |

The skeleton stands and the content has barely started. Today the stage files are 19.7 KB of YAML, the schema 11.7 KB, `moonwalk.json` 27 KB (7 KB gzipped, what a browser downloads) and `moonwalk.js` 22 KB, with Cytoscape's 374 KB from the CDN; the firmware carries none of it.

At full scope the stage files reach about 350 to 450 KB. That is content moving rather than growing: the procedural docs are 379 KB, two thirds of it steps and their detail; the scenario and clip steps 158 KB as JSON, overlapping and terser as YAML; CLAUDE.md's process part 18 KB; and the duplicates collapse, up to 13 copies of one process. The repo shrinks, which step 11 measures.

`moonwalk.json` reaches about 500 KB (120 KB gzipped) if it carries the prose, too heavy for an app that only draws the map. Split, it keeps what the map and the device read (ids, titles, `say`, `when`, `then`, dependencies) at about 150 KB (35 KB gzipped), and the prose stays in the docs build that renders the pages. `moonwalk.js` grows to about 30 to 40 KB with the app's live view. At about 1,500 steps, with the system view's duplicates near 2,500 nodes, the map draws a process's steps only when zoomed into it.

## Flows and decision points

Arazzo covers what the processes found so far need:

- **Decision points**: a step's success criteria pick `onSuccess` or `onFailure`, which go to another step, end or retry with a limit, so troubleshooting's tree and an install's desktop-or-ESP32 choice are branches.
- **Subprocesses**: a workflow runs another, so first light reuses installing.
- **Order**: `dependsOn` orders workflows, and steps without one between them may run side by side, as the merge gate's checks and judgment gates do.
- **Diagrams**: the generator draws each process as a Mermaid flowchart, which the docs already render, with a lane per actor. CLAUDE.md's six diagrams become generated.

Arazzo has no timers, events or swimlanes of its own. BPMN carries those, and is the move only if a process needs them, which none found so far does.

## Metrics on steps

A step declares what it measures, as Arazzo declares a step's `outputs`, and each run records the values on its instance, so a process carries its own measurements rather than a separate report knowing which script produces which number. The measurements exist today, spread over scripts: a scenario's tick time and heap, a build's flash size per target, the hot-path findings and the code ratchets, and the gate times. `collect_kpi.py` gathers them per commit into repo-health; the process file names them per step instead, and the trend of a value is the series of its runs.

The process itself is measured the same way: how long each step takes and how often it fails, and from a plan's first step to its release. These are the DORA metrics (lead time, deployment frequency, change failure rate, time to restore), the industry's standard measure of a delivery process. A value moving without a reason in its change is the irregularity CLAUDE.md asks to explain before committing; a control chart, the textbook of statistical process control, draws the line between noise and a shift.

## Where it runs

No server. The definitions are files in the repo, the generator runs in the docs build as `gen_help` does, `moonwalk.json` is a static file on Pages, and the runners are the tools that exist: MoonDeck, the scenario runner, MoonTube's Playwright runs, the browser and the agents. GitHub records the runs. Two existing tools are worth a look once the API is in OpenAPI (step 13): Redocly's Respect runs Arazzo workflows against an API and Spectral lints them, adopted only where they replace code of ours.

## Had this come before the help

Little would differ, because the order is the textbook one. The help is the reference, what a module and a control are, and processes are tutorials and how-tos that point into it, which Diátaxis builds on top of a reference. The keys the hooks need, a type name and a control name, are in `help.json` already, so the help does not change. Three things would have looked different:

- **The how-tos this branch wrote** from the catalog cards, such as LED drivers, HUB75 panels, watching the output, audio input, control surfaces, presets and home automation, would be process files rather than pages. They move in step 12.
- **A module guide that walks a procedure**, rather than saying how the module works, would be a process step. The guide rules already keep controls out, so these are few.
- **Publishing and consent** serve both files, which they already do the same way.

## Where the reader is: the device and MoonCloud

MoonWalk offers the next process rather than the whole list, so it needs to know what is done. Three sources, in order:

- **The device's own state** is the first record. A step is done when its `then` holds on the device now: a WiFi with an SSID is set up, an Effects card holding an effect has had its first light. The app reads `/api/state` and evaluates the expectations in `moonwalk.json`, so nothing is logged, and a step done by hand, by the installer or through MoonWalk counts the same. **The next process** is the first one not done whose `dependsOn` all are, the frontier of the dependency graph in topological order (Kahn's algorithm), so a device with its WiFi set up never offers setting it up. The website cannot read a device, since an https page cannot call an http address, so the device's state reaches the map only through the app. That is why the app hosts the map as well.
- **The browser** keeps what the state cannot show and the reader marks: a page read. It is `localStorage`, already in use: the reader's done marks (`mm_moonwalk_done`), the map's split (`mm_moonwalk_split`), the reader's choices such as their platform (`mm_moonwalk_inputs`), and on a device what the installer vouched for until the map records it (`mm_walked`). No cookies: they would travel with every request to the device for nothing.
- **Browser storage stays on its origin**, the scheme, host and port together. The docs site and the installer share one on Pages, so the installer's vouch shows on the site's map. Each device, the desktop app on `localhost:8080` and MoonDeck on `localhost:8420` are origins of their own. Cookies are scoped by host rather than port, so `localhost` ones would reach both local servers, but never a device, and a device's plain-http page cannot read the site's storage at all.
- **What crosses origins is carried.** The textbook carriers, from lightest to durable:
    - **The address**: the installer's link carries `?walked=install` to the device's map (step 6). It suits a handover at one moment.
    - **`postMessage`** between a page and a window or frame it opens, for a live exchange such as the app opening the site's map.
    - **The device's own config**: marks kept on the device reach every browser that opens it, the one shared home a device has. It costs a persisted control, which the 2026-10-16 decision kept out.
    - **MoonCloud** (below), with consent: the marks under the installation id, across browsers and devices.
- **One consent** covers what the app reads from the docs site, the help and MoonWalk, since both show that host the same thing, the browser's address (decided 2026-10-16). What MoonWalk sends gets its own, the MoonCloud member of step 18.
- **MoonCloud, with consent.** A MoonWalk member of MoonCloud, its own checkbox and off until switched on like Stats and Talk, keeps those marks under the installation id, so they follow the device to every browser that opens it. In aggregate it answers what no single device can: where people stop, a funnel per process, which shows the page or the step that needs work. It sends process and step ids and nothing typed, no name and no address, under the privacy policy's rules for every member. The website's own marks stay in the browser, since a website has no installation id.

## GitHub: issues, Projects, CI, the backlog and notes

MoonWalk in the repo is the definition: what the processes are. GitHub holds the work and the runs: what is to be done, in what order, and what happened. Each GitHub feature gets one job, so nothing is tracked twice.

- **Issues are the work items**: a bug, a feature request, a backlog item. Each carries a label naming the MoonWalk process it concerns (`walk:<process-id>`), so MoonWalk's map shows the open issues on each process, read from GitHub's public issues API, and a process with many is where the docs or the code need work. The issue forms are generated from MoonWalk: the steps of Log an issue become the bug form's fields, which the repo lacks today.
- **The backlog becomes issues.** Today it is three Markdown files of 3,408 lines beside 4 open issues, two backlogs for one job. The textbook backlog is an ordered list of items (Scrum's product backlog), and GitHub's form of it is issues ordered in a Project. Decided: each actionable item becomes an issue (step 20), created in bulk by a script the product owner reviews, and a long analysis stays in `docs/work/future/` linked from its issue. The backlog files go. The cost: an agent reads the backlog through `gh issue list` rather than a file, and it needs the network to.
- **A Project orders and tracks them**: one Project for MoonLight, with a Status field (Inbox, Backlog, Next, In progress, In review, Done), a Stage and a Process field holding MoonWalk's ids, and an Area. A board by Status is the Kanban view, and a table grouped by Stage is the work laid on the journey map's own columns.
- **Notes reach the Inbox**, the step after the product owner's Apple Notes scrapbook (step 19 tries whether it can replace it). A Project's draft item is a note that is not yet an issue: a title and a line, captured from the phone with GitHub's app, with `gh project item-create`, or by the agent when the product owner says to note something. Triage promotes a draft to an issue or deletes it. The work an agent spots and names in one sentence lands there too, rather than only in the chat.
- **Actions run what is already executable.** A MoonWalk step names a workflow rather than copying its steps, the local gates post check runs on the commit (step 14), and a pull request shows which gate passed where.
- **Access**: the agents' `gh` needs the `project` scope (`gh auth refresh -s project`); it has `repo` only today.

## The steps

1. ✅ **Inventory.** Done: [the processes inventory](../future/processes-inventory.md), 71 entries. The Researcher maps every process that exists as content: its steps, where each is written, who follows it and what verifies it. Sources: the table above, plus the web installer. The result counts the duplicates per process. Logging an issue is a how-to today and GitHub carries no issue or pull request template, so the inventory also notes the processes that have no home.
2. ✅ **The model.** Approved 2026-10-16: [MoonWalk](../future/moonwalk.md) and [its schema](../../../moonwalk/moonwalk.schema.json). The journey, the process format and the step shape chosen from the candidates above, written as a short spec with its JSON Schema. The product owner approves it.
3. ✅ **A pilot: the whole journey on a map.** Done 2026-10-16 and judged by the product owner in the browser. A working slice first, then iterate. `gen_moonwalk.py` validates the stage files and writes `moonwalk.json` in the docs build. The eleven stage files carry every process of the inventory as a stub (title, actor, goal, page), and three carry their steps: installing and first light on the desktop, and the commit, so one map shows both halves. A map page on the site draws it with Cytoscape.js: zoom, search, filters, `?focus=` from every page, and done marks in the browser. What it taught is in the model.
4. ✅ **A pilot on the device.** Done 2026-10-16: on the desktop build and on a board (the Waveshare ESP32-S3-RGB-Matrix), where the device's own state marked joining its WiFi done. A working slice in the app, proven on the desktop build before any board. The app hosts the map: `moonwalk.js` and `moonwalk.json` from Pages, fetched under the help's consent, opened at the card in view through the module index. Done comes from the device's own state, every expectation in `moonwalk.json` evaluated against `/api/state`, so a device with its WiFi set up shows that process done. The app offers the next process, the first one not done whose dependencies are, as § Where the reader is describes. A stepped process whose steps lack a `then` gains one here, since without it the device cannot see the step done.
5. ✅ **The process view, live.** Done 2026-10-16, judged by the product owner: the first light show and the desktop install are processes, their pages gone and their addresses forwarding. The second pilot, after the device. `moonwalk.js` renders a process whole, its intro, its steps in their numbered sections with their detail and clips, and its outro, in the map's widened panel, with the help's own renderer (`help.js`, which learns lists, headings and clips). One renderer for the site, the app and the hosts to come; the address is `moonwalk.html#<process>`, and a page MoonWalk takes over forwards there (mkdocs-redirects). For search and readers without JavaScript the MoonWalk page carries every process's prose as plain text, which the script hides once the view draws, as the card deck does. The stage files' prose writes its links relative to `docs/`, and the generator makes them absolute in `moonwalk.json`, which carries the prose. The prose gate reads the stage files. First `tutorials/first-light-show.md`, whose file is gone and whose address forwards; then `how-to/installing-to-desktop.md`, whose platform choice shows as a row of options above the steps, only the chosen platform's steps showing.
6. ✅ **MoonWalk in the installer and MoonDeck.** Done 2026-10-16: the installer shows the ways to install and vouches for an install, handing it to the device's map; MoonDeck marks a contributor's process done from its run logs. One `moonwalk.js`, more hosts, each showing the processes before anything is done and vouching for what it knows afterwards: `drawMap` takes a `done` option, the processes a host vouches for, beside the device's `state`.
    - **The installer** shows the choices before an install (which device, which platform, ESP32, desktop, Linux or Docker, and what each takes), and afterwards marks done what it just did, the install and the WiFi it pushed, then hands over to the device's map at "Device is online!". It knows the platform for certain, where a running device cannot tell a desktop from a Linux board or a container.
    - **MoonDeck** shows a contributor's processes before any script runs, and marks done the steps whose scripts ran and passed, read from its own run history: built, tested, gates passed.
    - **MoonBase** gets a link, not the map: the recovery image's flash is precious and it often runs offline on its own access point, so when online it links to the recovery process on the site. Its link moves when getting started migrates (step 12), since its target becomes a process then.
7. ✅ **The content reviewed.** Done 2026-10-16 for the structure, judged by the product owner: the ESP32 and computer installs became one process with a platform choice, Docker its fifth option; Set up folded into Install; the steps the same everywhere, settings and updating, became one step each; stages, dependencies and goals tuned. The full read of the prose waits for step 12. Every process read as its reader meets it, stage by stage: the titles and goals in that reader's words, the stage each sits in, its actor, what it depends on and its systems. A structure that does not hold is proposed for change, such as two processes sharing one goal and one entry becoming one process with a choice. The model's skeleton was written from the inventory in one sweep, so it is reviewed before the steps that build on it.
8. ✅ **The help and MoonWalk, linked.** Done 2026-10-16, judged by the product owner on the desktop and on a board (the Waveshare ESP32-S3-RGB-Matrix). The help says what a module or a control is, MoonWalk what to do with it, so each links to the other and neither repeats the other's text: a step's `say` instructs, the card's help explains.
    - ✅ **One key, the module type.** A step names the type, `GridLayout` and its control `width`, the key `help.json` already uses. The generator checks every type and control a step names against `help.json` in the same build, so a renamed control fails the build rather than leaving a dead pointer, and the device's done check matches its modules by type.
    - ✅ **From the help to MoonWalk.** Beside Docs, Source and Tests, a module's help lists the processes that use it, and a control's more names the step that sets it, from a `byControl` index beside `byModule`. A tap opens MoonWalk at that step, in the app and on the site's card decks alike. In the app the links show only when help is on, under the help's one consent.
    - ✅ **From MoonWalk to the help, followed live.** The onboarding checklist's construct (Shopify's setup guide, Intercom's checklists): the full view on request, and while the user works, one line with the current step. A step with a target has **Show me**: the map closes, the app selects the step's card, scrolls to its control and highlights it, and a bar holds *Step 3 of 11: Set the Grid's width*, where the card's own help line does the explaining. On each state patch the bar checks the step's `then` again and moves on when it holds; a step the state cannot see has **Done**, and a tap on the bar reopens the map at that step. The bar lives in `moonwalk.js`; the app hands it its state and a way to show a card. The bar floats under the step's control as a coach mark does, at the bottom when the step has none in view. A step the device already shows done is ticked in the map and has no Show me, and an expectation can be a range, JSON Schema's `minimum` and `maximum`, as "brightness low" is. A pane docked beside the app was the alternative: on a phone it covers the card it points at, and the map does not fit a strip.
    - ✅ **The pilot** is the first light show, walked step by step on a fresh desktop and on the board: its grid and Bouncing Balls steps ask for a visible change, since the defaults already meet 16 and 8. It is the one process whose targets are all modules and controls. A device on an older release can lack a control a step names: its link does not show and Show me finds nothing.
9. **One process through the other runners.** The process step 5 renders runs through the rest: the scenario runner performs its steps as a test, MoonTube films them, and an agent follows them.
10. **The developer process.** The commit, merge and release gates become process files: each check a step with its trigger, the diff touching a path, its MoonDeck script and its expectation. CLAUDE.md keeps the principles and roles and links the process, so the gate list has one home that agents and the product owner both follow.
    - **The gates are the contributor's process, whoever contributes.** Anyone who commits or opens a pull request runs them, the product owner too, who commits in the contributor role; the product owner's own part is the review, the merge and the release. CLAUDE.md's Roles and The Process are reviewed with this in mind. Four outside pull requests (#134, #140, #144, #145) each cost a review round on the same points, based on main, one topic within the size, the local checks, every platform building, so they become the process's steps, which a contributor meets before opening a pull request rather than in the review.
    - **Generated files stay out of a contributor's pull request.** The metrics and the scenarios' observation blocks are measured on the contributor's machine, and the merge regenerates them on ours. A step in the process, and a CI warning when a fork's pull request touches `docs/reference/metrics/` or a scenario's observations.
    - **Maintainer edits.** When a branch allows them, the maintainer makes the mechanical fixes, a dropped generated file or a platform stub, rather than asking for another round.
11. **The mapping.** Each artifact from step 1 mapped onto the model: what becomes a process file, what is generated from one, and what goes. It ends with the number of copies removed.
12. **The migration**, process by process, each old copy deleted as its process file takes over, so the docs and the tests stay complete throughout. Before the step closes, every process, the migrated ones and those already in MoonWalk, gets step 7's read as its reader meets it. It includes every page named for an activity, building, testing, integrating, troubleshooting and the how-tos, which become pages generated from their process; what on them is reference rather than steps moves to a reference page.
13. **The REST API in OpenAPI**, before step 12 reaches integrating and the processes made of API calls. The device's routes described in OpenAPI 3.1, checked against the server's routes in a test, so a step names an operation and integrating's reference is generated.
14. **Executions on GitHub.** A process file is the definition and one run of it is an instance, the split workflow engines make. GitHub keeps the instances; the definition stays in the repo as the one copy. Steps come in two kinds:
    - **Volatile steps** act on a device or in the app, such as a user choosing an effect. Nothing records them, since nothing outside the device changes.
    - **Recorded steps** change the repo or what ships: a plan step, a commit gate, a merge, a release, a verification on hardware, a gallery submission. Each run leaves a record.

    A run's measurements, the step metrics above, ride on its record: a check run's output on GitHub, and the value series repo-health draws.

    Automated recorded steps record as check runs on the commit or pull request, the local gates included through `gh api`, so a pull request shows which gate passed on which commit. Manual recorded steps record in an issue created from an issue form generated from the process file, a step per sub-issue, and a GitHub Project shows every run and the step it stands at. To decide: whether a step's kind is declared or follows from its action, and whether a plan becomes such an issue instead of a file in `docs/work/present/`, which changes CLAUDE.md's process.
15. **The checks.** Every script, scenario, clip, page and API operation a step names exists, every module and control a step targets is in `help.json`, and a process no runner verifies is reported. A check that could not run writes no report: `check_docgen` recorded a missing libclang as an error in `docgen.md`, which a contributor committed (#144); an unchecked result prints and leaves the report alone.
16. **Tests link to the test list.** A module's Tests link in the help opens the C++ file holding its tests on GitHub, where a reader meets code. It should open the module's entry in the test inventory the docs build maintains, `reference/tests/unit-tests.html#<module>`, whose test names read as the module's behavior, as a drawn catalog card's Tests link already does. `gen_help`'s `tests_url` is the one place to change, with the module's anchor checked against the inventory.
17. **Guides that add, and `@technical` trimmed**, from the help plan. A control's `@guide` line says only what its line cannot hold, and a module with depth gets a guide about itself, how it works and how to get a look, rather than one walking its controls. An `@technical` appendix keeps only what neither the code, the test names, the base class nor the help says, one or two sentences each; a fact every module of a role shares is said once in the role's base class. Judgment work over hundreds of headers, read here for the processes it holds: a guide's "how to get a look" is a process's steps, so the inventory of step 1 reads the guides and appendices, and what is procedural moves into a process file rather than being trimmed twice.
18. **MoonWalk in MoonCloud.** The member above: its card and consent text, a route and a table in the MoonCloud worker, the privacy policy's entry, and the funnel shown back on the card as Stats shows its totals. It follows step 4, since only the app has an installation id.
19. **The Project and the inbox on GitHub.** The Project with its fields and views, the `walk:` labels, the issue forms generated from MoonWalk, and the map's count of open issues per process. Apple Notes stays the scrapbook it is, the quickest place for a thought. The Inbox is tried as the step after it: a note worth acting on moves there and is triaged. Whether the Inbox can be the scrapbook too is learned by using it, and Apple Notes goes only if it can. Independent of the other steps, so it can run whenever the product owner picks it.
20. **The backlog moves to issues.** A script reads `backlog-core.md`, `backlog-light.md` and `backlog-mixed.md`, proposes one issue per actionable item with its area, its `walk:` label and a link to any analysis it cites, and writes the list for the product owner to review before anything is created. Then it creates the issues, adds them to the Project's Backlog, and the three files are deleted in the same change. An item already shipped or no longer wanted is dropped in the review rather than migrated. After step 19, since the issues land in its Project.

## To decide

- The process of step 9, the first through every runner, and whether it is the one step 5 renders.
- `moonwalk.json` carries the prose (decided in step 5): the live view renders from it, on the site and on a device.

## Appendix: the site's pages and MoonWalk

What happens to each page of the site once MoonWalk is complete (step 12). **Replaced**: the page's file goes, its steps and prose live in a process, and its address forwards to the live view. **Partly**: the procedure moves to a process, and what is reference or explanation stays on a page. **Stays**: reference, explanation, catalog or record, which MoonWalk links into.

| Page | MoonWalk | Remark |
|---|---|---|
| `index.md` | Stays | The home page; it points into the map |
| `moonwalk.md` | Stays | MoonWalk itself: the map, the live view, the plain text for search |
| `gettingstarted.md` | Replaced | Install on an ESP32, join the WiFi, first look; the installer's hard-coded Help link forwards |
| `tutorials/first-light-show.md` | Replaced ✅ | Step 5's pilot: the file is gone and the address forwards |
| `tutorials/first-script.md` | Replaced | Write a first MoonLive script |
| `tutorials/build-your-own-moonmodules.md` | Replaced | A contributor's process: write a module in C++ |
| `tutorials/how-moonlight-works.md` | Stays | Explanation of the cards; the first look links it |
| `tutorials/generative-effects.md` | Stays | Explanation of why effects look as they do; moves under explanation |
| `how-to/installing-to-desktop.md` | Replaced ✅ | Step 5's second page: the file is gone, the address forwards, the platform a choice |
| `how-to/installing-on-linux.md` | Replaced | Run on a Linux board, kept running |
| `how-to/updating-firmware.md` | Replaced | Update over the air, recover from MoonBase |
| `how-to/migrating-over-the-air.md` | Replaced | Migrate a WLED device |
| `how-to/backup-and-restore.md` | Replaced | |
| `how-to/presets.md` | Replaced | Three processes: presets, install from the gallery, share in it |
| `how-to/control-surface.md` | Replaced | Four routes, as a choice |
| `how-to/hold-to-play.md` | Replaced | A worked state document as a process |
| `how-to/watching-the-output.md` | Replaced | |
| `how-to/hub75-panels.md` | Replaced | Under "set up a driver" |
| `how-to/panel-cards.md` | Replaced | The card firmware table moves to a hardware reference page |
| `how-to/logging-an-issue.md` | Replaced | Also the source of the generated issue form (step 19) |
| `how-to/troubleshooting.md` | Replaced | A decision tree, as branches |
| `how-to/adding-a-stream-source.md` | Replaced | A contributor's process |
| `how-to/building.md` | Partly | Build, flash, update, Docker, Teensy and the IDF bump become processes; the ports, pins and firmware facts move to `reference/build-system.md` |
| `how-to/audio-input.md` | Partly | Setup becomes a process; the microphone and codec table stays as reference |
| `how-to/led-drivers.md` | Partly | Choosing a driver is reference; setting one up is a process |
| `how-to/led-signal-integrity.md` | Partly | The diagnosis becomes branches; the electrical background stays as explanation |
| `how-to/multi-board-installation.md` | Partly | The installation's steps become a process; the rest reads as a case study |
| `how-to/home-automation.md` | Partly | The three recipes become processes; its Hyper-V and Zigbee setup is the parked trim |
| `how-to/faq.md` | Partly | The questions stay; an answer that is a procedure links its process |
| `reference/integrating.md` | Partly | The HTTP recipes become API processes; the routes become the OpenAPI reference (step 13) |
| `reference/testing.md` | Partly | Adding a test and running scenarios become processes; the strategy and the inventories stay |
| `contributing/principles-and-process.md` | Partly | The gates become the developer process (step 10); the principles and roles stay |
| `contributing/coding-standards.md` | Stays | The rules |
| `contributing/documentation-standards.md` | Stays | The rules; it gains the stage files' authoring rules |
| `explanation/architecture/*.md` (9 pages) | Stays | The architecture, MoonWalk's systems |
| `explanation/mooncloud.md`, `security.md`, `why-we-write-our-own.md` | Stays | Explanation |
| `moonmodules/**` (8 pages, plus the generated catalogs) | Stays | The catalog, generated from `help.json`; the processes link into it |
| `reference/hardware/*.md` (10 pages) | Stays | Hardware reference; a bring-up procedure in one moves to a process |
| `reference/MIGRATING.md`, `attributions.md`, `build-system.md`, `performance.md` | Stays | Reference |
| `reference/metrics/*.md` (4 reports), `reference/tests/*` (generated) | Stays | Generated records |
| `legal/privacy-policy.md` | Stays | It gains the MoonCloud member's entry (step 18) |
| `friend-repos/*.md` | Stays | Digests of other projects |

**Count**: of the 72 written pages, 18 are replaced (two already), 10 partly, and the rest stay. Every tutorial and how-to the reader follows is in MoonWalk, and what stays is what a reader looks up.

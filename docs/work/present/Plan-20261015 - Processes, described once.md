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

The process model builds on [the help in the UI](Plan-20261014%20-%20Help%20in%20the%20UI%2C%20from%20the%20docs.md). A step that acts in the app names its target by the keys `help.json` already uses: the module's type name and the control's name, the address a tour step needs as well. The hook is written once, on the step, and the generator writes the reverse index into the help, so each module and each control lists the processes that use it and can start one there. Arazzo names API operations as targets, so a module or control as a target is an `x-` extension, which the specification provides for.

## The runners

A runner performs the steps it knows how to and skips or describes the rest:

- **Tests**: the scenario runner and the UI tests run a process's steps and check each expectation.
- **MoonTube** films the same steps with their captions.
- **The app** walks a user through them as a tour: each step opens its screen, points at a control and says what to do.
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
- **Share**: a card's `{ }` button copies the JSON, and an issue in the gallery repo submits it; its acceptance makes the entry. Recorded, and already an instance on GitHub, the pattern step 11 generalizes.
- **Record a preview**: the preview bar's ⏺ saves a three-second GIF to attach to that issue.

## Existing content, estimated

A first count from the titles and file shapes, which step 1 confirms. Procedural content moves; reference and explanation stay where they are and are linked.

| Content | Count | Fits the model | How |
|---|---|---|---|
| MoonTube clips, also the UI tests | 14 runs, about 250 steps | directly | UI steps with a caption and an `expect` become process steps; the film and the tour are runners |
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

Neither a Confluence nor a Jira. Confluence is a wiki, and the docs site is ours; Jira tracks work, and GitHub's issues and Projects do. What gets built is a JSON Schema, the process files, one generator of the order of `gen_help`, and an adapter in each runner that exists: the scenario runner, MoonTube, the app's tour and the docs build. Out of scope: a process engine, an editor, a server, and states or permissions beyond GitHub's.

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

No server. The definitions are files in the repo, the generator runs in the docs build as `gen_help` does, `moonwalk.json` is a static file on Pages, and the runners are the tools that exist: MoonDeck, the scenario runner, MoonTube's Playwright runs, the browser and the agents. GitHub records the runs. Two existing tools are worth a look in step 2: Redocly's Respect runs Arazzo workflows against an API and Spectral lints them, adopted only where they replace code of ours.

## Had this come before the help

Little would differ, because the order is the textbook one. The help is the reference, what a module and a control are, and processes are tutorials and how-tos that point into it, which Diátaxis builds on top of a reference. The keys the hooks need, a type name and a control name, are in `help.json` already, so the help does not change. Three things would have looked different:

- **The how-tos this branch wrote** from the catalog cards, such as LED drivers, HUB75 panels, watching the output, audio input, control surfaces, presets and home automation, would be process files rather than pages. They move in step 10.
- **A module guide that walks a procedure**, rather than saying how the module works, would be a process step. The guide rules already keep controls out, so these are few.
- **Publishing and consent** serve both files, which they already do the same way.

## Where the reader is: the device and MoonCloud

MoonWalk offers the next process rather than the whole list, so it needs to know what is done. Three sources, in order:

- **The device's own state** is the first record. A step is done when its `then` holds on the device now: a WiFi with an SSID is set up, an Effects card holding an effect has had its first light. The app reads `/api/state` and evaluates the expectations in `moonwalk.json`, so nothing is logged, and a step done by hand, by the installer or through a tour counts the same. **The next process** is the first one not done whose `dependsOn` all are, the frontier of the dependency graph in topological order (Kahn's algorithm), so a device with its WiFi set up never offers setting it up. The website cannot read a device, since an https page cannot call an http address, so the device's state reaches the map only through the app. That is why the app hosts the map as well.
- **The browser** keeps what the state cannot show and the reader marks: a page read, a tour finished. Per browser, as the pilot does.
- **MoonCloud, with consent.** A MoonWalk member of MoonCloud, its own checkbox and off until switched on like Stats and Talk, keeps those marks under the installation id, so they follow the device to every browser that opens it. In aggregate it answers what no single device can: where people stop, a funnel per process, which shows the page or the step that needs work. It sends process and step ids and nothing typed, no name and no address, under the privacy policy's rules for every member. The website's own marks stay in the browser, since a website has no installation id.

## GitHub: issues, Projects, CI, the backlog and notes

MoonWalk in the repo is the definition: what the processes are. GitHub holds the work and the runs: what is to be done, in what order, and what happened. Each GitHub feature gets one job, so nothing is tracked twice.

- **Issues are the work items**: a bug, a feature request, a backlog item. Each carries a label naming the MoonWalk process it concerns (`walk:<process-id>`), so MoonWalk's map shows the open issues on each process, read from GitHub's public issues API, and a process with many is where the docs or the code need work. The issue forms are generated from MoonWalk: the steps of Log an issue become the bug form's fields, which the repo lacks today.
- **The backlog becomes issues.** Today it is three Markdown files of 3,408 lines beside 4 open issues, two backlogs for one job. The textbook backlog is an ordered list of items (Scrum's product backlog), and GitHub's form of it is issues ordered in a Project. Decided: each actionable item becomes an issue (step 16), created in bulk by a script the product owner reviews, and a long analysis stays in `docs/work/future/` linked from its issue. The backlog files go. The cost: an agent reads the backlog through `gh issue list` rather than a file, and it needs the network to.
- **A Project orders and tracks them**: one Project for MoonLight, with a Status field (Inbox, Backlog, Next, In progress, In review, Done), a Stage and a Process field holding MoonWalk's ids, and an Area. A board by Status is the Kanban view, and a table grouped by Stage is the work laid on the journey map's own columns.
- **Notes reach the Inbox**, the step after the product owner's Apple Notes scrapbook (step 15 tries whether it can replace it). A Project's draft item is a note that is not yet an issue: a title and a line, captured from the phone with GitHub's app, with `gh project item-create`, or by the agent when the product owner says to note something. Triage promotes a draft to an issue or deletes it. The work an agent spots and names in one sentence lands there too, rather than only in the chat.
- **Actions run what is already executable.** A MoonWalk step names a workflow rather than copying its steps, the local gates post check runs on the commit (step 11), and a pull request shows which gate passed where.
- **Access**: the agents' `gh` needs the `project` scope (`gh auth refresh -s project`); it has `repo` only today.

## The steps

1. ✅ **Inventory.** Done: [the processes inventory](../future/processes-inventory.md), 71 entries. The Researcher maps every process that exists as content: its steps, where each is written, who follows it and what verifies it. Sources: the table above, plus the web installer. The result counts the duplicates per process. Logging an issue is a how-to today and GitHub carries no issue or pull request template, so the inventory also notes the processes that have no home.
2. ✅ **The model.** Approved 2026-10-16: [MoonWalk](../future/moonwalk.md) and [its schema](../../../moonwalk/moonwalk.schema.json). The journey, the process format and the step shape chosen from the candidates above, written as a short spec with its JSON Schema. The product owner approves it.
3. **A pilot: the whole journey on a map.** A working slice first, then iterate. `gen_moonwalk.py` validates the stage files and writes `moonwalk.json` in the docs build. The eleven stage files carry every process of the inventory as a stub (title, actor, goal, page), and three carry their steps: installing and first light on the desktop, and the commit, so one map shows both halves. A map page on the site draws it with Cytoscape.js: zoom, search, filters, `?focus=` from every page, and done marks in the browser. What the pilot teaches changes the model before step 4 builds on it.
4. **The REST API in OpenAPI.** The device's routes described in OpenAPI 3.1, checked against the server's routes in a test, so a step names an operation and integrating's reference is generated.
5. **The mapping.** Each artifact from step 1 mapped onto the model: what becomes a process file, what is generated from one, and what goes. It ends with the number of copies removed.
6. **One process through every runner.** The docs page and diagram generated, the MoonTube clip, the test, the tour and an agent following it. First light on the desktop is a candidate, short and seen by every user.
7. **The developer process.** The commit, merge and release gates become process files: each check a step with its trigger, the diff touching a path, its MoonDeck script and its expectation. CLAUDE.md keeps the principles and roles and links the process, so the gate list has one home that agents and the product owner both follow.
8. **Tests link to the test list.** A module's Tests link in the help opens the C++ file holding its tests on GitHub, where a reader meets code. It should open the module's entry in the test inventory the docs build maintains, `reference/tests/unit-tests.html#<module>`, whose test names read as the module's behavior, as a drawn catalog card's Tests link already does. `gen_help`'s `tests_url` is the one place to change, with the module's anchor checked against the inventory.
9. **Guided tours: the app as a runner.** The help is static today, one module's text at a time. A tour is a process the user performs while the app points the way, for a first user and for an advanced topic such as a fixture or a sync setup. To analyze:
    - **The textbook.** The product tour, or coach marks, as Shepherd.js, Intro.js and driver.js build it: an ordered array of steps, each a target element, its text and an optional action. A tour that branches is a state machine instead, which Arazzo's `onSuccess` and `onFailure` already express.
    - **Going to a screen.** A step opens its screen by an address, so the app's state is reachable by URL: a route per page, module and control, which a step, a docs link and a bookmark all use.
    - **One source with MoonTube.** A MoonTube run file is an ordered list of UI steps with an action, a caption and an `expect` per step, performed by the UI tests and filmed by `mtvideo`. The tour is the third caller of the same steps.
    - **One process with the scenarios.** A scenario JSON is an ordered list of steps against a running pipeline, each with what it expects. Step 5 decides whether the scenarios, the clips and the tours reduce to one step shape.
    - **Done from the device.** The app evaluates each step's `then` against its own state and offers the next process, as § Where the reader is describes.
    - **The docs.** A tour is a tutorial in Diátaxis' terms, so it can take over a how-to whose steps are all in the app; a guide that also reaches outside the app stays a page and can start a tour.
10. **The migration**, process by process, each old copy deleted as its process file takes over, so the docs and the tests stay complete throughout. It includes every page named for an activity, building, testing, integrating, troubleshooting and the how-tos, which become pages generated from their process; what on them is reference rather than steps moves to a reference page.
11. **Executions on GitHub.** A process file is the definition and one run of it is an instance, the split workflow engines make. GitHub keeps the instances; the definition stays in the repo as the one copy. Steps come in two kinds:
    - **Volatile steps** act on a device or in the app, such as a user choosing an effect. Nothing records them, since nothing outside the device changes.
    - **Recorded steps** change the repo or what ships: a plan step, a commit gate, a merge, a release, a verification on hardware, a gallery submission. Each run leaves a record.

    A run's measurements, the step metrics above, ride on its record: a check run's output on GitHub, and the value series repo-health draws.

    Automated recorded steps record as check runs on the commit or pull request, the local gates included through `gh api`, so a pull request shows which gate passed on which commit. Manual recorded steps record in an issue created from an issue form generated from the process file, a step per sub-issue, and a GitHub Project shows every run and the step it stands at. To decide: whether a step's kind is declared or follows from its action, and whether a plan becomes such an issue instead of a file in `docs/work/present/`, which changes CLAUDE.md's process.
12. **The checks.** Every script, scenario, clip, page and API operation a step names exists, every module and control a step targets is in `help.json`, and a process no runner verifies is reported.
13. **Guides that add, and `@technical` trimmed**, from the help plan. A control's `@guide` line says only what its line cannot hold, and a module with depth gets a guide about itself, how it works and how to get a look, rather than one walking its controls. An `@technical` appendix keeps only what neither the code, the test names, the base class nor the help says, one or two sentences each; a fact every module of a role shares is said once in the role's base class. Judgment work over hundreds of headers, read here for the processes it holds: a guide's "how to get a look" is a process's steps, so the inventory of step 1 reads the guides and appendices, and what is procedural moves into a process file rather than being trimmed twice.
14. **MoonWalk in MoonCloud.** The member above: its card and consent text, a route and a table in the MoonCloud worker, the privacy policy's entry, and the funnel shown back on the card as Stats shows its totals. It follows step 9, since only the app has an installation id.
15. **The Project and the inbox on GitHub.** The Project with its fields and views, the `walk:` labels, the issue forms generated from MoonWalk, and the map's count of open issues per process. Apple Notes stays the scrapbook it is, the quickest place for a thought. The Inbox is tried as the step after it: a note worth acting on moves there and is triaged. Whether the Inbox can be the scrapbook too is learned by using it, and Apple Notes goes only if it can. Independent of the other steps, so it can run whenever the product owner picks it.
16. **The backlog moves to issues.** A script reads `backlog-core.md`, `backlog-light.md` and `backlog-mixed.md`, proposes one issue per actionable item with its area, its `walk:` label and a link to any analysis it cites, and writes the list for the product owner to review before anything is created. Then it creates the issues, adds them to the Project's Backlog, and the three files are deleted in the same change. An item already shipped or no longer wanted is dropped in the review rather than migrated. After step 15, since the issues land in its Project.

## To decide

- The process of step 6, the first through every runner.
- Whether `moonwalk.json` is fetched by the app like `help.json`, and so carries the same consent.

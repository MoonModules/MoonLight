# MoonWalk

MoonWalk is the model every MoonLight process is written in, and the map that shows them. The processes live in the stage files under [moonwalk/](../../../moonwalk/), validated by [the schema](../../../moonwalk/moonwalk.schema.json). How to use the map and write a process is [the MoonWalk reference](../../reference/moonwalk.md); why the model has its shape follows.

## What it does

- **One model** holds every process, the end user's and the contributor's, so the whole journey is one map.
- **The map is navigable**: on the website and in the app, zoomed out to the whole model, zoomed in by search and filters, and opened at the place the reader is, a docs page or the card open in the app.
- **A process already done is not offered again**: what the reader has done is known, and the map and the app's suggestions leave it out.
- **The pages named for an activity are generated from it**: building, testing, integrating, troubleshooting, updating the firmware, logging an issue and the rest of the how-tos. So a process carries its prose as well as its steps.
- **Every runner reads the same steps**: the docs, the app, the tests and the agents.

## Three levels, one textbook each

| Level | What it is | Textbook | Zoom |
|---|---|---|---|
| The journey | stages, left to right: install, first light, everyday use, advanced use, maintain, integrate, plan and set up, develop, run on hardware, test, document and commit, review and merge, release | the user story map (Patton) | zoomed out |
| A process | a goal, its actor, the processes it depends on, its steps, its branches | an Arazzo workflow | middle |
| A step | a precondition, an action, an expectation | Given, When, Then (Gherkin) | zoomed in |

**Arazzo-shaped, not Arazzo.** Arazzo requires every step to call an API operation or another workflow, and most of the journey's steps are a click in the app, a command or a person plugging in a board. So the model keeps Arazzo's structure and names (`dependsOn`, `inputs`, a workflow per process, several per document) with its own step shape. A process whose steps are all API calls can be exported to Arazzo, which is where Redocly's Respect and Spectral come in.

## One YAML file per stage

The number in a stage file's name orders the backbone. The files are YAML because a process carries prose: multi-line text, Markdown and comments read well in YAML and badly in JSON strings, and MkDocs already depends on PyYAML. The schema validates YAML and JSON alike. YAML 1.1 reads an unquoted `on`, `off`, `yes` or `no` as a boolean, so the generator rejects a boolean where a name belongs.

## A step names an intent

A `when` names one action by intent rather than gesture, so each runner picks its own gesture: the app points at the card's +, and a test applies a state document. The runner list is derived from the action, never written down. A runner meeting an action it cannot perform describes it, as the scenario runner prints SKIP.

**A process is a how-to, never a tutorial.** Diátaxis keeps the two apart: a tutorial is a lesson with an outcome its teacher chose, a how-to serves the reader's own goal. A step says what to decide or do toward that goal; an example value goes in its detail, never in its check, and a check tests only what holds for any reader. The lessons live in MoonTube's clips and in the tutorials, and a step may embed a clip.

Running the same script from several processes is a call, its explanation living once, in the script's MoonDeck section or the process that owns it. **A choice is an input**, a JSON Schema as Arazzo's workflow inputs are, and a step runs only for the option its `given` names.

**Text and shape have caps, as the help's cards do.** Text past a cap goes to its Diátaxis kind rather than being squeezed: a table to a reference page, a reason to an explanation, a symptom to Troubleshooting, a second route to its own process.

**A step has no loop**, as Gherkin has none: a format with variables and repetition is a programming language, so work that repeats over boards or measures a spread stays a script a `run` step names. **A `call` gets no derived `then`**, since its result is not in its action; one is written by hand where the call changes the device. **A process no runner verifies is listed, not counted**, since a count of them could never fall: they are the ones a person alone can judge.

## Done is observed

**A step is done when its `then` holds now.** The app reads its own state, so "WiFi is set up" is done the moment a network is known, however it got there. This is the onboarding checklist's textbook rule, completion observed rather than recorded, and it needs no storage. A process is done when every expectation the state can answer holds, and it has at least one: its last step alone is not enough, since a last step is often one the state cannot see.

What the state cannot show is vouched for by whoever knows it. The installer marks the install done, since it knows the platform for certain where a running device cannot tell a desktop from a Linux board or a container. MoonDeck marks a process done when every script its steps run last passed, and a run counts only while it is newer than the sources it covers, as a firmware build does. Those marks stay in the browser. The website cannot read a device, since an https page cannot call a device's http address, so there only the reader's marks count.

## The definition in the repository, the runs on GitHub

A process file is the definition and one run of it is an instance, the split workflow engines make. The repository keeps the one definition; GitHub keeps the instances and the work. It is no process engine, editor or server: a schema, the process files, one generator and an adapter in each runner.

- **Two kinds of step.** A volatile step acts on a device or in the app, and nothing records it. A recorded step changes the repository or what ships, such as a commit gate, a merge, a release or a verification on hardware, and leaves a record of its run. The kind follows from the process, not the action: every gate process MoonDeck lists, plus `pick-spec-plan` and `contribute-pr`, records its steps.
- **Every CI job runs locally too.** A workflow is a thin caller of the MoonDeck script its process step names, which `test_workflows.py` holds it to, so GitHub is one runner of the processes among others: [run_process](../../../moondeck/MoonDeck.md#run_process) runs a process's script steps, and [act](https://github.com/nektos/act) runs the Linux jobs in containers. Only publishing (the release upload, Pages, the container registry) is GitHub's alone.
- **What copies a process is generated, what points at it is written.** The pull request template is generated from `contribute-pr`'s questions, the bug form from Log an issue; `CONTRIBUTING.md` is one sentence linking the process.
- **The work is the Project's cards**, mirrored into `docs/work/backlog_mirror/` by [project_mirror](../../../moondeck/MoonDeck.md#project_mirror), so git versions every card edit. A card accepted moves to Next and becomes an issue. A doc links an issue, the item's own or a tracking issue listing its card, since GitHub keeps an issue's address and not a card's, as [Pick, spec and plan a topic](../../moonwalk.md#pick-spec-plan) describes, and a plan that ships or stops becomes a closed issue labeled `plan`. A card's Process field, rather than a label per process, links work to the process it serves, and the map lists each process's open cards from it.

| Project field | Holds |
|---|---|
| Status | Inbox, Backlog, Next, In progress, Waiting (blocked on someone outside the project: a reply, a part, a tester), In review, Done |
| Area | the `src/` tree and the module roles, plus Process and docs |
| Module | the module's class name, empty for an item across modules |
| Kind | Bug, Feature, Restructure, Investigation |
| Trigger | what makes the item worth building, so a met trigger is found by scanning one column |
| Platform | classic, S3, P4, C3, Teensy, desktop, Pi, all |
| Stage, Process | the MoonWalk stage and process the item serves |

## The map

`moonwalk.js` draws `moonwalk.json` with Cytoscape.js, from cdnjs and about 430 KB, so it never enters the firmware. The layout is the story map's grid, stages as columns and processes stacked in `dependsOn` order, computed in a few lines rather than by a layout library. The parts of a page that are reference rather than steps, such as a table of ports or a test strategy, stay on a reference page and are linked.

# Plan: one test engine

Plan, for the product owner's approval (2026-10-20). Automated tests are one of MoonLight's core strengths, and they run as several separate machines: C++ unit tests, scenarios on the desktop and on a live device, JS and Python tests, and the Python checks (docgen, prose, code, specs). Each has its own runner, its own output and its own report. This plan finds one model above them, so that what exists, what ran and what it found are described once and read by everyone it concerns: the help, MoonWalk, MoonDeck, the docs and CI.

It starts from [Processes, described once](Plan-20261015%20-%20Processes%2C%20described%20once.md), step 17: the map should show which tests cover a step, and the help which tests cover a module and a control. That needs a machine-readable index of the tests, which does not exist. The index is this plan's first result, and the rest follows from it.

## Why

- **The test pages are generated from the source and read by nobody's code.** `unit-tests.md` and `scenario-tests.md` are Markdown, so the help can link a module to a page but not a control to the scenarios that set it, and MoonWalk cannot ask which steps a test proves.
- **Every check reports in its own shape.** docgen, prose and code each parse their own table in `docs/reference/metrics/`; the scenarios write observation blocks; the unit tests print to a console. `_ratchet.py` shares the comparison, not the record.
- **What a rule is and what checks it live apart.** The coding and documentation standards are prose pages with 161 rules, 63 of them checked by a script, and nothing ties a rule to its check except a sentence.
- **The website has moved to drawn pages** (the card decks, the map). The tests deserve the same: searchable, filterable, linked, with the proof of what ran.

**What it removes**: the two generated Markdown pages and their generator's page-writing, each check's own report parsing, and over time the hand-written rule lists of the standards pages, once each rule is a check that carries its reason.

## The textbook candidates

To decide in the model step, each named so the choice is explicit:

- **Results: JUnit XML**, the de facto xUnit result format. doctest, CTest (`--output-junit`), pytest (`--junitxml`) and `node --test` (`--test-reporter=junit`) already write it, and GitHub and most CI tools read it.
- **Findings: SARIF 2.1.0**, the OASIS standard for static analysis results, which CodeQL writes natively and GitHub's code scanning reads. The Python checks' findings (a rule, a file, a line) fit it.
- **Kinds: the test pyramid** (Fowler) and **Google's small, medium and large tests**: unit, scenario on the desktop, scenario on a live device, a size each rather than a name each.
- **Traceability: the traceability matrix** and **living documentation** (specification by example): a test names what it proves, and a report derives from the names, as a process step's tests do.
- **Heavier, and not expected to fit**: Allure and similar report frameworks, test-impact analysis (Bazel, pytest-testmon) and a test-management server. The model is a file and a record, not a service.

## What exists (step 1, the inventory, 2026-10-20)

Measured on the working tree; the commands are in the Researcher's report, and the counts below were spot-checked.

| Kind | Count | Runner and output | Identity today |
|---|---|---|---|
| C++ unit tests (doctest 2.4) | 244 files, 2,603 cases | `test_desktop.py` runs the binary; console only. A `junit` reporter exists unused, and `ctest` is registered but nothing calls it | file and the case's sentence |
| Desktop scenarios | 32 JSON files, 584 steps | `run_scenario.py` and `scenario_runner.cpp`; `PASS/FAIL` and `MEASURE` lines; `observed` blocks written back into the file | the file stem, and a step's `name` |
| Live scenarios | the same JSON, plus `run_network_live.py` and `run_network_roundtrip.py`, which are scripts | `run_live_scenario.py` over REST; the two probes print to the console and keep no record | as above |
| Python tests | 40 files, 456 tests | pytest through `test_host.py`; console only | pytest node id |
| JS tests | 45 files, 321 tests | `node --test`; console only | file and test string |
| Python checks | 15 `check_*.py`, 8,000 lines | each reports in its own shape: docgen, prose and code write three different Markdown tables with a ratchet; the rest print | a script and its arguments |
| CI | 5 workflows | CodeQL uploads SARIF to GitHub natively and keeps none; prose posts annotations | |

What the inventory settled:

- **Nothing writes JUnit or SARIF.** Four runners could with a flag (doctest, ctest, pytest, `node --test`); the scenario runners and every `check_*.py` could not without code.
- **Run records are one file per machine and gitignored**: `walk-runs.json` (a pass and a tree id per command), `durations.json`, 28 logs. No duration or observation sits beside a pass.
- **The observations are committed, 140 of them inside the scenario files** (about 700 KB), rewritten on every passing run, so a run dirties tracked files and CI warns a fork about them.
- **The three ratchets duplicate each other**: `_ratchet.py` has 37 lines, and docgen (217), prose (99) and code (47) each parse their own table.
- **One parser serves the tests already**: `_test_metadata.py` is read by the page generator, MoonDeck, both scenario runners and `test_desktop.py`. It is the seed of the registry.
- **A registry keyed on help.json types misses a quarter of the tags**: 66 `@module` and `@also` names are classes or helpers, not types (`Buffer`, `JsonSink`, `MoonModule`).
- **A scenario names an instance, never a type**: 55 distinct `(id, key)` pairs, 44 of 50 resolvable to a control of a help.json type, and prefix matching mislabels some. Five pairs are wired in `main.cpp` and have no type in the file.
- **No test names a process or a step**, and of the 22 module types the steps name, only `MidiService` has neither a unit test nor a scenario.
- **The test pages are generated at build and not committed** (`unit-tests.md` 413 KB, `scenario-tests.md` 165 KB), and 93 of 145 help modules link to a test entry.
- **Gates**: 62 script references across the process files. Not in any gate: clang-tidy, clang-query, the CodeQL fetch, footprint, the nonblocking check (only through `check_code`) and the network probes.

## The model (step 2, approved 2026-10-20)

[The schema](../../../moondeck/docs/tests.schema.json) holds the records. Three, all JSON; JUnit and SARIF are what runners and GitHub exchange, and the working model is ours:

1. **The registry, `tests.json`**: every test by a derived id (`unit:test/unit/core/unit_X.cpp::the digest matches`, `scenario:<name>`, `scenario:<name>#<step>`, `python:`, `js:`, `check:`), with its kind, size, title, file, the **subjects** it is about, the **controls** it sets or expects, the **steps** it proves, and the call that runs it. Generated like `help.json`.
2. **A run**, under `build/`, never committed: the test id, the git tree id it saw, a status, seconds, and **measurements** (target, metric, value, window). A runner that writes JUnit is read into it.
3. **A rule and a finding**, in the shape of SARIF's `reportingDescriptor` and `result`: a rule carries its reason and its standards anchor, a finding the rule id, a file and a line.

No runner is a prerequisite. doctest, the scenario runner, pytest and each check stay, change or go as the model is served best, and the engine is the model with MoonDeck as its one runner.

**Which textbook solution** is a decision, taken here:

| Question | Choice | Why |
|---|---|---|
| Result exchange | JUnit XML | the de facto xUnit format; four runners write it on a flag, and GitHub reads it |
| Finding exchange | SARIF 2.1.0 | the OASIS standard; CodeQL writes it and GitHub code scanning reads it |
| Working model | our JSON, above | JUnit has no controls, steps or measurements, and SARIF has no pass or fail; the model carries what neither does and reads or writes both at the edge |
| Test kinds | the test pyramid, sizes small, medium, large | unit, desktop scenario and live scenario are three sizes, not three machines |
| Traceability | the traceability matrix | a test names what it proves, and the report derives from the names |
| Identity | derived, as pytest's node id | see To decide |

**Where it is served.** The same way as the help and the map: generated in the docs build, published on Pages, and fetched by the app under the existing consent; the firmware carries none of it. At 413 KB and 165 KB as Markdown it is too large to load whole, so it is published sharded: a small index of subjects and counts, and a file per subject fetched when one is opened, as MoonWalk fetches a process's text. The device app opens a module's Tests as a link to the site's page, as its help does; showing a module's tests inline from its shard is a later, optional step.

## Hooks into the help and the walk

- **The help**: a module's Tests link opens its entry in the drawn test page (today it opens the generated page's section), and a control's help lists the scenarios that set or expect it, read from the scenario steps themselves, with no tagging.
- **MoonWalk**: a test names the steps it proves, the link living in the test (a `steps_proved` key in a scenario, a `@step` tag in a unit test); the map shows "Tested by" under a step, a test naming a step that does not exist fails the build, and a device-changing step no test covers is reported.
- **MoonDeck**: it runs a test or a kind by its registry entry and records the run, as it does for the gates.

## The steps

Each step ends in something that works and can be seen, and nothing waits for a later step to be useful.

1. ✅ **Inventory**, done 2026-10-20: above.
2. ✅ **The model**, approved 2026-10-20 with the derived id: this section and [the schema](../../../moondeck/docs/tests.schema.json).
3. ✅ **`tests.json` and a drawn page on the website**, done 2026-10-20. `moondeck/docs/gen_tests.py` writes the sharded registry (2,607 tests, 195 subjects, an index of 13 KB and a largest shard of 235 KB, 44 KB gzipped), the docs build publishes it as `tests/index.json` and a shard per subject, and `docs/assets/tests.js` draws [the test page](../../reference/tests.md): subjects grouped by the help page of the module (Light · Effects, Core · Services) or by the domain of the tests for internals, with counts, a search, kind filters, each test with its file and line and the command that runs it, and a scenario's steps with the contract and observed range of each measured step, with every test's name in the page as plain text for search. `unit-tests.md` and `scenario-tests.md` are gone, redirected, and the help's Tests links point at the new page. The build found and the fix removed eleven unit tests duplicated in `unit_MoonLiveLayout.cpp`. MoonDeck still draws its own scenario views from `_test_metadata.py` and the perf table in `generate_test_docs.py`, which step 6 replaces with the registry.
4. ✅ **Help and MoonWalk read it**, done 2026-10-20. A control's help lists the scenarios that set or expect it (46 controls, each link opening that test on the page, proved in the device app as well as on the site's decks), scenarios declare their instances' types once (`instances`, and the generator refuses a step naming an undeclared one), and a step shows "Tested by" for the tests that name it (`steps_proved` in a scenario, `// @step` above a unit test) and for the scenarios that share its control. The build refuses a test naming a step no process has. The unit tests ran 2,585 cases with none failing and the scenario sweep 17 passed with none failing, after the new keys went in. *Result: the links in the help and the map, on the site and in the device's app.*
5. ✅ **The gaps**, done 2026-10-20: the page's Gaps view lists the 25 device-changing steps no test covers, the 18 help modules no test is about, and the 9 controls a scenario sets that the help does not know. *Result: the list of what to test next.*
6. **Runs.** MoonDeck records a run in the model's shape, the cheap kinds first (pytest, `node --test` and doctest write JUnit on a flag), and the page shows the last result and its tree id beside each test. *Result: proof of what was tested, on the page.* Started 2026-10-20, units done: doctest's JUnit is read into runs, followed live so MoonDeck's log still streams, with 2,583 of the registry's 2,646 tests showing a result (the others are skipped on this host, or a case a macro generates, which the static parser cannot see; subcase runs count for their case, and the shared `.inc` bodies are one test per target). The same step registers the other kinds in the registry, each with its runs: the scenarios (the runner's `PASS` and `FAIL` lines), the Python and JS tests (pytest and `node --test` write JUnit on a flag), and the Python checks themselves (`check_docgen`, `check_prose`, `check_code` and the rest, as `check:<script>` entries whose result is their exit and whose findings are step 9's).
7. **Observations out of the committed files.** The contract a scenario compares against stays committed and small, the sample windows move to the run record, and a passing run stops rewriting tracked files. *Result: the scenario files shrink and a run leaves the tree clean.*
8. **The rules: the standards, enforced.** Every rule in the coding and documentation standards that a script can check is checked by one, so a process step's "apply the standards" is a gate rather than a reading. A check carries its rule, with its reason and its link, so the standards pages generate from the rules and shrink. The written standards stay as the one home of the rules only judgment can apply (a textbook name, the simple solution, minimalism), as Google's style guides sit beside their linters. The two pages hold 161 rules: 63 checked, 38 checkable (26 small, 11 medium, 1 large) and 60 judgment. Done before this plan: the inventory of every rule with its mark (2026-10-19, with ten contradictions between the pages and `.coderabbit.yaml` fixed), and the idle-module rule, checked by the module contract test and by `check_code`'s count of mutable statics. To do: the checks for the checkable, cheapest first, each pinned by a test that fails on the violation, joining the script that already reads that kind of file; one line per checked rule on the standards page, with a test that fails when a named check reports no such rule; and the review step in "Review and finish a contribution" then reading only the judgment rules. *Result: a smaller standards page, and each rule one click from its check.*
9. **The checks in one shape.** The Python checks write their findings as the model's findings, so the three ratchet tables become one report and the duplicated parsing goes (docgen 217 lines, prose 99, code 47 beside a shared 37). *Result: one report, less code.* The checks are already registered by step 6, so a check, its rule and its findings are one chain in the registry: the test page lists `check:check_docgen`, its rules and what it last found.
10. **GitHub.** Runs as check runs on the commit, findings as SARIF in code scanning, as the plan for executions on GitHub already intends. *Result: a pull request shows what passed and what a change broke.*

## To decide

Recommendations, now checked against the inventory:

- ✅ **The test id: derived, not declared**, as pytest's node id and JUnit's `classname` and `name` are: the kind, the file and the test's name (a scenario's own `name`). A declared key would mean tagging about 240 files, and a rename is then a new test, with git as its history. What a test proves is carried by tags (modules, controls, steps), never by its id. The inventory confirms nothing declares one today.
- ✅ **A subject is not always a help.json type.** A registry keyed only on types would lose 66 tags, so a test's subject is a module type, a class or a script, and a link to help and MoonWalk exists where the subject is a type.
- ✅ **A scenario declares its types.** Inferring a type from an instance name is lossy, and the generator once carried exactly that kind of guess. Each scenario names its instances once (`Grid` is a `GridLayout`), about 29 distinct ids across 32 files, and the control-level links come from that.
- ✅ **Run records: not committed. This changes for the observations.** The textbook commits a baseline and keeps measurements out of the repo. Here the observed windows are both the baseline and the history, and they are rewritten into the committed files on every pass. Split them: the contract a scenario compares against stays committed and small, the sample windows move to the run record under `build/`, and a passing run stops dirtying tracked files. This is the largest saving the inventory found.
- **Observations are measurements, a record of their own**: a target, a metric, a value and a window, which JUnit carries as properties and benchmark tools keep as a series, never as pass or fail.

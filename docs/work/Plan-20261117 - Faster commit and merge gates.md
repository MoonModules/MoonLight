# Plan: faster commit and merge gates

Plan of 2026-11-17, a Refactor, for the product owner's pick. The commit and merge gates cost more time than the change they guard: a round on 2026-11-17 ran the desktop build three times, the suite three times and the docs checks four times, each rerun after a one-line fix the agent could have avoided by writing the rule right the first time. The gates stay as strict as they are; what changes is when a rule is applied and how much a check reads.

## What exists

- **The trigger table** is machine-readable already: each gate in `moonwalk/13-document-commit.yaml` and `14-merge.yaml` carries its `given: changed:` globs and the script it runs. An agent reads it and types the gate chain by hand each round.
- **The checks read the whole tree**: `check_docgen`, `check_prose` (Vale) and `check_code` (lizard, jscpd) measure every file and compare totals with the committed counts, though a finding is listed per file already. `test_desktop.py` runs all 3,394 cases, `test_host.py --python` all 770.
- **Rough costs, measured on this laptop on 2026-11-17**: a desktop build after a widely included header changes, 5 to 8 minutes; the desktop suite, about 3 minutes; the Python tests, 1.5 to 2.5 minutes; `collect_kpi`, 1 to 2 minutes; an ESP32 build, several minutes; docgen, prose and check_code, under a minute each; the JS tests, 5 seconds.
- **Rules are applied after the fact**: an agent writes, the checks find a help line over its cap, a hard wrap, a negation or a function over the complexity limit, and the agent fixes and reruns. On 2026-11-17 that loop alone cost four docgen runs on one file.
- **Step 1 of the host plan, the same day, showed where a round's time goes**: a rename's two compile errors (a header declaration and a test helper) surfaced one full rebuild each, 5 to 8 minutes apiece, since the renamed headers are widely included; a BSD `sed -i -E` backup file (`ReceiverList.h-E`) was staged as the rename target and only the Reviewer found it; Network Send's help passed its 250-word cap after one added sentence, found by docgen four runs later; and the ESP32 build and the Python tests run side by side slowed each other on the laptop's cores.
- **CI runs every build and test** on each push (macOS, Windows, Linux, Android, Teensy, the sanitizers, JS, Python, Vale), and the merge gate reruns much of it locally.
- **The gate runner** the Project's card "A gate runner that runs each commit check once, on its own inputs" described is this plan's step 2; the card is removed, so the topic has this one home.

## End states

| | End state | Gains | Loses |
|---|---|---|---|
| A | **The gate runner only**: the card as written. | Each gate runs once per round, a rerun only where its inputs changed. | Every check still reads the whole tree, and the rules are still met after the fact. |
| **B** | **Rules at write time, checks per changed file, CI as the full sweep**: the steps below. | Most findings never happen; a commit round costs the changed files' checks and the affected tests; the merge gate reads CI instead of repeating it. | A per-file mode in three checks and a test map to maintain. |

**Recommended: B.**

## Steps

| # | Step | Status | Effort |
|---|---|---|---|
| 1 | **Measure**: `run_gates.py --time` records each gate's wall time on a typical change, so every later step shows its saving. | ⏳ | an hour |
| 2 | **The gate runner** (the card): `run_gates.py` reads the trigger table from the YAML, runs the gates the diff triggers, the fast ones first and independent ones in parallel as far as the machine's cores allow (a job budget from the CPU count, shared with the builds' own bounded `-j`, since an unbounded build once froze the laptop) and at low priority (`nice`, so a desktop MoonLight, the browser and the editor keep their cores; an idle machine runs the gates at full speed, and only competing work slows them), and caches each pass under a hash of its inputs, so a rerun after a fix runs only the gates whose inputs changed. It prints the one-line-per-gate report the process asks for. | ⏳ | a day |
| 3 | **Rules at write time**: a project hook (`.claude/settings.json`, PostToolUse on Edit and Write) runs the per-file checks of step 4 on the file just written and hands the findings back to the agent at once: a help line over its cap, a hard wrap, a negation, an em-dash, a British spelling, a complexity rise. The agent fixes the line while it is open, so the commit gate finds what the hook already reported. It also compiles the written C++ file alone (`-fsyntax-only` with the flags from the build's `compile_commands.json`), so a broken declaration surfaces in seconds rather than in a rebuild, and it reports a stray backup file (`*-E`, `*.orig`, `*.bak`) beside the file it wrote. The rules keep their one home in the standards and the checks; the hook calls them. | ⏳ | half a day |
| 4 | **Checks per changed file**: `check_docgen`, `check_prose` and `check_code` take the changed files and update the committed counts by their delta, since the counts are per file already. Cross-file measures (jscpd's clones, name spread) run in full at the merge gate and in CI. | ⏳ | a day |
| 5 | **Affected tests first**: the build's dependency files map a changed header to the test files that include it; a commit round runs those cases, and the full suite runs at the merge gate and in CI. `pytest` does the same for a changed script, by import. | ⏳ | a day |
| 6 | **CI is the merge gate's full sweep**: a gate CI runs (every build, the full suites, the sanitizers, Vale) is read from the pull request's checks rather than rerun locally; the local merge gate keeps what CI cannot do: the bench, the KPI on a board, the judgment gates. | ⏳ | an hour, in the YAML |
| 7 | **KPI per merge**: `collect_kpi` runs at the merge gate, and a commit message carries "as measured at <commit>" unless the change reaches the render path, which the trigger table can say by glob. | ⏳ | an hour |
| 8 | **The Reviewer per step**: the Reviewer reads each finished step while it is written, as #255's rename steps are, so the review at commit is the last step's, and its findings arrive before any check runs. A review reads a frozen diff: a file restaged while it reads is read again. | ⏳ | the process text |

## Every script, its cost and its value

Measured on 2026-11-17: the PR runs of #255 and `hub75-encoder` and the main push that merged #254. GitHub Actions is free on a public repository, so a CI job costs wall time and attention, never money; a PR's CI finishes when ThreadSanitizer does, 6 to 8 minutes. Local figures are this plan's measurements; "est." marks an estimate from what the script runs. Two facts the code shows that the process text does not: `check_code` runs an incremental desktop build itself (`check_nonblocking.build_output`), so after a header change it carries the 5 to 8 minutes and `build_desktop` then costs seconds; and `collect_kpi` runs `mm_tests` and the whole `mm_scenarios` suite again, so a code commit round runs the unit suite twice.

| Script or job | Where it runs | Cost | What it catches | Overlap | Conclusion | The change |
|---|---|---|---|---|---|---|
| **Local commit gates** | | | | | | |
| Reviewer reads the staged diff | local commit, on ten files | 5 to 15 min agent time (est.) | design, duplication, hot path, spec drift | CodeRabbit on request | change trigger | per finished step (step 8), so commit time carries only the last step's |
| `check_specs` | local commit, every change | seconds | an undocumented module, a dead doc link or `docPath` | `findings.yml` | keep | the hook runs it per written file |
| `check_prose` | local commit, every change | under 1 min | em-dash, British spelling, negation, "e.g." on added lines | `findings.yml`, `prose.yml` | optimize | the hook on the written file (step 3); the gate reads the diff as now |
| `build_docs --strict` | local commit, `**/*.md`, `docs/**`, `moonwalk/**` | 10 to 20 s | a broken link or dead anchor | none before the merge: `deploy-pages` renders after it | keep | the same call in `findings.yml`, so CI is the sweep |
| `check_docgen` | local commit, `**/*.md`, `src/**`, `test/**` | under 1 min | help-line caps, comment rules, hard wraps, a stale help.json | `findings.yml` | optimize | per changed file with a delta on the counts (step 4); the hook calls it |
| `project_mirror --check` | local commit, every change | seconds, network | a card edited on the Project since the last render | none | change trigger | pre-merge only: drift comes from the Project, never from the diff |
| `check_taglines` | local commit, README or CLAUDE.md | seconds | the platform sentence differing between the two | `findings.yml` | keep | none |
| `check_code` | local commit, `src/**`, `test/**` | under 1 min, plus the incremental build it triggers (30 s to 8 min) | complexity, length, nesting, clones, boundaries, hot-path calls, frames over 512 bytes | `findings.yml` runs the text half; the compiler half is local only | optimize | lizard and the compiler rules per changed file; jscpd and name spread at the merge and in CI |
| `build_desktop` | local commit, `src/**`, `test/**` | 30 s to 2 min incremental, 5 to 8 min after a wide header | a compile error or warning on clang | `check_code` built already; CI builds on 5 systems and GCC | keep | runs once per round through the runner (step 2) |
| `build_desktop --tests` and `test_desktop` | local commit, `src/**`, `test/**` | about 3 min, 3,403 cases | a behavior regression | CI's ASan lane runs the unit suite; `collect_kpi` runs it again | optimize | affected cases per commit (step 5), the full suite at the merge and in CI |
| `check_esp32_built --firmware esp32s3-n16r8` | local commit, `src/**` | 0.1 s; a stale binary costs an ESP32 build, 5 to 9 min | an ESP32 compile break; nothing else compiles ESP32 before main | `release.yml` builds 16 firmwares on a main push only | change trigger | one S3 build in CI on pull requests; local at the merge and when `src/platform/esp32/**` changes |
| `check_esp32_built --firmware esp32` | local commit, `moonbase/**` | as above | a MoonBase image break | same | keep | rare trigger |
| `build_desktop --no-jit` | local commit, `src/*/moonlive/**` | 2 to 8 min (est., a second build tree) | a helper left outside its guard, fatal under GCC | CI's GCC builds compile with the JIT on | keep | rare trigger |
| `collect_kpi --commit` | local commit, `src/**`, `test/**` | 1 to 2 min: `mm_tests` again, `mm_scenarios`, `idf.py size`, a serial tick capture | a tick, flash or size regression; the one gate that runs the scenario suite | its unit run repeats `test_desktop` | change trigger | per merge (step 7); its `mm_tests` run goes; the scenario suite moves to `desktop-tests` |
| `test_host --js` | local commit, `src/ui/**`, `mooninstaller/**` | 5 s | UI and installer logic, the Improv golden vector | `test.yml` js | keep | none |
| `check_devices` | local commit, `src/ui/**`, `mooninstaller/**` | seconds | a device catalog entry its three clients disagree on | `findings.yml` | keep | none |
| `test_host --python` | local commit, `moondeck/**`, `moonwalk/**`, `docs/moonmodules/**/*.md` | 1.5 to 2.5 min, 770 cases | MoonDeck scripts, MoonLive scripts as C++, the checks' own rules | `test.yml` python | optimize | the tests that import the changed script (step 5); a `moonwalk/**` or `.md` edit runs the generator's tests |
| `check_firmwares` | local commit, `moondeck/**` | seconds | `firmwares.json` drifted from `FIRMWARES` | `findings.yml` | keep | none |
| `improv_smoke_test` | local commit, Improv files, a board attached | 1 to 2 min (est.) | provisioning end to end on a real board | none | keep | rare trigger |
| **Local merge gates** | | | | | | |
| The commit checks over `main...HEAD` | local merge | 12 to 20 min for a code branch | what a green series of commits hides | every build and test CI ran on the PR | change trigger | read CI's checks (step 6); local keeps the ESP32 freshness, the KPI and the judgment gates |
| `build_desktop --gcc --tests` | local merge, when CI asks | 8 to 12 min (est., a full GCC tree) | `-Wstringop-truncation`, transitive includes | CI's Linux jobs compile with GCC on every PR | keep | conditional on a CI failure already |
| Judgment: docs in sync, title, perf snapshot, README | local merge | 5 to 10 min agent time | drift between docs, commits and the PR text | none | keep | none |
| **Release** | | | | | | |
| Every commit check, triggers ignored | release | 20 to 30 min | the sweep before the binary ships | CI on the tag reruns the builds | keep | one event per release |
| `build_esp32` per chip family | release | 5 families at 5 to 9 min each | a family that compiles differently | `release.yml` builds all 16 after the tag | keep | none |
| `verify_version`, `release_stage`, container, Pages | CI on tag and main push | 0.3, 0.7, 2.0, 1.2 min | a tag that disagrees with `library.json`; staging | none | keep | none |
| **CI jobs** | | | | | | |
| `findings.yml` checks | CI, every PR and main push | 1.7 to 2.0 min | the seven text checks, annotated and as SARIF | the local gates above | keep | the full sweep the local per-file checks lean on |
| `prose.yml` vale | CI, PR touching `.md`, `.h`, `.cpp` | 0.5 min | Vale errors on added lines, never failing | `findings.yml` runs Vale on the same lines and fails | delete | `findings.yml` covers it fully; its own header plans its removal |
| `generated-files.yml` | CI, fork PRs touching metrics or scenarios | seconds, skipped on our PRs | a contributor's measured file | none | keep | none |
| `test.yml` python | CI, every PR and main push | 0.9 to 1.1 min | as local | the local gate | keep | none |
| `test.yml` js | CI, every PR and main push | 0.3 to 0.4 min | as local | the local gate | keep | none |
| `test.yml` restore | CI, every PR and main push | 0.8 to 2.4 min, ccache | a restore regression with two readers | none locally | keep | none |
| `test.yml` teensy | CI, every PR and main push | 1.5 to 2.2 min | a platform seam the Teensy lacks; the one Teensy compile before main | `release.yml` on a main push | keep | none |
| `test.yml` sanitizers (address, thread, realtime) | CI, every PR and main push | 3 to 5.5, 5.7 to 9, 2.8 to 5 min; TSan is the PR's critical path | use-after-free, data races, hot-path allocation; TSan found four races in recent commits | the ASan lane repeats the unit suite | keep | none |
| `release.yml` on a PR: macOS, Windows, Linux x64 and arm64, Android | CI, PR touching `src/**`, `test/**` | 1 to 2.8 min each, about 12 runner min, 3 to 7 wall | MSVC and GCC breaks, installable programs | the local clang build | keep | one S3 ESP32 build added here (see `check_esp32_built`) |
| `release.yml` on main: 16 ESP32 builds | CI, main push | 4 to 9 min each, about 100 runner min, 9 wall | every shipped variant for `latest` | the local S3 gate | keep | none |
| `release.yml` on main: teensy, release, container, Pages | CI, main push | 2.2, 0.7, 2.0, 1.2 min | the published artifacts | none | keep | none |
| `codeql.yml` | CI, push to main and `next-iteration`, weekly | 7.2 min, never a gate | taint across the six packet parsers; three `localtime` findings | a branch push doubles it with main | change trigger | main and the weekly run, as its header says |
| CodeRabbit | on request on the PR | minutes, an hourly budget | an outside read | the Reviewer | keep | none |

### The largest savings, ranked

1. **Reruns after a rule fix, 15 to 25 min per commit round.** The 2026-11-17 round ran the build three times and the suite three times. The hook at write time (step 3) and the cached runner (step 2) cut each to one run.
2. **The merge gate reads CI, 12 to 20 min per merge.** Build, suite, Python, JS, docgen and the text half of `check_code` ran on the PR already; what stays local is the ESP32 freshness, the KPI, the bench and the judgment gates.
3. **One ESP32 build per branch, 5 to 9 min per commit.** `check_esp32_built` rebuilds the S3 after nearly any `src/**` edit; a CI S3 build on pull requests and a local build at the merge keep the guard at one build per branch plus platform changes.
4. **Affected tests, 3 to 4 min per commit.** The suite's affected cases instead of 3,403, and the Python tests that import the changed script instead of 770.
5. **KPI per merge and one suite run, 1 to 2 min per commit.** `collect_kpi` moves to the merge gate and stops running `mm_tests`; the scenario suite it carries moves into `desktop-tests`.
6. **Two CI changes with no wall saving:** `prose.yml` goes (fully covered by `findings.yml`), and CodeQL runs on main and weekly only (7 runner min per branch push).

## Open questions

- **How far "affected tests" may go before a full run**: every commit round runs the affected cases only, or the full suite once per round as now and the affected cases on a rerun. **Recommended**: affected only per commit, full at the merge gate and in CI, since CI runs the full suite on every push already.
- **Whether a hook may block**: the hook reports, or refuses the edit until its findings are fixed. **Recommended**: reports; a half-written change legitimately fails a check for a minute.

## Backlog

- The desktop build's header fan-out: a change to `DriverBase.h` rebuilds nearly every file; a precompiled header or narrower includes would cut the 5 to 8 minutes, a design study of its own.

## What it removes

The hand-typed gate chain and its rerun judgment; whole-tree reruns after a one-line fix; local reruns of what CI runs; a KPI run per commit; and most rerun loops, since the hook reports a rule as the line is written.

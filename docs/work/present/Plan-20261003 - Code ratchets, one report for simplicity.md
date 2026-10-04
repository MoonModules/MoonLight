# Plan: Code ratchets, one report for simplicity

The documentation ratchets made two kinds of quality measurable: `docgen.md` and `prose.md` are committed numbers that may only fall, and every touched file leaves them better. This plan does the same for code, with one report and one script, so the codebase moves step by step toward the leanest form that humans read and maintain with ease: no duplication, no unneeded complexity, no oversized files, a core that knows nothing about lights, a UI that knows no module, and a concept that lives in few files.

The plan is recursive: the guardrails it adds obey the rules they enforce. One script, no second process beside what exists, and every tool it wraps is the industry's.

## Why

- **Duplication is unmeasured.** The FNV-1a hash was written six times before a review caught it; the I80 and Parlio drivers share about 245 of 362 lines ([backlog-light](../future/backlog-light.md)). Nothing counts a clone, so a seventh copy costs nothing until a reviewer happens to look.
- **The complexity ratchet is partly dead.** `check_lizard.py` whitelists functions by name in `whitelizard.txt`; 36 of its 172 entries name files that no longer exist since the `src/core` reshuffle, so those pin nothing, and `repo-health` counts 298 functions over the threshold against a gate that knows 172.
- **Boundaries are stated, not checked.** The architecture says core knows nothing about lights, yet six core files include `light/` headers. The UI is documented as module-driven, yet `app.js` and `migrate.js` name module types in dozens of literals.
- **A concept is spread thin.** Renaming the light presets to fixture profiles touched 87 files, 23 of which name the type. The cost of changing a concept is invisible until someone pays it.
- **The guardrails themselves have grown by accretion.** Five scripts measure code (`check_lizard`, `check_clang_tidy`, `check_clang_query`, `check_nonblocking`, `check_platform_boundary`), three of them report rather than gate, the docgen and prose ratchets each carry their own copy of the ratchet mechanics, `collect_kpi` runs the boundary and spec checks a second time, and the coding standards describe a clang-format check that has no config file and no workflow.

## The standard

Each rule is a textbook measure with a known tool:

| Rule | The measure | The tool | Prior art |
|---|---|---|---|
| Duplication | copy-pasted token runs across files | jscpd (C++, JS and Python in one run, on npx, which moxygen already needs) | PMD CPD, SonarQube's duplication density |
| Complexity | cyclomatic complexity per function | lizard, as today | McCabe |
| Nesting | maximum nesting depth per function | lizard | SonarSource's cognitive complexity, the nesting half |
| Parameters | parameters per function | lizard | Fowler's long parameter list |
| Function size | lines per function | lizard, as today | Clean Code, Ousterhout |
| File size | lines per file | the script itself | the same |
| Domain boundary | a `src/core` file including `light/` | the script itself, beside the platform boundary | Clean Architecture's dependency rule |
| UI without modules | a module or type name as a literal in `src/ui` | the script itself, with the type list from `module_types.cpp` as `check_devices` reads it | the same rule, inward |
| Name spread | the files outside a module's own that name its type | the script itself | Ousterhout's change amplification, Fowler's shotgun surgery |
| Stack arrays | local arrays of 512 bytes or more | clang-query | the embedded stack budget, MISRA's guidance on stack use |
| Hot path | blocking calls reachable from the render ticks | the compiler's `nonblocking` effect, through `check_nonblocking` | real-time code's no-blocking rule |
| Raw allocation | `alloc`/`new` in the light domain outside `ScratchBuffer` | clang-query | single ownership, RAII |

Each measure is cited by what it counts, not by who named it: cyclomatic complexity, duplication density, file size, dependency direction and files touched per concept are each decades old and implemented by several independent tools, which is the test a rule has to pass here.

Two things in the product owner's notes are judgment, and go to the Reviewer's scope rather than a counter: **"Apple, not Android"**, every setting names the user who needs it, and **orthogonality**, a change that touches many areas signals coupling.

## The shape

One script, `moondeck/check/check_code.py`, writing one report, `docs/reference/metrics/code.md`, in the docgen shape: a headline, a per-rule table, per-area and per-file tables. The committed copy is the baseline; per rule and in total the counts may only fall, and a touched file gets all of its findings resolved so it leaves the list, with the same reasonable-effort clause CLAUDE.md gives docgen. A rule whose own limit changed says so in the commit.

The ratchet mechanics move into one module, `moondeck/check/_ratchet.py`, which `check_docgen`, `check_prose` and `check_code` all call: the committed-copy parse, the per-rule and total comparison, the growth attribution to working-tree files. Three copies of one mechanism is the first duplication the plan removes.

The deep tools stay as tools: `check_clang_tidy` and `check_clang_query` cost minutes repo-wide and answer questions a count cannot, so they keep their on-request, per-module shape under `check_module`. clang-query asks the compiler rather than a regex, which is what finds a raw byte write hiding from a `draw::` grep; one of its rules is worth promoting into the report later, since only an AST answers it: a `switch` on a type's enum outside the type's home file, the coding standards' "per-type behavior lives with the type" made measurable. `check_nonblocking`'s measurement stays a compile, and its count joins the report in step 9. One rule, one owner, as [testing.md](../../reference/testing.md) already states.

## Steps

Each step ships with the tree better than before it, and the sweeps the numbers then ask for are their own changes, by CLAUDE.md's rule that a change clears the files it already touches.

1. ✅ **The ratchet as one module.** Extract `_ratchet.py` from `check_docgen.py` and `check_prose.py`, both calling it, pinned by the existing `test_check_docgen.py` cases. No report changes.
2. ✅ **`check_code.py` with complexity and function size.** Lizard's two measures as per-file counts in `code.md`, replacing the name whitelist: `check_lizard.py` and `whitelizard.txt` go, and `repo_health.py` and `collect_kpi` count through the same two functions the report does. The gate listing in CLAUDE.md is step 11.
3. ✅ **File size, nesting depth and parameter count.** Files over 1000 lines, control flow nested deeper than 3 and more than 7 parameters, all SonarQube's defaults; nesting is the cheap half of cognitive complexity, the measure built for how a reader experiences code. The baseline after this step is 487 findings: 295 complex functions, 118 deeply nested, 50 long, 24 large files, `app.js` (8494 lines), `platform_desktop.cpp` and `HttpServerModule.cpp` among them.
4. 🚧 **Gains since a reference.** `repo-health.md` gains a table of the numbers a user feels, compared against a fixed reference rather than the previous commit: flash per target, free internal RAM and PSRAM per target (recorded per target with its date, as flash is; `collect_kpi` already reads the heap from a board and discards it), the tick per scenario, lines of code, and the code report's findings. A simplification then shows as a gain over weeks, where the per-commit delta shows only noise. The reference is a release tag, so the table says what the next release gives a user over the last one.
5. 🚧 **Boundaries.** The core-to-light include rule, baseline 6, and the platform boundary folded in as a rule with baseline 0, so `check_platform_boundary.py` goes and `collect_kpi` stops running it. The rule's text joins [mooncore.md](../../explanation/architecture/mooncore.md) beside the platform abstraction.
6. 🚧 **The UI and name spread.** Type and module names as literals in `src/ui`, per file; and name spread per registered type, both from the type list in `module_types.cpp`. Control type names (`palette`, `list`) are the UI's contract and are not findings.
7. ✅ **Duplication.** jscpd over `src`, `test`, `moondeck` and `mooninstaller`, with vendored files excluded, at jscpd's defaults of 5 lines and 50 tokens. A clone counts under both of its files. The first run found 412 clone pairs, 3.17% of all lines, most of them test fixtures copied between unit tests; the report's baseline is 1345 findings across 305 files.
8. 🚧 **Two rules from clang-query.** Its run on 2026-10-04 reports four inventories, and two of them are smells with a known move, which only the syntax tree answers.
   - **Large stack array**: a local array of 512 bytes or more, 18 today, 12 of them 1 KB or more, `HttpServerModule.cpp` and `HueDriver.h` among them. A task's stack is sized for its deepest call, so one large local costs every task that can reach it; the move is a member or a `ScratchBuffer`, sized once.
   - **Raw allocation in the light domain**: 70 raw `alloc`/`new` sites in 31 files, 16 of them light files, beside 113 `ScratchBuffer` members that already own most light memory. The move is a `ScratchBuffer` member.
   - The other two inventories stay as they are. Fixed-array member bytes (core 31 KB, light 11 KB) are a memory budget, which step 4's gains table reports. `ScratchBuffer` members are the managed heap working as intended.
   - clang-query counts the vendored `miniaudio.h` (69 arrays, 1857 comment rows), so it takes its list of files we own from `check_code.py`, one home for it.
   - Its comment inventory (52% of the public surface documented, 3338 declarations without a comment) is docgen's domain. Whether a public declaration without `///` becomes a docgen rule is an open decision below.
9. 🚧 **The hot path in the report.** `check_nonblocking`'s blocking calls reachable from the render ticks become a rule, "blocking call on the render path", counted per file like the others: 159 today, 85 in core, 64 in light, 7 in the platform layer, 3 in tests. The measurement stays the compiler's; its own baseline, `hotpath-baseline.txt`, goes, a fourth copy of the one ratchet. The baseline flags each new call site where a per-file count sees only a file's total rise, the trade every rule in the report makes.
10. 🚧 **`@moreinfo` that nothing refers to.** A docgen rule: an appendix section no `@xref` and no card links to is a finding, so the appendix total falls by cutting what no reader reaches.
11. 🚧 **The process.** CLAUDE.md's commit table lists `check_code` where it lists lizard and the platform boundary today; the Reviewer's scope gains the two judgment questions; coding-standards § Tooling describes the one report and drops the clang-format check that does not exist. `MoonDeck.md` and `testing.md` follow.
12. 🚧 **MoonCore as a library.** Once the core-to-light rule reads 0: a CMake target of `src/core` alone that compiles without `src/light`, so a crossing is a build error rather than a count. The end state of "core without light".

## Subtraction

- `check_lizard.py` and `whitelizard.txt`, replaced by the complexity rules of `check_code.py`.
- `check_platform_boundary.py`, folded into `check_code.py` as a rule.
- The second runs of the boundary and spec checks inside `collect_kpi.py`.
- The ratchet mechanics duplicated in `check_docgen.py` and `check_prose.py`.
- `hotpath-baseline.txt`, the hot path's own baseline, once its count is in the report.
- The clang-format check from the coding standards, which has no config and no workflow (done in step 2, since that section was open).
- `repo_health.py`'s and `collect_kpi.py`'s own complexity counting, done through `check_code` instead.

## Out of scope

- The sweeps themselves: splitting `app.js`, merging the I80 and Parlio drivers, moving the six core-to-light includes behind seams. Each is its own change, which the ratchet then records.
- Attributing a gain to one commit. The commit message already carries the KPI line, and a commit that names its catalog move is a simplification by its own account; the gains table is cumulative, which is the number that matters.
- Formatting. The old MoonLight had a `.clang-format` and dropped it: it reformats every file at once, which rewrites every diff and blame line, the opposite of step by step, and with agents writing every line the format is already uniform. The compiler's view is covered by `-Werror`, the reader's by docgen's comment-shape rules.
- CodeQL and the sanitizers, which CI runs as security and runtime checks and which measure nothing this plan counts.

## Open decisions

- **Agentless development.** The gate lists in CLAUDE.md are run by an agent today; an agentless or local-model workflow wants one runnable command per event. `precommit.py` was removed on 2026-10-01. Whether a gate runner returns, and in what shape, is a decision for the product owner before step 11.
- **Thresholds.** Lizard keeps CCN 10 and NLOC 60 as today; nesting, parameters and file length took SonarQube's defaults in step 3, and duplication jscpd's in step 7.
- **Raw allocation in light.** Whether every light allocation goes through `ScratchBuffer` is a rule the architecture implies but does not state; it is stated first, then counted.
- **Undocumented public declarations.** Generated pages show an empty entry for each, which argues for a docgen rule; the cut-first rule argues against adding comments for their own sake. Counting only public declarations in headers that generate a page is the middle way.
- **The reference for gains.** `v6.0.0` is the last release, so a gain reads as what the next release gives a user; the alternative is the commit where `code.md` first landed, which reads as what this plan gained. The first is recommended, since a user never sees the second.
- **Where the UI's type names go.** A few literals are the UI's contract (the containers `Layouts`, `Effects`, `Drivers` are pinned in `main.cpp`); step 6 lists them as the rule's allowed set, in the script, with the reason.

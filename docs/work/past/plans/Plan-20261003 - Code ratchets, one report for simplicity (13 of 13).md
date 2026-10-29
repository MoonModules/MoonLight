# Plan: Code ratchets, one report for simplicity

Plan of 2026-10-03, every step done by 2026-10-25. The documentation ratchets made two kinds of quality measurable: `docgen.md` and `prose.md` are committed numbers that may only fall. This plan did the same for code, with one report and one script, so the codebase moves step by step toward the leanest form a person reads and maintains with ease. The guardrails it added obey the rules they enforce: one script, no second process, and every tool it wraps is the industry's.

**Result**: `check_code.py` and its report replaced five scripts, the three copies of the ratchet mechanics became `_ratchet.py`, and core without light became a build error rather than a count.

## Why

- **Duplication is unmeasured.** The FNV-1a hash was written six times before a review caught it; the I80 and Parlio drivers share about 245 of 362 lines ([backlog-drivers](../../future/backlog-drivers.md)). Nothing counts a clone, so a seventh copy costs nothing until a reviewer happens to look.
- **The complexity ratchet is partly dead.** `check_lizard.py` whitelists functions by name in `whitelizard.txt`; 36 of its 172 entries name files that no longer exist since the `src/core` reshuffle, so those pin nothing, and `repo-health` counts 298 functions over the threshold against a gate that knows 172.
- **Boundaries are stated, not checked.** The architecture says core knows nothing about lights, yet six core files include `light/` headers. The UI is documented as module-driven, yet `app.js` and `migrate.js` name module types in dozens of literals.
- **A concept is spread thin.** Renaming the light presets to fixture profiles touched 87 files, 23 of which name the type. The cost of changing a concept is invisible until someone pays it.
- **The guardrails themselves have grown by accretion.** Five scripts measure code (`check_lizard`, `check_clang_tidy`, `check_clang_query`, `check_nonblocking`, `check_platform_boundary`), three of them report rather than gate, the docgen and prose ratchets each carry their own copy of the ratchet mechanics, `collect_kpi` runs the boundary and spec checks a second time, and the coding standards describe a clang-format check that has no config file and no workflow.

## Steps

1. ✅ **The ratchet as one module.** `_ratchet.py`, shared by `check_docgen`, `check_prose` and `check_code`.
2. ✅ **`check_code.py` with complexity and function size**, lizard's two measures as per-file counts; `check_lizard.py` and `whitelizard.txt` went.
3. ✅ **File size, nesting depth and parameter count**, at SonarQube's defaults.
4. ✅ **Gains since a reference**: the numbers a user feels against the last release tag ([repo_health](../../../../moondeck/MoonDeck.md#repo_health)).
5. ✅ **Boundaries.** The platform boundary as a rule, `check_platform_boundary.py` gone; the core-to-light rule, later replaced by the build in step 12.
6. ✅ **The UI and name spread**: module types as literals in `src/ui`, and a type named in more than 10 files beyond its own.
7. ✅ **Duplication**, jscpd at its defaults over `src`, `test`, `moondeck` and `mooninstaller`.
8. ✅ **Two rules from the compiler and the report**: `-Wframe-larger-than=512`, and buffers taken by hand in `src/light`.
9. ✅ **The hot path in the report**: blocking calls reachable from the render ticks, `hotpath-baseline.txt` gone.
10. ✅ **`@technical` that nothing refers to**, a docgen warning that ratchets.
11. ✅ **The process**: CLAUDE.md's gates, the Reviewer's two judgment questions, coding-standards' Tooling and checks.
12. ✅ **MoonCore as a library**: core compiles through an include root without `light/`, every core header on its own ([MoonCore](../../../explanation/architecture/mooncore.md)).
13. ✅ **What each commit added and saved**, `check_code.py --account` ([check_code](../../../../moondeck/MoonDeck.md#check_code)).

## Where it lives now

| What | Home |
|---|---|
| Every rule, its limit, the textbook measure behind it and the move that solves a finding | [MoonDeck, check_code](../../../../moondeck/MoonDeck.md#check_code), and [the test page](../../../reference/tests.md#check_code), generated from the check |
| The report and its committed counts | `docs/reference/metrics/code.md` and `code.json` |
| The shared ratchet mechanics | `moondeck/check/_ratchet.py`, used by `check_code`, `check_docgen` and `check_prose` |
| The gains since the last release | [MoonDeck, repo_health](../../../../moondeck/MoonDeck.md#repo_health) |
| A commit's own account | `check_code.py --account`, [MoonDeck, check_code](../../../../moondeck/MoonDeck.md#check_code) |
| Core without light, enforced by the build | [MoonCore, the domain boundary](../../../explanation/architecture/mooncore.md), `CMakeLists.txt` (`mm_core`), and the `relative include path` rule |
| How a finding becomes a fix, and why there is no formatter | [coding-standards, Tooling and checks](../../../contributing/coding-standards.md#tooling-and-checks) |
| The Reviewer's two judgment questions | [the pre-merge process](../../../moonwalk.md#pre-merge) |
| Left open | three decisions, [backlog-process-and-docs](../../future/backlog-process-and-docs.md#three-decisions-left-open-by-the-code-ratchets-2026-10-25-from-the-code-ratchets-plan) |

## Decided against

- **clang-format.** It reformats every file at once, which rewrites every diff and blame line, the opposite of step by step; the old MoonLight had one and dropped it, and with agents writing every line the format is already uniform.
- **clang-query as a rule source.** It counts the vendored `miniaudio.h` and costs minutes; the two rules it found worth keeping came from the compiler (`-Wframe-larger-than=512`) and a text rule (raw allocation in light), and it stays the on-request inventory.
- **The sweeps inside this plan**: splitting `app.js`, merging the I80 and Parlio drivers. Each is its own change, which the ratchet records.
- **CodeQL and the sanitizers in the report.** They are security and runtime checks that CI runs, and measure nothing the report counts.

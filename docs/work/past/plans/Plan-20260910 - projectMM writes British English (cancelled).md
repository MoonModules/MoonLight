# Plan: MoonLight writes British English

## Context

The project is American-spelled by an explicit rule ([coding-standards.md:18](../../../contributing/coding-standards.md), [CLAUDE.md:101](CLAUDE.md)), enforced by `check_prose.py` at the commit gate and by `hook_prose.py` on every write. The PO wants British: `colour`, `behaviour`, `initialise`, `centre`, `grey`, `catalogue`, `analyse`.

The deciding argument is user impact. The project has no official launch, so ADR-0013's "no migration code, documented break" applies at its cheapest: nothing outside our control has adopted our names yet. What *is* outside our control keeps its American spelling, and the PO accepts that discrepancy: **CSS/HTML properties, WLED/Home-Assistant wire keys, vendor symbols, SPDX headers.**

Measured scope: ~2,900 American occurrences across **445 files**. Two PO decisions widen it to the maximum: internal C++ identifiers are renamed **including the FastLED-mirroring ones** (`colorFromPalette` becomes `colourFromPalette`), and the **MoonLive script builtins are renamed** with the 18 shipped scripts updated and a MIGRATING entry.

## What the exploration settled

- **The switch is one dict.** `check_prose.py:50` holds `SPELLING = {"behaviour": "behavior", ...}` as substring stems. `hook_prose.py` shells out to it rather than copying it, so inverting that one dict flips both the write-time hook and the commit gate.
- **The hook blocks the migration itself.** `hook_prose.py` is a live `PostToolUse` hook (`.claude/settings.json:9`) that **refuses any write adding a British spelling**. Until it is inverted, every edit in this migration is rejected. This is step 1, not cleanup.
- **The persisted-key risk is theoretical, not actual.** **15 control registrations** across 12 effects contain American spellings (`colorMode` twice, `panCenter`, `oneColor`, `color_chaos`, …) and controls are addressed by name over `POST /api/control` and persisted into `.config/*.json`. But **no config under `build/fs/.config/` currently holds one**, and `deviceModels.json` sets none of them. Nothing on the bench breaks.
- **Those 13 names live only in C++**, not duplicated in `src/ui` or scripts, so each is a single-site rename.
- **The external contract is tiny and self-contained**: 214 CSS/HTML hits (all in `src/ui` + `mooninstaller`), 4 WLED/HA wire keys, 9 vendor symbols, 251 SPDX headers.
- **`meter` is out of scope.** All 111 uses are the *device* sense (an audio meter's ballistic); British keeps `meter` there. Only `metre` (length) would change, and the repo has none.
- **`docs/` is smaller than it looks.** 140 of 178 matching files are already `EXEMPT` in `check_prose.py` (history, backlog, generated moxygen/tests pages, friend-repos), leaving ~38 real files.
- **CodeRabbit is skipped for this change** (PO), so the ~100-file branch ceiling does not apply. The split below follows *risk*, not review limits.

## Hard exclusions: never rewrite these

A blind find-and-replace breaks the build and has done so before ([memory: a spelling sweep once renamed an API inside `draw.h`](CLAUDE.md#principles)). Every step is a reviewed rename, never a blanket `sed`.

| Never touch | Why | Count |
|---|---|---|
| `std::initializer_list`, `initializer_list` | C++ standard library | 5 |
| `CRGB`, `CHSV`, `esp_*_color_*`, `lcd_color_*` | vendor symbols | 9 |
| CSS `color:`, `bgcolor`, `type="color"`, `backgroundColor` | the language defines these | 214 |
| `"col"`, `"color"`, `"colors"`, `"cols"` in WLED/HA payloads | interop contract with software we do not control | 4 |
| `SPDX-License-Identifier` | legal identifier | 251 |
| `src/platform/desktop/vendor/`, `src/ui/vendor/` | upstream code, already EXEMPT | whole trees |
| `docs/history/`, `docs/backlog/`, `docs/friend-repos/` | quoted records; rewriting falsifies them | already EXEMPT |
| `analysis`, `analyses` | already correct in both dialects | 219 |
| `meter` (device sense) | British keeps it | 111 |

The files that speak a foreign protocol (`MqttModule`, `HttpServerModule`, `DevicesModule`, `HueDriver`) keep American **for the wire keys only**; their own comments and internal identifiers go British like everywhere else.

## Steps

Six branches, each independently committable and verifiable. Order matters: the enforcement must flip first, and the build-breaking renames come before the prose sweep so a bisect stays meaningful.

### 1. Flip the enforcement (small, blocking)

`moondeck/check/check_prose.py`: invert `SPELLING` to American→British, keeping the substring-stem style (`"initializ": "initialis"` covers *-ize/-ization/-izer*). Add the words the current dict lacks but the repo uses: `catalog`→`catalogue`, `gray`→`grey`, `neighbor`→`neighbour`, `synchroniz`→`synchronis`, `defense`→`defence`.

Three traps the inverted dict must handle, each needing an exclusion in the checker rather than a naive stem:
- `initializer_list` (C++ stdlib)
- `analysis`/`analyses` (already correct; only `analyze`→`analyse`)
- `license` as a *verb* and in SPDX headers stays; only the noun becomes `licence`

Update the rule statements: [docs/contributing/coding-standards.md:18](../../../contributing/coding-standards.md) (rewrite the paragraph and its rationale, which currently argues the opposite) and [CLAUDE.md:101](CLAUDE.md). Both must name the external-contract exception explicitly, or the next reader will "fix" a CSS property.

**Verify:** `uv run moondeck/check/check_prose.py` now flags American spellings in added lines; write a file containing `colour` and confirm the hook permits it.

### 2. MoonLive script API + the 18 shipped scripts (user-visible)

Rename the builtins in `src/core/moonlive/MoonLiveBuiltins_common.h` and `src/light/moonlive/MoonLiveBuiltins_light.h`: `setPaletteColor`→`setPaletteColour`, `setPaletteColorZ`, `setPalEntryHSV` (unchanged: HSV is an initialism), `paletteR/G/B` (unchanged). `setRGB` stays: RGB is an initialism, not a spelling.

Update the **12 call sites across 9 files** under `moonlive/effects/` (aurora, balls, comet-trail, fractal, metal, noise, octopus, stadbeest-eyes, stadbeest-legs). `setPaletteColorZ` is used only by `aurora.mle` (2 of those 12).

**MIGRATING entry**, following the format of the `floor` entry at [docs/reference/MIGRATING.md:27](../../../reference/MIGRATING.md): state the action (rename the call in any script you wrote), and name the shadowing hazard, which is the real trap: `/moonlive` (user) shadows `/.moonlive` (factory), so a stale user copy of a shipped script keeps calling the old name and fails with "unknown function" at the call site, while the factory copy is fixed.

**Verify:** `./build/macos/test/mm_tests -tc="*every script in moonlive*"` (the test that compiles every shipped script), then the Xtensa codegen test that compiles all 33 at the device budget. On the bench, load a renamed script on a board and see it run.

### 3. C++ identifiers (build-breaking, largest)

~139 files under `src/`. Rename in dependency order so the tree builds at each stage: `src/platform` → `src/core` → `src/light` → `src/ui`.

Includes the FastLED-mirroring names per the PO's decision: `colorFromPalette`→`colourFromPalette` (86 uses), and the **15 control registrations**: `colorMode` (twice: `SolidEffect.h:48` and `AudioSpectrumEffect.h:34`), `colorByAge`, `colorByColumn`, `colorByHeight`, `colorBars`, `colorSpeed`, `oneColor`, `randomColors`, `color_chaos`, `backgroundColorR/G/B`, `panCenter`, `tiltCenter`.

**One module TYPE name changes, and it is the widest-reaching single rename**: `ColorTrailsEffect`→`ColourTrailsEffect`. A type name is a persisted key, a REST string (`POST /api/modules {"type":"ColorTrailsEffect"}`), and a catalog identifier, so it also renames files: `src/light/effects/ColorTrailsEffect.h`, `test/unit/light/unit_ColorTrailsEffect.cpp`, its entry in `test/CMakeLists.txt`, `test/scenario_runner.cpp`, `moondeck/docs/screenshot_modules.py`, and the generated `docs/moonmodules/light/moxygen/ColorTrailsEffect.md`. It is the only American-spelled type name, and no local config or scenario references it, so nothing on the bench breaks; it still earns a line in the MIGRATING entry because a saved config elsewhere would lose that effect.

Each rename is verified against the exclusion table above before applying. Where a renamed identifier sits beside a vendor one, a one-line comment records why they differ (Principle 2: a bespoke choice carries its reason).

**Verify:** `uv run moondeck/build/build_desktop.py --tests` (zero warnings), `uv run moondeck/test/test_desktop.py`, then all three ESP32 variants build. The scenario suite must produce **identical observations**: a rename changes no behavior, so any moved number is a real bug.

### 4. Tests and their names (~84 files)

Test *descriptions* are functional documentation and go British. Test *code* follows step 3's identifiers. Check `test/scenarios/*.json` for any American spelling inside recorded observation blocks (data, not prose: leave it unless it is a name we own).

**Verify:** the full suite passes with the same case count; `docs/tests/*.md` regenerated via `generate_test_docs.py` (they are generated, so the source test names are the fix).

### 5. Docs (~38 non-exempt files, plus the catalog pages)

`docs/*.md` outside the exempt trees, plus `README.md` and `docs/index.md`. `check_taglines.py` must still pass (the three front pages agree).

Generated pages (`docs/moonmodules/*/moxygen/`, `docs/tests/`) are **not** edited by hand; they are regenerated from the sources changed in steps 3 and 4.

**The catalog pages need hand editing and the checker will not tell you.** `docs/moonmodules/light/effects.md` is hand-written prose carrying 345 backticked control names, including every renamed one (`colorMode`, `colorByAge`, `panCenter`, …), but it sits under `docs/moonmodules/`, which `check_prose.py:37` exempts as "partly generated". So it is skipped silently while holding stale names. Same for the other catalog pages (`modifiers.md`, `layouts.md`, `drivers.md`). Grep them explicitly for the renamed identifiers rather than trusting the gate; `check_specs.py` catches a missing module block but not a stale control name inside one.

**Verify:** `uv run moondeck/check/check_specs.py` (no drift), `uv run moondeck/check/check_taglines.py`, `uv run moondeck/check/check_prose.py`.

### 6. Tooling and the last sweep (~29 files)

`moondeck/` (22 files) and `mooninstaller/` (7). In `mooninstaller`, the CSS and the installer's own HTML attributes stay American; its prose does not.

Finally, register `check_prose.py` in the pre-commit gate table. Its docstring currently says it is deliberately *unregistered* because pre-existing violations would fail every commit until a sweep lands. **This migration is that sweep**, so the last step of the last branch is to register it and delete that caveat.

## Verification (end to end)

1. `uv run moondeck/build/build_desktop.py --tests`: zero warnings, zero errors.
2. `uv run moondeck/test/test_desktop.py`: same case count as before the migration.
3. `uv run moondeck/scenario/run_scenario.py`: **observations unchanged**; a rename that moves a tick or heap number is a bug, not noise.
4. Three ESP32 variants build (`esp32-16mb`, `esp32s3-n16r8`, `esp32p4rev1-eth`).
5. `uv run moondeck/check/check_prose.py`, `check_specs.py`, `check_taglines.py`, `check_devices.py` all pass.
6. `uv run moondeck/build/build_desktop.py --gcc --tests`: GCC catches what clang misses, and this migration touches ~445 files of identifiers ([memory: run --gcc before pushing anything with new/renamed symbols](CLAUDE.md)).
7. **On the bench** (PO's call, one flash): a board runs, the UI renders its cards, and a renamed MoonLive script compiles and animates. Renamed control names are the thing to look at: a control whose name changed loses its persisted value and falls back to its default.

## Risks

- **A blanket `sed` breaks the build.** `std::initializer_list` and the vendor symbols are in the same files as the words being changed. Every step is a reviewed rename with the exclusion table in hand.
- **Silent wire-contract drift.** The current rule warns that "a wire key that drifts between dialects breaks a cross-device contract without a compile error." The 4 WLED/HA keys are the whole exposure; they are listed above and stay American.
- **The grep hazard becomes permanent.** After this, a search for `color` misses `colour` and vice versa, forever, because the external-contract words never convert. This is the cost the PO has accepted; the mitigation is that the American remnants are confined to CSS, four wire keys, and vendor symbols.
- **A stale user script fails silently** at the call site, not at load. Named in the MIGRATING entry.
- **`docs/history/` keeps American spelling** by design (rewriting a record falsifies it), so the repo permanently contains both dialects in prose. Already true of the em-dash rule.

## Open questions (raised after approval, not yet decided)

The collision map ran after the plan was approved and turned up three things the plan above does not account for. They are the PO's to settle, and the plan is not executable until they are.

1. **`src/ui/migrate.js` exists and this plan ignores it.** It carries `FILE_RENAMES`, `TYPE_RENAMES` and `CONTROL_RENAMES`, scoped by `onTypes`, and migrates config JSON. So the 15 control registrations and `ColorTrailsEffect`→`ColourTrailsEffect` could be carried for existing configs at near-zero cost. The Context above says ADR-0013 means "no migration code", which is the general rule; a mechanism nonetheless exists. Use it, or take the pure documented break?

2. **`setPaletteColor` may not be safely renameable.** MoonLive scripts are fetched from GitHub at a pinned tag (`src/ui/app.js:5900`), and `migrate.js` never touches script text, only config JSON. A user script on a device in the wild calling `setPaletteColor` therefore fails at the call site with "unknown function" and nothing can repair it. `/moonlive` (user) shadowing `/.moonlive` (factory) means even a stale copy of a shipped script keeps failing after the factory copy is fixed. The PO chose "rename, update the scripts, MIGRATING entry" knowing there is no launch yet, so this may simply be an accepted cost; it is recorded here rather than reversed.

3. **Two CI gates are stricter than step 3 and step 5 assume.** `check_specs.py:126-129` compares registered control names against the hand-written catalog by exact substring, so a rename that lands in the code before the doc fails the gate immediately rather than at the end; `check_registertype_docpaths()` (`check_specs.py:335-377`) validates that the `light/effects.md#colortrails` anchor resolves, so the type rename must move the anchor and the heading in the same commit. Separately, `generate_test_docs.py --check` makes any test rename a CI failure until `docs/tests/*.md` is regenerated, which step 4 should do rather than discover.

**The rule is already not holding.** The repo contains British spellings today, in test names that propagated into the generated docs: "centre" in `test/unit/light/unit_BlockModifier.cpp:30`, `unit_MirrorModifier.cpp:53` and `unit_CircleModifier.cpp:29`, and "greyscale" in `unit_StarFieldEffect.cpp:15`, all of which reach `docs/tests/unit-tests.md`. They pass because `check_prose.py` reads added lines only and is not in the commit gate.

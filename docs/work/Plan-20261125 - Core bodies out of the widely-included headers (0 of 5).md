# Plan: core bodies out of the widely-included headers

Plan of 2026-11-25, a Restructure, for the next round (product owner, 2026-11-25). **Goal: a body edit in core rebuilds one file, not the test suite, so the edit-build-test loop on a laptop takes seconds where it takes minutes.**

The trigger: on 2026-11-25 a batch of fixes in `MoonModule.h`, `WorkerPool.h` and `Effects.h` cost a full host and test rebuild per iteration, about ten minutes each on the product owner's M2.

## What exists

- **The rule.** [coding-standards.md](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp) makes light-domain modules and the `MoonModule` base header-only, and core service modules `.h` + `.cpp`. Core services follow it: `HttpServerModule.cpp` (70 commits since August), `MoonLiveCompiler.cpp` (47), `Scheduler.cpp` (18), `Control.cpp` (16) each rebuild one object per edit.
- **Why a header edit costs so much.** An object depends on its `.cpp` and every header it includes, which the compiler's dependency files track; a body edit in a header changes the preprocessed text of every includer, so ccache misses on all of them. A comment-only edit hits, since ccache's preprocessed mode strips comments.
- **The build, measured on 2026-11-25.** The host build holds 571 objects: 72 app, 354 test (350 in `mm_tests`), 145 one-header interface checks. Builds run 4 jobs on the M2 with 8 GB (`build_jobs()`), at about 1.9 s of wall time per object. `module_types.cpp` includes every module (329 headers, a 2.2 MB object) and compiles three times, for the app, `mm_tests` and `mm_scenarios`, about 20 s each. No LTO on the host (`-O3`) or the ESP32 (`-O2`).
- **Tests carry the fan-out.** 239 of the 350 test objects reach `MoonModule.h` through their fixtures (`GridLayout.h`, `Layouts.h`, `Scheduler.h`, `Layer.h`); the app alone has 23.

## Where it pays

Dependent objects from the host build's dependency files, commits since 2026-08-01 from git, and the share of body edits from a sample of each header's diffs (estimated). Saving per body edit is (dependents − 1) × 1.9 s, plus about 60 s where `module_types.cpp` is among them; per month it is a floor, since a commit takes three to five builds.

| Header | Dependents | Commits | Body edits | Saved per body edit | Saved per month |
|--|--|--|--|--|--|
| `light/layers/Layer.h` | 139 | 37 | 76% | 4.4 min + 1 | 32 min |
| `core/module/Control.h` | 268 | 27 | 48% | 8.5 min | 29 min |
| `core/module/MoonModule.h` | 262 | 21 | 56% | 8.3 min | 26 min |
| `light/drivers/Drivers.h` | 21 | 60 | 83% | 1.6 min | 21 min |
| `core/util/JsonSink.h` | 224 | 11 | 67% | 7.1 min | 14 min |
| `light/drivers/DriverBase.h` | 48 | 46 | 71% | 1.5 min | 13 min |
| `core/moonlive/MoonLive.h` | 81 | 28 | 66% | 2.5 min | 12 min |
| `light/layouts/Layouts.h` | 142 | 11 | 70% | 4.5 min | 9 min |
| `core/module/Scheduler.h` | 220 | 16 | 21% | 6.9 min | 6 min |

Gain nothing: `platform.h` (134 commits, but 11% body edits: it is declarations already), `format.h`, `ScratchBuffer.h`, `HeapBytes.h`, `StateDocument.h`, and the module headers with twenty dependents or fewer (`HttpServerModule.h`, `DevicesModule.h`, `SystemModule.h`).

## The textbook

Interface in the header, implementation in the `.cpp`, as Lakos' *Large-Scale C++ Software Design* teaches for physical design: a client recompiles when the interface changes, not when an implementation does. The per-frame one-liners stay inline, because without LTO a call across translation units is a real call. A body defined in a header and called from many files is emitted once already (COMDAT), so moving it to a `.cpp` leaves the ESP32 image's size as it is.

## Other levers on the average build

The split shortens a rebuild after a body edit. These shorten the rest, each measured against the same baseline before it is kept:

| Lever | What it does | Where it helps | Its cost |
|--|--|--|--|
| **`-ftime-trace`** (Clang) with ClangBuildAnalyzer | Names the headers and templates that take the compile time, from the compiler's own trace | Decides the order of every other lever, so it comes first | None in the build; a report to read |
| **Ninja** instead of Unix Makefiles | The host build uses Unix Makefiles today; Ninja schedules across targets and checks dependencies faster, most visible on an incremental build | Every host build | The build scripts and any path that assumes a Makefile build tree |
| **`module_types.cpp` once** | One object or static library linked into the app, `mm_tests` and `mm_scenarios` instead of three compiles of the heaviest file | Every module-header edit, about 40 s | The registry's link order across three targets |
| **Precompiled `doctest.h` and the standard library** (`target_precompile_headers` on the test targets) | Step 1 measured doctest at 249 s of the build's 815 s of compiler time, parsed again by every test object; a header holding doctest and the standard library alone is untouched by a core edit, so it saves about 0.6 s per rebuilt test object on every build | Every test build, cold or after an edit: the cheapest lever measured | A missing include can hide behind the precompiled header; the base header stays out of it, or a core edit rebuilds it |
| **Unity build** (`CMAKE_UNITY_BUILD`) for the tests | Fewer, larger translation units | Full builds, CI | Worse incremental builds; hides missing includes and lets file-local names clash |
| **ccache tuned** | Today's host hit rate is 32.8%; `sloppiness` and direct mode settings raise it where timestamps or macros miss | Branch switches, repeated gate runs | Few; a wrong sloppiness setting can serve a stale object |
| **Tests at `-O2` instead of `-O3`** | Less optimizer time per test object | Every test build | Tests no longer exercise the exact code the release runs; a bug that shows only at `-O3` slips through, so CI keeps one `-O3` run |
| **ccache keys free of the build path** (`CCACHE_BASEDIR`, `-fdebug-prefix-map`) | Each ESP32 firmware builds in a folder of its own, so the same ESP-IDF source hashes differently per folder and one firmware's cache serves no other | Every first build of a firmware, and every firmware after a shared source changes | A setting per build script; debug paths in an image read relative |
| **ESP-IDF built once per sdkconfig** | The IDF components built once per sdkconfig and linked into each firmware that shares it, as Espressif's prebuilt IDF libraries for Arduino are | The ESP32 builds, most of whose time is IDF | Firmwares differ in sdkconfig more than they share, so the gain depends on how many share one; the build scripts gain a cache to keep correct |
| **`extern template`** for the heavy templates (`ModuleFactory`, `ScratchBuffer`) | Instantiated once instead of in every includer | Every includer | A declaration per instantiation to keep in step |

More jobs is not one of them: the M2's 8 GB caps builds at 4 jobs (`build_jobs()`), and more swaps.

**The machine around the build.** The gate builds run under `nice`, which keeps the laptop responsive, so they take what the foreground leaves. On 2026-11-25 the load average stood at 25 on 8 cores during a gate run: GitKraken refreshing on the changed files (77% of a core), the host MoonLight rendering (74%), the browser tabs showing its preview and graph (about 140% across WebKit and WindowServer) and VS Code's C++ indexer (38%). The fresh ESP32 build took 520 to 581 s. Step 1 measures on a quiet machine, and the gate process names what to close during a run.

What helps, largest first:

1. **GitKraken closed during a gate run**: it refreshes on exactly the file churn a run causes.
2. **MoonLight's browser tabs closed, or on a static card**: the animated preview and graph keep WebKit and WindowServer busy.
3. **VS Code's C++ indexer kept out of the build trees**: `build/**` in `C_Cpp.files.exclude` and `files.watcherExclude`, so generated files are not re-indexed.
4. **The host MoonLight stopped during a run**, or its frame rate capped while nothing watches it; it renders as fast as it can today, and a cap is the product owner's decision.
5. **`nice` kept**: it keeps the laptop usable while four jobs compile, which the unbounded build of 2026-11-09 showed is needed; a quieter foreground is what speeds the niced build.

## The test suite's share

The tests are 354 of the 571 objects and carry most of the fan-out, so their shape is part of the build time (product owner, 2026-11-25). On 2026-11-25 the suite held 4,153 cases and 872,065 assertions. A case is one named behavior; an assertion is each `CHECK` inside it, and nearly all of them come from loops in a few audit cases, such as one `CHECK` per compared pixel across 79 effects. An assertion costs a comparison and doctest's bookkeeping, a second or two over the whole run; the run's 90 to 140 s sits in a few slow cases, and the build's in the test objects themselves. Every behavior stays pinned (principle 4); what is measured is where the suite spends:

- **Run time per case**, from doctest's `--duration`: the slowest cases, the sleeps among them, become polls on a condition.
- **Compile time per test object**, from step 1's `-ftime-trace`: a fixture that includes a whole layout or the module registry where a forward declaration serves includes less.
- **Overlap**, from `llvm-cov` per case: a case whose covered lines are a subset of another's is a candidate to merge, its description kept as the other's.
- **Assertions in tight loops** count the mismatches and `CHECK(mismatches == 0)` once, which keeps the pin, prints one readable failure and drops the per-pixel bookkeeping.

## Risks and costs

- **The hot path loses inlining** if a per-frame function moves to a `.cpp`: a call per pixel or per frame where there was none. Guarded by the inline list per header, the scenarios' host tick and an ESP32 frame rate compared with the baseline at each step.
- **Every target must build the new `.cpp`.** The host CMake, the ESP32 component, the Teensy build and Android each list their sources; a target that misses one fails to link on that platform alone. Guarded by CI's build of every target per step.
- **Two files to read where there was one.** A contributor reading `MoonModule` follows declarations into a `.cpp`; out-of-line definitions repeat each signature. The doc comments stay on the declarations in the header, where docgen and the help read them.
- **ESP32 flash** is expected to stay as it is (bodies are emitted once either way); measured per step, since a body losing `inline` can change what the optimizer keeps.
- **A large core diff conflicts with open pull requests.** Steps land when few are open, one header group per step, so a conflict stays small.
- **Gains are estimates until measured.** The per-month figures assume today's commit rate and a body-edit share sampled from diffs; step 1's baseline and each step's remeasure are what count.

## Steps

1. ✅ **Measure the baseline** (2026-11-25, at `94d9dcc00` on the M2, quiet, 4 jobs, ccache off, `-ftime-trace`). The cold host and test build takes 289 s, 815 s of compiler time (368 s parsing, 443 s optimizing). One body edit rebuilds: `Control.h` 291 s and 298 objects, `MoonModule.h` 222 s and 290, `JsonSink.h` 182 s and 234, `Layer.h` 166 s and 133, `Drivers.h` 37 s and 20. **`doctest.h` takes 249 s of the 815**, parsed again in each of 403 units at about 0.6 s each, a fifth of every rebuild above; then `Layer.h` 22 s, `EffectBase.h` 19 s, `AudioService.h` 16 s. The heaviest units are `unit_Effects_gridsweep.cpp` 39 s, `unit_Effects_framerate.cpp` 20 s and each copy of `module_types.cpp` 20 s. The reference: the ESP32 image 2,430 KB and the scenarios' host ticks of the commit's KPI line. The step was: a `-ftime-trace` report of a full host and test build, and one body edit each in `MoonModule.h`, `Control.h`, `Layer.h`, `JsonSink.h` and `Drivers.h`, timed as a rebuild of the host and the tests on the M2; the ESP32 image size and the host tick of the scenarios. The same five edits are timed again after each step.
2. ⏳ **`MoonModule.h` and `Control.h`, together** (one include chain, about 55 minutes a month). Kept inline: `tick`, `tick20ms`, `tick1s`, `enabled`, `role`, `publishTiming`, `addAccumUs`, `addChildTimeUs`, `respectsEnabled`, the small virtual defaults, and Control's typed getters. To `MoonModule.cpp` and `Control.cpp`: adding, removing and moving children, `quiesceForMutation`, `onEnabled`, control definition, naming, the JSON and state paths. With `quiesceForMutation` in `MoonModule.cpp` including `Scheduler.h`, the free `noteStructureChange()` and its forward declaration go, the include cycle with them. The File shape rule changes: the `MoonModule` base becomes `.h` + `.cpp`, with the inline exceptions named.
3. ⏳ **`Layer.h`** (about 32 minutes a month). Kept inline: `tick`, `frameMs`, `addWorkerUs`, `contributes`, the per-pixel set, get and blend helpers. To `Layer.cpp`: layout changes, extrude and warp setup, control handling, the prepare paths. Layer is light-domain infrastructure rather than a catalog module, which its one-line reason at the top of the file says.
4. ⏳ **`JsonSink.h`** (no per-frame code, about 14 minutes a month): everything but the one-line writers.
5. ⏳ **`Drivers.h` and `DriverBase.h`** (about 34 minutes a month together, most of it `module_types.cpp` compiled three times). Kept inline: the ticks, `composite`, `anyLit`, the brightness and color-order helpers. To `.cpp`: the split and retry logic, fixture persistence, `defineControls`, output enumeration.

`Scheduler.h`, `MoonLive*.h` and `ModuleFactory.h` follow for uniformity where a step touches them anyway; their payoff is small.

## Open questions

1. **The other levers.** Which of the table's levers join this plan as steps and which become plans of their own, decided on step 1's trace: Ninja and `module_types.cpp` once look cheapest.
2. **LTO.** With `-flto` the inline list could shrink, at the cost of link time and a change to both builds; measured, not assumed.
3. **The test fixtures' fan-out.** Fixtures that include `GridLayout.h` and `Layouts.h` where a forward declaration serves shrink the test side further, include-what-you-use.

## What it removes

The free `noteStructureChange()` and its forward declaration in `MoonModule.h`; the "header-only" status of the `MoonModule` base in the File shape rule; a full rebuild per body edit in the five headers above.

# Plan: faster builds

Plan of 2026-11-25, a Restructure, for the next round (product owner, 2026-11-25). **Goal: the edit-build-test loop on a laptop takes seconds where it takes minutes: a body edit in core rebuilds one file, and a test object stops paying for what every test object parses again.**

The trigger: on 2026-11-25 a batch of fixes in `MoonModule.h`, `WorkerPool.h` and `Effects.h` cost a full host and test rebuild per iteration, ten minutes and more on the product owner's M2 under load.

## What exists

- **The rule.** [coding-standards.md](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp) makes light-domain modules and the `MoonModule` base header-only, and core service modules `.h` + `.cpp`. Core services follow it: `HttpServerModule.cpp` (70 commits since August), `MoonLiveCompiler.cpp` (47), `Scheduler.cpp` (18), `Control.cpp` (16) each rebuild one object per edit.
- **Why a header edit costs so much.** An object depends on its `.cpp` and every header it includes, which the compiler's dependency files track; a body edit in a header changes the preprocessed text of every includer, so ccache misses on all of them. A comment-only edit hits, since ccache's preprocessed mode strips comments.
- **The build.** The host build holds 571 objects: 72 app, 354 test (350 in `mm_tests`), 145 one-header interface checks, on Unix Makefiles. Builds run 4 jobs on the M2 with 8 GB (`build_jobs()`). `module_types.cpp` includes every module (329 headers, a 2.2 MB object) and compiles three times, for the app, `mm_tests` and `mm_scenarios`. No LTO on the host (`-O3`) or the ESP32 (`-O2`).
- **Tests carry the fan-out.** 239 of the 350 test objects reach `MoonModule.h` through their fixtures (`GridLayout.h`, `Layouts.h`, `Scheduler.h`, `Layer.h`); the app alone has 23.

## The baseline

Measured on 2026-11-25 at `72399d9c3` on the M2, 4 jobs, ccache off, each edit the fastest of two runs, the host, `mm_tests` and `mm_scenarios` rebuilt; the baseline beside steps 2 and 3, in alternating rounds so load hits all three alike:

| What | Baseline | Step 2 | Step 3 | Objects |
|--|--|--|--|--|
| Cold build | 154 s | 161 s | 162 s | all |
| A body edit in `MoonModule.h` | 142 s | 122 s (−14%) | 117 s (−18%) | 293 |
| A body edit in `Control.h` | 155 s | 127 s (−18%) | 118 s (−24%) | 301 |
| A body edit in `Layer.h` | 106 s | 91 s (−14%) | 84 s (−21%) | 136 |
| A body edit in `JsonSink.h` | 144 s | 117 s (−19%) | 106 s (−26%) | 237 |
| A body edit in `Drivers.h` | 41 s | 39 s (−5%) | 27 s (−34%) | 23 |
| The five edits together | 588 s | 496 s (−16%) | 452 s (−23%) | |

The cold build stays as it is: with four jobs busy throughout, a shorter tail and a parse saved per unit barely move it, and the precompiled header costs its own compile. The steps pay in the rebuild after an edit, the wait a developer has. Step 1's first measurement, at `94d9dcc00`, read 289 s cold and 222 s for `MoonModule.h` while Spotlight indexed the build output, before its exclusion; the trace below comes from it, and its shares hold.

The cold build is 815 s of compiler time: 368 s parsing, 443 s optimizing. **`doctest.h` alone takes 249 s**, parsed again in each of 403 units at about 0.6 s each, a fifth of every rebuild above; then `Layer.h` 22 s, `EffectBase.h` 19 s and `AudioService.h` 16 s across their includers. The heaviest units are `unit_Effects_gridsweep.cpp` 39 s, `unit_Effects_framerate.cpp` 20 s and each copy of `module_types.cpp` 20 s; at 4 jobs a 39 s unit can be the tail a whole build waits for. The reference for each step: the ESP32 image 2,430 KB and the scenarios' host ticks of the commit's KPI line.

## Where a header split pays

Dependent objects from the host build's dependency files, commits since 2026-08-01 from git, and the share of body edits from a sample of each header's diffs (estimated). Per month is a floor, since a commit takes three to five builds; the per-edit saving is the baseline's rebuild less one object and a link.

| Header | Dependents | Commits | Body edits | Saved per body edit | Saved per month |
|--|--|--|--|--|--|
| `light/layers/Layer.h` | 139 | 37 | 76% | about 2.5 min | 32 min |
| `core/module/Control.h` | 268 | 27 | 48% | about 4.5 min | 29 min |
| `core/module/MoonModule.h` | 262 | 21 | 56% | about 3.5 min | 26 min |
| `light/drivers/Drivers.h` | 21 | 60 | 83% | about 0.5 min | 21 min |
| `core/util/JsonSink.h` | 224 | 11 | 67% | about 3 min | 14 min |
| `light/drivers/DriverBase.h` | 48 | 46 | 71% | about 1.5 min | 13 min |

Gain nothing from a split: `platform.h` (134 commits, but 11% body edits: it is declarations already), `format.h`, `ScratchBuffer.h`, `HeapBytes.h`, `StateDocument.h`, and the module headers with twenty dependents or fewer (`HttpServerModule.h`, `DevicesModule.h`, `SystemModule.h`).

## The textbook

Interface in the header, implementation in the `.cpp`, as Lakos' *Large-Scale C++ Software Design* teaches for physical design: a client recompiles when the interface changes, not when an implementation does. The per-frame one-liners stay inline, because without LTO a call across translation units is a real call. A body defined in a header and called from many files is emitted once already (COMDAT), so moving it to a `.cpp` leaves the ESP32 image's size as it is. A precompiled header parses a stable set of headers once for a target, which pays where every unit includes the same heavy header, as every test includes doctest.

## Steps

Cheapest first, each measured against the baseline: the same cold build and five edits, the ESP32 image and the host ticks; the ESP32 steps by a firmware's fresh and incremental build. A wall time on this laptop carries the load of whatever else runs, and load only ever adds time, so each figure is the fastest of two runs on a quiet machine, the textbook rule for timing benchmarks. The start of step 3 measures the baseline, step 2 and step 3 that way together.

1. ✅ **Measure the baseline**, as above (2026-11-25).
2. ✅ **A precompiled header for the tests** (2026-11-25): `doctest.h`, `<cstdint>`, `<cstdio>`, `<cstring>`, `<string>`, `<vector>`, `<algorithm>`, `<memory>`, `<filesystem>` and `<thread>` through `target_precompile_headers` on `mm_tests`; `test_main.cpp` skips it, since it defines `DOCTEST_CONFIG_IMPLEMENT` before its include, and the target's ccache launcher carries the sloppiness settings ccache needs to cache a compile that uses one. The project's own headers stay out of it, so a core edit leaves it as it is. The paired measurement: the five edits −16% together (−14% to −19% per core header, −5% for `Drivers.h`, whose dependents are app objects and `module_types.cpp`); the cold build +5%. The test objects' parsing halved (345 → 183 s of compiler time): the header holds the standard library the tests include as well as doctest, and Clang instantiates its templates once (`-fpch-instantiate-templates`).
3. ✅ **The tail** (2026-11-25): `module_types.cpp` compiled once, as the object library `mm_module_types` the app, `mm_tests`, `mm_scenarios` and the Android library link; the three compiled it with the same definitions and flags. The grid sweep splits by test case into `unit_Effects_gridsweep.cpp`, `unit_Effects_buffersweep.cpp` and `unit_Effects_tubesweep.cpp`, sharing `effect_grid_sweep.h`, so their instantiations of every effect compile in parallel. `unit_Effects_framerate.cpp` stays whole: its time is one test over every effect, which splits only once the generated effect list offers subsets. The paired measurement: the five edits −23% against the baseline together, 7 points past step 2; `Drivers.h` −34%, its rebuild mostly the registry compiled three times.
4. ✅ **Ninja for the host build** (2026-11-25): `build_host.py`, `package_host.py` and `check_sanitizers.py` configure with Ninja where it is installed, from one helper (`generator_args`), and remove a build directory left by Unix Makefiles, since CMake keeps a directory's generator; Windows keeps the Visual Studio generator of its clang-cl toolset. Measured on step 3's code, fastest of two: cold 162 → 105 s (−35%), an edit in `Drivers.h` 30 → 18 s (−40%), `JsonSink.h` 122 → 105 s, `Layer.h` 83 → 75 s, `Control.h` 109 → 110 s, a build with nothing to do 1 s. Make walks the targets and directories in turn, each with its own overhead, where Ninja holds every compile in one graph: the smaller the rebuild, the more that overhead weighs, and a large rebuild is the compiles alone. The first edit after switching build folders rebuilt the other folder's touched headers too, so the `MoonModule.h` pair is left out.
5. ✅ **ccache keys free of the build path for the ESP32 builds**: measured 2026-11-25, no change made. The 25 firmware build directories hold 25 different `sdkconfig.h` files (flash size, partition table, features such as `CONFIG_MM_AUDIO_CODEC`, the PSRAM workarounds), and nearly every ESP-IDF source includes it, so a firmware's IDF compiles differ from another's in content, not only in path, and a key free of the path shares nothing between them. It would serve the same firmware built from another checkout path, which CI, building from one path, never does. The same count answers ESP-IDF built once per sdkconfig, dropped as a step: no two firmwares share one. What speeds an ESP32 build is the incremental build per firmware directory, building the board under test alone, and the header splits below.
6. ✅ **`MoonModule.h` and `Control.h`, together** (2026-11-25) (one include chain). Kept inline: `tick`, `tick20ms`, `tick1s`, `enabled`, `role`, `publishTiming`, `addAccumUs`, `addChildTimeUs`, `respectsEnabled`, the small virtual defaults, and Control's typed getters. To `MoonModule.cpp` and `Control.cpp`: adding, removing and moving children, `quiesceForMutation`, `onEnabled`, control definition, naming, the JSON and state paths. With `quiesceForMutation` in `MoonModule.cpp` including `Scheduler.h`, the free `noteStructureChange()` and its forward declaration go, the include cycle with them. The File shape rule changes: the `MoonModule` base becomes `.h` + `.cpp`, with the inline exceptions named. Built in `MoonModule.cpp` and `Control.cpp`, listed once in `src/core/sources.cmake` for every target; `sanitizeHostname`, out of line and reached from `tick1s`, carries `MM_NONBLOCKING`. Measured with Ninja, fastest of two: a body edit 136 → 1 s in `MoonModule` and 125 → 1 s in `Control` (291 and 299 objects to 2), a declaration edit in `MoonModule.h` 136 → 119 s, the slimmer header parsing faster; the cold build waits for a quiet rerun. A declaration edit still rebuilds every includer, which step 7 takes on.
7. ✅ **`platform.h` split by concern** (2026-11-25), the complement to step 6: twelve headers in `src/platform/` (`platform_base`, `_clock`, `_heap`, `_tasks`, `_system`, `_fs`, `_net`, `_ota`, `_video`, `_leds`, `_audio`, `_io`), `platform.h` the umbrella. 204 includers moved to the concerns they use, 83 gained direct includes, 38 keep the umbrella. Objects per header on the host build: base 279, clock 259, heap 254, system 233, net 223, tasks 183, io 163, audio 156, fs 101, leds 52, ota 19, video 18, against 342 for `platform.h` before. Clock, heap, system and net stay wide because inline bodies in widely included headers reach them; the declaration-edit timing waits for a quiet run. The step as planned: moving bodies out helps a body edit, while a declaration edit still rebuilds every includer, and `platform.h` is the most edited header (134 commits since August, 89% of them declarations, a new platform function each time) with the widest reach (342 objects). One header per concern (time, heap and allocation, filesystem, network, GPIO and buses, tasks, audio, firmware update), each file including only what it uses, so a new network function rebuilds the network's users alone; `platform.h` stays a thin umbrella while the includes move. Lakos' levelization, and the interface segregation principle. Measured as a declaration edit in one concern, before and after. Wide and mechanical, so it lands when few pull requests are open. The next candidates, decided on this step's measured gain: `Control.h` (about 14 declaration edits since August, 268 objects: the control types, `ListSource`, the list and grid API) and `Scheduler.h` (about 13, 220 objects: most includers need only `Scheduler::instance()`), as one step; `MoonModule.h` (about 9, 262 objects) likely stays whole, since nearly every includer needs the class itself. Together they come to about a quarter of `platform.h`'s rebuilt objects (about 9,000 against 40,700).
8. ⏳ **`JsonSink.h`** (no per-frame code): everything but the one-line writers.
9. ⏳ **`Layer.h`**. Kept inline: `tick`, `frameMs`, `addWorkerUs`, `contributes`, the per-pixel set, get and blend helpers. To `Layer.cpp`: layout changes, extrude and warp setup, control handling, the prepare paths. Layer is light-domain infrastructure rather than a catalog module, which its one-line reason at the top of the file says.
10. ⏳ **`Drivers.h` and `DriverBase.h`**. Kept inline: the ticks, `composite`, `anyLit`, the brightness and color-order helpers. To `.cpp`: the split and retry logic, fixture persistence, `defineControls`, output enumeration.
11. ⏳ **The tests at `-O2`** (the product owner's decision): less optimizer time per test object, the larger half of the compiler's time. CI keeps one `-O3` run, so the code the release runs stays tested.

`Scheduler.h`, `MoonLive*.h` and `ModuleFactory.h` follow the splits for uniformity where a step touches them anyway; their payoff is small.

## Other levers

Measured before any joins the steps:

| Lever | What it does | Where it helps | Its cost |
|--|--|--|--|
| **ccache tuned** | Today's host hit rate is 32.8%; `sloppiness` and direct mode settings raise it where timestamps or macros miss | Branch switches, repeated gate runs | Few; a wrong sloppiness setting can serve a stale object |
| **Unity build** (`CMAKE_UNITY_BUILD`) for the tests | Fewer, larger translation units | Full builds, CI | Worse incremental builds; hides missing includes and lets file-local names clash |
| **`extern template`** for the heavy templates (`ModuleFactory`, `ScratchBuffer`) | Instantiated once instead of in every includer | Every includer | A declaration per instantiation to keep in step |
| **The compile launcher in Python** | `diag_launcher.py` wraps every compile to keep its warnings for the code check, so each object starts an interpreter too, about 300 per core-edit rebuild | Every host build with the hot-path warnings on | A launcher in a compiled language, or the warnings read from the build log, keeps the code check's input |

More jobs is not one of them: the M2's 8 GB caps builds at 4 jobs (`build_jobs()`), and more swaps. Nor is LTO (product owner, 2026-11-25): it optimizes at link time, so every edit pays a longer link; ESP-IDF leaves it unsupported, and its section placement for IRAM can break under it; and the removal it adds to size is mostly done already, by `-ffunction-sections`, `-fdata-sections` and the linker's `--gc-sections`.

## The machine around the build

The gate builds run under `nice`, which keeps the laptop responsive, so they take what the foreground leaves. On 2026-11-25 the load average stood at 25 on 8 cores during a gate run: GitKraken refreshing on the changed files (77% of a core), the host MoonLight rendering (74%), the browser tabs showing its preview and graph (about 140% across WebKit and WindowServer), VS Code's C++ indexer (38%) and Spotlight indexing the build output (about 150%). Builds ran two to three times slower than on the quiet machine of step 1. What helps, largest first:

1. **Spotlight kept out of `~/Developer/GitHub`** (done 2026-11-25): it indexed every object a build wrote.
2. **Terminal and Visual Studio Code listed as Developer Tools** (System Settings → Privacy & Security → Developer Tools): macOS's Gatekeeper (`syspolicyd`, 61% of a core on 2026-11-25) otherwise assesses every freshly linked binary, `mm_tests` after each rebuild among them.
3. **GitKraken closed during a gate run**: it refreshes on exactly the file churn a run causes.
4. **MoonLight's browser tabs closed, or on a static card**: the animated preview and graph keep WebKit and WindowServer busy.
5. **VS Code's C++ indexer kept out of the build trees**: `build/**` in `C_Cpp.files.exclude` and `files.watcherExclude`, so generated files are not re-indexed.
6. **The host MoonLight stopped during a run**, or its frame rate capped while nothing watches it; it renders as fast as it can today, and a cap is the product owner's decision.
7. **`nice` kept**: it keeps the laptop usable while four jobs compile, which the unbounded build of 2026-11-09 showed is needed; a quieter foreground is what speeds the niced build.

## The test suite's share

The tests are 354 of the 571 objects and carry most of the fan-out, so their shape is part of the build time (product owner, 2026-11-25). On 2026-11-25 the suite held 4,153 cases and 872,065 assertions. A case is one named behavior; an assertion is each `CHECK` inside it, and nearly all of them come from loops in a few audit cases, such as one `CHECK` per compared pixel across 79 effects. An assertion costs a comparison and doctest's bookkeeping, a second or two over the whole run; the run's 90 to 140 s sits in a few slow cases, and the build's in the test objects themselves. Every behavior stays pinned (principle 4); what is measured is where the suite spends:

- **Run time per case**, from doctest's `--duration`: the slowest cases, the sleeps among them, become polls on a condition.
- **Compile time per test object**, from the baseline's trace: a fixture that includes a whole layout or the module registry where a forward declaration serves includes less.
- **Overlap**, from `llvm-cov` per case: a case whose covered lines are a subset of another's is a candidate to merge, its description kept as the other's.
- **Assertions in tight loops** count the mismatches and `CHECK(mismatches == 0)` once, which keeps the pin, prints one readable failure and drops the per-pixel bookkeeping.

## Risks and costs

- **The hot path loses inlining** if a per-frame function moves to a `.cpp`: a call per pixel or per frame where there was none. Guarded by the inline list per header, the scenarios' host tick and an ESP32 frame rate compared with the baseline at each step.
- **Every target must build the new `.cpp`.** The host CMake, the ESP32 component, the Teensy build and Android each list their sources; a target that misses one fails to link on that platform alone. Guarded by CI's build of every target per step.
- **A precompiled header can hide a missing include**: a test that compiles only because the header holds what it forgot. The one-header interface checks keep compiling each header alone, which catches it for the `src/` headers; a test file has no such check.
- **A cold build costs 5% more** after steps 2 and 3 (154 → 162 s): the precompiled header compiles first, and four busy jobs leave a shorter tail little to gain. CI builds mostly from scratch, so its test build slows by about that much while every rebuild after an edit gains.
- **ccache caches the test target only with looser settings** (`pch_defines`, `time_macros`, scoped to `mm_tests`): a cached test object could outlive a change to `__DATE__` or a define the precompiled header holds, harmless for tests that read neither.
- **Two files to read where there was one.** A contributor reading `MoonModule` follows declarations into a `.cpp`; out-of-line definitions repeat each signature. The doc comments stay on the declarations in the header, where docgen and the help read them.
- **ESP32 flash** is expected to stay as it is (bodies are emitted once either way); measured per step, since a body losing `inline` can change what the optimizer keeps.
- **A large core diff conflicts with open pull requests.** The split steps land when few are open, one header group per step, so a conflict stays small.
- **Gains are estimates until measured.** The per-month figures assume today's commit rate and a body-edit share sampled from diffs; each step's remeasure against the baseline is what counts.

## Open questions

1. **The other levers.** Which join the steps, measured first.
2. **The test fixtures' fan-out.** Fixtures that include `GridLayout.h` and `Layouts.h` where a forward declaration serves shrink the test side further, include-what-you-use.
3. **One warning set on every host.** The macOS build lacks `-Wshadow`, which the Linux and Android builds turn into errors, so the local gates passed a shadowed name CI then rejected (2026-11-25); the same flags everywhere, with what macOS then finds fixed.

## What it removes

The free `noteStructureChange()` and its forward declaration in `MoonModule.h`; the "header-only" status of the `MoonModule` base in the File shape rule; two of the three compiles of `module_types.cpp`; the re-parse of doctest in every test object; a full rebuild per body edit in the split headers.

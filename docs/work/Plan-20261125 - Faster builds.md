# Plan: faster builds

Plan of 2026-11-25, a Restructure, for the next round (product owner, 2026-11-25). **Goal: the edit-build-test loop on a laptop takes seconds where it takes minutes: a body edit in core rebuilds one file, and a test object stops paying for what every test object parses again.**

The trigger: on 2026-11-25 a batch of fixes in `MoonModule.h`, `WorkerPool.h` and `Effects.h` cost a full host and test rebuild per iteration, ten minutes and more on the product owner's M2 under load.

## Steps

1. ✅ **Measure the baseline**: history in git.
2. ✅ **A precompiled header for the tests**: [the compiler cache](../reference/build-system.md#the-compiler-cache).
3. ✅ **The tail**, `mm_module_types` compiled once and the grid sweep split: [the compiler cache](../reference/build-system.md#the-compiler-cache).
4. ✅ **Ninja for the host build**: [build directories](../reference/build-system.md#build-directories).
5. ✅ **ccache keys for the ESP32 builds**, no change made: [the compiler cache](../reference/build-system.md#the-compiler-cache).
6. ✅ **`MoonModule.h` and `Control.h`**: [file shape](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp).
7. ✅ **`platform.h` split by concern**: [the platform interface](../moonmodules/platform/index.md).
8. ✅ **`JsonSink.h`**: [file shape](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp).
9. ✅ **`Layer.h`**: [file shape](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp).
10. ✅ **`Drivers.h` and `DriverBase.h`**: [file shape](../contributing/coding-standards.md#file-shape-header-only-vs-h-cpp).
11. ❌ **The tests at `-O2`** (measured 2026-11-26, not adopted): the cold test build 191 → 160 s and a core-header edit 42 → 37 s, against tests that no longer compile the header-only light domain at the release's `-O3`, a second CI configuration and a rule exception, for about 5 s an edit once steps 8 to 10 had split the most-edited headers.

The machine settings (Spotlight, Developer Tools, the indexer exclusions, `nice`) live in the `build-setup` process of `moonwalk/09-plan-setup.yaml`; the open levers and questions are cards on the Project.

## What it removes

The free `noteStructureChange()` and its forward declaration in `MoonModule.h`; the "header-only" status of the `MoonModule` base in the File shape rule; two of the three compiles of `module_types.cpp`; the re-parse of doctest in every test object; a full rebuild per body edit in the split headers.

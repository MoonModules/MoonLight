# Plan: prepare only what a change reshapes

Plan of 2026-10-22, a Restructure. A control whose module says `affectsPrepare` makes `Scheduler::reactToControlChange` call `requestPrepareTree()`, and the next frame prepares every module in the tree. hpwit's show moves two Region windows from a service every 20 ms, so the whole tree is prepared 50 times a second. Measured:

- **shiffy** (ESP32-S3, 3,840 lights, LCD-MM): one Region move costs about 0.5 s; with the service running a frame takes 1.0 to 1.7 s, where the five layers alone run at 23 to 28 fps. An opacity change, which prepares nothing, costs nothing.
- **The desktop** (128×128, Art-Net to an FPP): the frame stays near 5 ms, but each prepare stops and restarts the Art-Net sender's task, and a 128×128 frame takes about 50 ms at its 500 µs packet gap, so the FPP never receives a whole frame.
- **Both**: every move marks the tree dirty, so `Effects.json` is written to flash about once a second while a show runs.

The goal: a Region move costs a frame or less on shiffy, the Art-Net output keeps flowing with the show running, and a service driving controls does not write flash once a second.

## What exists

- `MoonModule::affectsPrepare(control)` answers yes or no; `ModifierBase` answers yes for every control.
- `Scheduler::prepareTree()` runs `applyState()` on every top module, which prepares each module and its children, then `onTreePrepared()`.
- Some modules gate their own prepare: `ParallelLedDriver` reinits the bus only when `frameBytes_` changed. Others redo their work unconditionally: `NetworkSendDriver` stops and restarts its send task, and `MoonLiveEffect` rebuilds its controls.
- A control declared `live` is driven continuously and is not configuration, so it never marks the tree dirty.

## End states

1. **A change prepares its scope.** `affectsPrepare` returns a scope instead of a yes: none, the module's subtree, its layer, or the tree. The scheduler prepares only that scope at the frame boundary. A module whose prepare changes what another reads, a buffer's size, a channel count or a light count, requests the tree itself. *Gains*: a Region move prepares one layer; drivers and other layers are untouched. *Loses*: every module's prepare must report when its outputs change, and a missed report leaves a stale reader.
2. **Every prepare is cheap when nothing changed.** The tree is still walked, and each module skips its work when its inputs are unchanged, as `ParallelLedDriver` does. *Gains*: no scheduling change. *Loses*: every module needs its own gate, the walk itself still costs, and a missed gate is a silent slowdown.
3. **Both: scoped prepares, and gates where a module is prepared anyway.** *Gains*: the smallest cost per change, and a tree prepare also gets cheaper. *Loses*: the most code of the three.

Recommended: **1**. It is the textbook incremental rebuild: work follows the dependency, and the one rule is that a module requests the wider scope when its outputs change. End state 2's gates become optional optimizations inside it.

## Steps

1. ✅ **A scope per change.** `affectsPrepare` keeps saying whether a control reshapes; a new `prepareScope()` says how far, `Self`, `Parent` (the nearest container) or `Tree`, defaulting to `Tree` so every existing module behaves as today. `ModifierBase` answers `Parent`. The scheduler holds up to four pending containers per frame, each claimed with one atomic compare-and-swap whatever task asks, a fifth widening the request to the tree: the show moves two layers every frame. `onTreePrepared()` still runs across the tree after a container prepare, so the Control card re-resolves its targets. Unit test: a Region control change prepares its own layer, not a sibling layer, the layout or a driver.
2. ✅ **Escalation by the reader.** `Drivers` records the first layer's shape it sized from (the layer, how many composite, its mapping, light count, channel count, and its buffer's count when the drivers read it directly) and prepares itself again in `onTreePrepared()` when that changed. The reader knows what it reads, so no layer has to guess. Unit test: an unchanged layer leaves the drivers alone, a channel-count change reaches them.
3. ✅ **A service's writes are live.** A value a service writes through the control surface is driven, not configuration: it never marks the tree dirty, so it is never saved, while a person's move is. Unit test: a service script moving a slot leaves the tree clean, and the same move from the API marks it dirty.
4. ✅ **Merging.** Several moves of one container in one frame prepare it once, two containers each once, five the tree. Unit tests.
5. ✅ **A script is read only when it may have changed.** Measuring on shiffy put each layer's rebuild at 37 to 78 ms, almost all of it `MoonLiveScript::sync` reading the script file to hash it, 30 ms for 714 bytes and 70 ms for 1,924 under load. The file API already announces every change through `notifyFileChanged`; a script now reads its file again only after a change in or above its folders, or under a new name. Unit test: an edit nobody announced keeps the compiled program, an announced one recompiles.
6. ✅ **A prepare that changed nothing a module holds keeps it.** The idempotent-prepare rule, broken by two modules met here: `AudioService` reopened its microphone and rebound its sync socket on every prepare, and `NetworkSendDriver` restarted its send task. Each now compares what it opened with. Unit tests for both.
7. ✅ **A layer at opacity 0 above the bottom costs nothing**: it neither renders nor blends, so the show alternates voronoi and the mandala per scene by opacity, with no rebuild. Unit test.
8. ✅ **Measure** with `scenario_MoonLive_hpwit_show` on shiffy and on the desktop with the FPP: the `show` step at 20 fps or more on shiffy, and the FPP still receiving while the service runs.

## Decided

- A value a service writes through the control surface is live and never saved; a person's move is saved (2026-10-22).
- Core's scope names are domain-neutral: `Self`, `Parent`, `Tree`, a modifier answering `Parent` (2026-10-22).
- `prepareScope()` beside `affectsPrepare`, rather than a scope replacing its yes or no: two files instead of 25 overrides, the same behavior.

## Cost

About 150 lines of code and 150 of tests. Under 40 bytes in the Scheduler, nothing per module, nothing per frame: the frame boundary already checks the pending prepare.

## Measured (2026-10-22)

| | Before | After |
|---|---|---|
| shiffy, show with its service | 1,300 to 1,700 ms a frame | 44 ms, 22 fps, the same as the layers standing still |
| Waveshare 64×64 HUB75, same | 273 ms | 43 ms, 23 fps |
| desktop 128×128 to the FPP | Art-Net frozen | about 1,100 fps, the sender undisturbed |
| flash saves while a show runs | about one a second | none |

## What it removes

The whole-tree prepare on every structural control, and the flash write a running service caused once a second. It adds one virtual and one rule, a module asking for the tree when its outputs change; nothing existing goes, since `affectsPrepare` keeps its meaning.

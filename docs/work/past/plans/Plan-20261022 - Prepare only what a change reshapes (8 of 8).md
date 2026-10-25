# Plan: prepare only what a change reshapes

Plan of 2026-10-22, a Restructure, every step done 2026-10-22. A control whose module said `affectsPrepare` prepared the whole tree on the next frame. hpwit's show moves two Region windows from a service every 20 ms, so the tree was prepared 50 times a second: shiffy (ESP32-S3, 3,840 lights) took 1.0 to 1.7 s a frame, the desktop's Art-Net sender restarted on every prepare so the FPP never received a whole frame, and `Effects.json` was written to flash about once a second.

**Result**: shiffy runs the show at 22 fps, the same as its layers standing still; the FPP keeps receiving; nothing is saved while a service drives the show.

## Where it lives now

| What | Home |
|---|---|
| The scope (`Self`, `Parent`, `Tree`), the merging of up to four scopes a frame, the reader preparing itself again, the idempotent prepare | [MoonModule, Event triggering between modules](../../../explanation/architecture/moonmodule.md#event-triggering-between-modules) |
| A service's writes are live and never saved; a person's are | [MoonModule, Persistence](../../../explanation/architecture/moonmodule.md#persistence) |
| A script read again only after an announced change | [MoonLive](../../../explanation/architecture/moonlive.md) |
| A layer at opacity 0 above the bottom costs nothing | `Layer.h`, and [the StadBeest's looks](../../../reference/examples/multi-board-installation.md#3-one-surface-layout-for-every-device) |
| The measurements | [performance.md, Prepare only what a change reshapes](../../../reference/performance.md#prepare-only-what-a-change-reshapes) |
| The proof | `scenario_MoonLive_hpwit_show`, and the unit tests named in each step's commit |

## Steps

1. ✅ **A scope per change.** `prepareScope()` beside `affectsPrepare`: `Self`, `Parent` or `Tree`, a modifier answering `Parent` ([MoonModule](../../../explanation/architecture/moonmodule.md#event-triggering-between-modules)).
2. ✅ **Escalation by the reader.** `Drivers` prepares itself again when the first layer's shape it sized from changed.
3. ✅ **A service's writes are live**, never saved, while a person's are ([Persistence](../../../explanation/architecture/moonmodule.md#persistence)).
4. ✅ **Merging.** Up to four containers prepare once each per frame; a fifth widens to the tree.
5. ✅ **A script is read only when it may have changed**, after an announced file change ([MoonLive](../../../explanation/architecture/moonlive.md)).
6. ✅ **A prepare that changed nothing a module holds keeps it**: `AudioService` keeps its microphone and socket, `NetworkSendDriver` its send task.
7. ✅ **A layer at opacity 0 above the bottom costs nothing**, so a show switches looks by opacity (`Layer.h`).
8. ✅ **Measure** with `scenario_MoonLive_hpwit_show` on shiffy and the desktop with the FPP ([performance.md](../../../reference/performance.md#prepare-only-what-a-change-reshapes)).

## The end states weighed

1. **A change prepares its scope**, the module asking for the tree when its outputs change. Picked: the textbook incremental rebuild, work following the dependency. Its risk, a module that misses its report leaving a stale reader, is why `Drivers` checks the shape it sized from rather than trusting each layer to report.
2. **Every prepare cheap when nothing changed**, each module gating its own work. Not picked: every module needs its own gate, the walk still costs, and a missed gate is a silent slowdown. Its gates stay optional optimizations inside 1, as `ParallelLedDriver`'s was.
3. **Both.** Not picked: the most code for a gain 1 already delivers.

## Decided

- A value a service writes through the control surface is live and never saved; a person's move is saved.
- Core's scope names are domain-neutral, `Self`, `Parent`, `Tree`, a modifier answering `Parent`.
- `prepareScope()` beside `affectsPrepare`, rather than a scope replacing its yes or no: two files instead of 25 overrides, the same behavior.

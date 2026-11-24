# Plan: layers rendered in parallel

Plan of 2026-11-23, a Refactor, for approval. **Goal: a frame takes as long as its slowest layer, not the sum of its layers**, on every board with a second core.

## What exists

- **Effects ticks its layers one after another on the render thread**, then composites them. Measured on a NanoPi R3S (four A55 cores) on 2026-11-23, five layers at 640×360: the frame took 49.5 ms, of which the layers 46.2 ms in sum, the largest 21.5 ms; after the rotation and Beat Sweep fixes of the same day, 29 ms in sum, the largest 8.4 ms.
- **A layer renders into its own buffer**: its effects and modifiers touch only that buffer until the composite, so layers are independent work, the textbook case for a worker pool.
- **Shared state a layer's tick reads or writes**: `AudioService::latestFrame()` and `beatNow()`, read-only during the render; `Palettes::active()`, written between frames; MoonLive's sinks, per thread (step 1).
- **The cores**: a classic ESP32, an S3 and a P4 have two, one of them carrying WiFi, the network and the audio task; a C3 has one; a Pi or NanoPi four; a PC more.

## End state

- A worker pool, one worker per spare core, takes layers from a queue while the render thread takes its share; Effects waits for all of them, then composites. On one core the pool is empty and the order is the one there is today.
- On the R3S the frame drops to about its slowest layer. An ESP32 or a P4 renders on one thread until a measured step shows its second core has room beside the drivers.
- Every effect, modifier and builtin runs correctly on any worker: no shared mutable state in a layer's tick.

## Steps

1. ✅ **Make a layer's tick self-contained (2026-11-23):** what a layer's tick shares is per thread or applied between frames, [Parallelism](../explanation/architecture/moonmodule.md#parallelism).
2. ✅ **The worker pool in the core (2026-11-23):** [WorkerPool](../../src/core/util/WorkerPool.h), its claim word in the header.
3. 🧪 **Effects dispatches its layers to the pool (2026-11-23)**, awaiting the thread sanitizer's first run in CI; waits, then composites in the layers' order as now. A computer starts one worker a core less the render loop (`platform::cpuCores()`); an ESP32 starts none, `renderThreads` 1, since its second core carries the drivers, the network and the audio, which a worker there would compete with: a measured step of its own. On the R3S, four layers at 640×360: 39 to 75 fps, the frame 25.5 to 13.0 ms, Effects 21.9 to 9.9 ms against a largest layer of 9 ms. Tests: four layers over 20 frames with workers and without, byte for byte; every test case now starts on the default palette, since DemoReel's request had landed inside the next case's first frame; a modifier's rebuild runs on the render loop before the layers, one layer at a time, since layers share the layout a rebuild places lights through. The thread sanitizer runs the whole suite in CI.
4. ✅ **Measure and document (2026-11-24):** the R3S's numbers and the pool in [Parallelism](../explanation/architecture/moonmodule.md#parallelism); a P4 and an S3 are measured with the ESP32 worker, a step of its own.

## Open questions

- Whether a layer with a live modifier keeps its warp on the worker that rendered it (simplest) or the composite pass takes all warps.
- On an ESP32, whether the audio task and the render worker share the second core well, or the worker yields while audio sync is busy.

## What it removes

The MoonLive sinks as mutable statics, the light domain's largest group of them. It adds the pool, one seam with one implementation per platform family.

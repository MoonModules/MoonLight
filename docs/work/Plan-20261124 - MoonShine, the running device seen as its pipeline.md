# Plan: MoonShine, the running device seen as its pipeline

Plan of 2026-11-24, a Feature, approved and started the same day. **Goal: understand a running device from the top down: what its pipeline is, where its frame time and memory go, why, and which code does it, from one overview down to a single frame and a line of source; and see what each optimization bought against the code it cost.**

## The mantra: minimal code, minimal intrusion, maximal benefit

MoonShine watches the device it runs on, so it costs that device as little as it can and tells as much as it can (product owner, 2026-11-26). It runs on every board, and the smallest boards first, since they are the ones that need tuning most.

- **The hot path counts, and that is all.** A tick adds to a number or keeps a maximum, beside a clock read that is there already: no allocation, no lock, no string, no log line.
- **The rest computes when someone reads it.** Sums, shares, the bottleneck and the JSON are built on a read of the page, from the counters, so a device nobody watches pays for its counters alone.
- **Recording is off until asked.** A feature that records over time, the event ring, costs one branch per tick while off.
- **The computer carries the weight.** What a board's flash has no room for, trends, traces, the ledger and the source map, lives in MoonDeck.
- **Every step states its price.** Flash, RAM and the per-frame cost, measured on a board, in this plan and in the commit that adds it, beside what it shows that was hidden before.

The product owner's idea of the same day: a visualizer of a running device that shows the pipeline as a flow of nodes, with drill-down to measurements, built from textbook solutions, with the process industry and intensive-care monitoring as inspiration. It is named MoonShine (product owner, 2026-11-24): a still is a small process plant, stages and pipes and gauges, watched and tuned for what comes out at the end. And its first purpose (product owner, 2026-11-24): the code has grown past what a person holds in mind, so the tool is the way in, navigated from the top down.

## What exists

- **The figures, live.** Every module reports its time per frame, its object size and the heap it holds (`tickTimeUs`, `classSize`, `dynamicBytes`), pushed over the WebSocket when they change. The device reports frame rate, free heap, the largest free block, the allocation peak, each task's stack mark, and `renderWait`, the render core's wait for the output core.
- **How the time is measured.** A parent brackets each child's tick with the wall clock, and once a second the sum is divided by the frame count: an average per frame over the last second. A parent's figure includes its children.
- **A graph view.** The device map is hand-written SVG with layout, zoom, pan and search, and draws any list a module marks as a graph (`listAsGraph`), rows as nodes and `edges` as lines.
- **The pipeline, in the code.** Layouts, then Effects, whose layers run on a worker pool, each layer ticking its effects and modifiers into its own buffer; then Drivers, which blends the layers into one output buffer and hands it to the drivers on the encode task.
- **Offline.** Scenarios record tick, free heap and the largest block per measure step (p50, p95); the KPI line and `repo-health.json` carry them per commit, beside the code counts of `code.json` and the commit's own `Commit:` line.
- **Gaps.** With layers in parallel a parent reads less than the sum of its children. The module list of `/api/system` is flat. The UI has no trend line. Nothing sets a performance change beside the complexity it added.
- **Backlog this absorbs.** "Canvas or node-graph view" and "Modifier chain visualization" (ui), and "Memory: the largest buffers, by name" (core).

## The textbook

| Source | What it prescribes | Used for |
|--|--|--|
| ISA-101, the process industry's display standard | Four display levels: overview, unit, detail, diagnostics. Gray is normal, color means abnormal. An analog bar with its normal band, a trend beside it | The four levels, and how a healthy device looks |
| NEWS2, the early warning score of UK hospitals | A fixed set of vital signs, each with a normal range and a score of 0 to 3; one aggregate; a single extreme value escalates alone | The device's vitals and its one score |
| ISA-18.2 and IEC 60601-1-8 | An alarm is actionable; three priorities | The alarm list, which the status severities already are |
| TouchDesigner's Probe, Blender's node timings, Houdini's performance monitor | Time on the node itself; self beside self plus children; a short history per node; a capture triggered by a frame over budget | The pipeline graph |
| Apache Flink and NiFi, ROS rqt_graph | Heat color against fixed thresholds with the number beside it; wire width for data volume | Node color and wires |
| Perfetto, the trace viewer of Android and Chromium | A flame chart per thread and counter tracks, in the browser, on a trace the page hands over | One frame, in detail |
| Game engine profilers (Unreal Insights, Unity) | A bar per frame against the target; a track per thread | The frame budget |
| The USE method | Utilization, saturation and errors per resource | Which columns a stage has |
| The Linux kernel's patch rules, rustc's performance tracking | An optimization states its cost with its number; every commit is benchmarked | The ledger |
| The C4 model (Simon Brown) | Software is read top down in four zooms: the system, its containers, their components, the code | The same four levels as a map of the code |
| The balance sheet; the treemap (Shneiderman) and the Sankey diagram | A total is split into named owners and it balances; area or width is the amount | Where the memory goes |

Two things no source has: a tool that draws a render pipeline as a node graph from the device's own page, and a view that sets a performance gain beside the complexity it added. Both are small and ours, on the conventions above.

## The map is the way into the code

The four levels are C4's four zooms, so the view of the running device is also the table of contents of the source: the device, its pipeline's stages, a module, the code. A node is a module that exists in the tree right now, so what a reader meets first is what this device runs, a fraction of the whole codebase. Each node says what it is in one line, from the summary its header already carries for the docs, and links to its page. How much code stands behind it (lines, functions over the complexity limit, from the code check) and its source at the device's build are MoonDeck's, so the device carries none of it (product owner, 2026-11-27). A reader goes from "what is slow" or "what is this" to the file that does it, top down, with no search.

## End state

Four levels, as ISA-101 names them, and a ledger beside them, in two homes (product owner, 2026-11-24).

**On the device's own page, the light version:** levels 1 and 2, small enough for an ESP32's flash, as the MoonShine card under System, beside Tasks.

1. **The device.** A handful of vitals, each an analog bar with its normal band: frame time against its budget, free heap, the largest free block, the render core's wait, and the outputs' pace against the rate asked of them. One score for the device, raised at once by a single vital far outside its band. Below it the alarms, the modules whose status is a warning or an error, errors first. A healthy device is gray.
2. **The pipeline.** The node graph: layouts, the layers with their effects and modifiers, the blend, the drivers and their outputs, left to right, in lanes by the thread that runs them. A node carries its own time and its heap, colored against the frame budget, a container its own time beside its children's, its longest tick of the second beside its average, and an output its pace. A wire is as wide as the bytes it carries each frame. The frame's longest of the second stands beside its average above the graph.

**In MoonDeck, the full MoonShine**, looking at any device on the network, with room for what a board's flash has none for:

3. **A module.** The trend of its time and heap over minutes or hours, its buffers by name, how much code stands behind it, and a link to its source at the device's build on GitHub.
4. **A frame.** A recorded stretch of frames opened in Perfetto: one track per thread, one slice per module tick, counter tracks for the heap. Recorded on request, or by a trigger when a frame runs over its budget.
5. **Scenarios.** A scenario runs from the same view, shows on the graph while it runs, and keeps each module's figures and its trace, so two runs compare stage by stage.
6. **Graphs over time** (product owner, 2026-11-25). A scenario's figures drawn as time series, one small chart per figure stacked on one shared time axis (Tufte's small multiples, a Grafana dashboard's panels): frame time with its worst frame, each module's own time, heap and the largest block per memory type, each output's pace and queue. The scenario's own events (a step to more lights, a layer added, load switched on) are marked across all of them. A temporary bottleneck shows as a spike in one chart at a moment where the others say why, which the p50 and p95 of a measure step average away.

**The ledger.** One row per commit that names an optimization: what the scenarios measured before and after (frame time, heap, flash) beside what the code check counted (lines, complex functions, duplicated lines). It answers whether 1% was worth its code.

**Why MoonDeck and not a web tool of its own:** a page served over https, as a hosted tool is, may not read a device that answers on http, which every browser blocks as mixed content. MoonDeck serves from the developer's own machine over http, already runs the scenarios and holds the checkout the source links and the ledger read.

## The budget, and a pipeline's contract

- **The budget** is the frame time the outputs ask for: the fastest rate an enabled driver is set to, such as the HDMI or panel card `fps`, else 60 frames a second. A heavy show is still fine slower, so the bands are 60 and above as normal, down to 30 as acceptable, down to 20 as the limit (product owner, 2026-11-24), which are the vital's scores of 0 to 3.
- **What a device can do depends on its pipeline, so a contract is per configured pipeline** (product owner, 2026-11-24). A classic ESP32 drives 16,384 lights with one layer mapped one to one, which needs the fewest buffers; several blended layers with modifiers do far fewer. A contract names a board, a pipeline, its lights, its frame rate and its memory, and is a scenario, so a live run proves or breaks it. [performance.md](../reference/performance.md) has 16,384 lights at 11 fps on an Olimex `esp32-lite`, the first to settle.
- **The view explains the cost, stage by stage.** Each stage shows its time per light, a figure that holds at any size, so the frame time is the sum over the stages of time per light times lights, and the view says which stage a larger wall would break. Each buffer shows why it exists: the output buffer is there because two layers are blended, a layer's table because its layout is mapped. That is the decomposition that says what is feasible before a board is asked to prove it.

## The memory balance sheet

The device has so much memory, and the sheet says where it is: one line per owner, the modules with their buffers by name, the tasks' stacks, the network and the system, and a last line for what no owner reports. The lines add up to the total, so a remainder that grows is a finding in itself. Drawn as a treemap or a Sankey from the total down to a buffer.

**Per memory type** (product owner, 2026-11-25). A board has more than one kind of RAM, and where a buffer lives matters as much as its size: internal RAM is fast and scarce, PSRAM is large and slower, and a buffer an interrupt or a DMA engine reads per byte belongs in internal RAM ([platform.h](../../src/platform/platform.h) says why). So the sheet has a column per type, each balancing to its own total: internal RAM, PSRAM and executable RAM on an ESP32, as the IDF's heap capabilities name them; RAM1, RAM2 (`DMAMEM`) and PSRAM (`EXTMEM`) on a Teensy 4; one column on a computer. An owner needs no new hook: `ptrIsPsram` already tells each buffer's type, and the platform layer reports each type's total and free, the one place that knows them. Flash gets a sheet of its own.

## The tick time balance sheet

The frame has so much time, and the sheet says where it goes, as the memory sheet does for bytes (product owner, 2026-11-25). The frame period is the total: one line per owner, each module's own time (so a parent and its children count once), the blend, the render core's wait for the output core, the system's work between frames (the HTTP server, the WebSocket, the network stack), the idle time left before the next frame is due, and a last line for what no owner reports. The lines add up to the frame period, so a remainder that grows is a finding, as it is for memory.

With layers on a worker pool, time runs on several threads at once and wall time does not add up across them. So the sheet has a column per thread, each balancing to the same frame period: the CPU utilization of `top` and the USE method. The thread with the least idle sets the frame rate, the critical path. Drawn as a stacked bar per thread against the budget, the frame bar of a game engine's profiler.

## The sheets on the graph

Both sheets are drawn on the pipeline graph, not as tables below it (product owner, 2026-11-25).

- **Chips.** A node carries a colored chip per memory type it holds (internal RAM, PSRAM) and a chip per thread it spends time on, one color per memory type and one per thread. Threads, not cores: a computer moves threads between cores, while an ESP32 pins each thread to a core, which the legend names.
- **Every type stands alone.** Internal RAM is scarce and PSRAM practically unlimited, so a type is never added to another: each has its own total, free and share, and a chip's relative figure is its share of its own type's total, or of the frame for time.
- **Absolute or relative.** A selector above the graph switches every chip between bytes and microseconds, and percentages.
- **The strip.** Above the graph, a bar per memory type (held by modules, unaccounted, free) and a bar per thread (busy, waiting, idle): the lines that belong to no node, and the place internal RAM running out shows first.
- **Every lane.** The graph shows the whole device: the light pipeline, the services (network, HTTP, MQTT and the rest of the module tree), and the system tasks. On an ESP32 a system task is a FreeRTOS task with its CPU time, from FreeRTOS's runtime stats, and its stack; on a computer, the process's threads with their CPU time.
- **The bottleneck.** The module with the largest own time on the thread with the least idle, the critical path, has a color of its own, always, so the module that sets the frame rate is in view.

## Hooks in the code

Few, and in one place each.

- **Timing** stays where it is, the parent's bracket around each child's tick. The same bracket writes a begin and an end event for the trace, so no module is instrumented by hand.
- **A named section inside a module**, such as Drivers' blend apart from its wait, is a scoped zone, the textbook construct of Perfetto and Tracy: one line where a module wants it, costing one branch while recording is off, or compiled out, which step 6 decides.
- **The task table is taken when someone reads it**, the Tasks card or MoonShine, at most once a second: walking it suspends the scheduler for about a millisecond on an S3, which a device nobody watches skips (product owner, 2026-11-27).
- **MoonLight's own allocations are counted where they happen**, `alloc`, `allocInternal` and every `new`, measured at 1.6 µs more per allocation and free on a classic ESP32 and kept for the line it buys (product owner, 2026-11-27).
- **Source links** need no hook: the device reports its build's commit and each module's type, and docgen writes the index from type to file that MoonDeck resolves.

## Steps

The pipeline graph comes first, so there is something to look at from the start; the order after it is decided as the work shows, built, seen, tested, then decided (product owner, 2026-11-24). Every step is designed for an ESP32 and a Teensy as well as a computer, and built on a computer and a Pi first for the shorter loop.

1. ✅ **The pipeline as a graph, on today's figures.** [MoonShineModule](../../src/light/layers/MoonShineModule.h).
2. 🚧 (a module's own time beside its total, the thread it runs on and its parent are on the graph; the second's longest frame on the render bar and each module's longest tick in its tooltip, and a live modifier's share of the live pass, 2026-11-26, the longest tick measured on an S3 at 0.03 µs a tick over the bare sum, about 1 µs a frame, the frame rate unchanged at 104 to 107; each output's pace from the one `paceFps` rule and the frames dropped where NetworkSend, PanelCard and HLS count them, 2026-11-27; queue fill and the HUB75 engines' drops wait) **Honest figures.** A module's own time beside its total; the worst frame of the second beside the average; a modifier's tick timed; the thread a module ran on; parent and depth in the module list. An output reports its pace: frames sent a second beside the rate asked of it, frames dropped, and how full its queue is, the saturation of the USE method, from the counters an engine keeps already (the HUB75 engines count frames encoded and submitted).
3. 🚧 (both sheets on the graph, seen on an ESP32-P4, a classic ESP32, an S3 with a HUB75 panel, a Pi 5 and a NanoPi R3S, 2026-11-25; the tasks' stacks and control blocks are a line of their own and a chip on each task, 54 KB of a classic ESP32's internal RAM, 2026-11-26; the network is the free heap its bring-ups took less the task stacks they started, so what it allocates later stays unaccounted; the system is what was used before mm_main less its task stacks, and MoonLight's own allocations are counted where they happen, 2026-11-26; ESP-IDF's per-task heap tracking measured 13 KB of internal RAM and 14 µs more per allocation on a classic ESP32 and is not adopted, so "unaccounted" is what has no owner, and the output engines' DMA buffers, which they take straight from the heap, until those are counted too; a computer's sheet is its process's threads with their CPU share and its resident memory against what the machine has available, on Linux, macOS and Windows, 2026-11-27, read when someone looks and 0 bytes on a board) **The balance sheets.** Memory: every owner reports its buffers by name, the sheet adds them up against the total per memory type and shows the remainder. Tick time: each thread's owners and its idle, adding up to the frame period. Both drawn on the graph as [the sheets on the graph](#the-sheets-on-the-graph) describes: chips, the strip, the selector, the services' and system tasks' lanes, the bottleneck.
4. ⏳ **The budget and a pipeline's contract.** The budget from the outputs with its bands; time per light on each stage and the reason on each buffer; the first contract, a classic ESP32 at 16,384 lights, as a scenario.
5. ⏳ **The vitals.** The overview: bars with bands, the score, the alarm list.
6. ⏳ **The event ring.** A ring of events in the core, a begin and an end per module tick with its thread and the heap as a counter, its size set per platform and off until asked for, where it costs one branch a tick. On a computer and a Pi first, then an ESP32 and a Teensy with a ring of their own size.
7. ⏳ **MoonDeck's full view.** The graph of any device on the network as the map of its code: each node's summary, the code behind it and its source link; trends and a module's history; a button that hands the ring's last seconds to Perfetto, and the trigger that records the frames around one over budget.
8. ⏳ **Scenarios in the view.** A run shows on the graph, a measure step keeps each module's figures and its trace, and two runs compare: Houdini's difference profile, the differential flame graph. The run's figures are kept as a time series, a sample a second, and drawn as graphs over time with the scenario's events marked, to find a bottleneck that lasts seconds. Load is a dimension of a scenario: the same pipeline at several levels of other work, the outputs' pace recorded at each, as Anjin's run of the PIO cadence test on a Pi 5 does by hand (2026-11-24), so a contract can say how much load an output holds its pace under.
9. ⏳ **The ledger.** The repo-health page gains the table of optimizations, built from the per-commit metrics already written; the commit process asks an optimization for its number and its cost, as the kernel does.

## Open questions

1. **The contracts after the first.** Which boards and pipelines get one, and the frame rate and memory at which each counts as met.
2. **The ring on a board, decided at step 6** (product owner, 2026-11-27). Switched at run time with a branch per tick, so every firmware records on request, or compiled out; and its size on a board without PSRAM, measured then rather than guessed. MabuTrace, an ESP32 component that feeds Perfetto, holds its events in a 64 KB ring at about 12 bytes each; its code is GPL-3.0, so the pattern is taken and the code is written here.
3. **Perfetto's viewer** loads from Google's site, the trace staying in the browser. Tried as it is first; MoonDeck serves a copy if a venue with no connection needs it.
4. **The trace format.** Chrome's JSON events are the least code and Perfetto reads them, while calling them legacy; its own protobuf packets are the format it recommends.
5. **CPU time.** Wall time counts a wait as work. Whether a thread's CPU time is worth reading per module on the platforms that give it.
6. **How light the device's version is.** What the graph list and the vitals cost in flash on an ESP32, measured at step 1, decides what else may join them there.
7. **The code that is in no pipeline.** Core utilities, the platform layer and the tooling are behind no node of a running device; whether the map shows them as the layers the architecture names.

## What it removes

The flat module list of `/api/system` and the Tasks card's line of module times, once the graph list carries the same figures; the memory and time sheets as tables, once the graph's chips and strip carry them; the backlog cards "Canvas or node-graph view", "Modifier chain visualization" and "Memory: the largest buffers, by name", which this plan builds.

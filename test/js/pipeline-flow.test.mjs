// The device flow (flow.js): where each node goes, its chips against the bars, and when it heats. Run in Node, on the pure functions.
// Run: `node --test test/js/pipeline-flow.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { flowLayout, flowHeat, flowClass, flowTime, flowTitle, splitRows, barTotals, chipsOf, bandView, toggleBand, barLabel } from "../../src/ui/flow.js";

const row = (name, stage, group) => ({ name, stage, ...(group ? { group } : {}) });
const rows = [row("Grid", 0), row("E1", 1, "A"), row("M1", 2, "A"), row("A", 3, "A"), row("E2", 1, "B"), row("B", 3, "B"), row("Drivers", 4), row("D1", 5)];

test("a stage is a column, left to right", () => {
    const { pos } = flowLayout(rows);
    const xs = ["Grid", "E1", "M1", "A", "Drivers", "D1"].map(n => pos.get(n).x);
    assert.deepEqual(xs, [...xs].sort((a, b) => a - b));
    assert.equal(pos.get("E1").x, pos.get("E2").x);
});

test("a layer's nodes share a band, and two layers' boxes stay apart", () => {
    const { pos, boxes } = flowLayout(rows);
    assert.equal(boxes.length, 2);
    assert.ok(pos.get("E2").y > pos.get("E1").y);
    assert.ok(boxes[1].y >= boxes[0].y + boxes[0].h);
});

test("the same rows land on the same spots, so a redraw keeps every node in place", () => {
    assert.deepEqual([...flowLayout(rows).pos], [...flowLayout(rows.map(r => ({ ...r, us: 5 }))).pos]);
});

test("a node stays gray under half the frame budget, warns to the budget, errs over it", () => {
    assert.equal(flowHeat(100), "");
    assert.equal(flowHeat(9000), "flow-warm");
    assert.equal(flowHeat(20000), "flow-hot");
});

test("a node's figure is its own time, with its total beside it when children add to it", () => {
    assert.equal(flowTime({ us: 40, own: 40 }), "40 µs");
    assert.equal(flowTime({ us: 452, own: 12 }), "12 µs / 452 µs");
    assert.equal(flowTime({ us: 0, own: 0 }), "");
});

test("a node's tooltip names the module's one line when help is loaded, and both times", () => {
    const r = { name: "Pulse1", type: "PulseEffect", us: 452, own: 12, internal: 100, psram: 0, size: 100 };
    assert.match(flowTitle(r, "A `pulse` of light."), /^Pulse1 \(PulseEffect\)\nA pulse of light\.\n12 µs own, 452 µs with children/);
    assert.doesNotMatch(flowTitle(r, ""), /\n\n/);
});

test("a spike chip shows a longest tick past a millisecond and twice the average, and nothing for a steady node", () => {
    const totals = { memory: [1, 1], frame: 10000 };
    const spiky = chipsOf({ us: 400, worst: 5000, time: [[0, 400]] }, totals, false).time;
    assert.ok(spiky.some(c => c.cls === "flow-chip-spike" && c.text === "▲5.0 ms"));
    assert.ok(!chipsOf({ us: 400, worst: 700, time: [[0, 400]] }, totals, false).time.some(c => c.cls === "flow-chip-spike"));     // under a millisecond
    assert.ok(!chipsOf({ us: 8000, worst: 10000, time: [[0, 8000]] }, totals, false).time.some(c => c.cls === "flow-chip-spike")); // within twice the average
});

test("a node's tooltip names its longest tick of the second, the stutter its average hides", () => {
    const r = { name: "Pulse1", type: "PulseEffect", us: 452, own: 452, worst: 8000, internal: 0, psram: 0, size: 100 };
    assert.match(flowTitle(r, ""), /452 µs own, 452 µs with children per frame, longest tick 8\.0 ms/);
    assert.doesNotMatch(flowTitle({ ...r, worst: 0 }, ""), /longest/);
});

test("an output's tooltip names its DMA buffers, and one with none says nothing of them", () => {
    const r = { name: "ParallelLed", type: "ParallelLedDriver", us: 1900, own: 1900, internal: 52000, psram: 0, size: 2068, fps: 18, dma: 36864 };
    assert.match(flowTitle(r, ""), /\nDMA buffers 36KB/);
    assert.doesNotMatch(flowTitle({ ...r, dma: 0 }, ""), /DMA/);
});

test("an output near the edge shows its headroom as a red chip, and one with room only in its tooltip", () => {
    const totals = { frame: 10000 };
    const near = chipsOf({ us: 100, fps: 30, headroom: 12, time: [[2, 100]] }, totals, false).time;
    assert.ok(near.some(c => c.cls === "flow-chip-spike" && c.text === "headroom 12%"));
    assert.ok(!chipsOf({ us: 100, fps: 30, headroom: 80, time: [[2, 100]] }, totals, false).time.some(c => c.text.startsWith("headroom")));
    assert.match(flowTitle({ name: "Net", type: "NetworkSendDriver", us: 100, own: 100, fps: 30, headroom: 80 }, ""), /Headroom 80%, its margin before it glitches/);
});

const bars = [
    { name: "internal RAM", bar: "memory", type: 0, parts: [["modules", 100], ["module types", 0], ["unaccounted", 100], ["free", 200]] },
    { name: "PSRAM", bar: "memory", type: 1, parts: [["modules", 1000], ["module types", 0], ["unaccounted", 0], ["free", 9000]] },
    { name: "render loop", bar: "time", lane: 0, parts: [["modules", 4000], ["waiting", 1000], ["between modules", 0], ["idle", 5000]] },
];

test("the bars stay out of the layout, which places the nodes alone", () => {
    const { bars: b, nodes } = splitRows([...bars, ...rows]);
    assert.equal(b.length, 3);
    assert.deepEqual([...flowLayout(nodes).pos.keys()], rows.map(r => r.name));
});

test("a chip's share is of its own memory type, and time's of the frame", () => {
    const totals = barTotals(bars);
    assert.deepEqual(totals, { memory: [400, 10000], frame: 10000 });
    const r = { internal: 100, psram: 500, time: [[0, 2500], [1, 1000]] };
    const abs = chipsOf(r, totals, false), rel = chipsOf(r, totals, true);
    assert.deepEqual(abs.mem.map(c => c.text), ["100B", "500B"]);
    assert.deepEqual(rel.mem.map(c => c.text), ["25.0%", "5.0%"]);   // internal RAM is scarce: 100 B is a quarter of it
    assert.deepEqual(rel.time.map(c => [c.cls, c.text]), [["flow-chip-lane0", "25.0%"], ["flow-chip-lane1", "10.0%"]]);
    assert.deepEqual(abs.time.map(c => c.text), ["2.5 ms", "1.0 ms"]);
});

test("a system task's chips are its stack and its CPU time, a type it does not hold has no chip", () => {
    const { mem, time } = chipsOf({ type: "task", stackFree: 2048, time: [[3, 400]] }, barTotals(bars), false);
    assert.deepEqual(mem.map(c => c.cls), ["flow-chip-stack"]);
    assert.deepEqual(time.map(c => c.cls), ["flow-chip-lane3"]);
});

test("the bottleneck has a class of its own beside its heat", () => {
    assert.match(flowClass({ own: 20000, bottleneck: true, enabled: true }), /flow-hot.*flow-bottleneck/);
    assert.doesNotMatch(flowClass({ own: 10, enabled: true }), /bottleneck/);
});

// A device's bands as the rows come: System with two children, Network alone, and tasks on two cores and none.
const band = [
    { name: "System", group: "Modules", us: 40, own: 10, internal: 100, psram: 1000, time: [[0, 10]] },
    { name: "Tasks", group: "Modules", parent: "System", us: 20, own: 20, internal: 10, psram: 200, time: [[0, 20]], bottleneck: true },
    { name: "Stats", group: "Modules", parent: "System", us: 10, own: 10, internal: 0, psram: 50, time: [[0, 10]] },
    { name: "Network", group: "Modules", us: 5, own: 5, internal: 0, psram: 80, time: [[0, 5]] },
    { name: "main", group: "System tasks", type: "task", core: 0, time: [[3, 900]], internal: 4096, stackFree: 4000 },
    { name: "IDLE0", group: "System tasks", type: "task", core: 0, time: [[3, 100]], internal: 1536, psram: 512, stackFree: 1000 },
    { name: "mmEncode", group: "System tasks", type: "task", core: 1, time: [[3, 300]], stackFree: 2000 },
    { name: "tcpip", group: "System tasks", type: "task", core: -1, time: [[3, 40]], stackFree: 2600 },
];
const named = (rows, name, group) => rows.find(r => r.name === name && (!group || r.group === group));

test("a top-level module stands folded, its subtree's bytes summed and its time its children's too", () => {
    const view = bandView(band);
    assert.deepEqual(view.filter(r => r.group === "Modules").map(r => r.name), ["System", "Network"]);
    const sys = named(view, "System");
    assert.equal(sys.count, 2);
    assert.equal(sys.internal, 110);
    assert.equal(sys.psram, 1250);
    assert.deepEqual(sys.time, [[0, 40]]);
    assert.equal(sys.bottleneck, true);           // the bottleneck inside stays in view
    assert.equal(named(view, "Network").count, 0);
});

test("a module whose top level is drawn in the pipeline stands as a top level of its own", () => {
    const view = bandView([...band, { name: "Drivers", stage: 4 }, { name: "FixtureProfiles", group: "Modules", parent: "Drivers", us: 1, internal: 10 }]);
    assert.deepEqual(view.filter(r => r.group === "Modules").map(r => r.name), ["System", "Network", "FixtureProfiles"]);
});

test("opening a top level adds a band of its own, its own figures first, and closing takes it away", () => {
    toggleBand("System");
    const open = bandView(band).filter(r => r.group === "System");
    assert.deepEqual(open.map(r => r.name), ["System", "Tasks", "Stats"]);
    assert.deepEqual(open[0].time, [[0, 10]]);
    assert.equal(named(bandView(band), "System", "Modules").open, true);
    toggleBand("System");
    assert.equal(bandView(band).filter(r => r.group === "System").length, 0);
});

test("system tasks stand per core, the idle tasks' time shown apart as the time left, with the bytes its tasks hold", () => {
    const cores = bandView(band).filter(r => r.group === "System tasks");
    assert.deepEqual(cores.map(r => r.name), ["Core 0", "Core 1", "Either core"]);
    assert.deepEqual(cores[0].time, [[3, 900]]);
    assert.equal(cores[0].idle, 100);
    assert.equal(cores[0].count, 2);
    assert.equal(cores[0].internal, 5632);
    assert.equal(cores[0].psram, 512);
    assert.match(flowTitle(cores[0]), /holding 5\.5 ?KB internal/);
    assert.ok(chipsOf(cores[0], { memory: [], frame: 1000 }).time.some(c => c.cls === "flow-chip-idle" && c.text === "idle 100 µs"));
    toggleBand("Core 0");
    assert.deepEqual(bandView(band).filter(r => r.group === "Core 0").map(r => r.name), ["main", "IDLE0"]);
    toggleBand("Core 0");
});

test("an opened node's two drawings have places of their own", () => {
    toggleBand("System");
    const { pos } = flowLayout(bandView(band));
    assert.notDeepEqual(pos.get("Modules/System"), pos.get("System/System"));
    toggleBand("System");
});

test("a box's label has room above its first row of nodes", () => {
    const { pos, boxes } = flowLayout(bandView(band));
    const top = Math.min(...["System", "Network"].map(n => pos.get("Modules/" + n).y));
    const box = boxes.find(b => b.name === "Modules");
    assert.ok(top - box.y >= 8 + 12, "the label's 12 px and the box's padding sit above the nodes");
    boxes.slice(1).forEach((b, i) => assert.ok(b.y >= boxes[i].y + boxes[i].h, "two boxes stay apart"));
});

test("a band starts below every node its columns already hold, so four drivers and the Modules band stay apart", () => {
    const deep = [row("Grid", 0), row("E1", 1, "L"), row("L", 3, "L"), row("Drivers", 4), row("D1", 5), row("D2", 5), row("D3", 5), row("D4", 5),
        ...["Files", "System", "Network", "Firmware", "Control", "Audio", "Pins"].map((n, i) => row(n, i % 6, "Modules"))];
    const { pos, nodeW } = flowLayout(deep);
    const rects = [...pos.entries()].map(([n, p]) => ({ n, x: p.x, y: p.y, w: nodeW, h: 46 }));
    for (const a of rects) for (const b of rects) {
        if (a.n >= b.n) continue;
        const meet = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
        assert.ok(!meet, `${a.n} and ${b.n} overlap`);
    }
});

test("a memory bar's label names its largest free block, which fragmentation keeps under the free total", () => {
    const bar = { name: "internal RAM", bar: "memory", type: 0, parts: [["modules", 35000], ["free", 121000]], largest: 77824 };
    assert.equal(barLabel(bar, 156000, 0), "internal RAM 152KB, largest free block 76KB");
    assert.equal(barLabel({ ...bar, largest: undefined }, 156000, 0), "internal RAM 152KB");
    assert.equal(barLabel({ name: "render loop", bar: "time", parts: [] }, 50000, 4000), "render loop 46.0 ms busy");
});

test("a task's tooltip names its state and priority, and its core names the task it runs now", () => {
    assert.match(flowTitle({ name: "wifi", type: "task", us: 100, own: 100, internal: 6757, stackFree: 4420, state: "blocked", prio: 23 }, ""), /\nblocked, priority 23/);
    assert.match(flowTitle({ name: "Core 0", type: "core", count: 9, us: 900, idle: 100, internal: 50000, psram: 0, running: "main" }, ""), /, running main now$/);
});

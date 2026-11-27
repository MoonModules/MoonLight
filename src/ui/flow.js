// The device as a flow: a list drawn as bars on top and nodes in columns below (ListSource::listAsFlow). A row with `bar` is a bar, its
// `parts` adding up to its total: one per memory type and one per thread. Every other row is a node with its time and heap as colored
// chips, an `edges` array of {to, bytes} its wires, and a `group` boxing the nodes of one layer or band. Positions follow from the rows
// alone, so a redraw puts every node where it was.

import { fmtBytes } from "./uikit.js";

const SVG = "http://www.w3.org/2000/svg";
const FLOW_W = 960, NODE_H = 46, GAP = 12, PAD = 24, BOX_PAD = 8, LABEL_H = 12, CHIP_H = 11, BAND_COLS = 6;
const FLOW_BUDGET_US = 16667;   // 60 frames a second, until the device names its budget (plan step 4)
export const LANES = ["render loop", "layer workers", "encode task", "system tasks"];
export const MEMORY_TYPES = ["internal RAM", "PSRAM"];

let relative = false;   // the selector: chips and bars in bytes and microseconds, or in shares
const expanded = new Set();   // the top-level modules and cores opened into a band of their own

const MODULES = "Modules", TASKS = "System tasks";
const sum = (rows, key) => rows.reduce((s, r) => s + (r[key] || 0), 0);
const laneUs = (r, lane) => (r.time || []).filter(t => t[0] === lane).reduce((s, t) => s + t[1], 0);
const coreName = (core) => core < 0 ? "Either core" : "Core " + core;
// Each band row taken in turn across the columns, a band's own count starting at 0; the key tells an opened node's two drawings apart.
const inColumns = (rows, group) => rows.map((r, i) => ({ ...r, stage: i % BAND_COLS, group, key: group + "/" + r.name }));

// A top-level module folded: its subtree's bytes summed, its time with its children's, the bottleneck shown when it is inside.
function foldModule(top, kids) {
    const all = [top, ...kids];
    return { ...top, count: kids.length, open: expanded.has(top.name), internal: sum(all, "internal"), psram: sum(all, "psram"),
        own: top.us, time: top.us ? [[(top.time && top.time[0] && top.time[0][0]) || 0, top.us]] : [], bottleneck: all.some(r => r.bottleneck) || undefined };
}

// A core: its tasks' CPU time apart from the idle tasks', which is the time left on it, and the bytes its tasks hold.
function foldCore(core, tasks) {
    const idle = tasks.filter(t => /^IDLE/.test(t.name)), busy = tasks.filter(t => !/^IDLE/.test(t.name));
    const us = busy.reduce((s, t) => s + laneUs(t, 3), 0);
    return { name: coreName(core), type: "core", core, count: tasks.length, open: expanded.has(coreName(core)), enabled: true,
        us, own: us, idle: idle.reduce((s, t) => s + laneUs(t, 3), 0), time: us ? [[3, us]] : [], edges: [],
        internal: sum(tasks, "internal"), psram: sum(tasks, "psram"), running: (tasks.find(t => t.now) || {}).name };
}

// The bands as drawn: the Modules band holds the top-level modules folded, the System tasks band one node per core, and each opened
// one adds a band of its own below, its own figures first and then its parts. The pipeline's rows pass unchanged.
export function bandView(nodes) {
    const pipeline = nodes.filter(r => r.group !== MODULES && r.group !== TASKS);
    const mods = nodes.filter(r => r.group === MODULES), tasks = nodes.filter(r => r.group === TASKS);
    // A module whose top level is a pipeline node, as FixtureProfiles under Drivers is, stands as a top level of its own.
    const isTop = (r) => !r.parent || !mods.some(t => t.name === r.parent);
    const tops = mods.filter(isTop), kidsOf = (name) => mods.filter(r => r.parent === name && !isTop(r));
    const cores = [...new Set(tasks.map(t => t.core ?? -1))].sort((a, b) => (a < 0) - (b < 0) || a - b);
    const tasksOf = (core) => tasks.filter(t => (t.core ?? -1) === core);
    const out = [...pipeline, ...inColumns(tops.map(t => foldModule(t, kidsOf(t.name))), MODULES)];
    for (const t of tops) if (expanded.has(t.name) && kidsOf(t.name).length) out.push(...inColumns([t, ...kidsOf(t.name)], t.name));
    out.push(...inColumns(cores.map(c => foldCore(c, tasksOf(c))), TASKS));
    for (const c of cores) if (expanded.has(coreName(c))) out.push(...inColumns(tasksOf(c), coreName(c)));
    return out;
}

// Open a folded node into its band, or close it.
export function toggleBand(name) {
    if (!expanded.delete(name)) expanded.add(name);
}

// The bars and the nodes, apart.
export function splitRows(rows) {
    return { bars: rows.filter(r => r.bar), nodes: rows.filter(r => !r.bar) };
}

const total = (bar) => bar.parts.reduce((s, p) => s + p[1], 0);

// What a chip's share is of: each memory type's bar total, and the frame (any time bar's total, the render loop's first).
export function barTotals(bars) {
    const memory = [];
    let frame = 0;
    for (const b of bars) {
        if (b.bar === "memory") memory[b.type] = total(b);
        else if (b.bar === "time" && !frame) frame = total(b);
    }
    return { memory, frame };
}

// Where each node and each group box goes: columns by stage, a group's nodes in a band of their own, so two boxes stay apart.
export function flowLayout(rows) {
    const stages = [...new Set(rows.map(r => r.stage))].sort((a, b) => a - b);
    const colW = FLOW_W / Math.max(1, stages.length), nodeW = Math.min(colW - 24, 150);
    const colX = (stage) => stages.indexOf(stage) * colW + (colW - nodeW) / 2;
    const floor = stages.map(() => 0);        // the next free row of each column, below every node and band already in it
    const groupStages = new Map();            // group -> the columns its nodes take, known before its band is placed
    for (const r of rows) if (r.group !== undefined) (groupStages.get(r.group) ?? groupStages.set(r.group, new Set()).get(r.group)).add(r.stage);
    const bands = new Map();                  // group -> {top, used: stage -> rows}; a band starts 0.6 of a row below what its columns hold, room for its box
    const pos = new Map();
    let bandTop = 0;
    for (const r of rows) {
        let slot;
        if (r.group === undefined) slot = floor[stages.indexOf(r.stage)]++;
        else {
            if (!bands.has(r.group)) {
                const below = Math.max(0, ...[...groupStages.get(r.group)].map(st => floor[stages.indexOf(st)]));
                const top = Math.max(bands.size ? bandTop + 0.6 : 0, below ? below + 0.6 : 0);
                bands.set(r.group, { top, used: new Map(), minX: Infinity, maxX: 0 });
            }
            const b = bands.get(r.group);
            slot = b.top + (b.used.get(r.stage) ?? 0);
            b.used.set(r.stage, (b.used.get(r.stage) ?? 0) + 1);
            b.minX = Math.min(b.minX, colX(r.stage));
            b.maxX = Math.max(b.maxX, colX(r.stage) + nodeW);
            bandTop = Math.max(bandTop, b.top + Math.max(...b.used.values()));
            for (const st of groupStages.get(r.group)) floor[stages.indexOf(st)] = Math.max(floor[stages.indexOf(st)], b.top + Math.max(...b.used.values()) + 0.6);
        }
        pos.set(r.key ?? r.name, { x: colX(r.stage), y: PAD + slot * (NODE_H + GAP) });
    }
    const boxes = [...bands].map(([name, b]) => ({ name, x: b.minX - BOX_PAD, w: b.maxX - b.minX + 2 * BOX_PAD,
        y: PAD + b.top * (NODE_H + GAP) - BOX_PAD - LABEL_H, h: Math.max(...b.used.values()) * (NODE_H + GAP) - GAP + 2 * BOX_PAD + LABEL_H }));
    const maxSlot = Math.max(0, ...[...pos.values()].map(p => (p.y - PAD) / (NODE_H + GAP) + 1));
    return { pos, boxes, nodeW, height: PAD + maxSlot * (NODE_H + GAP) };
}

// A node's class: its heat against the budget (gray under half, a warning to the whole, an error over it), and the bottleneck's own.
export function flowHeat(us) {
    return us >= FLOW_BUDGET_US ? "flow-hot" : us >= FLOW_BUDGET_US / 2 ? "flow-warm" : "";
}
export const flowClass = (r) => ["flow-node", flowHeat(r.own ?? r.us), r.bottleneck ? "flow-bottleneck" : "",
    r.severity ? "flow-" + r.severity : "", r.enabled === false ? "flow-off" : ""].filter(Boolean).join(" ");

const fmtUs = (us) => us < 1000 ? us + " µs" : (us / 1000).toFixed(1) + " ms";
const fmtShare = (v, of) => of ? (100 * v / of).toFixed(v * 1000 < of ? 2 : 1) + "%" : "";
// A node's time: its own, and with its children's too when they differ ("12 µs / 452 µs").
export const flowTime = (r) => {
    const own = r.own ?? r.us ?? 0, sum = r.us ?? 0;
    if (!sum && !own) return "";
    return own === sum ? fmtUs(sum) : `${fmtUs(own)} / ${fmtUs(sum)}`;
};

// A node's chips: a row per memory type it holds, a row per lane it spends time on, absolute or as a share of the type or the frame.
export function chipsOf(r, totals, rel = relative) {
    const mem = [], time = [];
    ["internal", "psram"].forEach((key, type) => {
        if (r[key]) mem.push({ cls: "flow-chip-mem" + type, text: rel ? fmtShare(r[key], totals.memory[type]) : fmtBytes(r[key]) });
    });
    if (r.stackFree !== undefined) mem.push({ cls: "flow-chip-stack", text: "stack " + fmtBytes(r.stackFree) });
    for (const [lane, us] of r.time || []) time.push({ cls: "flow-chip-lane" + lane, text: rel ? fmtShare(us, totals.frame) : fmtUs(us) });
    if (r.idle !== undefined) time.push({ cls: "flow-chip-idle", text: "idle " + (rel ? fmtShare(r.idle, totals.frame) : fmtUs(r.idle)) });
    // A spike: the second's longest tick, shown only when it is past a millisecond and twice the average, so the graph flags a stutter and stays quiet otherwise.
    if (r.worst >= 1000 && r.worst > 2 * (r.us || 0)) time.push({ cls: "flow-chip-spike", text: "▲" + fmtUs(r.worst) });
    // An output near the edge: under a fifth of its headroom left, the margin where a ring or a send starts to glitch.
    if (r.headroom !== undefined && r.headroom < 20) time.push({ cls: "flow-chip-spike", text: "headroom " + r.headroom + "%" });
    return { mem, time };
}

// A node's tooltip: what the module is (its header's one line, when the help is loaded), then its figures.
export const flowTitle = (r, line) => r.type === "core" ? `${r.name}: ${r.count} tasks, ${fmtUs(r.us)} busy and ${fmtUs(r.idle)} idle per frame, holding ${fmtBytes(r.internal || 0)} internal and ${fmtBytes(r.psram || 0)} PSRAM${r.running ? `, running ${r.running} now` : ""}` : `${r.name} (${r.type})${line ? "\n" + line.replace(/`/g, "") : ""}\n`
    + (r.count ? `${fmtUs(r.us || 0)} per frame and its bytes with its ${r.count} modules'` : `${fmtUs(r.own ?? r.us ?? 0)} own, ${fmtUs(r.us || 0)} with children per frame`)
    + (r.worst ? `, longest tick ${fmtUs(r.worst)}` : "")
    + `, ${fmtBytes(r.internal || 0)} internal, ${fmtBytes(r.psram || 0)} PSRAM`
    + (r.type === "task" ? `, ${fmtBytes(r.stackFree || 0)} stack free${r.state ? `\n${r.state}, priority ${r.prio}` : ""}` : `, ${fmtBytes(r.size || 0)} object`)
    + (r.fps !== undefined ? `\nSends ${r.fps} frames a second${r.dropped ? `, ${r.dropped} dropped since start` : ""}` : "")
    + (r.dma ? `\nDMA buffers ${fmtBytes(r.dma)}` : "")
    + (r.headroom !== undefined ? `\nHeadroom ${r.headroom}%, its margin before it glitches` : "")
    + (r.bottleneck ? "\nThe bottleneck: the largest on the busiest thread" : "");

function el(parent, tag, attrs = {}, text) {
    const e = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text !== undefined) e.textContent = text;
    return parent.appendChild(e);
}
function html(parent, tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return parent.appendChild(e);
}

// A part's class: the bar's own color for what modules hold or do, the system tasks' color for their stacks, the network's own, grays for what no owner holds, a wait's color for a wait.
const partClass = (bar, name) => ({ free: "flow-seg-free", idle: "flow-seg-free", unaccounted: "flow-seg-unaccounted", "between modules": "flow-seg-unaccounted",
    waiting: "flow-seg-wait", "module types": "flow-seg-types " + barColor(bar), tasks: "flow-chip-lane3", MoonLight: "flow-seg-types " + barColor(bar), network: "flow-seg-net", system: "flow-seg-sys", "heap overhead": "flow-seg-overhead" }[name] || barColor(bar));
const barColor = (bar) => bar.bar === "memory" ? "flow-chip-mem" + bar.type : "flow-chip-lane" + bar.lane;

// The selector and the legend above the bars.
function drawHead(box, bars, nodes, redraw) {
    const head = html(box, "div", "flow-head");
    for (const [label, rel] of [["Absolute", false], ["Relative", true]]) {
        const b = html(head, "button", "graph-filter", label);
        b.type = "button";
        b.setAttribute("aria-pressed", String(relative === rel));
        b.addEventListener("click", () => { relative = rel; redraw(); });
    }
    const legend = html(head, "div", "flow-legend");
    const key = (cls, text) => { const k = html(legend, "span", "flow-key"); html(k, "i", cls); k.append(text); };
    for (const b of bars) key(barColor(b), b.bar === "memory" ? b.name : LANES[b.lane]);
    if (nodes.some(n => n.type === "task")) key("flow-chip-lane3", LANES[3]);
    if (nodes.some(n => n.bottleneck)) key("flow-key-bottleneck", "bottleneck");
}

// A bar's label: a memory bar's total with its largest free block, which fragmentation keeps under the free part; a time bar's busy time.
export const barLabel = (b, t, idle) => b.bar === "memory"
    ? `${b.name} ${fmtBytes(t)}${b.largest ? `, largest free block ${fmtBytes(b.largest)}` : ""}`
    : `${b.name} ${fmtUs(t - idle)} busy`;

// One bar per memory type and per thread, each part as wide as its share of the bar.
function drawBars(box, bars) {
    const strip = html(box, "div", "flow-strip");
    let framed = false;
    for (const b of bars) {
        const t = total(b);
        // Every time bar adds up to the frame, so the frame is named once, with the second's longest frame beside it, the stutter an average hides.
        if (b.bar === "time" && !framed) {
            framed = true;
            const worst = (bars.find(x => x.worst) || {}).worst;
            html(strip, "div", "flow-frame", `frame ${fmtUs(t)}${worst ? `, longest ${fmtUs(worst)}` : ""}`);
        }
        const row = html(strip, "div", "flow-bar");
        // A time bar's label is its busy time, which differs per thread; the idle part closes the bar.
        const idle = b.bar === "time" ? (b.parts.find(p => p[0] === "idle") || [0, 0])[1] : 0;
        html(row, "span", "flow-bar-label", barLabel(b, t, idle));
        const track = html(row, "div", "flow-bar-track");
        for (const [name, v] of b.parts) {
            if (!v) continue;
            const seg = html(track, "span", "flow-seg " + partClass(b, name));
            const share = t ? 100 * v / t : 0;
            seg.style.width = share + "%";
            const figure = relative ? fmtShare(v, t) : b.bar === "memory" ? fmtBytes(v) : fmtUs(v);
            seg.title = `${name}: ${figure}`;
            if (share >= 12) seg.textContent = `${name} ${figure}`;
        }
    }
}

// A folded node's count, "▸ 3" closed and "▾ 3" open, in its top right corner; a click on it opens or closes the band, a click elsewhere on the node opens its card.
function drawBadge(g, r, p, nodeW, redraw) {
    const text = (r.open ? "▾ " : "▸ ") + r.count, w = 8 + 5 * text.length;
    const badge = el(g, "g", { class: "flow-badge", role: "button", tabindex: 0 });
    el(badge, "title", {}, r.open ? "Close" : `Open its ${r.count} ${r.type === "core" ? "tasks" : "modules"}`);
    el(badge, "rect", { x: p.x + nodeW - w - 3, y: p.y + 3, width: w, height: CHIP_H, rx: 3 });
    el(badge, "text", { x: p.x + nodeW - w / 2 - 3, y: p.y + 11.5 }, text);
    const toggle = (ev) => { ev.stopPropagation(); toggleBand(r.name); redraw(); };
    badge.addEventListener("click", toggle);
    badge.addEventListener("keydown", (ev) => { if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); toggle(ev); } });
}

// A row of chips centered under the node's name, each as wide as its text.
function drawChips(g, chips, cx, y) {
    const widths = chips.map(c => 6 + 4.6 * c.text.length), gap = 3;
    let x = cx - (widths.reduce((s, w) => s + w, 0) + gap * (chips.length - 1)) / 2;
    chips.forEach((c, i) => {
        el(g, "rect", { class: "flow-chip " + c.cls, x, y, width: widths[i], height: CHIP_H, rx: 3 });
        el(g, "text", { class: "flow-chip-text", x: x + widths[i] / 2, y: y + 8.5 }, c.text);
        x += widths[i] + gap;
    });
}

function drawWires(svg, nodes, pos, nodeW) {
    const byName = new Map(nodes.map(r => [r.name, r]));
    const maxBytes = Math.max(2, ...nodes.flatMap(r => (r.edges || []).map(e => e.bytes || 0)));
    for (const r of nodes) {
        if (!r.enabled) continue;
        for (const e of r.edges || []) {
            const to = byName.get(e.to);
            if (!to || !to.enabled || !pos.has(e.to)) continue;
            const a = pos.get(r.name), b = pos.get(e.to);
            const x1 = a.x + nodeW, y1 = a.y + NODE_H / 2, x2 = b.x, y2 = b.y + NODE_H / 2, mx = (x1 + x2) / 2;
            const w = e.bytes > 1 ? 1 + 7 * Math.log(e.bytes) / Math.log(maxBytes) : 1;
            el(svg, "path", { class: "flow-wire" + (e.bytes ? "" : " flow-wire-map"), d: `M${x1},${y1} C${mx},${y1} ${mx},${y2} ${x2},${y2}`, "stroke-width": w.toFixed(1) });
            if (e.bytes) el(svg, "text", { class: "flow-wire-label", x: mx, y: (y1 + y2) / 2 - 3 }, fmtBytes(e.bytes) + "/frame");
        }
    }
}

// Draw the rows into `container`; `opts.open(name)` is what a click on a node does and `opts.summary(type)` the module's one-line help, else an empty string.
export function mountFlow(container, rows, details, opts) {
    const { bars } = splitRows(rows), nodes = bandView(splitRows(rows).nodes);
    const { pos, boxes, nodeW, height } = flowLayout(nodes);
    const totals = barTotals(bars);
    container.replaceChildren();
    const box = html(container, "div", "list-flow-box");
    drawHead(box, bars, nodes, () => mountFlow(container, rows, details, opts));
    drawBars(box, bars);
    const svg = el(box, "svg", { class: "list-flow", viewBox: `0 0 ${FLOW_W} ${Math.max(height, NODE_H + 2 * PAD)}` });
    for (const b of boxes) {
        el(svg, "rect", { class: "flow-group", x: b.x, y: b.y, width: b.w, height: b.h, rx: 8 });
        el(svg, "text", { class: "flow-group-label", x: b.x + 6, y: b.y + LABEL_H - 1 }, b.name);
    }
    drawWires(svg, nodes, pos, nodeW);
    const redraw = () => mountFlow(container, rows, details, opts);
    for (const r of nodes) {
        const p = pos.get(r.key ?? r.name);
        const g = el(svg, "g", { class: flowClass(r), tabindex: 0, role: "button" });
        el(g, "title", {}, flowTitle(r, opts && opts.summary ? opts.summary(r.type) : ""));
        el(g, "rect", { class: "flow-box", x: p.x, y: p.y, width: nodeW, height: NODE_H, rx: 6 });
        el(g, "text", { class: "flow-name", x: p.x + nodeW / 2, y: p.y + 13 }, r.name);
        const { mem, time } = chipsOf(r, totals);
        drawChips(g, mem, p.x + nodeW / 2, p.y + 18);
        drawChips(g, time, p.x + nodeW / 2, p.y + 18 + CHIP_H + 3);
        if (r.count) drawBadge(g, r, p, nodeW, redraw);
        const open = () => opts && opts.open && opts.open(r.name);
        if (r.type === "task" || r.type === "core") continue;   // a system task or a core has no card to open
        g.addEventListener("click", open);
        g.addEventListener("keydown", (ev) => { if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); open(); } });
    }
}

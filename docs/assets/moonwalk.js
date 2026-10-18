// MoonWalk's journey map: every process from moonwalk/*.yaml, drawn from moonwalk.json as a story map, stages (or systems) as columns and each process's steps inside it. The model is docs/work/future/moonwalk.md.
// Two hosts: on the docs site it adds a map button to every page's header and draws the map on the MoonWalk page, through Material's document$ so instant navigation mounts it too; the device's app imports it from the site and calls drawMap with its own state, so a device shows what it has done and what comes next.

const ROOT = new URL("../", import.meta.url);
const MODEL = new URL("moonwalk/moonwalk.json", ROOT);
// Cytoscape.js, the graph library: compound nodes give the three levels, and it never enters the firmware.
const CYTOSCAPE = "https://cdnjs.cloudflare.com/ajax/libs/cytoscape/3.30.4/cytoscape.min.js";
const LS_DONE = "mm_moonwalk_done";   // the processes this reader marked done, which the state cannot show on a website
const LS_VIEW = "mm_moonwalk_view";   // the reader's view of the map: actor, columns and hide done, kept per host
const LS_SPLIT = "mm_moonwalk_split";   // the reader's share for the text beside the map, once they drag it
const LS_INPUTS = "mm_moonwalk_inputs";   // the reader's choices, such as their platform, kept per input name

// The reader's platform, the likely answer to a "which computer" choice, from the browser they read on.
function ownPlatform(options) {
    const ua = (navigator.userAgentData?.platform || navigator.platform || navigator.userAgent || "").toLowerCase();
    const guess = ua.includes("win") ? "windows" : ua.includes("mac") ? "macos" : ua.includes("linux") ? "linux" : "";
    return options.find((o) => o.toLowerCase() === guess);
}
function readInputs() {
    try { return JSON.parse(localStorage.getItem(LS_INPUTS) || "{}"); } catch { return {}; }
}
function keepInput(name, value) {
    try { localStorage.setItem(LS_INPUTS, JSON.stringify({ ...readInputs(), [name]: value })); } catch { /* kept for this view only */ }
}

// A process's choices as the reader sees them: their own pick first, then what the host knows (the installer knows it installs a board), then the reader's operating system.
function chosenOf(p, presets = {}) {
    const chosen = {};
    const inputs = Object.entries(p.inputs?.properties || {});
    const keptAll = inputs.length ? readInputs() : {};   // read once per call, since the map asks for every step on each layout
    for (const [name, spec] of inputs) {
        const options = spec.enum || [];
        const kept = keptAll[name];
        chosen[name] = options.includes(kept) ? kept : options.includes(presets[name]) ? presets[name] : ownPlatform(options) || options[0];
    }
    return chosen;
}

// Whether a step applies to the reader's choices: every `given` naming an input must match its chosen option, one (`equals`) or one of several (`in`).
export const applies = (s, chosen) => (s.given || []).every((g) => !g.input || (g.in ? g.in.includes(chosen[g.input]) : chosen[g.input] === g.equals));
const ACTOR_COLOR = { user: "#2e7d32", contributor: "#1565c0", "product-owner": "#6a1b9a", agent: "#ef6c00", ci: "#546e7a" };   // who does a step, one color on the map and in the text
const ACTORS = Object.keys(ACTOR_COLOR);
const COL = 340, PROC_H = 64, STEP_H = 36, GAP = 28;

// The map's styles, injected once, since the app has no copy of the site's stylesheet. Its colors are the docs theme's variables, which a host that is not the site maps its own colors onto, else fixed ones.
const STYLES = `
.mm-map { --mw-line: var(--md-default-fg-color--lightest, #8884); --mw-bg: var(--md-default-bg-color, transparent); --mw-muted: var(--md-default-fg-color--light, #888); --mw-accent: var(--md-accent-fg-color, #526cfe);
  display: grid; grid-template-columns: minmax(0, 1fr) 8px var(--mw-panel, 20rem); grid-template-rows: auto 75vh; gap: 0.5rem; }
.mm-map-bar { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; font-size: 0.75rem; }
.mm-map-bar input[type="search"] { flex: 1 1 12rem; }
.mm-map-bar input[type="search"], .mm-map-bar select, .mm-map-bar button, .mm-map-chip { border: 1px solid var(--mw-line); background: var(--mw-bg); color: inherit; border-radius: 4px; padding: 4px 8px; font: inherit; }
.mm-map-canvas, .mm-map-panel { border: 1px solid var(--mw-line); border-radius: 6px; min-height: 0; }
.mm-map-panel { overflow: auto; font-size: 0.75rem; padding: 0 0.75rem; }
.mm-map-panel h3 { margin-top: 0.75rem; color: var(--mw-accent); }   /* the process's title, in the accent color */
.mm-map-meta { color: var(--mw-muted); }
.mm-map-chips { flex-basis: 100%; display: flex; flex-wrap: wrap; gap: 4px; }
.mm-map-chip { border-radius: 999px; padding: 2px 10px; cursor: pointer; }
.mm-map-chip.on { border-color: var(--mw-accent); }
.mm-map-next { cursor: pointer; text-decoration: underline; }
.mm-map.mm-map-reading { --mw-panel: 60%; }
.mm-map-split { cursor: col-resize; border-radius: 4px; background: var(--mw-line); touch-action: none; }
.mm-map-split:hover, .mm-map-split:focus-visible { background: var(--mw-accent); outline: none; }
.mm-map-view { line-height: 1.5; }   /* the panel's size, the host's secondary text */
.mm-map-view h4 { margin: 1.2em 0 0.4em; }
.mm-map-view video, .mm-map-view img { max-width: 100%; border-radius: 6px; }
.mm-map-step { margin: 0.8em 0 0.3em; padding: 0.3em 0.6em; font-weight: bold; border: 2px solid var(--mw-actor, var(--mw-muted)); border-radius: 6px; background: color-mix(in srgb, currentColor 6%, transparent); }   /* the line a step on the map scrolls to, drawn as its box there */
.mm-map-off { opacity: 0.45; }
.mm-map-call { display: flex; gap: 0.5em; align-items: center; margin: 0.2em 0 0.4em; }
.mm-map-call code { flex: 1; min-width: 0; overflow-x: auto; white-space: nowrap; font-size: 0.9em; padding: 2px 6px; border-radius: 4px; background: var(--mw-line); }
.mm-map-note { font-weight: normal; color: var(--mw-muted); font-size: 0.9em; }
.mm-map-show { margin-left: 0.5em; font-weight: normal; border: 1px solid var(--mw-line); background: var(--mw-bg); color: inherit; border-radius: 4px; padding: 1px 8px; font-size: 0.75rem; cursor: pointer; }
.mm-walk-bar { --mw-line: var(--md-default-fg-color--lightest, #8884); --mw-accent: var(--md-accent-fg-color, #526cfe);
  position: fixed; left: 50%; bottom: 1rem; transform: translateX(-50%); z-index: 1000; display: flex; gap: 0.5rem; align-items: center; max-width: calc(100vw - 2rem);
  padding: 0.5rem 0.75rem; border: 1px solid var(--mw-accent); border-radius: 8px; background: var(--md-default-bg-color, #fff); color: var(--md-default-fg-color, #000); box-shadow: 0 4px 16px #0004; font-size: 0.85rem; }
.mm-walk-bar.mm-walk-anchored { bottom: auto; transform: none; }
.mm-walk-bar .mm-walk-say { cursor: pointer; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mm-walk-bar button { border: 1px solid var(--mw-line); background: transparent; color: inherit; border-radius: 4px; padding: 2px 8px; font: inherit; cursor: pointer; }
@media (max-width: 760px) { .mm-map { grid-template-columns: 1fr; grid-template-rows: auto 60vh auto; } .mm-map-split { display: none; } }`;

function ensureStyles() {
    if (document.getElementById("mm-map-styles")) return;
    const el = document.createElement("style");
    el.id = "mm-map-styles";
    el.textContent = STYLES;
    document.head.appendChild(el);
}

function readDone() {
    try { return new Set(JSON.parse(localStorage.getItem(LS_DONE) || "[]")); } catch { return new Set(); }
}
function writeDone(done) {
    try { localStorage.setItem(LS_DONE, JSON.stringify([...done])); } catch { /* a private window keeps marks for the visit only */ }
}

// A host vouching for processes it saw done, such as the installer after an install: kept as marks on this origin, where the map reads them.
export function markDone(ids) {
    const done = readDone();
    for (const id of ids || []) done.add(id);
    writeDone(done);
}

// The page's address under the site root, the key moonwalk.json's byPage index uses.
function here() {
    const path = location.href.split(/[?#]/)[0];
    return path.startsWith(ROOT.href) ? decodeURIComponent(path.slice(ROOT.href.length)) || "index.html" : "";
}

function mapButton() {
    const header = document.querySelector(".md-header__inner");
    if (!header) return;
    let a = header.querySelector(".mm-map-link");
    if (!a) {
        a = document.createElement("a");
        a.className = "md-header__button mm-map-link";
        a.title = "This page on MoonWalk, the journey map";
        a.setAttribute("aria-label", a.title);
        a.textContent = "\u{1F5FA}️";
        header.insertBefore(a, header.querySelector(".md-search") || null);
    }
    a.href = new URL(`moonwalk.html?focus=${encodeURIComponent(here())}`, ROOT).href;
}

function loadScript(src) {
    if (window.cytoscape) return Promise.resolve();
    return new Promise((resolve, reject) => {
        const s = document.createElement("script");
        s.src = src;
        s.onload = resolve;
        s.onerror = () => reject(new Error(`could not load ${src}`));
        document.head.appendChild(s);
    });
}

// The map's columns: the journey's stages, or the architecture's systems, where a process touching several stands in each of their columns.
function columns(model, by) {
    if (by !== "system") return model.stages.map((s) => ({ id: s.id, title: s.title, processes: s.processes }));
    const cols = model.systems.map((name) => ({ id: name, title: name, processes: [] }));
    const none = { id: "none", title: "No system", processes: [] };
    for (const stage of model.stages)
        for (const pid of stage.processes) {
            const named = model.processes[pid].systems || [];
            if (!named.length) none.processes.push(pid);
            for (const name of named) cols.find((c) => c.id === name).processes.push(pid);
        }
    return [...cols, none].filter((c) => c.processes.length);
}

// The story map's grid: a column each, its processes stacked, a process's steps inside it, one below the other. Every node carries its process id, since in the system view one process has several.
// The map's nodes and their places. `keep(pid, step)` leaves a step out, and the steps below move up, so hiding leaves no hole.
function elements(model, by, keep = () => true) {
    const out = [];
    columns(model, by).forEach((column, x) => {
        out.push({ data: { id: `col:${column.id}`, label: column.title, kind: "stage" } });
        let y = 0;
        for (const pid of column.processes) {
            const p = model.processes[pid];
            const steps = (p.steps || []).filter((s) => keep(pid, s));
            const nid = `${column.id}::${pid}`;
            out.push({ data: { id: nid, pid, parent: `col:${column.id}`, label: p.title, kind: "process", actor: p.actor, stepped: steps.length > 0 },
                       position: { x: x * COL, y } });
            steps.forEach((s, k) => {
                out.push({ data: { id: `${nid}/${s.id}`, pid, parent: nid, label: s.say, kind: "step", actor: s.actor },
                           position: { x: x * COL, y: y + (k + 1) * STEP_H } });
                if (k) out.push({ data: { id: `next:${nid}/${k}`, source: `${nid}/${steps[k - 1].id}`, target: `${nid}/${s.id}`, kind: "next" } });
            });
            y += steps.length ? (steps.length + 1) * STEP_H + GAP : PROC_H;
        }
    });
    // Dependencies are the journey's order, so only the stage view draws them.
    if (by !== "system")
        for (const [pid, p] of Object.entries(model.processes))
            for (const dep of p.dependsOn || [])
                out.push({ data: { id: `dep:${dep}>${pid}`, source: `${model.processes[dep].stage}::${dep}`, target: `${p.stage}::${pid}`, kind: "dep" } });
    return out;
}

// A theme color as rgb(), since Material writes hsla() with degrees, which Cytoscape does not parse; the browser resolves it. Probed inside the map, where a host defines the variables.
function themeColor(root, variable, fallback) {
    const probe = document.createElement("span");
    probe.style.color = `var(${variable}, ${fallback})`;
    root.appendChild(probe);
    const rgb = getComputedStyle(probe).color;
    probe.remove();
    return rgb;
}

function style(root) {
    const fg = themeColor(root, "--md-default-fg-color", "#222");
    const bg = themeColor(root, "--md-default-bg-color", "#fff");
    const accent = themeColor(root, "--md-accent-fg-color", "#526cfe");
    return [
        { selector: "node", style: { label: "data(label)", color: fg, "font-size": 12, "text-wrap": "wrap", "text-valign": "center", "text-halign": "center", "min-zoomed-font-size": 7 } },
        { selector: "node[kind='stage']", style: { shape: "round-rectangle", "background-color": fg, "background-opacity": 0.04, "border-width": 1, "border-color": fg, "border-opacity": 0.2, "text-valign": "top", "font-size": 22, "font-weight": "bold", padding: 24, "min-zoomed-font-size": 4 } },
        { selector: "node[kind='process']", style: { shape: "round-rectangle", width: 280, height: 44, "text-max-width": 260, "background-color": bg, "border-width": 3, "border-color": (n) => ACTOR_COLOR[n.data("actor")] || fg } },
        { selector: "node[kind='process'][?stepped]", style: { "text-valign": "top", "font-weight": "bold", padding: 10 } },
        { selector: "node[kind='step']", style: { shape: "round-rectangle", width: 260, height: 28, "text-max-width": 250, "font-size": 10, "background-color": fg, "background-opacity": 0.06, "border-width": 2, "border-color": (n) => ACTOR_COLOR[n.data("actor")] || fg, "min-zoomed-font-size": 9 } },
        { selector: "edge", style: { width: 1.5, "line-color": fg, "line-opacity": 0.35, "curve-style": "bezier", "target-arrow-shape": "triangle", "target-arrow-color": fg, "arrow-scale": 0.8 } },
        { selector: "edge[kind='next']", style: { width: 1, "line-style": "dotted", "target-arrow-shape": "none" } },
        { selector: "node.mm-off, node.mm-skip", style: { opacity: 0.25 } },
        { selector: "node.mm-here", style: { "border-color": accent, "border-width": 3, "background-color": accent, "background-opacity": 0.25 } },
        { selector: ".done", style: { opacity: 0.4 } },
        { selector: ".faded", style: { opacity: 0.08 } },
        { selector: "node:selected", style: { "border-color": accent, "border-width": 5 } },
    ];
}

// A step's expectation against a device's state: a control's value or a module that exists. A step names a module by type, so it holds when any module of that type meets it. An HTTP status or a file the state cannot answer, so those give undefined.
function holds(exp, ofType) {
    const mods = ofType(exp.module) || [];
    if ("exists" in exp) return (mods.length > 0) === exp.exists;
    if (!("control" in exp)) return undefined;
    return mods.some((m) => {
        const c = m.controls?.find((c) => c.name === exp.control);
        if (!c) return false;
        // A list counts as its number of rows, so "a network is known" is `notEquals: 0`; equals and notEquals compare as text, since a document's number and the file's number or string mean the same value.
        const value = Array.isArray(c.value) ? c.value.length : c.value;
        if ("equals" in exp) return String(value) === String(exp.equals);
        if ("notEquals" in exp) return String(value) !== String(exp.notEquals);
        // A range compares as numbers, each bound inclusive and either one open.
        const n = Number(value);
        return !Number.isNaN(n) && !(n < exp.minimum) && !(n > exp.maximum);
    });
}

// A device's modules by type, the key a step names them by.
function typesOf(stateDoc) {
    const byType = new Map();
    const walk = (mods) => { for (const m of mods || []) { if (!byType.has(m.type)) byType.set(m.type, []); byType.get(m.type).push(m); walk(m.children); } };
    walk(stateDoc?.modules);
    return byType;
}

// Whether the state shows a step done: every expectation it can answer holds, and there is at least one; undefined when it can answer none, which only the reader can mark.
export function stepHolds(step, stateDoc, byType = typesOf(stateDoc)) {
    const answers = (step.then || []).map((e) => holds(e, (t) => byType.get(t))).filter((v) => v !== undefined);
    return answers.length ? answers.every(Boolean) : undefined;
}

// Where a step points in the app: the module type its action names, and the control it sets.
export function targetOf(step) {
    const arg = step.when && Object.values(step.when)[0];
    return arg && typeof arg === "object" && arg.module ? { type: arg.module, control: arg.control } : null;
}

// The step a followed process stands at: from `i`, past every step the state shows done.
export function advance(steps, i, stateDoc) {
    const byType = typesOf(stateDoc);   // once per state change, which comes every second
    while (i < steps.length && stepHolds(steps[i], stateDoc, byType) === true) i++;
    return i;
}

// The processes a device's state shows done: every expectation the state can answer holds, and there is at least one. Observed rather than recorded, so a step done by hand, by the installer or through MoonWalk counts the same.
export function doneOnDevice(model, stateDoc) {
    const byType = typesOf(stateDoc);
    const done = new Set();
    for (const [pid, p] of Object.entries(model.processes)) {
        const answers = (p.steps || []).flatMap((s) => s.then || []).map((e) => holds(e, (t) => byType.get(t))).filter((v) => v !== undefined);
        if (answers.length && answers.every(Boolean)) done.add(pid);
    }
    return done;
}

// The processes a contributor's runs show done: every step the change triggers has a pass that still holds, and there is at least one. `steps` is what MoonDeck's /api/gates reports per process, each step triggered or not and fresh or not.
export function doneBySteps(steps) {
    const done = new Set();
    for (const [pid, list] of Object.entries(steps || {})) {
        const needed = list.filter((s) => s.triggered);
        if (needed.length && needed.every((s) => s.fresh)) done.add(pid);
    }
    return done;
}

// A run's duration as a reader takes it in: `42s`, `3m05s`.
export const took = (seconds) => {
    if (seconds == null) return "";
    const s = Math.round(seconds);   // rounded first, so 119.6 reads 2m00s rather than 1m60s
    return s < 60 ? `${s}s` : `${Math.floor(s / 60)}m${String(s % 60).padStart(2, "0")}s`;
};

// A process followed in the host while the user performs it, the onboarding checklist's construct: one bar holding the current step, which the host's state moves on and a tap turns back into the map. The bar floats under the step's target as a coach mark does, above it when there is no room below, and at the bottom of the screen when the step has none in view. The host passes `show(type, control, reveal)`, which returns the target's element and with `reveal` brings it into view first, and `reopen(at)`, which opens the map at `process/step`. Returns `update(stateDoc)`, which the host calls on each state change, and `stop()`.
export function follow(model, pid, stepId, { show, reopen }) {
    ensureStyles();
    const p = model.processes[pid];
    const chosen = readInputs();
    const steps = (p?.steps || []).filter((s) => applies(s, chosen));
    let i = Math.max(0, steps.findIndex((s) => s.id === stepId));
    const bar = control("div", { className: "mm-walk-bar", role: "status" });
    const say = control("span", { className: "mm-walk-say", title: "Open MoonWalk at this step" });
    const next = control("button", { type: "button" });
    const close = control("button", { type: "button", title: "Stop following" }, "\u2715");
    bar.append(say, next, close);
    document.body.append(bar);
    let stopped = false, anchor = null, frame = 0, shownAt = 0;
    // Under the target, else above it, kept on screen; a target out of view, or none, leaves the bar at the bottom.
    const place = () => {
        frame = 0;
        const t = steps[i] && targetOf(steps[i]);
        // The host redrew its cards: find the target again, and show it again while it is new, since opening a page redraws it once more when that page's state arrives.
        if (t && !anchor?.isConnected) anchor = show(t.type, t.control, Date.now() - shownAt < 3000) || null;
        const r = anchor?.getBoundingClientRect();
        const inView = r && r.bottom > 0 && r.top < innerHeight;
        bar.classList.toggle("mm-walk-anchored", Boolean(inView));
        if (!inView) { bar.style.top = bar.style.left = ""; return; }
        const h = bar.offsetHeight, w = bar.offsetWidth;
        bar.style.top = `${r.bottom + 8 + h <= innerHeight ? r.bottom + 8 : Math.max(8, r.top - 8 - h)}px`;
        bar.style.left = `${Math.min(Math.max(8, r.left), innerWidth - w - 8)}px`;
    };
    const later = () => { if (!frame) frame = requestAnimationFrame(place); };
    addEventListener("scroll", later, { capture: true, passive: true });
    addEventListener("resize", later);
    const stop = () => { stopped = true; bar.remove(); removeEventListener("scroll", later, { capture: true }); removeEventListener("resize", later); };
    const render = () => {
        if (i >= steps.length) { say.textContent = `\u2714 ${p.title}: done`; next.hidden = true; return; }
        const s = steps[i];
        say.textContent = `Step ${i + 1} of ${steps.length}: ${s.say}`;
        // A step the state can answer moves on by itself, and Skip passes it; one it cannot is the reader's to mark Done.
        next.hidden = false;
        next.textContent = (s.then || []).some((e) => "module" in e) ? "Skip" : "Done";
    };
    const go = (n) => {
        i = n;
        render();
        const t = steps[i] && targetOf(steps[i]);
        anchor = t ? show(t.type, t.control, true) || null : null;
        shownAt = Date.now();
        later();
    };
    say.addEventListener("click", () => { stop(); reopen(`${pid}/${steps[Math.min(i, steps.length - 1)]?.id || ""}`); });
    next.addEventListener("click", () => go(i + 1));
    close.addEventListener("click", stop);
    go(i);
    return {
        update(stateDoc) {
            if (stopped) return;
            const n = advance(steps, i, stateDoc);
            if (n !== i) go(n);
            else later();   // a redraw can move the target
        },
        stop,
    };
}

// What comes next: the processes not done whose dependencies all are, in the journey's order, the frontier of the dependency graph.
export function nextOf(model, done, actor) {
    return model.stages.flatMap((s) => s.processes).filter((pid) => {
        const p = model.processes[pid];
        return !done.has(pid) && (!actor || p.actor === actor) && (p.dependsOn || []).every((d) => done.has(d));
    });
}

function control(tag, attrs, text) {
    const el = document.createElement(tag);
    Object.assign(el, attrs);
    if (text) el.textContent = text;
    return el;
}

// What the panel adds after a step: who does it when that is not the process's actor, and what it waits on.
function stepNote(s, p) {
    const notes = [];
    if (s.actor && s.actor !== p.actor) notes.push(s.actor);
    for (const g of s.given || []) {
        if (g.input) notes.push(`${g.input}: ${g.equals}`);
        else if (g.changed) notes.push(`changes ${g.changed.join(", ")}`);
    }
    return notes.length ? ` (${notes.join("; ")})` : "";
}

// A step's call as a reader types it: a MoonDeck script with its arguments, or a command; null for a step that runs nothing.
export function commandOf(step) {
    const r = step.when?.run;
    if (!r) return null;
    return r.script ? `uv run ${r.script}${r.args ? ` ${r.args}` : ""}` : r.command || null;
}

// The call as code with a button that copies it. Where the browser keeps the clipboard from an insecure page, as on a device's own http address, the button selects the text for a manual copy.
function callLine(cmd) {
    const row = control("div", { className: "mm-map-call" });
    const code = control("code", {}, cmd);
    const copy = control("button", { type: "button", className: "mm-map-show", title: "Copy the command" }, "Copy");
    copy.addEventListener("click", async () => {
        try {
            await navigator.clipboard.writeText(cmd);
            copy.textContent = "Copied";
            setTimeout(() => { copy.textContent = "Copy"; }, 1500);
        } catch {
            const range = document.createRange();
            range.selectNodeContents(code);
            getSelection().removeAllRanges();
            getSelection().addRange(range);
        }
    });
    row.append(code, copy);
    return row;
}

// What to do for a step: its call to copy, or the manual step's own words.
function howTo(s) {
    const cmd = commandOf(s);
    if (cmd) return callLine(cmd);
    return s.when?.manual ? control("p", { className: "mm-map-note" }, s.when.manual) : null;
}

// Whether a process carries its own prose, so the view shows it whole rather than linking a page: in the map already, or marked `text` and fetched when the view opens.
const hasProse = (p) => Boolean(p.text || p.intro || p.outro || (p.steps || []).some((s) => s.detail));

// A process's prose, its intro, outro and step details, fetched once when its view first opens and merged into the map's process, so the map a page loads stays small.
const texts = new Map();
function withText(pid, p) {
    if (!texts.has(pid)) {
        texts.set(pid, fetch(new URL(`moonwalk/text/${pid}.json`, ROOT)).then((r) => {
            if (!r.ok) throw new Error(`answered ${r.status}`);
            return r.json();
        }).then((t) => {
            Object.assign(p, { intro: t.intro, outro: t.outro });
            for (const s of p.steps || []) if (t.details?.[s.id]) s.detail = t.details[s.id];
            delete p.text;
        }).catch((e) => { texts.delete(pid); throw e; }));   // a failed fetch is tried again on the next open
    }
    return texts.get(pid);
}

// The process view: a process with its prose reads whole in the panel, widened for reading, its intro, its steps in their numbered sections, each with its detail, and its outro; one without prose shows its steps and links its page.
function showPanel(panel, model, pid, done, onDone, host = {}) {
    const p = model.processes[pid];
    if (p?.text) {
        // Drawn once its prose has arrived, unless the reader opened another process meanwhile.
        panel.dataset.opening = pid;
        return withText(pid, p).then(() => { if (panel.dataset.opening === pid) showPanel(panel, model, pid, done, onDone, host); },
            (e) => { if (panel.dataset.opening === pid) panel.replaceChildren(control("p", { className: "mm-map-note" }, `${p.title}: its text could not load (${e.message}).`)); });
    }
    panel.dataset.opening = pid;
    panel.replaceChildren();
    host.root?.classList.toggle("mm-map-reading", Boolean(p && hasProse(p)));
    if (!p) return;
    if (host.back) {
        const back = control("p", { className: "mm-map-next" }, "\u2190 All choices");
        back.addEventListener("click", host.back);
        panel.append(back);
    }
    const meta = [model.stages.find((s) => s.id === p.stage)?.title, p.actor, ...(p.systems || [])].join(" · ");
    panel.append(control("h3", {}, p.title), control("p", { className: "mm-map-meta" }, meta), control("p", {}, p.goal));
    if (hasProse(p)) {
        const view = control("div", { className: "mm-map-view" });
        const md = (text) => { const d = control("div"); d.innerHTML = host.helpMarkdown(text); view.append(d); };
        if (p.intro) md(p.intro);   // the intro first, which says what the choice below is for
        // A choice the process offers, such as the platform, as a row of options above the steps: only the chosen option's steps show.
        const chosen = chosenOf(p, host.choices);
        for (const [name, spec] of Object.entries(p.inputs?.properties || {})) {
            const options = spec.enum || [];
            const row = control("div", { className: "mm-map-chips" });
            row.append(control("span", { className: "mm-map-meta" }, (spec.description || name) + " "));
            for (const o of options) {
                const b = control("button", { type: "button", className: "mm-map-chip" + (o === chosen[name] ? " on" : "") }, o);
                b.addEventListener("click", () => { keepInput(name, o); if (host.hideOff) host.relayout?.(); showPanel(panel, model, pid, done, onDone, host); });
                row.append(b);
            }
            view.append(row);
        }
        host.offChoice?.(p, chosen);   // the map grays out the steps of the options not chosen
        // A step that does not apply, for another choice or not needed for this change, is grayed as on the map, or left out when the reader hides them.
        const steps = p.steps || [];
        const runOf = (s) => host.steps?.[pid]?.find((r) => r.id === s.id);
        const off = steps.map((s) => !applies(s, chosen) || runOf(s)?.triggered === false);
        let current = null;
        const sectionOf = steps.map((s) => (current = s.section || current));
        const live = new Set(sectionOf.filter((sec, i) => sec && !off[i]));
        let chapter = 0, section = null;
        steps.forEach((s, i) => {
            if (off[i] && host.hideOff) return;
            // A heading once per section, numbered by the sections with a step that applies; one whose steps are all grayed is grayed with them.
            if (sectionOf[i] && sectionOf[i] !== section) {
                section = sectionOf[i];
                view.append(live.has(section) ? control("h4", {}, `${++chapter}. ${section}`) : control("h4", { className: "mm-map-off" }, section));
            }
            // A step and its detail are one block, the part of the text that belongs to the step on the map.
            const block = control("div");
            block.dataset.step = s.id;
            if (off[i]) block.classList.add("mm-map-off");
            // The instruction in its box from the map, bordered in its actor's color, what it waits on as a quiet aside after it.
            const line = control("p", { className: "mm-map-step" }, s.say);
            if (ACTOR_COLOR[s.actor]) line.style.setProperty("--mw-actor", ACTOR_COLOR[s.actor]);
            if (stepNote(s, p)) line.append(control("span", { className: "mm-map-note" }, stepNote(s, p)));
            // In a host that can show a card, a step with a target follows from here: the map gives way to the app and the bar.
            // A step the device already shows done is ticked, as a checklist ticks a finished item, and has nothing to show.
            const run = runOf(s);
            if (host.types && stepHolds(s, host.state, host.types) === true) line.append(control("span", { className: "mm-map-meta" }, " \u2714 done"));
            else if (run) {
                // A script step: not needed for this change, passed and still holding, or to run, with how long it took last time.
                if (!run.triggered) line.append(control("span", { className: "mm-map-note" }, " not needed for this change"));
                else if (run.fresh) line.append(control("span", { className: "mm-map-meta" }, " \u2714 passed"));
                else if (run.running) line.append(control("span", { className: "mm-map-meta" }, " running"));
                else if (host.run && run.card) {
                    const b = control("button", { type: "button", className: "mm-map-show", title: run.seconds == null ? "Not measured yet" : `Up to ${took(run.seconds)} in its recent runs${run.slow ? ", slow: running it is a choice" : ""}` }, run.seconds == null ? "Run" : `Run \u00b7 ${run.slow ? "\u{1F40C} " : ""}${took(run.seconds)}`);
                    b.addEventListener("click", () => host.run(run));
                    line.append(b);
                }
            } else if (host.follow && targetOf(s)) {
                const b = control("button", { type: "button", className: "mm-map-show" }, "Show me");
                b.addEventListener("click", () => host.follow(pid, s.id));
                line.append(b);
            }
            block.append(line);
            const how = howTo(s);
            if (how) block.append(how);
            if (s.detail) { const d = control("div"); d.innerHTML = host.helpMarkdown(s.detail); block.append(d); }
            view.append(block);
        });
        if (p.outro) md(p.outro);
        panel.append(view);
    } else {
        if (p.url) {
            const line = control("p");
            line.append(control("a", { href: new URL(p.url, ROOT).href, target: host.newTab ? "_blank" : "", rel: "noopener" }, "Read the page"));
            panel.append(line);
        }
        if (p.steps?.length) {
            const ol = control("ol");
            for (const s of p.steps) {
                const li = control("li", {}, s.say + stepNote(s, p));
                li.dataset.step = s.id;
                const how = howTo(s);
                if (how) li.append(how);
                ol.append(li);
            }
            panel.append(ol);
        }
    }
    host.current = pid;
    host.spy?.(panel);
    // Done on the device is what the state shows, so it has no checkbox to undo; what the state cannot show is the reader's own mark.
    if (host.device?.has(pid)) { panel.append(control("p", { className: "mm-map-done" }, `\u2714 ${host.doneBy}`)); return; }
    const label = control("label", { className: "mm-map-done" });
    const box = control("input", { type: "checkbox", checked: done.has(pid) });
    box.addEventListener("change", () => onDone(pid, box.checked));
    label.append(box, " Done");
    panel.append(label);
}

// What the reader can do from where they are: the processes here (touching the card in view, or the page they came from) and the next ones to do, neither showing what is done. Returns them, so the map can fit to the same set.
export function choicesOf(model, done, actor, here) {
    const open = (pid) => !done.has(pid) && (!actor || model.processes[pid].actor === actor);
    const hereOpen = here.filter(open).slice(0, 6);
    const next = nextOf(model, done, actor).filter((pid) => !hereOpen.includes(pid)).slice(0, 5);
    return { here: hereOpen, next };
}

// The panel before a process is picked: the choices, each a link to its place on the map.
function showChoices(panel, model, choices, onPick) {
    panel.replaceChildren();
    const list = (title, pids, empty) => {
        panel.append(control("h3", {}, title));
        if (!pids.length) { panel.append(control("p", { className: "mm-map-meta" }, empty)); return; }
        const ol = control("ol");
        for (const pid of pids) {
            const li = control("li", { className: "mm-map-next" }, model.processes[pid].title);
            li.addEventListener("click", () => onPick(pid));
            ol.append(li);
        }
        panel.append(ol);
    };
    if (choices.hereAsked) list("Here", choices.here, "Everything here is done.");
    list("Next", choices.next, "Nothing left on this path.");
    panel.append(control("p", { className: "mm-map-meta" }, "Pick one, or click a process on the map, for its goal, its steps and its page."));
}

// The handle between the map and the text: dragged, or moved by the arrow keys, it sets the text's share, kept for this reader. A share rather than pixels, so it holds when the window or the dialog is resized.
function dragSplit(host, split, panel) {
    const share = (px) => {
        const w = host.getBoundingClientRect().width;
        const value = `${Math.round(Math.min(Math.max(px, 160), w - 160) / w * 100)}%`;
        host.style.setProperty("--mw-panel", value);
        try { localStorage.setItem(LS_SPLIT, value); } catch { /* kept for this visit only */ }
    };
    try { const kept = localStorage.getItem(LS_SPLIT); if (kept) host.style.setProperty("--mw-panel", kept); } catch { /* the default share */ }
    split.addEventListener("pointerdown", (down) => {
        split.setPointerCapture(down.pointerId);
        const move = (ev) => share(host.getBoundingClientRect().right - ev.clientX);
        split.addEventListener("pointermove", move);
        split.addEventListener("pointerup", () => split.removeEventListener("pointermove", move), { once: true });
    });
    split.addEventListener("keydown", (ev) => {
        if (ev.key !== "ArrowLeft" && ev.key !== "ArrowRight") return;
        ev.preventDefault();
        const step = host.getBoundingClientRect().width * 0.05;
        share(panel.getBoundingClientRect().width + (ev.key === "ArrowLeft" ? step : -step));
    });
}

// Draw the map into `host`, and return its teardown. A host passes what it knows: `state` (a device's /api/state document), `steps` (MoonDeck's /api/gates per process: each script step triggered, fresh, its seconds and its card), `run` (MoonDeck's: runs one such step), `modules` (the types of the cards in view), `here` (the processes or stages it is about, by id), `open` (a process or `process/step` to open at), `show`, `reopen` and `onFollow` (a host that can show a card: Show me hands it the followed process, see follow), `choices` (a choice's option the host knows, by input name), `done` (processes it vouches for, kept as marks), `actor` (the reader it starts filtered to) and `newTab` (pages open beside the host rather than over it).
export async function drawMap(host, opts = {}) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    host.classList.add("mm-map");
    ensureStyles();
    const bar = control("div", { className: "mm-map-bar" });
    const search = control("input", { type: "search", placeholder: "Search processes, steps, modules" });
    const actor = control("select");
    actor.append(control("option", { value: "" }, "Every actor"), ...ACTORS.map((a) => control("option", { value: a }, a)));
    // The reader's own view comes back as they left it; the host's actor is the first visit's.
    let view = {};
    try { view = JSON.parse(localStorage.getItem(LS_VIEW) || "{}") || {}; } catch { /* the defaults */ }
    const keepView = () => { try { localStorage.setItem(LS_VIEW, JSON.stringify({ actor: actor.value, by: by.value, hide: hide.checked, hideOff: hideOff.checked })); } catch { /* kept for this visit only */ } };
    actor.value = "actor" in view && ["", ...ACTORS].includes(view.actor) ? view.actor : opts.actor || "";
    const hide = control("input", { type: "checkbox" });
    hide.checked = Boolean(view.hide);
    // Steps for another choice, or not needed for this change: grayed in the text as on the map, or left out of the text.
    const hideOff = control("input", { type: "checkbox" });
    hideOff.checked = Boolean(view.hideOff);
    const hideOffLabel = control("label", { title: "Steps for another choice, or not needed for this change" });
    hideOffLabel.append(hideOff, " Hide steps that don't apply");
    const hideLabel = control("label");
    hideLabel.append(hide, " Hide done");
    const by = control("select", { title: "Columns" });
    by.append(control("option", { value: "stage" }, "Columns: stage"), control("option", { value: "system" }, "Columns: system"));
    if (view.by === "system") by.value = "system";
    for (const c of [actor, by, hide, hideOff]) c.addEventListener("change", keepView);
    const all = control("button", { type: "button", title: "Zoom to the processes the filters show" }, "Fit selection");
    const chips = control("div", { className: "mm-map-chips" });
    bar.append(search, actor, by, hideLabel, hideOffLabel, all, chips);
    const canvas = control("div", { className: "mm-map-canvas" });
    const panel = control("aside", { className: "mm-map-panel" });
    const split = control("div", { className: "mm-map-split", tabIndex: 0, title: "Drag to share the space between the map and the text" });
    split.setAttribute("role", "separator");
    split.setAttribute("aria-orientation", "vertical");
    host.append(bar, canvas, split, panel);
    dragSplit(host, split, panel);

    let model;
    try {
        await loadScript(CYTOSCAPE);
        const r = await fetch(MODEL);
        if (!r.ok) throw new Error(`${MODEL.href} answered ${r.status}`);
        model = await r.json();
    } catch (e) {
        canvas.textContent = `The map could not load: ${e.message}`;
        return;
    }
    // The help's renderer, so a process reads as a module's help does; imported here rather than at the top, so a test reads this module's rules without a served site.
    const { helpMarkdown } = await import("./help.js");
    // Done is the device's state and the reader's marks together; the device's part is not the reader's to unmark.
    // What the host can see done: a device's state, or a contributor's passing runs in MoonDeck.
    const device = new Set([...(opts.state ? doneOnDevice(model, opts.state) : []), ...doneBySteps(opts.steps)]);
    if (opts.done?.length) markDone(opts.done);
    const marks = readDone();
    const done = new Set([...marks, ...device]);
    const host_ = { device, newTab: opts.newTab, root: host, helpMarkdown, choices: opts.choices, doneBy: opts.state ? "Done on this device" : "Done: its scripts last passed" };
    host_.state = opts.state;
    host_.hideOff = hideOff.checked;
    hideOff.addEventListener("change", () => { host_.hideOff = hideOff.checked; host_.relayout?.(); if (host_.current) showPanel(panel, model, host_.current, done, mark, host_); });
    host_.steps = opts.steps;
    if (opts.run) host_.run = opts.run;
    host_.types = opts.state ? typesOf(opts.state) : null;   // indexed once, for every step the panel ticks
    if (opts.show) host_.follow = (pid, stepId) => opts.onFollow?.(follow(model, pid, stepId, opts));
    // On the site the open process is the page's address, so a link, a search result or a bookmark opens it; in the app the address is the app's own.
    const remember = (pid) => { if (!opts.state) history.replaceState(null, "", pid ? `#${pid}` : location.pathname + location.search); };
    const picked = new Set();   // the system chips switched on; none means every system
    // A chip for each system some process touches, so none filters to an empty map.
    const used = new Set(Object.values(model.processes).flatMap((p) => p.systems || []));
    for (const name of model.systems.filter((n) => used.has(n))) {
        const chip = control("button", { type: "button", className: "mm-map-chip" }, name);
        chip.addEventListener("click", () => {
            picked.has(name) ? picked.delete(name) : picked.add(name);
            chip.classList.toggle("on", picked.has(name));
            fit(picked.size ? apply() : (apply(), cy.collection()));
        });
        chips.append(chip);
    }
    // A step that does not apply, for the reader's choice or not needed for this change, leaves the layout when the reader hides them.
    const keepStep = (pid, s) => !hideOff.checked || (applies(s, chosenOf(model.processes[pid], opts.choices)) && opts.steps?.[pid]?.find((r) => r.id === s.id)?.triggered !== false);
    const cy = window.cytoscape({ container: canvas, elements: elements(model, by.value, keepStep), style: style(host), layout: { name: "preset" }, minZoom: 0.05, maxZoom: 3, autoungrabify: true });   // the layout is the model's order, so a block taps but never drags
    const nodesOf = (pid) => cy.nodes().filter((n) => n.data("pid") === pid);
    // A drag that starts on a block pans the map, as one on the background does; a tap without moving still opens the block.
    let dragFrom = null, dragged = 0;
    cy.on("tapstart", "node", (ev) => { dragFrom = ev.renderedPosition; dragged = 0; });
    cy.on("tapdrag", (ev) => {
        if (!dragFrom) return;
        const at = ev.renderedPosition;
        dragged += Math.abs(at.x - dragFrom.x) + Math.abs(at.y - dragFrom.y);
        cy.panBy({ x: at.x - dragFrom.x, y: at.y - dragFrom.y });
        dragFrom = at;
    });
    const tapped = () => dragged < 6;   // a pan that ends on a block is no tap on it
    cy.on("tapend", () => { dragFrom = null; });
    // The canvas follows its host, a resized dialog in the app or a narrower window on the site.
    const resize = new ResizeObserver(() => cy.resize());
    resize.observe(canvas);

    function apply() {
        const q = search.value.trim().toLowerCase();
        const visible = cy.collection();
        cy.batch(() => {
            for (const [pid, p] of Object.entries(model.processes)) {
                const nodes = nodesOf(pid);
                const text = [p.title, p.goal, ...(p.systems || []), ...(p.steps || []).map((s) => `${s.say} ${JSON.stringify(s.when)}`)].join(" ").toLowerCase();
                const show = (!actor.value || p.actor === actor.value) && !(hide.checked && done.has(pid)) && (!q || text.includes(q))
                    && (!picked.size || (p.systems || []).some((name) => picked.has(name)));
                nodes.toggleClass("faded", !show).toggleClass("done", done.has(pid));
                if (show) visible.merge(nodes.filter((n) => n.data("kind") === "process"));
            }
        });
        // A script step the change does not need grays out, and one whose pass still holds is done, over its process's own mark.
        cy.nodes("[kind='step']").forEach((n) => {
            const r = opts.steps?.[n.data("pid")]?.find((x) => n.id().endsWith(`/${x.id}`));
            if (r) n.toggleClass("mm-skip", !r.triggered).toggleClass("done", r.fresh);
        });
        return visible;
    }
    // Fit to a set, but never closer than 1.2, and keep the set centered when the cap applies.
    const fit = (eles) => {
        const target = eles.length ? eles : cy.elements();
        cy.fit(target, 40);
        if (cy.zoom() > 1.2) { cy.zoom(1.2); cy.center(target); }
    };

    search.addEventListener("input", () => fit(search.value.trim() ? apply() : (apply(), cy.collection())));
    hide.addEventListener("change", () => apply());
    all.addEventListener("click", () => fit(apply()));   // what the actor, the chips, the search and hide done leave; nothing left fits the whole map
    // The layout again, for the columns, the reader's choices or the steps they hide.
    const relayout = () => {
        cy.elements().remove();
        cy.add(elements(model, by.value, keepStep));
        const shown = apply();
        // The open process stays in view; otherwise what the filters leave, or the whole map.
        const open = host_.current ? nodesOf(host_.current).filter((n) => n.data("kind") === "process") : cy.collection();
        if (open.length) { open.select(); fit(open.union(open.children())); } else fit(picked.size || search.value.trim() ? shown : cy.collection());
    };
    host_.relayout = relayout;
    by.addEventListener("change", relayout);
    // The step being read: the first step block in the text's view lights its node on the map, and the map follows it when it is off screen.
    const reading = (pid, stepId) => {
        cy.nodes(".mm-here").removeClass("mm-here");
        const node = nodesOf(pid).filter((n) => n.data("kind") === "step" && n.id().endsWith(`/${stepId}`));
        node.addClass("mm-here");
        const box = node.boundingBox(), view = cy.extent();
        if (node.length && (box.y1 < view.y1 || box.y2 > view.y2 || box.x1 < view.x1 || box.x2 > view.x2)) cy.animate({ center: { eles: node } }, { duration: 200 });
    };
    host_.spy = (root) => {
        host_.observer?.disconnect();
        const blocks = [...root.querySelectorAll("[data-step]")];
        if (!blocks.length) return;
        const seen = new Set();
        host_.observer = new IntersectionObserver((entries) => {
            for (const e of entries) e.isIntersecting ? seen.add(e.target) : seen.delete(e.target);
            const first = blocks.find((b) => seen.has(b));
            if (first) reading(host_.current, first.dataset.step);
        }, { root, rootMargin: "0px 0px -60% 0px" });
        blocks.forEach((b) => host_.observer.observe(b));
    };
    // A step for an option the reader did not choose grays out on the map, as it is left out of the text.
    host_.offChoice = (p, chosen) => nodesOf(p.id).forEach((n) => {
        const s = n.data("kind") === "step" && (p.steps || []).find((st) => n.id().endsWith(`/${st.id}`));
        n.toggleClass("mm-off", Boolean(s) && !applies(s, chosen));
    });
    // A step tapped on the map opens its process and scrolls the text to the step.
    const toStep = async (pid, stepId) => {
        if (host_.current !== pid) await showPanel(panel, model, pid, done, mark, host_);   // a process's text may still be on its way
        // The text alone scrolls, since scrollIntoView would move the page around the map as well.
        const block = panel.querySelector(`[data-step="${CSS.escape(stepId)}"]`);
        if (block) panel.scrollTo({ top: block.getBoundingClientRect().top - panel.getBoundingClientRect().top + panel.scrollTop, behavior: "smooth" });
    };
    const mark = (id, on) => {
        on ? marks.add(id) : marks.delete(id);
        writeDone(marks);
        on || device.has(id) ? done.add(id) : done.delete(id);
        apply();
    };
    const open = (pid) => {
        remember(pid);
        cy.elements().unselect();
        const target = nodesOf(pid).filter((n) => n.data("kind") === "process");
        target.select();
        showPanel(panel, model, pid, done, mark, host_);
        fit(target.union(target.children()));
    };
    cy.on("tap", "node[kind='process']", (ev) => { if (!tapped()) return; remember(ev.target.data("pid")); showPanel(panel, model, ev.target.data("pid"), done, mark, host_); });
    cy.on("tap", "node[kind='step']", (ev) => { if (!tapped()) return; const pid = ev.target.data("pid"); remember(pid); toStep(pid, ev.target.id().split("/").pop()); });

    // Where the reader is: the processes touching the cards in view, or the page they came from. A link naming one process opens it; anything else opens on the choices.
    const focus = new URLSearchParams(location.search).get("focus") || "";
    const touching = (opts.modules || []).flatMap((m) => (model.byModule[m] || []).map((ref) => ref.split("/")[0]));
    // What the host names comes first: a process by id, or a stage standing for its processes.
    const named = (opts.here || []).flatMap((id) => model.processes[id] ? [id] : model.stages.find((s) => s.id === id)?.processes || []);
    const here = [...new Set(named.length ? named : touching.length ? touching : model.byPage[focus] || [])];
    const choose = () => {
        remember(null);
        host_.observer?.disconnect();
        host_.current = null;
        cy.nodes(".mm-here").removeClass("mm-here");
        host.classList.remove("mm-map-reading");
        const choices = { ...choicesOf(model, done, actor.value, here), hereAsked: here.length > 0 };
        showChoices(panel, model, choices, open);
        const shown = [...choices.here, ...choices.next];
        cy.elements().unselect();
        const target = cy.collection(shown.map((id) => nodesOf(id))).filter((n) => n.data("kind") === "process");
        target.select();
        fit(target.length ? target : cy.collection());
    };
    host_.back = choose;
    // A new actor refilters the map; the choices follow unless a process is open for reading.
    actor.addEventListener("change", () => { apply(); if (!host_.current) choose(); });
    apply();
    // What the host or the address names, a process or one of its steps, opens; anything else opens on the choices.
    // A redirect from a page MoonWalk took over keeps that page's anchor after the process's, so only the first part counts.
    const [asked, step] = (opts.open || decodeURIComponent(location.hash.slice(1)).split("#")[0] || focus).split("/");
    if (model.processes[asked]) { open(asked); if (step) toStep(asked, step); }
    else choose();
    // What a host calls when it closes the map: Cytoscape's window listeners and both observers go with it.
    return () => { cy.destroy(); resize.disconnect(); host_.observer?.disconnect(); };
}

function mount() {
    mapButton();
    const host = document.getElementById("mm-map");
    if (!host) return;
    document.documentElement.classList.add("mm-walk-js");   // the page's plain text gives way to the live view
    drawMap(host);
}

// Mounted where there is a page to mount on; a test imports the module for its rules alone.
if (globalThis.document$) globalThis.document$.subscribe(mount); else if (globalThis.document) mount();

// MoonWalk's journey map: every process from moonwalk/*.yaml, drawn from moonwalk.json as a story map, stages (or systems) as columns and each process's steps inside it. The model is docs/work/future/moonwalk.md.
// Two hosts: on the docs site it adds a map button to every page's header and draws the map on the MoonWalk page, through Material's document$ so instant navigation mounts it too; the device's app imports it from the site and calls drawMap with its own state, so a device shows what it has done and what comes next.

const ROOT = new URL("../", import.meta.url);
const MODEL = new URL("moonwalk/moonwalk.json", ROOT);
// Cytoscape.js, the graph library: compound nodes give the three levels, and it never enters the firmware.
const CYTOSCAPE = "https://cdnjs.cloudflare.com/ajax/libs/cytoscape/3.30.4/cytoscape.min.js";
const LS_DONE = "mm_moonwalk_done";   // the processes this reader marked done, which the state cannot show on a website
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

// Whether a step applies to the reader's choices: every `given` naming an input must match its chosen option.
export const applies = (s, chosen) => (s.given || []).every((g) => !g.input || chosen[g.input] === g.equals);
const ACTORS = ["user", "contributor", "product-owner", "agent", "ci"];
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
.mm-map-panel h3 { margin-top: 0.75rem; }
.mm-map-meta { color: var(--mw-muted); }
.mm-map-chips { flex-basis: 100%; display: flex; flex-wrap: wrap; gap: 4px; }
.mm-map-chip { border-radius: 999px; padding: 2px 10px; cursor: pointer; }
.mm-map-chip.on { border-color: var(--mw-accent); }
.mm-map-next { cursor: pointer; text-decoration: underline; }
.mm-map.mm-map-reading { --mw-panel: 60%; }
.mm-map-split { cursor: col-resize; border-radius: 4px; background: var(--mw-line); touch-action: none; }
.mm-map-split:hover, .mm-map-split:focus-visible { background: var(--mw-accent); outline: none; }
.mm-map-view { font-size: 0.85rem; line-height: 1.5; }
.mm-map-view h4 { margin: 1.2em 0 0.4em; }
.mm-map-view video, .mm-map-view img { max-width: 100%; border-radius: 6px; }
.mm-map-step { margin: 0.8em 0 0.3em; font-weight: bold; }
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
function elements(model, by) {
    const out = [];
    columns(model, by).forEach((column, x) => {
        out.push({ data: { id: `col:${column.id}`, label: column.title, kind: "stage" } });
        let y = 0;
        for (const pid of column.processes) {
            const p = model.processes[pid];
            const steps = p.steps || [];
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
    const actor = { user: "#2e7d32", contributor: "#1565c0", "product-owner": "#6a1b9a", agent: "#ef6c00", ci: "#546e7a" };
    return [
        { selector: "node", style: { label: "data(label)", color: fg, "font-size": 12, "text-wrap": "wrap", "text-valign": "center", "text-halign": "center", "min-zoomed-font-size": 7 } },
        { selector: "node[kind='stage']", style: { shape: "round-rectangle", "background-color": fg, "background-opacity": 0.04, "border-width": 1, "border-color": fg, "border-opacity": 0.2, "text-valign": "top", "font-size": 22, "font-weight": "bold", padding: 24, "min-zoomed-font-size": 4 } },
        { selector: "node[kind='process']", style: { shape: "round-rectangle", width: 280, height: 44, "text-max-width": 260, "background-color": bg, "border-width": 3, "border-color": (n) => actor[n.data("actor")] || fg } },
        { selector: "node[kind='process'][?stepped]", style: { "text-valign": "top", "font-weight": "bold", padding: 10 } },
        { selector: "node[kind='step']", style: { shape: "round-rectangle", width: 260, height: 28, "text-max-width": 250, "font-size": 10, "background-color": fg, "background-opacity": 0.06, "border-width": 2, "border-color": (n) => actor[n.data("actor")] || fg, "min-zoomed-font-size": 9 } },
        { selector: "edge", style: { width: 1.5, "line-color": fg, "line-opacity": 0.35, "curve-style": "bezier", "target-arrow-shape": "triangle", "target-arrow-color": fg, "arrow-scale": 0.8 } },
        { selector: "edge[kind='next']", style: { width: 1, "line-style": "dotted", "target-arrow-shape": "none" } },
        { selector: "node.mm-here", style: { "border-color": accent, "border-width": 3, "background-color": accent, "background-opacity": 0.25 } },
        { selector: ".done", style: { opacity: 0.4 } },
        { selector: ".faded", style: { opacity: 0.08 } },
        { selector: "node:selected", style: { "border-color": accent, "border-width": 5 } },
    ];
}

// A step's expectation against a device's state: a control's value or a module that exists. An HTTP status or a file the state cannot answer, so those give undefined.
function holds(exp, moduleNamed) {
    const m = moduleNamed(exp.module);
    if ("exists" in exp) return Boolean(m) === exp.exists;
    if (!("control" in exp)) return undefined;
    const c = m?.controls?.find((c) => c.name === exp.control);
    if (!c) return false;
    // A list counts as its number of rows, so "a network is known" is `notEquals: 0`; the rest compare as text, since a document's number and the file's number or string mean the same value.
    const value = String(Array.isArray(c.value) ? c.value.length : c.value);
    return "equals" in exp ? value === String(exp.equals) : value !== String(exp.notEquals);
}

// The processes a device's state shows done: every expectation the state can answer holds, and there is at least one. Observed rather than recorded, so a step done by hand, by the installer or by a tour counts the same.
export function doneOnDevice(model, stateDoc) {
    const byName = new Map();
    const walk = (mods) => { for (const m of mods || []) { byName.set(m.name, m); walk(m.children); } };
    walk(stateDoc?.modules);
    const done = new Set();
    for (const [pid, p] of Object.entries(model.processes)) {
        const answers = (p.steps || []).flatMap((s) => s.then || []).map((e) => holds(e, (n) => byName.get(n))).filter((v) => v !== undefined);
        if (answers.length && answers.every(Boolean)) done.add(pid);
    }
    return done;
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

// Whether a process carries its own prose, so the view shows it whole rather than linking a page.
const hasProse = (p) => Boolean(p.intro || p.outro || (p.steps || []).some((s) => s.detail));

// The process view: a process with its prose reads whole in the panel, widened for reading, its intro, its steps in their numbered sections, each with its detail, and its outro; one without prose shows its steps and links its page.
function showPanel(panel, model, pid, done, onDone, host = {}) {
    const p = model.processes[pid];
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
        const chosen = {};
        for (const [name, spec] of Object.entries(p.inputs?.properties || {})) {
            const options = spec.enum || [];
            chosen[name] = options.includes(readInputs()[name]) ? readInputs()[name] : ownPlatform(options) || options[0];
            const row = control("div", { className: "mm-map-chips" });
            row.append(control("span", { className: "mm-map-meta" }, (spec.description || name) + " "));
            for (const o of options) {
                const b = control("button", { type: "button", className: "mm-map-chip" + (o === chosen[name] ? " on" : "") }, o);
                b.addEventListener("click", () => { keepInput(name, o); showPanel(panel, model, pid, done, onDone, host); });
                row.append(b);
            }
            view.append(row);
        }
        let chapter = 0, section = null;
        for (const s of (p.steps || []).filter((s) => applies(s, chosen))) {
            // A heading once per section, numbered by the sections the choice leaves, since each option may open the same one.
            if (s.section && s.section !== section) { section = s.section; view.append(control("h4", {}, `${++chapter}. ${section}`)); }
            // A step and its detail are one block, the part of the text that belongs to the step on the map.
            const block = control("div");
            block.dataset.step = s.id;
            block.append(control("p", { className: "mm-map-step" }, s.say + stepNote(s, p)));
            if (s.detail) { const d = control("div"); d.innerHTML = host.helpMarkdown(s.detail); block.append(d); }
            view.append(block);
        }
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
            for (const s of p.steps) { const li = control("li", {}, s.say + stepNote(s, p)); li.dataset.step = s.id; ol.append(li); }
            panel.append(ol);
        }
    }
    host.current = pid;
    host.spy?.(panel);
    // Done on the device is what the state shows, so it has no checkbox to undo; what the state cannot show is the reader's own mark.
    if (host.device?.has(pid)) { panel.append(control("p", { className: "mm-map-done" }, "\u2714 Done on this device")); return; }
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

// Draw the map into `host`, and return its teardown. The app passes `state` (its /api/state document), `modules` (the names of the cards in view, where the map opens), `actor` (the reader it starts filtered to) and `newTab` (pages open beside the app rather than over it).
export async function drawMap(host, opts = {}) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    host.classList.add("mm-map");
    ensureStyles();
    const bar = control("div", { className: "mm-map-bar" });
    const search = control("input", { type: "search", placeholder: "Search processes, steps, modules" });
    const actor = control("select");
    actor.append(control("option", { value: "" }, "Every actor"), ...ACTORS.map((a) => control("option", { value: a }, a)));
    actor.value = opts.actor || "";
    const hide = control("input", { type: "checkbox" });
    const hideLabel = control("label");
    hideLabel.append(hide, " Hide done");
    const by = control("select", { title: "Columns" });
    by.append(control("option", { value: "stage" }, "Columns: stage"), control("option", { value: "system" }, "Columns: system"));
    const all = control("button", { type: "button" }, "Whole map");
    const chips = control("div", { className: "mm-map-chips" });
    bar.append(search, actor, by, hideLabel, all, chips);
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
        model = await (await fetch(MODEL)).json();
    } catch (e) {
        canvas.textContent = `The map could not load: ${e.message}`;
        return;
    }
    // The help's renderer, so a process reads as a module's help does; imported here rather than at the top, so a test reads this module's rules without a served site.
    const { helpMarkdown } = await import("./help.js");
    // Done is the device's state and the reader's marks together; the device's part is not the reader's to unmark.
    const device = opts.state ? doneOnDevice(model, opts.state) : new Set();
    const marks = readDone();
    const done = new Set([...marks, ...device]);
    const host_ = { device, newTab: opts.newTab, root: host, helpMarkdown };
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
    const cy = window.cytoscape({ container: canvas, elements: elements(model, by.value), style: style(host), layout: { name: "preset" }, minZoom: 0.05, maxZoom: 3 });
    const nodesOf = (pid) => cy.nodes().filter((n) => n.data("pid") === pid);
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
    all.addEventListener("click", () => fit(cy.collection()));
    by.addEventListener("change", () => {
        cy.elements().remove();
        cy.add(elements(model, by.value));
        fit(picked.size || search.value.trim() ? apply() : (apply(), cy.collection()));
    });
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
    // A step tapped on the map opens its process and scrolls the text to the step.
    const toStep = (pid, stepId) => {
        if (host_.current !== pid) showPanel(panel, model, pid, done, mark, host_);
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
    cy.on("tap", "node[kind='process']", (ev) => { remember(ev.target.data("pid")); showPanel(panel, model, ev.target.data("pid"), done, mark, host_); });
    cy.on("tap", "node[kind='step']", (ev) => { const pid = ev.target.data("pid"); remember(pid); toStep(pid, ev.target.id().split("/").pop()); });

    // Where the reader is: the processes touching the cards in view, or the page they came from. A link naming one process opens it; anything else opens on the choices.
    const focus = new URLSearchParams(location.search).get("focus") || "";
    const touching = (opts.modules || []).flatMap((m) => (model.byModule[m] || []).map((ref) => ref.split("/")[0]));
    const here = [...new Set(touching.length ? touching : model.byPage[focus] || [])];
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
    const named = decodeURIComponent(location.hash.slice(1)) || focus;
    if (model.processes[named]) open(named);
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

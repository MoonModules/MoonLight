// MoonWalk's journey map: every process from moonwalk/*.yaml, drawn from moonwalk.json as a story map, stages (or systems) as columns and each process's steps inside it. The model is docs/work/future/moonwalk.md.
// On every page it adds a map button to the header that opens the map at that page; on the map page it draws the map, through Material's document$ so instant navigation mounts it too.

const ROOT = new URL("../", import.meta.url);
const MODEL = new URL("moonwalk/moonwalk.json", ROOT);
// Cytoscape.js, the graph library: compound nodes give the three levels, and it never enters the firmware.
const CYTOSCAPE = "https://cdnjs.cloudflare.com/ajax/libs/cytoscape/3.30.4/cytoscape.min.js";
const LS_DONE = "mm_moonwalk_done";   // the processes this reader marked done, which the state cannot show on a website
const ACTORS = ["user", "contributor", "product-owner", "agent", "ci"];
const COL = 340, PROC_H = 64, STEP_H = 36, GAP = 28;

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

// A theme color as rgb(), since Material writes hsla() with degrees, which Cytoscape does not parse; the browser resolves it.
function themeColor(variable, fallback) {
    const probe = document.createElement("span");
    probe.style.color = `var(${variable}, ${fallback})`;
    document.body.appendChild(probe);
    const rgb = getComputedStyle(probe).color;
    probe.remove();
    return rgb;
}

function style() {
    const fg = themeColor("--md-default-fg-color", "#222");
    const bg = themeColor("--md-default-bg-color", "#fff");
    const accent = themeColor("--md-accent-fg-color", "#526cfe");
    const actor = { user: "#2e7d32", contributor: "#1565c0", "product-owner": "#6a1b9a", agent: "#ef6c00", ci: "#546e7a" };
    return [
        { selector: "node", style: { label: "data(label)", color: fg, "font-size": 12, "text-wrap": "wrap", "text-valign": "center", "text-halign": "center", "min-zoomed-font-size": 7 } },
        { selector: "node[kind='stage']", style: { shape: "round-rectangle", "background-color": fg, "background-opacity": 0.04, "border-width": 1, "border-color": fg, "border-opacity": 0.2, "text-valign": "top", "font-size": 22, "font-weight": "bold", padding: 24, "min-zoomed-font-size": 4 } },
        { selector: "node[kind='process']", style: { shape: "round-rectangle", width: 280, height: 44, "text-max-width": 260, "background-color": bg, "border-width": 3, "border-color": (n) => actor[n.data("actor")] || fg } },
        { selector: "node[kind='process'][?stepped]", style: { "text-valign": "top", "font-weight": "bold", padding: 10 } },
        { selector: "node[kind='step']", style: { shape: "round-rectangle", width: 260, height: 28, "text-max-width": 250, "font-size": 10, "background-color": fg, "background-opacity": 0.06, "border-width": 2, "border-color": (n) => actor[n.data("actor")] || fg, "min-zoomed-font-size": 9 } },
        { selector: "edge", style: { width: 1.5, "line-color": fg, "line-opacity": 0.35, "curve-style": "bezier", "target-arrow-shape": "triangle", "target-arrow-color": fg, "arrow-scale": 0.8 } },
        { selector: "edge[kind='next']", style: { width: 1, "line-style": "dotted", "target-arrow-shape": "none" } },
        { selector: ".done", style: { opacity: 0.4 } },
        { selector: ".faded", style: { opacity: 0.08 } },
        { selector: "node:selected", style: { "border-color": accent, "border-width": 5 } },
    ];
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

// The panel beside the map: the clicked process's goal, its steps, its page, and the done mark.
function showPanel(panel, model, pid, done, onDone) {
    const p = model.processes[pid];
    panel.replaceChildren();
    if (!p) return;
    const meta = [model.stages.find((s) => s.id === p.stage)?.title, p.actor, ...(p.systems || [])].join(" · ");
    panel.append(control("h3", {}, p.title), control("p", { className: "mm-map-meta" }, meta), control("p", {}, p.goal));
    if (p.url) {
        const line = control("p");
        line.append(control("a", { href: new URL(p.url, ROOT).href }, "Read the page"));
        panel.append(line);
    }
    if (p.steps?.length) {
        const ol = control("ol");
        for (const s of p.steps) ol.append(control("li", {}, s.say + stepNote(s, p)));
        panel.append(ol);
    }
    const label = control("label", { className: "mm-map-done" });
    const box = control("input", { type: "checkbox", checked: done.has(pid) });
    box.addEventListener("change", () => onDone(pid, box.checked));
    label.append(box, " Done");
    panel.append(label);
}

async function drawMap(host) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    const bar = control("div", { className: "mm-map-bar" });
    const search = control("input", { type: "search", placeholder: "Search processes, steps, modules" });
    const actor = control("select");
    actor.append(control("option", { value: "" }, "Every actor"), ...ACTORS.map((a) => control("option", { value: a }, a)));
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
    panel.append(control("p", {}, "Click a process for its goal, its steps and its page."));
    host.append(bar, canvas, panel);

    let model;
    try {
        await loadScript(CYTOSCAPE);
        model = await (await fetch(MODEL)).json();
    } catch (e) {
        canvas.textContent = `The map could not load: ${e.message}`;
        return;
    }
    const done = readDone();
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
    const cy = window.cytoscape({ container: canvas, elements: elements(model, by.value), style: style(), layout: { name: "preset" }, minZoom: 0.05, maxZoom: 3 });
    const nodesOf = (pid) => cy.nodes().filter((n) => n.data("pid") === pid);

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
    const fit = (eles) => { cy.fit(eles.length ? eles : cy.elements(), 40); if (cy.zoom() > 1.2) cy.zoom({ level: 1.2, position: eles.length ? eles.position() : undefined }); };

    search.addEventListener("input", () => fit(search.value.trim() ? apply() : (apply(), cy.collection())));
    actor.addEventListener("change", () => fit(apply()));
    hide.addEventListener("change", () => apply());
    all.addEventListener("click", () => fit(cy.collection()));
    by.addEventListener("change", () => {
        cy.elements().remove();
        cy.add(elements(model, by.value));
        fit(picked.size || search.value.trim() ? apply() : (apply(), cy.collection()));
    });
    const mark = (id, on) => { on ? done.add(id) : done.delete(id); writeDone(done); apply(); };
    cy.on("tap", "node[kind='process'], node[kind='step']", (ev) => showPanel(panel, model, ev.target.data("pid"), done, mark));
    apply();

    // Open where the reader came from: a page's processes, or one process by id.
    const focus = new URLSearchParams(location.search).get("focus") || "";
    const ids = model.byPage[focus] || (model.processes[focus] ? [focus] : []);
    const target = cy.collection(ids.map((id) => nodesOf(id))).filter((n) => n.data("kind") === "process");
    if (target.length) {
        target.select();
        showPanel(panel, model, ids[0], done, mark);
        fit(target.union(target.children()));
    } else fit(cy.collection());
}

function mount() {
    mapButton();
    const host = document.getElementById("mm-map");
    if (host) drawMap(host);
}

if (window.document$) window.document$.subscribe(mount); else mount();

// The test page: every unit test and scenario, from the registry the docs build publishes (tests/index.json and a shard per subject), drawn like the card decks and the map.
// The index lists the subjects with their counts; a shard holds the tests about one subject and is fetched when it opens. Text goes in with textContent only.

const ROOT = new URL("../", import.meta.url);
const REPO = "https://github.com/MoonModules/MoonLight/blob/main/";
const KINDS = { unit: "Unit", scenario: "Scenario", live: "Live" };

const STYLE = `
.mm-tests { display: grid; grid-template-columns: minmax(12rem, 18rem) minmax(0, 1fr); gap: 1rem; align-items: start; }
.mm-tests-bar { grid-column: 1 / -1; display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
.mm-tests-bar input[type="search"] { flex: 1 1 14rem; padding: 4px 8px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 4px; background: transparent; color: inherit; font: inherit; }
.mm-tests-chip { padding: 2px 10px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 999px; background: transparent; color: inherit; font: inherit; cursor: pointer; }
.mm-tests-chip[aria-pressed="true"] { border-color: var(--md-accent-fg-color, #526cfe); color: var(--md-accent-fg-color, #526cfe); }
.mm-tests-subjects { max-height: 75vh; overflow: auto; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 6px; }
.mm-tests-subjects summary { padding: 4px 10px; cursor: pointer; font-weight: bold; }
.mm-tests-subjects summary .mm-tests-count { font-weight: normal; }
.mm-tests-subjects ul { margin: 0; padding: 0; list-style: none; }
.mm-tests-subjects li { display: flex; justify-content: space-between; gap: 0.5rem; padding: 3px 10px 3px 24px; cursor: pointer; }
.mm-tests-subjects li:hover, .mm-tests-subjects li[aria-current="true"] { background: color-mix(in srgb, var(--md-accent-fg-color, #526cfe) 15%, transparent); }
.mm-tests-count { color: var(--md-default-fg-color--light, #888); font-size: 0.85em; white-space: nowrap; }
.mm-tests-list article { margin: 0 0 0.8rem; padding: 0.4rem 0.7rem; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-left: 4px solid var(--mw-kind, #888); border-radius: 6px; }
.mm-tests-list article[data-kind="unit"] { --mw-kind: #2f9e44; }
.mm-tests-list article[data-kind="scenario"] { --mw-kind: #1c7ed6; }
.mm-tests-list article[data-kind="live"] { --mw-kind: #e8590c; }
.mm-tests-title { font-weight: bold; }
.mm-tests-meta { color: var(--md-default-fg-color--light, #888); font-size: 0.85em; }
.mm-tests-list p { margin: 0.3rem 0; }
.mm-tests-run { display: flex; gap: 0.5rem; align-items: center; }
.mm-tests-run code { flex: 1; min-width: 0; overflow-x: auto; white-space: nowrap; font-size: 0.85em; }
.mm-tests-list button { padding: 0 8px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 4px; background: transparent; color: inherit; font: inherit; font-size: 0.8em; cursor: pointer; }
.mm-tests-list table { font-size: 0.85em; margin: 0.2rem 0 0.4rem; }
.mm-tests-list ol { margin: 0.3rem 0; }
.mm-tests details { margin: 0; padding: 0; border: 0; box-shadow: none; background: none; }   /* the theme draws a details as an admonition: here it is a plain section */
.mm-tests details > summary { padding: 4px 1.8rem 4px 10px; background: none; }
.mm-tests details > summary::before { display: none; }
.mm-tests-list details { margin: 0.4rem 0; }
.mm-tests-list details > summary { padding-left: 0; }
.mm-tests-text { display: none; }
@media (max-width: 60em) { .mm-tests { grid-template-columns: 1fr; } }
`;

function el(tag, attrs = {}, text) {
    const e = document.createElement(tag);
    Object.assign(e, attrs);
    if (text !== undefined) e.textContent = text;
    return e;
}

const fps = (us) => (us ? `${Math.round(1e6 / us)} fps` : "");
const kb = (b) => (b == null ? "" : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`);
const range = (v, f) => (v ? (v.min === v.max ? f(v.min) : `${f(v.max)} to ${f(v.min)}`) : "");   // a tick shortest is the most fps
const rangeHigh = (v, f) => (v ? (v.min === v.max ? f(v.min) : `${f(v.min)} to ${f(v.max)}`) : "");

// A measured step's numbers, one row per target: the floor its contract holds it to and the range it was observed in.
function perfTable(perf) {
    const table = el("table");
    const head = el("tr");
    for (const h of ["Target", "FPS (contract, observed)", "Heap", "Largest block"]) head.append(el("th", {}, h));
    table.append(head);
    for (const [target, p] of Object.entries(perf)) {
        const c = p.contract || {}, o = p.observed || {};
        const row = el("tr");
        row.append(el("td", {}, target),
            el("td", {}, [c.tick_us ? `${fps(c.tick_us)} floor` : "", range(o.tick_us, fps)].filter(Boolean).join(" / ")),
            el("td", {}, [c.free_heap ? `${kb(c.free_heap)} floor` : "", rangeHigh(o.free_heap, kb)].filter(Boolean).join(" / ")),
            el("td", {}, [c.max_alloc_block ? `${kb(c.max_alloc_block)} floor` : "", rangeHigh(o.max_alloc_block, kb)].filter(Boolean).join(" / ")));
        table.append(row);
    }
    return table;
}

function runLine(run) {
    const cmd = `uv run ${run.script}${run.args ? ` ${run.args}` : ""}`;
    const row = el("div", { className: "mm-tests-run" });
    const code = el("code", {}, cmd);
    const copy = el("button", { type: "button", title: "Copy the command" }, "Copy");
    copy.addEventListener("click", async () => {
        try {
            await navigator.clipboard.writeText(cmd);
            copy.textContent = "Copied";
            setTimeout(() => { copy.textContent = "Copy"; }, 1500);
        } catch {
            const range = document.createRange();   // the clipboard is kept from an insecure page: select the line instead
            range.selectNodeContents(code);
            getSelection().removeAllRanges();
            getSelection().addRange(range);
        }
    });
    row.append(code, copy);
    return row;
}

// A test's last result on the machine that built the page: passed or failed, how long, and whether on the tree as it is now.
function resultLine(run) {
    const p = el("p", { className: "mm-tests-meta" });
    const mark = run.status === "pass" ? "\u2714 passed" : run.status === "fail" ? "\u2716 failed" : "skipped";
    p.append(el("strong", {}, mark), ` in ${run.seconds >= 1 ? `${run.seconds.toFixed(1)} s` : `${Math.round((run.seconds || 0) * 1000)} ms`}`, run.current ? ", on this tree" : ", on an older tree");
    if (run.at) p.append(`, ${run.at.replace("T", " ").replace("Z", " UTC")}`);
    return p;
}

function testCard(id, t, run) {
    const card = el("article");
    card.dataset.kind = t.kind;
    card.id = id;
    card.append(el("div", { className: "mm-tests-title" }, t.title));
    card.append(el("div", { className: "mm-tests-meta" }, `${KINDS[t.kind] || t.kind}, ${t.size}`));
    if (run) card.append(resultLine(run));
    if (t.description) card.append(el("p", {}, t.description));
    const where = el("p", { className: "mm-tests-meta" });
    where.append(el("a", { href: REPO + t.file + (t.line ? `#L${t.line}` : ""), target: "_blank", rel: "noopener" }, t.line ? `${t.file}:${t.line}` : t.file));
    if (t.subjects.length > 1) where.append(` · also about ${t.subjects.slice(1).join(", ")}`);
    card.append(where);
    if (t.sequence?.length) {
        const d = el("details");
        d.append(el("summary", {}, `${t.sequence.length} steps`));
        const ol = el("ol");
        for (const s of t.sequence) {
            const li = el("li");
            li.append(el("strong", {}, s.name), ` (${s.op})`, s.description ? ` ${s.description}` : "");
            if (s.perf) li.append(perfTable(s.perf));
            ol.append(li);
        }
        d.append(ol);
        card.append(d);
    }
    if (t.run) card.append(runLine(t.run));
    return card;
}

export async function drawTests(host) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    if (!document.getElementById("mm-tests-style")) document.head.append(el("style", { id: "mm-tests-style" }, STYLE));
    host.classList.add("mm-tests");
    let index;
    try {
        const r = await fetch(new URL("tests/index.json", ROOT));
        if (!r.ok) throw new Error(`answered ${r.status}`);
        index = await r.json();
    } catch (e) {
        host.textContent = `The tests could not load: ${e.message}`;
        return;
    }
    const runs = await fetch(new URL("tests/runs.json", ROOT)).then((r) => (r.ok ? r.json() : {})).catch(() => ({}));   // none where nothing ran
    document.querySelector(".mm-tests-text")?.remove();   // the plain text is for search and for a reader without JavaScript

    const bar = el("div", { className: "mm-tests-bar" });
    const search = el("input", { type: "search", placeholder: `Search ${Object.keys(index.subjects).length} subjects` });
    bar.append(search);
    const on = new Set();
    for (const [kind, label] of Object.entries(KINDS)) {
        const chip = el("button", { type: "button", className: "mm-tests-chip" }, label);
        chip.setAttribute("aria-pressed", "false");
        chip.addEventListener("click", () => { on.has(kind) ? on.delete(kind) : on.add(kind); chip.setAttribute("aria-pressed", String(on.has(kind))); drawSubjects(); if (current) show(current); });
        bar.append(chip);
    }
    // What no test covers, from gaps.json: fetched when the reader asks, drawn in place of a subject's tests.
    const gapsButton = el("button", { type: "button", className: "mm-tests-chip", title: "What no test covers" }, "Gaps");
    gapsButton.addEventListener("click", () => showGaps());
    bar.append(gapsButton);
    const subjects = el("div", { className: "mm-tests-subjects" });
    const list = el("div", { className: "mm-tests-list" });
    host.replaceChildren(bar, subjects, list);

    let current = "";
    const opened = new Set();   // the groups the reader opened, kept across a redraw
    const counts = (kinds) => Object.entries(kinds).filter(([k]) => !on.size || on.has(k)).reduce((n, [, v]) => n + v, 0);
    // The subjects by group, a group a closed section until the reader opens it, or the search or the open subject needs it.
    function drawSubjects() {
        const q = search.value.trim().toLowerCase();
        const groups = new Map();
        for (const [name, s] of Object.entries(index.subjects)) {
            if ((q && !name.toLowerCase().includes(q)) || !counts(s.kinds)) continue;
            if (!groups.has(s.group)) groups.set(s.group, []);
            groups.get(s.group).push(name);
        }
        subjects.replaceChildren();
        for (const group of [...groups.keys()].sort()) {
            const names = groups.get(group);
            const d = el("details");
            d.open = Boolean(q) || opened.has(group) || names.includes(current);
            d.addEventListener("toggle", () => { d.open ? opened.add(group) : opened.delete(group); });
            const total = names.reduce((n, s) => n + counts(index.subjects[s].kinds), 0);
            const summary = el("summary");
            summary.append(el("span", {}, group), " ", el("span", { className: "mm-tests-count" }, `${names.length} subjects, ${total} tests`));
            d.append(summary);
            const ul = el("ul");
            for (const name of names) {
                const li = el("li");
                li.setAttribute("aria-current", String(name === current));
                li.append(el("span", {}, name), el("span", { className: "mm-tests-count" }, String(counts(index.subjects[name].kinds))));
                li.addEventListener("click", () => show(name));
                ul.append(li);
            }
            d.append(ul);
            subjects.append(d);
        }
    }
    async function show(name, testId = "") {
        current = name;
        history.replaceState(null, "", `#${name.toLowerCase()}${testId ? `:${testId}` : ""}`);   // the slug the help links to, as the page headings give it
        drawSubjects();
        list.replaceChildren(el("p", {}, `Loading ${name}…`));
        try {
            const r = await fetch(new URL(`tests/${index.files[name]}`, ROOT));
            if (!r.ok) throw new Error(`answered ${r.status}`);
            const shard = await r.json();
            if (current !== name) return;   // another subject was opened meanwhile
            list.replaceChildren(el("h2", {}, name));
            for (const [id, t] of Object.entries(shard.tests)) if (!on.size || on.has(t.kind)) list.append(testCard(id, t, runs[id]));
            const target = testId && [...list.querySelectorAll("article")].find((a) => a.id === testId);
            if (target) { target.scrollIntoView({ block: "start" }); target.style.outline = "2px solid var(--md-accent-fg-color, #526cfe)"; }
        } catch (e) {
            list.replaceChildren(el("p", {}, `${name}: its tests could not load (${e.message}).`));
        }
    }
    async function showGaps() {
        current = "";
        history.replaceState(null, "", "#gaps");
        drawSubjects();
        list.replaceChildren(el("p", {}, "Loading the gaps…"));
        try {
            const r = await fetch(new URL("tests/gaps.json", ROOT));
            if (!r.ok) throw new Error(`answered ${r.status}`);
            const g = await r.json();
            const section = (title, note, items) => {
                list.append(el("h2", {}, `${title} (${items.length})`), el("p", { className: "mm-tests-meta" }, note));
                const ul = el("ul");
                for (const x of items) ul.append(x);
                list.append(ul);
            };
            list.replaceChildren();
            section("Steps no test proves", "A step that changes the device and that no test names or shares a control with. Open one to see its process.",
                g.steps.map((s) => { const li = el("li"); li.append(el("a", { href: new URL(`moonwalk.html#${s.step}`, ROOT).href }, s.step), ` (${s.verb}) ${s.say}`); return li; }));
            section("Modules no test is about", "A module type the help knows, that no test names as its subject or in an also.", g.modules.map((m) => el("li", {}, m)));
            section("Controls a scenario sets that the help does not know", "Type.control pairs a scenario sets or expects, with no entry in the help: a missing help line, or a name that moved.", g.unknownControls.map((c) => el("li", {}, c)));
        } catch (e) {
            list.replaceChildren(el("p", {}, `The gaps could not load (${e.message}).`));
        }
    }
    search.addEventListener("input", drawSubjects);
    drawSubjects();
    // #subject opens a subject, and #subject:kind:name that test in it, as the help's control links address it; #gaps what no test covers.
    const [asked, ...rest] = decodeURIComponent(location.hash.slice(1)).split(":");
    if (asked === "gaps") showGaps();
    const named = Object.keys(index.files).find((s) => s.toLowerCase() === asked.toLowerCase());
    if (named) show(named, rest.join(":"));
}

const mount = document.getElementById("mm-tests");
if (mount) drawTests(mount);

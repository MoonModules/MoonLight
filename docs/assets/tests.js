// The test page: every test, unit, scenario, Python, JS and check, from the registry the docs build publishes (tests/index.json and a shard per subject), drawn like the card decks and the map.
// The index lists the subjects with their counts; a shard holds the tests about one subject and is fetched when it opens. Text goes in with textContent only.

const ROOT = new URL("../", import.meta.url);
let REPO = "https://github.com/MoonModules/MoonLight/blob/main/";   // the index names the branch the page was built from
const KINDS = { unit: "Unit", scenario: "Scenario", live: "Live", python: "Python", js: "JS", check: "Check" };

const STYLE = `
.mm-tests { display: block; }
.mm-tests-bar { display: flex; margin-bottom: 0.8rem; flex-wrap: wrap; gap: 0.5rem; align-items: center; }
.mm-tests-bar input[type="search"] { flex: 1 1 14rem; padding: 4px 8px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 4px; background: transparent; color: inherit; font: inherit; }
.mm-tests-chip { padding: 2px 10px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 999px; background: transparent; color: inherit; font: inherit; cursor: pointer; }
.mm-tests-chip[aria-pressed="true"] { border-color: var(--md-accent-fg-color, #526cfe); color: var(--md-accent-fg-color, #526cfe); }
.mm-tests-subjects { border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 6px; padding: 0.3rem 0.4rem; }
.mm-tests .mm-tests-subjects details, .mm-tests .mm-tests-subjects ul, .mm-tests .mm-tests-subjects li { margin: 0; padding: 0; }
.mm-tests .mm-tests-subjects summary, .mm-tests .mm-tests-subjects li:not(.mm-tests-detail) { display: flex; align-items: baseline; gap: 0.5rem; padding: 0.22rem 0.4rem; line-height: 1.4; list-style: none; cursor: pointer; }
.mm-tests-subjects summary::-webkit-details-marker { display: none; }
.mm-tests-subjects summary::after { display: none; }
.mm-tests details { border: 0 !important; box-shadow: none !important; background: none !important; }   /* the theme draws a details as an admonition: here it is a plain tree node */
.mm-tests .mm-tests-subjects summary { background: none !important; }
.mm-tests-subjects summary::before { all: unset; content: "\\25B8"; display: inline-block; flex: none; width: 1em; color: var(--md-default-fg-color--light, #888); }   /* the disclosure triangle, at the left of the line it opens */
.mm-tests-subjects details[open] > summary::before { content: "\\25BE"; }
.mm-tests-subjects summary { font-weight: bold; }
.mm-tests-subject { margin-left: 1.3rem !important; }   /* one indent per level: group, subject, test */
.mm-tests-subject > summary { font-weight: normal; }
.mm-tests-subject ul { margin-left: 1.3rem !important; list-style: none; }
.mm-tests-file { margin-left: 1.3rem !important; }
.mm-tests-file > summary { font-weight: normal; }
.mm-tests-subjects li:not(.mm-tests-detail):hover, .mm-tests-subjects li[aria-current="true"] { background: color-mix(in srgb, var(--md-accent-fg-color, #526cfe) 15%, transparent); }
.mm-tests-detail { display: block; padding: 0.2rem 0 0.7rem 1.4rem !important; cursor: default; }
.mm-tests-gaps { margin-bottom: 0.8rem; }
.mm-tests-count { color: var(--md-default-fg-color--light, #888); font-size: 0.85em; white-space: nowrap; }
.mm-tests-badges { display: inline-flex; gap: 0.3rem; }
.mm-tests-badge { font-size: 0.8em; padding: 0 6px; border-radius: 999px; white-space: nowrap; }
.mm-tests-badge.bad { background: #c92a2a; color: #fff; }
.mm-tests-badge.warn { background: #e8590c; color: #fff; }
.mm-tests-badge.ok { background: #2f9e44; color: #fff; }
.mm-tests-badge.none { background: #868e96; color: #fff; }
.mm-tests-mark { flex: none; width: 1.2em; text-align: center; font-size: 0.85em; }
.mm-tests-mark.ok { color: #2f9e44; } .mm-tests-mark.bad { color: #c92a2a; } .mm-tests-mark.none { color: #868e96; }
.mm-tests-name { min-width: 0; overflow-wrap: anywhere; }
.mm-tests-list article { max-height: 22rem; overflow: auto; margin: 0 0 0.8rem; padding: 0.4rem 0.7rem; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-left: 4px solid var(--mw-kind, #888); border-radius: 6px; }
.mm-tests-list article[data-kind="unit"] { --mw-kind: #2f9e44; }
.mm-tests-list article[data-kind="scenario"] { --mw-kind: #1c7ed6; }
.mm-tests-list article[data-kind="live"] { --mw-kind: #e8590c; }
.mm-tests-list article[data-kind="python"] { --mw-kind: #f59f00; }
.mm-tests-list article[data-kind="js"] { --mw-kind: #fab005; }
.mm-tests-list article[data-kind="check"] { --mw-kind: #7048e8; }
.mm-tests-title { font-weight: bold; }
.mm-tests-meta { color: var(--md-default-fg-color--light, #888); font-size: 0.85em; }
.mm-tests-list p { margin: 0.3rem 0; }
.mm-tests-list p.mm-tests-clamp { display: -webkit-box; -webkit-line-clamp: 3; -webkit-box-orient: vertical; overflow: hidden; cursor: pointer; }   /* a long description shows three lines, a click shows all */
.mm-tests-run { display: flex; gap: 0.5rem; align-items: center; }
.mm-tests-run code { flex: 1; min-width: 0; overflow-x: auto; white-space: nowrap; font-size: 0.85em; }
.mm-tests-list button { padding: 0 8px; border: 1px solid var(--md-default-fg-color--lightest, #8884); border-radius: 4px; background: transparent; color: inherit; font: inherit; font-size: 0.8em; cursor: pointer; }
.mm-tests-list table { font-size: 0.85em; margin: 0.2rem 0 0.4rem; }
.mm-tests-list ol { margin: 0.3rem 0; }
.mm-tests-list details { margin: 0.4rem 0; }
.mm-tests-text { display: none; }
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

// A check's rules with what each last found on the machine that built the page, and every file it found something in.
function findingsBlock(f) {
    const d = el("details");
    d.append(el("summary", {}, `${f.total} findings under ${Object.values(f.rules).filter((r) => r.count).length} of ${Object.keys(f.rules).length} rules`));
    const table = el("table");
    const head = el("tr");
    for (const h of ["Rule", "Findings", "What it says"]) head.append(el("th", {}, h));
    table.append(head);
    for (const [rule, r] of Object.entries(f.rules).sort((a, b) => b[1].count - a[1].count)) {
        const row = el("tr");
        const what = el("td", {}, r.reason || "");
        if (r.fix) what.append(el("br"), el("em", {}, `Move: ${r.fix}`));   // the catalog move that solves a finding, kept with its rule
        if (r.standard) what.append(" ", el("a", { href: REPO + r.standard, target: "_blank", rel: "noopener" }, "the standard"));
        row.append(el("td", {}, rule), el("td", {}, String(r.count)), what);
        table.append(row);
    }
    d.append(table);
    if (f.committed) d.append(el("p", { className: "mm-tests-meta" }, "The counts committed with the tree. Run the check to see the files."));
    // Every file with its count, most first, and an erroring file's errors in full: an error blocks a commit, so its reader needs to know what to fix.
    if (f.files.length) {
        const files = el("details");
        files.append(el("summary", {}, `${f.files.length} files`));
        const list = el("ul");
        for (const x of f.files) {
            const item = el("li");
            item.append(el("code", {}, x.file), ` ${x.count}`);
            if (x.errors) {
                const errs = el("ul");
                errs.append(...x.errors.map((e) => el("li", {}, `${e.line ? `line ${e.line}: ` : ""}${e.message}`)));
                item.append(errs);
            }
            list.append(item);
        }
        files.append(list);
        d.append(files);
    }
    return d;
}

// Where something is wrong on a subject: failed tests, and the errors and warnings of a check. Only what is wrong shows.
function badges(s) {
    const box = el("span", { className: "mm-tests-badges" });
    const red = (s.failed || 0) + (s.errors || 0);   // one red number: a test that failed and an error a check found are both something to fix
    const parts = [[red, "\u2716", "bad", [s.failed && `${s.failed} failed tests`, s.errors && `${s.errors} errors`].filter(Boolean).join(", ")],
                   [s.warnings, "\u26a0", "warn", `${s.warnings} warnings`], [s.passed, "\u2714", "ok", `${s.passed} tests passed`], [s.norun, "?", "none", `${s.norun} tests with no result on this machine`]];
    for (const [n, mark, cls, title] of parts) if (n) box.append(el("span", { className: `mm-tests-badge ${cls}`, title }, `${mark} ${n}`));
    return box;
}

function testCard(id, t, run, findings) {
    const card = el("article");
    card.dataset.kind = t.kind;
    card.id = id;
    card.append(el("div", { className: "mm-tests-title" }, t.title));
    card.append(el("div", { className: "mm-tests-meta" }, `${KINDS[t.kind] || t.kind}, ${t.size}`));
    if (run) card.append(resultLine(run));
    if (t.kind === "check" && findings) card.append(findingsBlock(findings));
    if (t.description) {
        const p = el("p", t.description.length > 240 ? { className: "mm-tests-clamp", title: "Click to show all" } : {}, t.description);
        p.addEventListener("click", () => p.classList.toggle("mm-tests-clamp"));
        card.append(p);
    }
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

// A test's mark in the tree: how its last run on this machine ended.
const MARK = { pass: ["\u2714", "ok", "passed"], fail: ["\u2716", "bad", "failed"] };
const markOf = (run, found) => (found?.errors ? ["\u2716", "bad", `found ${found.errors} errors`] : MARK[run?.status]) || ["?", "none", run?.status === "skip" ? "skipped" : "no result on this machine"];   // a check that found errors is wrong, though its run passed

// The page-wide listeners of the mounted page, which the next mount removes: with instant navigation a page is left without a reload.
let leave = null;

export async function drawTests(host) {
    if (host.dataset.mounted) return;
    leave?.abort();
    leave = new AbortController();
    const { signal } = leave;
    host.dataset.mounted = "1";
    if (!document.getElementById("mm-tests-style")) document.head.append(el("style", { id: "mm-tests-style" }, STYLE));
    host.classList.add("mm-tests");
    let index;
    try {
        const r = await fetch(new URL("tests/index.json", ROOT));
        if (!r.ok) throw new Error(`answered ${r.status}`);
        index = await r.json();
        if (index.blob) REPO = index.blob;
    } catch (e) {
        host.textContent = `The tests could not load: ${e.message}`;
        return;
    }
    const findings = await fetch(new URL("tests/findings.json", ROOT)).then((r) => (r.ok ? r.json() : {})).catch(() => ({}));
    const runs = await fetch(new URL("tests/runs.json", ROOT)).then((r) => (r.ok ? r.json() : {})).catch(() => ({}));   // none where nothing ran
    document.querySelector(".mm-tests-text")?.remove();   // the plain text is for search and for a reader without JavaScript

    const bar = el("div", { className: "mm-tests-bar" });
    const search = el("input", { type: "search", placeholder: `Search ${Object.keys(index.subjects).length} subjects and the tests you opened` });
    bar.append(search);
    const kinds = new Set();   // the kind filters
    const state = new Set();   // the state filters: errors, warnings, no result
    const chip = (label, title, set, key) => {
        const b = el("button", { type: "button", className: "mm-tests-chip", title }, label);
        b.setAttribute("aria-pressed", "false");
        b.addEventListener("click", () => { set.has(key) ? set.delete(key) : set.add(key); b.setAttribute("aria-pressed", String(set.has(key))); draw(); });
        return b;
    };
    for (const [key, label, title] of [["bad", "Errors", "Failed tests and check errors"], ["warn", "Warnings", "Subjects a check warned about"], ["none", "No result", "Tests with no result on this machine"]]) bar.append(chip(label, title, state, key));
    for (const [kind, label] of Object.entries(KINDS)) bar.append(chip(label, "", kinds, kind));
    // What no test covers, from gaps.json: fetched when the reader asks, drawn in a card.
    const gapsButton = el("button", { type: "button", className: "mm-tests-chip", title: "What no test covers" }, "Gaps");
    const tree = el("div", { className: "mm-tests-subjects" });
    host.replaceChildren(bar, tree);

    // The detail of a test opens inline, under its row, as in a tree view; a click on the row opens or closes it.
    let pinned = "", pinnedContent = null;
    const gapsPane = el("div", { className: "mm-tests-list mm-tests-gaps" });
    bar.after(gapsPane);
    const shards = new Map();         // subject -> its tests, once fetched
    const openGroups = new Set(), openSubjects = new Set(), openFiles = new Set();   // what the reader opened, kept across a redraw
    // The counts of a list of tests, in the shape a subject's index entry has, so a file's line reads like its subject's.
    const tally = (subject, list) => {
        const n = { failed: 0, errors: 0, warnings: 0, passed: 0, norun: 0 };
        for (const [id, t] of list) {
            const status = runs[id]?.status;
            if (status === "pass") n.passed++; else if (status === "fail") n.failed++; else if (Object.keys(runs).length) n.norun++;
            if (t.kind === "check") { n.errors += findings[subject]?.errors || 0; n.warnings += findings[subject]?.warnings || 0; }
        }
        return n;
    };
    const kindCount = (s) => Object.entries(s.kinds).filter(([k]) => !kinds.size || kinds.has(k)).reduce((n, [, v]) => n + v, 0);

    async function load(subject) {
        if (shards.has(subject)) return shards.get(subject);
        const r = await fetch(new URL(`tests/${index.files[subject]}`, ROOT));
        if (!r.ok) throw new Error(`answered ${r.status}`);
        const tests = (await r.json()).tests;
        shards.set(subject, tests);
        return tests;
    }

    async function showDetail(key, build) {
        try { pinnedContent = await build(); } catch (e) { pinnedContent = el("p", {}, `It could not load (${e.message}).`); }
        pinned = key;
        draw();
    }

    function hideDetail() {
        if (!pinned) return;
        pinned = "";
        pinnedContent = null;
        draw();
    }

    document.addEventListener("keydown", (ev) => { if (ev.key === "Escape") hideDetail(); }, { signal });

    const testBuild = (subject, id) => async () => {
        const t = (await load(subject))[id];
        const box = el("div");
        box.append(el("p", { className: "mm-tests-meta" }, subject), testCard(id, t, runs[id], findings[subject]));
        return box;
    };
    // The tests of an opened subject the filters keep: the kind, and for the state filters a test that failed or has no result.
    function visible(subject) {
        const tests = shards.get(subject) || {};
        return Object.entries(tests).filter(([id, t]) => {
            if (kinds.size && !kinds.has(t.kind)) return false;
            const status = runs[id]?.status;
            const wrong = markOf(runs[id], t.kind === "check" ? findings[subject] : null)[1] === "bad";   // the row's own mark decides, so a filter and the tree agree
            if (state.has("bad") || state.has("none")) return (state.has("bad") && wrong) || (state.has("none") && status !== "pass" && status !== "fail");
            return true;
        });
    }

    function draw() {
        gapsPane.replaceChildren(...(pinned === "gaps" && pinnedContent ? [pinnedContent] : []));
        const q = search.value.trim().toLowerCase();
        const groups = new Map();
        for (const [name, s] of Object.entries(index.subjects)) {
            if (!kindCount(s)) continue;
            const inside = shards.has(name) && Object.values(shards.get(name)).some((t) => q && t.title.toLowerCase().includes(q));
            if (q && !name.toLowerCase().includes(q) && !inside) continue;
            if (state.size) {
                const wrongHere = state.has("bad") && (s.failed || s.errors), warned = state.has("warn") && s.warnings, bare = state.has("none") && s.norun;
                if (!wrongHere && !warned && !bare) continue;
            }
            if (!groups.has(s.group)) groups.set(s.group, []);
            groups.get(s.group).push(name);
        }
        tree.replaceChildren();
        for (const group of [...groups.keys()].sort()) {
            const names = groups.get(group);
            const g = el("details");
            g.open = Boolean(q) || state.size > 0 || openGroups.has(group);
            g.addEventListener("toggle", () => { g.open ? openGroups.add(group) : openGroups.delete(group); });
            const sums = { failed: 0, errors: 0, warnings: 0, passed: 0, norun: 0 };   // summed over the group; badges() makes the one red number
            for (const n of names) for (const k of Object.keys(sums)) sums[k] += index.subjects[n][k] || 0;
            const gs = el("summary");
            gs.append(el("span", {}, group), " ", el("span", { className: "mm-tests-count" }, `${names.length} subjects, ${names.reduce((n, x) => n + kindCount(index.subjects[x]), 0)} tests`), " ", badges(sums));
            g.append(gs);
            for (const name of names) {
                const s = index.subjects[name];
                const sd = el("details", { className: "mm-tests-subject" });
                sd.open = openSubjects.has(name) || (Boolean(q) && shards.has(name));
                sd.addEventListener("toggle", () => {
                    if (sd.open && !openSubjects.has(name)) { openSubjects.add(name); load(name).catch(() => {}).then(draw); }
                    else if (!sd.open) openSubjects.delete(name);
                });
                const ss = el("summary");
                ss.append(el("span", {}, name), s.title ? el("span", { className: "mm-tests-count" }, s.title) : "", badges(s), el("span", { className: "mm-tests-count" }, String(kindCount(s))));
                sd.append(ss);
                // One node per file the subject's tests come from, as a test explorer shows a suite: its badges count what is under it, so every number on a line above can be traced to its tests.
                const files = new Map();
                for (const [id, t] of visible(name)) {
                    if (!files.has(t.file)) files.set(t.file, []);
                    files.get(t.file).push([id, t]);
                }
                for (const [file, list] of files) {
                    const key = `${name}|${file}`;
                    const fd = el("details", { className: "mm-tests-file" });
                    fd.open = files.size === 1 || Boolean(q) || state.size > 0 || openFiles.has(key) || list.some(([id]) => id === pinned);
                    fd.addEventListener("toggle", () => { fd.open ? openFiles.add(key) : openFiles.delete(key); });
                    const fs = el("summary", { title: file });
                    fs.append(el("span", {}, file.split("/").pop()), el("span", { className: "mm-tests-count" }, KINDS[list[0][1].kind] || list[0][1].kind), badges(tally(name, list)), el("span", { className: "mm-tests-count" }, String(list.length)));
                    fd.append(fs);
                    const ul = el("ul");
                    for (const [id, t] of list) {
                        const [mark, cls, why] = markOf(runs[id], t.kind === "check" ? findings[name] : null);
                        const li = el("li");
                        li.dataset.id = id;
                        li.setAttribute("aria-current", String(pinned === id));
                        li.append(el("span", { className: `mm-tests-mark ${cls}`, title: why }, mark), el("span", { className: "mm-tests-name" }, t.title));
                        li.addEventListener("click", () => {
                            if (pinned === id) return hideDetail();   // a second click closes it
                            history.replaceState(null, "", `#${name.toLowerCase()}:${id}`);
                            showDetail(id, testBuild(name, id));
                        });
                        ul.append(li);
                        if (pinned === id && pinnedContent) {
                            const open = el("li", { className: "mm-tests-detail mm-tests-list" });
                            open.append(pinnedContent);
                            ul.append(open);
                        }
                    }
                    fd.append(ul);
                    sd.append(fd);
                }
                if (sd.open && !shards.has(name)) { const ul = el("ul"); ul.append(el("li", {}, "Loading…")); sd.append(ul); }
                g.append(sd);
            }
            tree.append(g);
        }
    }

    async function showGaps() {
        history.replaceState(null, "", "#gaps");
        showDetail("gaps", async () => {
            const r = await fetch(new URL("tests/gaps.json", ROOT));
            if (!r.ok) throw new Error(`answered ${r.status}`);
            const g = await r.json(), box = el("div");
            const section = (title, note, items) => {
                box.append(el("h3", {}, `${title} (${items.length})`), el("p", { className: "mm-tests-meta" }, note));
                const ul = el("ul");
                for (const x of items) ul.append(x);
                box.append(ul);
            };
            section("Steps no test proves", "A step that changes the device and that no test names or shares a control with. Open one to see its process.",
                g.steps.map((s) => { const li = el("li"); li.append(el("a", { href: new URL(`moonwalk.html#${s.step}`, ROOT).href }, s.step), ` (${s.verb}) ${s.say}`); return li; }));
            section("Modules no test is about", "A module type the help knows, that no test names as its subject or in an also.", g.modules.map((m) => el("li", {}, m)));
            section("Controls a scenario sets that the help does not know", "Type.control pairs a scenario sets or expects, with no entry in the help: a missing help line, or a name that moved.", g.unknownControls.map((c) => el("li", {}, c)));
            return box;
        });
    }
    gapsButton.addEventListener("click", () => { if (pinned === "gaps") return hideDetail(); showGaps(); });
    bar.append(gapsButton);

    search.addEventListener("input", draw);
    draw();
    // #subject opens a subject, and #subject:kind:name pins that test's card, as the help's control links address it; #gaps what no test covers.
    async function follow() {
        const [asked, ...rest] = decodeURIComponent(location.hash.slice(1)).split(":");
        if (asked === "gaps") return showGaps();
        const named = Object.keys(index.files).find((s) => s.toLowerCase() === asked.toLowerCase());
        if (!named) return;
        openGroups.add(index.subjects[named].group);
        openSubjects.add(named);
        await load(named).catch(() => {});
        draw();
        const id = rest.join(":");
        const row = id && [...tree.querySelectorAll("li")].find((li) => li.dataset.id === id);
        if (row) { await showDetail(id, testBuild(named, id)); [...tree.querySelectorAll("li")].find((li) => li.dataset.id === id)?.scrollIntoView({ block: "center" }); }
        else (tree.querySelector(".mm-tests-subject[open]") || tree).scrollIntoView({ block: "start" });
    }
    addEventListener("hashchange", () => { if (!location.hash.slice(1).endsWith(pinned) || !pinned) follow(); }, { signal });
    await follow();
}

// Mounted on every page Material shows, since instant navigation swaps the page without loading this module again.
const mount = () => { const host = document.getElementById("mm-tests"); if (host) drawTests(host); };
if (globalThis.document$) globalThis.document$.subscribe(mount); else if (globalThis.document) mount();

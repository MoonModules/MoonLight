// How a module describes itself, its help and its emoji, shared by the app and the docs site's card deck so both show the same; it imports nothing, as the site serves it beside its own scripts.

// Inline markdown as HTML: everything escaped first, so text from the file cannot become markup. Code spans and link targets are set aside before bold and italics, as CommonMark parses them first, so a `*` inside them stays literal.
const emphasis = s => s
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/\*([^*\s][^*]*)\*/g, "<em>$1</em>");
const inline = s => {
    const kept = [];
    const keep = html => `\u0000${kept.push(html) - 1}\u0000`;
    let out = emphasis(s
        .replace(/`([^`]+)`/g, (_, code) => keep(`<code>${code}</code>`))
        .replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, (_, text, url) => keep(`<a href="${url}" target="_blank" rel="noopener">${emphasis(text)}</a>`)));
    // A link's text may hold a code span, so a second pass restores what the first one put back; a NUL in the text itself is no placeholder and stays.
    for (let pass = 0; pass < 2; pass++) out = out.replace(/\u0000(\d+)\u0000/g, (m, i) => kept[i] ?? m);
    return out;
};

// The block forms MoonWalk's prose adds to the help's paragraphs and tables: a list, a heading (one level under the view's own), and a picture or a clip on a paragraph of its own. Only an https address becomes a source.
const isList = p => p.split("\n").every(l => /^- /.test(l));
const list = p => `<ul>${p.split("\n").map(l => `<li>${inline(l.slice(2))}</li>`).join("")}</ul>`;
const HEADING = /^(#{2,4}) (.+)$/;
const MEDIA = /^!\[([^\]]*)\]\((https:\/\/[^)\s"]+)\)$/;
function media(alt, src) {
    return /\.(webm|mp4)$/.test(src)
        ? `<video src="${src}" controls playsinline title="${alt}"></video>`
        : `<img src="${src}" alt="${alt}">`;
}
function block(p) {
    if (isTable(p)) return table(p);
    if (isList(p)) return list(p);
    const h = p.match(HEADING);
    if (h) return `<h${h[1].length + 2}>${inline(h[2])}</h${h[1].length + 2}>`;
    const m = p.match(MEDIA);
    if (m) return media(m[1], m[2]);
    return inline(p);
}

// A GitHub-flavored markdown table, a paragraph whose every line is a `| … |` row: the first row heads it and the `|---|` row below it is dropped.
const isTable = p => p.split("\n").every(l => /^\|.*\|$/.test(l.trim()));
const isSeparator = l => /^\|[\s:|-]+\|$/.test(l.trim());
function table(p) {
    const lines = p.split("\n").filter((l, i) => !(i === 1 && isSeparator(l)));
    const rows = lines.map(l => l.trim().slice(1, -1).split("|").map(c => inline(c.trim())));
    const body = rows.slice(1).map(r => `<tr>${r.map(c => `<td>${c}</td>`).join("")}</tr>`).join("");
    return `<table><tr>${rows[0].map(c => `<th>${c}</th>`).join("")}</tr>${body}</table>`;
}

export function helpMarkdown(text) {
    const esc = String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
    // A fenced code block is lifted out before the text splits into paragraphs, so a blank line inside it stays code; it keeps its text as written, already escaped, with no inline formatting.
    const code = [];
    const held = esc.replace(/\u0000/g, "").trim().replace(/^```[a-z]*\n([\s\S]*?)\n```$/gm, (_, body) => `\u0000${code.push(body) - 1}\u0000`);
    // Trimmed, so the text's last line is a block like any other.
    return held.split(/\n\n+/).map(p => /^\u0000(\d+)\u0000$/.test(p) ? `<pre><code>${code[+p.slice(1, -1)]}</code></pre>` : block(p)).join("<br><br>");
}

// A URL taken from help.json, only when it is https and cannot leave its attribute; anything else is no link at all.
export function safeUrl(u) {
    return typeof u === "string" && /^https:\/\/[^\s"'<>`]+$/.test(u) ? u : null;
}

// A module's page on the docs site, from its registry link (`light/effects.md#fire`); the site has flat URLs, so `.md` becomes `.html`.
export function docUrl(docPath) {
    return "https://moonmodules.org/MoonLight/moonmodules/" + docPath.replace(/\.md(#.*)?$/, ".html$1");
}

// The MoonWalk processes a module or one of its controls takes part in, as links: `index` is moonwalk.json's byModule and byControl with its processes, `page` the MoonWalk page the host knows. A link carries its step as `ref`, `process/step`, for a host that opens MoonWalk in place, and its address opens the page at that step.
export function walkLinks(index, type, control, page) {
    const refs = (control ? index?.byControl?.[`${type}.${control}`] : index?.byModule?.[type]) || [];
    // A module lists each process once, at its first step; a control names the step itself.
    const firsts = new Map();
    for (const ref of refs) { const pid = ref.split("/")[0]; if (!firsts.has(pid)) firsts.set(pid, ref); }
    return [...firsts].filter(([pid]) => index.processes?.[pid])
        .map(([pid, ref]) => ({ label: `MoonWalk: ${index.processes[pid].title}`, href: `${page}#${ref.split("/").map(encodeURIComponent).join("/")}`, ref }));
}

// A walk link as an element, its step on `data-walk`. Its address is the host's own page with the ids encoded after the #, so nothing from the file chooses where it leads.
function walkAnchor(w) {
    const a = document.createElement("a");
    a.href = w.href; a.target = "_blank"; a.rel = "noopener"; a.textContent = w.label; a.dataset.walk = w.ref;
    return a;
}

// A module's help, progressive disclosure: the line and the summary always show, the guide and the rest open on a tap, and the picture loads only then. `walks` are walkLinks' links, listed after the source and the tests.
export function moduleHelp(help, docPath, walks = []) {
    const box = document.createElement("details");
    box.className = "card-helpbox";
    // The line and the summary always show; the arrow opens the guide, the picture, the attribution and the links.
    const line = document.createElement("summary");
    line.innerHTML = helpMarkdown(help.line);
    if (help.summary) {
        const s = document.createElement("div");
        s.className = "help-summary";
        s.innerHTML = helpMarkdown(help.summary);
        line.appendChild(s);
    }
    box.appendChild(line);
    const body = document.createElement("div");
    body.className = "card-helpbox-body";
    const part = (cls, html) => { const d = document.createElement("div"); d.className = cls; d.innerHTML = html; body.appendChild(d); };
    let img = null;
    const image = safeUrl(help.image);
    if (image) { img = document.createElement("img"); img.className = "help-image"; img.alt = help.alt || ""; body.appendChild(img); }
    for (const g of help.guide || []) part("help-guide", `<strong>${helpMarkdown(g.title)}</strong><br>${helpMarkdown(g.text)}`);
    if (help.attribution) part("help-attribution", "Attribution: " + helpMarkdown(help.attribution));
    // Built as elements, each URL through safeUrl, so nothing in the file becomes an attribute of its own.
    const links = document.createElement("div");
    links.className = "help-links";
    for (const [label, url] of [["Docs", docPath ? docUrl(docPath) : null], ["Source", help.source], ["Tests", help.tests]]) {
        const href = safeUrl(url);
        if (!href) continue;
        if (links.childNodes.length) links.append(" · ");
        const a = document.createElement("a");
        a.href = href; a.target = "_blank"; a.rel = "noopener"; a.textContent = label;
        links.appendChild(a);
    }
    for (const a of walks.map(walkAnchor)) {
        if (links.childNodes.length) links.append(" · ");
        links.appendChild(a);
    }
    if (links.childNodes.length) body.appendChild(links);
    box.appendChild(body);
    box.addEventListener("toggle", () => { if (box.open && img && !img.src) img.src = image; });
    return box;
}

// A control's help as Material's helper text, a line under the field rather than a hover tooltip, which fails on touch; any more opens from the line, the MoonWalk steps that set the control with it.
export function controlHelp(row, help, walks = []) {
    const anchors = walks.map(walkAnchor);
    if (!help?.line && !anchors.length) return;
    let el;
    if (!help?.line) {
        el = document.createElement("div");   // no line of its own, but steps that set it: the links alone
        anchors.forEach((a, i) => { if (i) el.append(" · "); el.appendChild(a); });
    } else if (help.more || anchors.length) {
        el = document.createElement("details");
        const s = document.createElement("summary");
        s.innerHTML = helpMarkdown(help.line);
        el.appendChild(s);
        const d = document.createElement("div");
        d.className = "control-helper-more";
        d.innerHTML = help.more ? helpMarkdown(help.more) : "";
        for (const a of anchors) { if (d.childNodes.length) d.append(" · "); d.appendChild(a); }
        el.appendChild(d);
    } else {
        el = document.createElement("div");
        el.innerHTML = helpMarkdown(help.line);
    }
    el.className = "control-helper";
    row.classList.add("has-help");
    row.appendChild(el);
}

// Role and dimension emoji, derived here rather than repeated in every module's tags(); their meaning is documented in docs/tutorials/how-moonlight-works.md, "The emoji on every card".
export const ROLE_EMOJI = {
    effect:     "🔥",
    driver:     "☸️",
    modifier:   "💎",
    layout:     "🚥",
    layer:      "🥞",
    effects:    "🥞",   // the container a preset captures, the same pancake as the Layers it holds
    service:    "🛰️",
    generic:    "⚙️",
};
export const DIM_EMOJI = {
    1: "📏",
    2: "🟦",
    3: "🧊",
};

// Grapheme clusters, so a multi-codepoint emoji such as 🌫️ stays whole.
const _graphemeSeg = (typeof Intl !== "undefined" && Intl.Segmenter)
    ? new Intl.Segmenter(undefined, {granularity: "grapheme"})
    : null;
export function graphemes(s) {
    if (!s) return [];
    if (_graphemeSeg) return [..._graphemeSeg.segment(s)].map(seg => seg.segment);
    return [...s];
}

// A script is marked and a compiled module is not, so the compiled chip matches by absence and costs no module a tag.
export const SCRIPTED_EMOJI = "\u{1F4DD}";      // 📝 the module runs a MoonLive script
export const COMPILED_EMOJI = "\u{1F4E6}";      // 📦 chip only, never a tag; not the gear, which is the generic role's
// What an effect listens to, a chip group of its own.
export const AUDIO_EMOJI = ["\u{1F3B5}", "\u{1F3B6}"];   // volume, frequency
// The power functions an effect is built on, kept last and together on a card and in the chip row.
export const KERNEL_EMOJI = ["🖌️", "✨", "💨", "🌫️", "🎡"];   // the legend's order

// Each chip's meaning as its tooltip, in the legend's words, so a filter chip explains itself.
export const EMOJI_LABEL = {
    // what the module IS
    "\u{1F525}": "effect",
    "☸️": "driver",
    "\u{1F48E}": "modifier",
    "\u{1F6A5}": "layout",
    "\u{1F95E}": "layer",
    "\u{1F6F0}\uFE0F": "service",
    "⚙️": "generic",
    [SCRIPTED_EMOJI]: "runs a MoonLive script",
    [COMPILED_EMOJI]: "compiled into the firmware",
    // the shape it works in
    "\u{1F4CF}": "1D: a line",
    "\u{1F7E6}": "2D: a picture",
    "\u{1F9CA}": "3D: a volume",
    // where it came from
    "\u{1F4AB}": "MoonLight",
    "\u{1F319}": "MoonModules",
    "\u{1F419}": "WLED",
    "⚡️": "FastLED",
    "\u{1F985}": "a named contributor",
    // what it listens to and what it does
    "\u{1F3B5}": "reacts to volume: how loud the room is",
    "\u{1F3B6}": "reacts to frequency: which notes are playing",
    "\u{1F5BC}\uFE0F": "shows a picture made elsewhere: the network, the Picture service or a recorded show",
    "\u{1F3AF}": "aims moving heads",
    "\u{1F47E}": "pixel art: games and sprites",
    "\u{1F9EC}": "a simulation: cells evolving off their own last frame",
    "\u{1F4F9}": "motion-tracking aware",
    // the power functions, shown together at the end of a row
    "\u{1F58C}\uFE0F": "power function: a shader, every pixel computed from its position",
    "✨": "power function: particles, born, moved by forces, and dying",
    "\u{1F4A8}": "power function: transport, light carried and fading rather than redrawn",
    "\u{1F32B}\uFE0F": "power function: a noise field, the cloud and smoke family",
    "\u{1F3A1}": "power function: polar, composed around a center",
};

// A type's emoji: role, then dimension, then its tags(), deduplicated, with the power functions last in a fixed order so two effects on the same kernels show the same run.
export function emojiTagsFor(t) {
    const out = [];
    const kernels = [];
    const seen = new Set();
    const push = (ch) => {
        if (!ch || seen.has(ch)) return;
        seen.add(ch);
        (KERNEL_EMOJI.includes(ch) ? kernels : out).push(ch);
    };
    push(ROLE_EMOJI[t.role]);
    push(DIM_EMOJI[t.dim]);
    for (const ch of graphemes(t.tags || "")) push(ch);
    kernels.sort((a, b) => KERNEL_EMOJI.indexOf(a) - KERNEL_EMOJI.indexOf(b));
    return out.concat(kernels);
}

// A type without its role noun, as ModuleFactory::displayNameFor names it, in the 15 characters a module name holds.
const ROLE_NOUNS = ["Effect", "Modifier", "Layout", "Driver", "Module", "Service"];
export const kNameMax = 15;
export function typeDisplayName(type) {
    const noun = ROLE_NOUNS.find(n => type.length > n.length && type.endsWith(n));
    return (noun ? type.slice(0, -noun.length) : type).slice(0, kNameMax);
}

// The filter chips for the emoji present, grouped as the legend reads (kind, role, dimension, origin, listening, power functions, then anything unclassified, so a new emoji still shows), each with its tooltip from `labels`; `onToggle` gets the active set.
export function emojiChipRow(present, onToggle, labels = EMOJI_LABEL) {
    const CHIP_GROUPS = [
        [SCRIPTED_EMOJI, COMPILED_EMOJI],
        Object.values(ROLE_EMOJI),
        Object.values(DIM_EMOJI),
        ["\u{1F4AB}", "\u{1F319}", "\u{1F419}", "⚡️"],
        AUDIO_EMOJI,
        KERNEL_EMOJI,
    ];
    const groups = CHIP_GROUPS.map(g => present.filter(e => g.includes(e)));
    const classified = new Set(CHIP_GROUPS.flat());
    groups.push(present.filter(e => !classified.has(e)));
    const active = new Set();
    const row = document.createElement("div");
    row.className = "type-picker-chips";
    let first = true;
    for (const group of groups) {
        if (!group.length) continue;
        if (!first) {
            const sep = document.createElement("span");
            sep.className = "type-picker-chip-sep";
            sep.setAttribute("aria-hidden", "true");
            row.appendChild(sep);
        }
        first = false;
        for (const emoji of group) {
            const chip = document.createElement("button");
            chip.type = "button";
            chip.className = "type-picker-chip";
            chip.textContent = emoji;
            const label = labels[emoji];
            if (label) { chip.title = label; chip.setAttribute("aria-label", label); }
            chip.addEventListener("click", () => {
                if (active.has(emoji)) { active.delete(emoji); chip.classList.remove("active"); }
                else { active.add(emoji); chip.classList.add("active"); }
                onToggle(active);
            });
            row.appendChild(chip);
        }
    }
    return row;
}

// Whether a module's emoji carry every active chip; the compiled chip matches what has no scripted marker.
export function chipsMatch(emoji, active) {
    const has = new Set(emoji);
    for (const chip of active) {
        if (chip === COMPILED_EMOJI ? has.has(SCRIPTED_EMOJI) : !has.has(chip)) return false;
    }
    return true;
}

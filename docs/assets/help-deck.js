// The card decks: a catalog's modules drawn from help.json by the app's own help.js, so a card reads as the module's help does on a device, and the attribution catalog drawn from its own table.
// Mounted on every page that holds a `.mm-deck`, through Material's document$ so instant navigation mounts it too. A deck with a `data-page` holds that page's modules; one without holds the table inside it, which stays what search indexes and a reader without JavaScript sees.

import { helpMarkdown, safeUrl, moduleHelp, controlHelp, walkLinks, emojiTagsFor, emojiChipRow, chipsMatch, typeDisplayName, EMOJI_LABEL, graphemes } from "./help.js";

const HELP = new URL("../help/help.json", import.meta.url);
const WALK = new URL("../moonwalk/moonwalk.json", import.meta.url);
const WALK_PAGE = new URL("../moonwalk.html", import.meta.url).href;

// The script is running, so the plain list under each deck can hide until its deck mounts.
document.documentElement.classList.add("mm-deck-js");

// The attribution kinds, in the legend's order, each its chip's tooltip.
const KIND_LABEL = {
    "\u{1F9D1}": "a person",
    "\u{1F3E2}": "a company or shop",
    "\u{1F9F0}": "a project or library",
    "\u{1F4DC}": "a standard or protocol",
    "\u{1F9EE}": "an algorithm",
    "\u{1F3A8}": "an art form or culture",
};

// A playing card: the name and its emoji, a picture and a line on its face, and `back` under a fixed head on the other side, turned with a tap.
function playingCard({id, name, emoji, labels, image, alt, line, back, text}) {
    const el = document.createElement("article");
    el.className = "mm-pcard";
    el.tabIndex = 0;
    el.setAttribute("aria-label", name);
    el.title = name;   // the name on hover, which a small card has no room to print
    if (id) el.id = id;   // a link to the entry lands on its card

    const front = document.createElement("div");
    front.className = "mm-pcard-face mm-pcard-front";
    const pips = document.createElement("div");
    pips.className = "mm-pcard-pips";
    for (const e of emoji) {
        const s = document.createElement("span");
        s.textContent = e;
        if (labels[e]) s.title = labels[e];
        pips.appendChild(s);
    }
    // The name beside the emoji, at their scale, so it reads at every card size.
    const pipName = document.createElement("span");
    pipName.className = "mm-pcard-name";
    pipName.textContent = name;
    pips.appendChild(pipName);
    front.appendChild(pips);
    if (image) {
        const img = document.createElement("img");
        img.loading = "lazy";
        img.src = image;
        img.alt = alt;
        front.appendChild(img);
    } else {
        // No picture: the card's emoji stand in, so every face has the same shape.
        const glyph = document.createElement("div");
        glyph.className = "mm-pcard-glyph";
        glyph.textContent = emoji.join("");
        front.appendChild(glyph);
    }
    const p = document.createElement("p");
    p.innerHTML = line;
    front.appendChild(p);

    // The back: a fixed head, a tiny picture and the title, so a turned card always says what it is, and the rest below it scrolling on its own.
    const backFace = document.createElement("div");
    backFace.className = "mm-pcard-face mm-pcard-back";
    const head = document.createElement("div");
    head.className = "mm-pcard-head";
    if (image) {
        const thumb = document.createElement("img");
        thumb.loading = "lazy";
        thumb.src = image;
        thumb.alt = "";
        head.appendChild(thumb);
    }
    const h = document.createElement("h3");
    h.textContent = name;
    head.appendChild(h);
    const scroll = document.createElement("div");
    scroll.className = "mm-pcard-scroll";
    scroll.append(...back);
    backFace.append(head, scroll);

    const inner = document.createElement("div");
    inner.className = "mm-pcard-inner";
    inner.append(front, backFace);
    el.appendChild(inner);
    // The face turned away is inert, so a keyboard reaches only what shows.
    backFace.inert = true;
    const flip = () => { const b = el.classList.toggle("flipped"); front.inert = b; backFace.inert = !b; };
    // A tap turns the card, unless it lands on something of its own to press, or ends a drag of the deck.
    el.addEventListener("click", (e) => {
        if (el.closest(".mm-deck-row")?.dataset.dragged) return;
        if (!e.target.closest("a, summary")) flip();
    });
    // Only on the card itself: a link or a disclosure on its back handles its own keys.
    el.addEventListener("keydown", (e) => { if (e.target === el && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); flip(); } });
    el._emoji = emoji;
    el._text = text.toLowerCase();
    return el;
}

// A module's card: its help on the back, the line first as plain text rather than the app's disclosure, so a tap on it turns the card rather than folding the help.
function moduleCard(type, help, walk) {
    const name = typeDisplayName(type);
    const line = document.createElement("p");
    line.className = "mm-pcard-line";
    line.innerHTML = helpMarkdown(help.line || "");
    const back = [line];
    const box = moduleHelp({...help, image: null}, help.doc, walkLinks(walk, type, null, WALK_PAGE));
    const summary = box.querySelector(".help-summary");
    if (summary) back.push(summary);
    back.push(box.querySelector(".card-helpbox-body"));
    for (const [ctrl, ch] of Object.entries(help.controls || {})) {
        const row = document.createElement("div");
        row.className = "control-row";
        const label = document.createElement("span");
        label.className = "control-label";
        label.textContent = ctrl;
        row.appendChild(label);
        controlHelp(row, ch, walkLinks(walk, type, ctrl, WALK_PAGE));
        back.push(row);
    }
    return playingCard({
        id: (help.doc || "").split("#")[1], name, emoji: emojiTagsFor(help), labels: EMOJI_LABEL,
        image: safeUrl(help.image), alt: help.alt || `${name} preview`, line: helpMarkdown(help.line || ""),
        back, text: `${type} ${name} ${help.line}`,
    });
}

// An attribution's card from its table row: the build wrote the row, with its picture at thumbnail size, the entry's link on its name, and the modules that name it.
function entryCard(cell) {
    const nameLink = cell.name.querySelector("a:not(:has(img))");
    const name = (nameLink || cell.name).textContent.trim().split("\n")[0];
    // The table shows a 48-pixel thumbnail; the card asks the same host for a larger one.
    const thumb = cell.name.querySelector("img")?.getAttribute("src");
    const image = thumb ? safeUrl(thumb.replace(/([?&](?:width|size)=)96\b/, "$1480")) : null;
    const known = document.createElement("p");
    known.className = "mm-pcard-line";
    known.innerHTML = cell.known.innerHTML;
    const back = [known];
    if (cell.by?.textContent.trim()) {
        const by = document.createElement("p");
        by.innerHTML = `<strong>Named by</strong> ${cell.by.innerHTML}`;
        back.push(by);
    }
    if (nameLink) {
        const visit = document.createElement("p");
        visit.append(nameLink.cloneNode(true));
        back.push(visit);
    }
    return playingCard({
        // The entry's first name as an anchor, so a hand-typed `#jos-stam` brings that card forward.
        id: name.split(" / ")[0].normalize("NFD").replace(/[\u0300-\u036f]/g, "").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, ""),
        name, emoji: graphemes(cell.kind.textContent.trim()), labels: KIND_LABEL,
        image, alt: "", line: cell.known.innerHTML, back,
        text: `${name} ${cell.known.textContent} ${cell.by?.textContent || ""}`,
    });
}

// The rows of a deck's table as cells named by their column's heading, so a column added later moves nothing.
function tableEntries(table) {
    const heads = [...table.querySelectorAll("thead th")].map(th => th.textContent.trim().toLowerCase());
    const col = (label) => heads.indexOf(label);
    return [...table.querySelectorAll("tbody tr")].map(tr => {
        const td = tr.children;
        return {name: td[col("name")], kind: td[col("kind")], known: td[col("known for")], by: td[col("attributed by")]};
    });
}

// The deck spans the content area, between the navigation and the contents columns, so cards never pass over either; the first card starts at the page's text, and the end padding lets the last card reach the middle.
function align(deck, host) {
    if (deck.closest(".mm-deck-spread")) { deck.style.width = deck.style.marginLeft = ""; return; }
    const area = (document.querySelector(".md-content") || document.documentElement).getBoundingClientRect();
    const left = host.getBoundingClientRect().left;
    const pad = Math.max(16, left - area.left);
    deck.style.width = `${Math.min(area.width, document.documentElement.clientWidth)}px`;
    deck.style.marginLeft = `${area.left - left}px`;
    const w = deck.querySelector(".mm-pcard:not([hidden])")?.offsetWidth || 240;
    deck.style.paddingLeft = `${pad}px`;
    deck.style.paddingRight = `${Math.max(pad, (deck.clientWidth - w) / 2)}px`;
}

// The fan, a hand of cards: each card turns about one pivot below the deck, so the cards lie on an arc with the middle one straight, and a card is pulled toward the middle by the square of its distance, so the outer ones overlap more.
function fan(deck) {
    if (deck.closest(".mm-deck-spread")) {
        for (const c of deck.children) { c.style.transform = ""; c.style.zIndex = ""; }
        return;
    }
    const first = deck.querySelector(".mm-pcard:not([hidden])");
    // The arc flattens with the card, so a long row of small cards stays level.
    const amp = Math.min(1, first ? first.offsetWidth / 240 : 1);
    const mid = deck.scrollLeft + deck.clientWidth / 2;
    for (const c of deck.children) {
        if (c.hidden) continue;
        const d = (c.offsetLeft + c.offsetWidth / 2 - mid) / c.offsetWidth;   // in cards from the middle
        const k = Math.max(-4, Math.min(4, d));
        const pull = -Math.sign(k) * k * k * 0.12 * c.offsetWidth;            // overlap growing outward
        c.style.transform = `translateX(${pull * amp}px) rotate(${k * 6 * amp}deg)`;
        c.style.zIndex = String(20 - Math.round(Math.abs(k) * 4));
    }
}

// Dragging the deck with a mouse, as a finger already scrolls it, and letting go glides on and slows down as a flick does on touch; snapping waits for the glide, and a drag past a few pixels is not a tap.
function draggable(deck) {
    let x0 = 0, left0 = 0, down = false, v = 0, lastX = 0, lastT = 0, glide = 0;
    // The browser's own image drag would take the gesture and cancel this one.
    deck.addEventListener("dragstart", (e) => e.preventDefault());
    deck.addEventListener("pointerdown", (e) => {
        if (e.pointerType !== "mouse" || e.button !== 0) return;
        cancelAnimationFrame(glide);
        down = true; x0 = lastX = e.clientX; left0 = deck.scrollLeft; v = 0; lastT = e.timeStamp;
        delete deck.dataset.dragged;
    });
    deck.addEventListener("pointermove", (e) => {
        if (!down) return;
        const dx = e.clientX - x0;
        if (Math.abs(dx) > 5 && !deck.dataset.dragged) {
            deck.dataset.dragged = "1";
            deck.classList.add("dragging");
            deck.setPointerCapture(e.pointerId);
        }
        if (!deck.dataset.dragged) return;
        deck.scrollLeft = left0 - dx;
        const dt = Math.max(1, e.timeStamp - lastT);
        v = 0.8 * v + 0.2 * ((lastX - e.clientX) / dt);   // px per ms, smoothed over the last moves
        lastX = e.clientX; lastT = e.timeStamp;
    });
    const end = (e) => {
        if (!down) return;
        down = false;
        // A pause before letting go is a stop, not a flick.
        if (e.timeStamp - lastT > 80) v = 0;
        let prev = performance.now();
        const step = (now) => {
            const dt = now - prev; prev = now;
            deck.scrollLeft += v * dt;
            v *= Math.pow(0.996, dt);                       // friction: the glide halves in about 170 ms
            if (Math.abs(v) > 0.02) glide = requestAnimationFrame(step);
            else deck.classList.remove("dragging");         // snapping back on: the nearest card centers
        };
        glide = requestAnimationFrame(step);
        // The click that ends a drag fires after this, so the marker clears on the next turn.
        setTimeout(() => delete deck.dataset.dragged, 0);
    };
    deck.addEventListener("pointerup", end);
    deck.addEventListener("pointercancel", end);
}

// The card size, a scale the cards and their text follow, remembered in this browser for every deck; a first visit opens at the smallest size that still shows the name and the line.
const kStartScale = "0.55";
const LS_SCALE = "mm_deckScale";
function sizeSlider(host, onChange) {
    const input = document.createElement("input");
    input.type = "range";
    input.min = "0.1"; input.max = "1.6"; input.step = "0.05";
    let saved = null;
    try { saved = localStorage.getItem(LS_SCALE); } catch { /* storage blocked: the start size */ }
    input.value = saved || kStartScale;
    input.className = "mm-deck-size";
    input.title = "Card size";
    input.setAttribute("aria-label", "Card size");
    // Below half size a card is its picture, and smaller still the deck is spread out on the table, every card in rows.
    const apply = () => {
        host.style.setProperty("--mm-card-scale", input.value);
        host.classList.toggle("mm-deck-compact", Number(input.value) < 0.8);
        host.classList.toggle("mm-deck-small", Number(input.value) < 0.5);
        host.classList.toggle("mm-deck-spread", Number(input.value) < 0.35);
        onChange();
    };
    input.addEventListener("input", () => {
        apply();
        try { localStorage.setItem(LS_SCALE, input.value); } catch { /* not remembered, still applied */ }
    });
    apply();
    return input;
}

// The ‹ › buttons with the count between them.
function navButtons(step, count) {
    const nav = document.createElement("div");
    nav.className = "mm-deck-nav";
    for (const [label, dir] of [["‹", -1], ["›", 1]]) {
        const b = document.createElement("button");
        b.type = "button";
        b.textContent = label;
        b.setAttribute("aria-label", dir < 0 ? "Previous card" : "Next card");
        b.addEventListener("click", () => step(dir));
        nav.appendChild(b);
    }
    nav.insertBefore(count, nav.lastChild);
    return nav;
}

// The deck: search, the size slider, the ‹ › buttons, the filter chips and the fanned row of cards, the same for every kind of card.
function mountDeck(host, cards, noun, chipOrder) {
    const search = document.createElement("input");
    search.type = "search";
    search.placeholder = `Search ${cards.length} ${noun}`;
    search.className = "mm-deck-search";
    const count = document.createElement("span");
    count.className = "mm-deck-count";
    const deck = document.createElement("div");
    deck.className = "mm-deck-row";
    deck.append(...cards);

    let present = [];
    for (const c of cards) for (const e of c._emoji) if (!present.includes(e)) present.push(e);
    if (chipOrder) present = chipOrder.filter(e => present.includes(e));
    let active = new Set();
    const refresh = () => {
        const q = search.value.trim().toLowerCase();
        let shown = 0;
        for (const c of cards) {
            c.hidden = !(chipsMatch(c._emoji, active) && (!q || c._text.includes(q)));
            if (!c.hidden) shown++;
        }
        count.textContent = `${shown} of ${cards.length}`;
        deck.scrollLeft = 0;
        align(deck, host);
        fan(deck);
    };
    const chips = emojiChipRow(present, (a) => { active = a; refresh(); }, chipOrder ? KIND_LABEL : EMOJI_LABEL);

    // One visible card and the gap after it, so a filtered deck moves a card at a time.
    const step = (dir) => {
        const c = cards.find(x => !x.hidden);
        const gap = parseFloat(getComputedStyle(deck).columnGap) || 0;
        if (c) deck.scrollBy({left: dir * (c.offsetWidth + gap), behavior: "smooth"});
    };
    const nav = navButtons(step, count);

    const bar = document.createElement("div");
    bar.className = "mm-deck-bar";
    bar.append(search, sizeSlider(host, () => { align(deck, host); fan(deck); }), nav);
    host.replaceChildren(bar, chips, deck);   // what the build wrote gives way to the deck
    host.classList.add("mm-deck-ready");
    draggable(deck);
    search.addEventListener("input", refresh);
    deck.addEventListener("scroll", () => requestAnimationFrame(() => fan(deck)), {passive: true});
    deck.addEventListener("keydown", (e) => {
        if (e.key === "ArrowRight") { e.preventDefault(); step(1); }
        if (e.key === "ArrowLeft") { e.preventDefault(); step(-1); }
    });
    mounted.add({deck, host});
    refresh();
    // A link to an entry brings its card to the front of the deck, turned to its back.
    const target = location.hash && cards.find(c => c.id === location.hash.slice(1));
    if (target) {
        deck.scrollLeft = target.offsetLeft + target.offsetWidth / 2 - deck.clientWidth / 2;
        target.click();
        host.scrollIntoView();
    }
}

async function mount(host) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    const page = host.dataset.page;
    if (!page) {
        const table = host.querySelector("table");
        if (table) mountDeck(host, tableEntries(table).map(entryCard), "entries", Object.keys(KIND_LABEL));
        else host.classList.add("mm-deck-ready");   // nothing to draw: what the build wrote shows
        return;
    }
    let modules;
    // MoonWalk's index gives the cards their links into it; without it they show the help alone.
    const walk = fetch(WALK).then(r => r.ok ? r.json() : null).catch(() => null);
    try {
        modules = (await (await fetch(HELP)).json()).modules;
    } catch {
        host.textContent = "The cards could not load: help/help.json is missing or unreadable.";
        host.classList.add("mm-deck-ready");
        return;
    }
    const onPage = (doc) => doc === page || doc.startsWith(page + "#");   // a registry link may name the page without a card
    const types = Object.keys(modules).filter(t => onPage(modules[t].doc || ""))
        .sort((a, b) => typeDisplayName(a).localeCompare(typeDisplayName(b)));
    const index = await walk;
    mountDeck(host, types.map(t => moduleCard(t, modules[t], index)), "modules");
}

// One resize listener for every deck: instant navigation swaps pages without reloading, so a listener per mount would pile up over detached decks.
const mounted = new Set();
window.addEventListener("resize", () => {
    for (const m of mounted) {
        if (!m.deck.isConnected) { mounted.delete(m); continue; }
        align(m.deck, m.host); fan(m.deck);
    }
});
const mountAll = () => document.querySelectorAll(".mm-deck").forEach(mount);
if (window.document$) window.document$.subscribe(mountAll); else mountAll();

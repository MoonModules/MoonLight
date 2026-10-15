// The catalog as a card deck: every module of a page, drawn from help.json by the app's own help.js, so a card reads as the module's help does on a device.
// Mounted on every page that holds a `.mm-deck`, through Material's document$ so instant navigation mounts it too.

import { helpMarkdown, safeUrl, moduleHelp, controlHelp, emojiTagsFor, emojiChipRow, chipsMatch, typeDisplayName, EMOJI_LABEL } from "./help.js";

const HELP = new URL("../help/help.json", import.meta.url);

// A playing card: the name, the picture and the one line on its face, the module's help and controls on its back, turned with a tap.
function card(type, help) {
    const name = typeDisplayName(type);
    const emoji = emojiTagsFor(help);
    const el = document.createElement("article");
    el.className = "mm-pcard";
    el.tabIndex = 0;
    el.setAttribute("aria-label", name);
    el.title = name;   // the name on hover, which a small card has no room to print
    const anchor = (help.doc || "").split("#")[1];
    if (anchor) el.id = anchor;   // a link to the module lands on its card

    const front = document.createElement("div");
    front.className = "mm-pcard-face mm-pcard-front";
    const pips = document.createElement("div");
    pips.className = "mm-pcard-pips";
    for (const e of emoji) {
        const s = document.createElement("span");
        s.textContent = e;
        if (EMOJI_LABEL[e]) s.title = EMOJI_LABEL[e];
        pips.appendChild(s);
    }
    // The name beside the emoji, at their scale, so it reads at every card size.
    const pipName = document.createElement("span");
    pipName.className = "mm-pcard-name";
    pipName.textContent = name;
    pips.appendChild(pipName);
    front.appendChild(pips);
    if (safeUrl(help.image)) {
        const img = document.createElement("img");
        img.loading = "lazy";
        img.src = safeUrl(help.image);
        img.alt = help.alt || `${name} preview`;
        front.appendChild(img);
    }
    const p = document.createElement("p");
    p.innerHTML = helpMarkdown(help.line || "");
    front.appendChild(p);

    // The back: a fixed head, a tiny picture and the title, so a turned card always says what it is, and the help below it scrolling on its own.
    const back = document.createElement("div");
    back.className = "mm-pcard-face mm-pcard-back";
    const head = document.createElement("div");
    head.className = "mm-pcard-head";
    if (safeUrl(help.image)) {
        const thumb = document.createElement("img");
        thumb.loading = "lazy";
        thumb.src = safeUrl(help.image);
        thumb.alt = "";
        head.appendChild(thumb);
    }
    const h = document.createElement("h3");
    h.textContent = name;
    head.appendChild(h);
    const scroll = document.createElement("div");
    scroll.className = "mm-pcard-scroll";
    back.append(head, scroll);
    // The line as plain text rather than the app's disclosure, so a tap on it turns the card rather than folding the help.
    const line = document.createElement("p");
    line.className = "mm-pcard-line";
    line.innerHTML = helpMarkdown(help.line || "");
    scroll.appendChild(line);
    const box = moduleHelp({...help, image: null}, help.doc);
    const summary = box.querySelector(".help-summary");
    if (summary) scroll.appendChild(summary);
    scroll.appendChild(box.querySelector(".card-helpbox-body"));
    for (const [ctrl, ch] of Object.entries(help.controls || {})) {
        const row = document.createElement("div");
        row.className = "control-row";
        const label = document.createElement("span");
        label.className = "control-label";
        label.textContent = ctrl;
        row.appendChild(label);
        controlHelp(row, ch);
        scroll.appendChild(row);
    }

    const inner = document.createElement("div");
    inner.className = "mm-pcard-inner";
    inner.append(front, back);
    el.appendChild(inner);
    // The face turned away is inert, so a keyboard reaches only what shows.
    back.inert = true;
    const flip = () => { const b = el.classList.toggle("flipped"); front.inert = b; back.inert = !b; };
    // A tap turns the card, unless it lands on something of its own to press, or ends a drag of the deck.
    el.addEventListener("click", (e) => {
        if (el.closest(".mm-deck-row")?.dataset.dragged) return;
        if (!e.target.closest("a, summary")) flip();
    });
    // Only on the card itself: a link or a disclosure on its back handles its own keys.
    el.addEventListener("keydown", (e) => { if (e.target === el && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); flip(); } });
    el._emoji = emoji;
    el._text = `${type} ${name} ${help.line}`.toLowerCase();
    return el;
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

async function mount(host) {
    if (host.dataset.mounted) return;
    host.dataset.mounted = "1";
    const page = host.dataset.page;
    let modules;
    try {
        modules = (await (await fetch(HELP)).json()).modules;
    } catch {
        host.textContent = "The cards could not load: help/help.json is missing or unreadable.";
        return;
    }
    const onPage = (doc) => doc === page || doc.startsWith(page + "#");   // a registry link may name the page without a card
    const types = Object.keys(modules).filter(t => onPage(modules[t].doc || ""))
        .sort((a, b) => typeDisplayName(a).localeCompare(typeDisplayName(b)));

    const search = document.createElement("input");
    search.type = "search";
    search.placeholder = `Search ${types.length} modules`;
    search.className = "mm-deck-search";
    const count = document.createElement("span");
    count.className = "mm-deck-count";
    const deck = document.createElement("div");
    deck.className = "mm-deck-row";
    const cards = types.map(t => card(t, modules[t]));
    deck.append(...cards);

    const present = [];
    for (const c of cards) for (const e of c._emoji) if (!present.includes(e)) present.push(e);
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
    const chips = emojiChipRow(present, (a) => { active = a; refresh(); });

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
    host.replaceChildren(bar, chips, deck);   // the cards the build wrote as a table give way to the deck
    draggable(deck);
    search.addEventListener("input", refresh);
    deck.addEventListener("scroll", () => requestAnimationFrame(() => fan(deck)), {passive: true});
    deck.addEventListener("keydown", (e) => {
        if (e.key === "ArrowRight") { e.preventDefault(); step(1); }
        if (e.key === "ArrowLeft") { e.preventDefault(); step(-1); }
    });
    mounted.add({deck, host});
    refresh();
    // A link to a module brings its card to the front of the deck, turned to its help.
    const target = location.hash && cards.find(c => c.id === location.hash.slice(1));
    if (target) {
        deck.scrollLeft = target.offsetLeft + target.offsetWidth / 2 - deck.clientWidth / 2;
        target.click();
        host.scrollIntoView();
    }
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

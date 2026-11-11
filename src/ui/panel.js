// The module panel sized as a desktop window is: its left edge is the split handle beside the preview
// (preview3d.js), and grips on its bottom edge and bottom-left corner make it taller, the corner wider too.
//
// A card's areas are fixed or expandable. Fixed ones, a control or a row, keep their natural height; an
// expandable one, the device map or a text area, is marked with markExpandable and takes a share of the
// height the panel gains or gives up, in proportion to its own natural height, so every card scales with the panel.
// The height is kept per page in this browser; a double-click on a grip returns the page to its natural size.

const KEY = "MoonLight.panel.v1";   // {<page>: height in px}
const MIN_AREA = 80;                 // px: the least an expandable area shrinks to, the device map then zoomed out to fit
let main_ = null, page_ = "", widen_ = null, done_ = null, raf_ = 0, lastW_ = 0;

function load() {
    try { return JSON.parse(localStorage.getItem(KEY) || "{}") || {}; } catch (_) { return {}; }
}
function save(p) {
    try { localStorage.setItem(KEY, JSON.stringify(p)); } catch (_) { /* a private window keeps it for this visit */ }
}

/// Mark `el` as an area that grows with the panel; `base`, when given, is its natural height in px, for an
/// area that draws itself to whatever height it is given and so cannot be measured, as the device map does.
export function markExpandable(el, base) {
    el.classList.add("expandable");
    if (base !== undefined) el.dataset.base = String(Math.round(base));
}

/// Each area's height once `extra` px, negative to shrink, are shared among them in proportion to their natural heights.
export function shareExtra(bases, extra) {
    const total = bases.reduce((sum, b) => sum + b, 0);
    return bases.map(b => (total > 0 ? b + (extra * b) / total : b));
}

// The panel's content height: the top-level card's bottom edge within `main`, where `main`'s own scrollHeight is the window's height while the cards are shorter.
function contentHeight() {
    const root = main_.querySelector(".panel-root");
    return root ? root.getBoundingClientRect().bottom - main_.getBoundingClientRect().top + main_.scrollTop : main_.scrollHeight;
}

// Size the page's expandable areas so the panel's content is `height` px: they grow past their natural heights, or give height up down to MIN_AREA.
function fit(height) {
    if (!main_) return;
    const areas = [...main_.querySelectorAll(".expandable")];
    for (const el of areas) el.style.height = "";
    if (!height || !areas.length) return;
    const bases = areas.map(el => (el.dataset.base ? Number(el.dataset.base) : el.offsetHeight));
    areas.forEach((el, i) => { el.style.height = `${bases[i]}px`; });
    shareExtra(bases, height - contentHeight()).forEach((h, i) => {
        areas[i].style.height = `${Math.round(Math.max(Math.min(MIN_AREA, bases[i]), h))}px`;
    });
}

/// Fit the page drawn last, `page` naming whose height to use.
export function panelFit(page) {
    page_ = page;
    fit(load()[page_] || 0);
}

/// Fit again on the next frame, after an expandable area was rebuilt or changed its natural height.
export function panelRefit() {
    if (raf_) return;
    raf_ = requestAnimationFrame(() => { raf_ = 0; fit(load()[page_] || 0); });
}

/// The panel's bottom edge, its bottom-left corner inside it, for the top-level card's border: invisible, as a window's edges are.
export function panelGrips() {
    const bottom = document.createElement("div");
    bottom.className = "panel-grip panel-grip-bottom";
    bottom.setAttribute("role", "separator");
    bottom.setAttribute("aria-orientation", "horizontal");
    bottom.setAttribute("aria-label", "Resize the panel; double-click for its natural size");
    const corner = document.createElement("div");
    corner.className = "panel-grip panel-grip-corner";
    bottom.appendChild(corner);
    return bottom;
}

/// Wire the grips once, by delegation on `main`, which redraws its cards on every page; `widen` moves the
/// panel's left edge to a pointer's x and `done` keeps that width.
export function setupPanel(main, widen, done) {
    main_ = main;
    widen_ = widen;
    done_ = done;
    let drag = null;
    const move = (e) => {
        if (!drag) return;
        if (drag.corner && widen_) widen_(e.clientX);
        drag.height = Math.max(1, drag.from + (e.clientY - drag.y));   // 0 would read as "natural"
        fit(drag.height);
        main_.scrollTop = main_.scrollHeight;   // the grip stays under the pointer as the panel grows past the window
    };
    const stop = () => {
        if (!drag) return;
        const p = load();
        p[page_] = Math.round(drag.height);
        save(p);
        if (drag.corner && done_) done_();
        drag = null;
        document.body.classList.remove("panel-resizing", "corner");
        window.removeEventListener("pointermove", move);
        window.removeEventListener("pointerup", stop);
        window.removeEventListener("pointercancel", stop);
    };
    main.addEventListener("pointerdown", (e) => {
        const grip = e.target.closest(".panel-grip");
        if (!grip) return;
        e.preventDefault();
        const from = contentHeight();
        drag = { corner: grip.classList.contains("panel-grip-corner"), y: e.clientY, from, height: from };
        document.body.classList.add("panel-resizing");
        document.body.classList.toggle("corner", drag.corner);
        window.addEventListener("pointermove", move);
        window.addEventListener("pointerup", stop);
        window.addEventListener("pointercancel", stop);
    });
    main.addEventListener("dblclick", (e) => {
        if (!e.target.closest(".panel-grip")) return;
        const p = load();
        delete p[page_];
        save(p);
        fit(0);
    });
    // A wider or narrower panel changes what its areas need, the map's rows among them, so it fits again.
    new ResizeObserver(() => {
        if (main_.clientWidth === lastW_) return;
        lastW_ = main_.clientWidth;
        panelRefit();
    }).observe(main);
}

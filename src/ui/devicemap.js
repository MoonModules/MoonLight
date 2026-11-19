// The device map: a list drawn as a graph, each device a node placed by a force layout, its traffic the edges, its groups boxes around
// their members, and the peers' own lists merged in. The device's page draws it from flash; MoonDeck serves the same file for its
// Live tab. What differs between the two comes in as hooks on `opts.hooks`: what a click on a node opens, on both the node's popup
// (devicepopup.js), which reads its own hooks from the same `opts`; a row's age, the update a node's channel offers and the release
// list it reads, and the room the page gives the picture.

import { typeDisplayName } from "./help.js";
import { peerFirmware } from "./deviceinstall.js";

// The hooks a page passes; each has a default that does as little as it can, so a page passing none still shows the map.
const NO_HOOKS = {
    openNode() {},                          // (row, opts): a node was clicked or opened from the keyboard
    openGroup() {},                         // ({id, name, rows}, opts): a group's box was clicked or opened from the keyboard
    rowAge: () => "",                       // (row): the age class its row reads as, "age-fresh" and kin
    update: () => null,                     // (row, releases): the update its channel offers, {label}, or null
    releases: () => null,                   // (): the release list, or a promise of it
    fit() {},                               // (box, naturalHeight): the picture's natural height moved, or (box) its box was rebuilt
};
const hooks = (opts) => ({ ...NO_HOOKS, ...(opts && opts.hooks) });

// A list drawn as a graph (ListSource::listAsGraph): each row a node, keyed by `ip` or else `name`,
// colored by `type`, ringed when `self`, faded by its detail's age, and linked by an `edges` array of
// {to, label}, the adjacency list. The linked nodes, this device among them, are placed by
// Fruchterman-Reingold force-directed layout, hand-written: nodes repel, edges attract, a cooling
// step limits each move, and positions are kept on the container, so a new device settles into the
// picture rather than reshuffling it; this device sits fixed at the center. The unlinked nodes
// sit in a grid beneath, by name, so a swarm of idle devices reads as a list rather than a cloud.
const GRAPH_W = 600, GRAPH_LINKED_H = 300, GRAPH_CELL_W = 120, GRAPH_CELL_H = 76;   // GRAPH_W: the width before the map's box is measured
const GRAPH_MARGIN_X = GRAPH_CELL_W / 2, GRAPH_MARGIN_TOP = 24, GRAPH_MARGIN_BOTTOM = 60;   // a label fits inside
const GRAPH_PAD_X = 12;
const GRAPH_SHOW = [["ip", "IP", false], ["version", "version", false], ["traffic", "traffic", true]];   // what a viewer shows or hides, and its default; a node's lights and services, its emoji, always show
const GRAPH_SHOW_KEY = "MoonLight.graph.show.v2";   // v2: address and version start hidden
const GRAPH_ZOOM_MAX = 8;   // the closest the map zooms in, eight times
const GRAPH_WLED_ASK_MS = 60000, GRAPH_WLED_INFO_MS = 600000;   // a WLED asked again a minute after no answer, ten minutes after one, as an update in place moves its version
const GRAPH_GROUP_GAP = 12;   // the least room between two group boxes that share no member
const GRAPH_CELL_DOT = 14;   // a grid node's dot from its cell's top, which leaves its four label lines inside the cell
const GRAPH_LABEL_DY = 20, GRAPH_LINE_H = { "graph-label": 12, "graph-sublabel": 11, "graph-services": 13 };   // a node's first baseline below its dot, and each line's height
const GRAPH_CHAR_W = { "graph-label": 6.2, "graph-sublabel": 5, "graph-services": 13, "graph-edge-label": 5.4 };   // a character's width, about, at each line's font size
// Each kind Discovery names its own color (devTypeStr), and a WLED flavor its own; a kind it does not name is generic, white on dark.
const GRAPH_KIND_COLORS = { MoonLight: "var(--accent)", WLED: "var(--yellow)", "Hue bridge": "var(--green)", group: "var(--accent-soft)", generic: "var(--fg)", MoonBase: "var(--red)", ESPHome: "var(--teal)", Tasmota: "var(--blue)", "WLED-MM": "var(--orange)" };
// Each light output's driver type as the kind of light it drives, which a node shows; a driver it does not name shows its type, and moving heads show 🎯 whatever drives them.
const GRAPH_OUTPUT_EMOJI = { RmtLedDriver: "\u{1F4A1}", ParallelLedDriver: "\u{1F4A1}", PwmLightDriver: "\u{1F506}", Hub75Driver: "\u{1F532}", HdmiDriver: "\u{1F4FA}", PanelCardDriver: "\u{1F9F1}", HueDriver: "\u{1F3EE}", NetworkSendDriver: "\u{1F4E1}", NdiDriver: "\u{1F3A5}", HlsDriver: "\u{1F3A5}", RtspDriver: "\u{1F3A5}" };
// Each protocol a module reports, a service it offers (NetworkUse::service) or traffic (NetworkUse::flow), as the emoji its node or line shows; one it does not name shows its name.
const GRAPH_PROTOCOL_EMOJI = { HTTP: "\u{1F310}", OSC: "\u{1F39B}\uFE0F", "RTP-MIDI": "\u{1F3B9}", "audio sync": "\u{1F50A}", "Art-Net": "\u{1F3AD}", "E1.31": "\u{1F39A}\uFE0F", DDP: "\u{1F4E6}", ColorLight: "\u{1F9F1}", "WLED API": "\u{1F419}", Hue: "\u{1F3EE}" };

// The kind a node is colored by: a WLED's flavor, WLED or WLED-MM, once its /json/info says which, and generic until then, so a WLED-MM
// that does not answer stays generic rather than showing as a plain WLED; every other row its type.
function graphKind(row) {
    if (row.type === "WLED") return row.flavor ?? "generic";
    return String(row.type ?? "generic");
}

function graphHash(text) {
    let h = 2166136261;   // FNV-1a, so a kind keeps its color and a node its seed spot
    for (let i = 0; i < text.length; i++) h = Math.imul(h ^ text.charCodeAt(i), 16777619);
    return h >>> 0;
}

// Place the nodes into `pos` and answer the picture's height: the linked ones by force in the top
// area, the rest in rows of cells beneath it.
function layoutGraph(nodes, links, pos, w = GRAPH_W, minH = 0) {
    const linked = new Set(links.flatMap(([i, j]) => [i, j]));
    nodes.forEach((node, i) => { if (node.self) linked.add(i); });
    const force = [...linked];
    // Room for every linked node's cell twice over, so the force layout spreads them rather than pinning them to the edges.
    let areaH = force.length > 1 ? Math.max(GRAPH_LINKED_H, Math.ceil((force.length * GRAPH_CELL_W * GRAPH_CELL_H * 2) / w))
                                 : (force.length ? GRAPH_CELL_H + GRAPH_MARGIN_TOP : 0);
    const idle = nodes.map((node, i) => i).filter(i => !linked.has(i))
        .sort((a, b) => String(nodes[a].row.name ?? nodes[a].key).localeCompare(String(nodes[b].row.name ?? nodes[b].key)));
    const cols = Math.max(1, Math.floor(w / GRAPH_CELL_W)), cellW = w / cols;
    const gridH = GRAPH_CELL_H * Math.ceil(idle.length / cols);
    const natural = areaH + gridH;
    if (force.length > 1) areaH += Math.max(0, minH - natural);   // a taller box gives the linked nodes the room
    // A resized picture carries its nodes with it, in proportion, so it grows rather than reshuffles.
    const was = pos.frame;
    if (was && was.w > 0 && was.areaH > 0 && areaH > 0 && (was.w !== w || was.areaH !== areaH)) {   // only a frame with a force area scales
        for (const p of pos.values()) if (!p.idle) { p.x *= w / was.w; p.y *= areaH / was.areaH; }
    }
    pos.frame = { w, areaH, natural };
    layoutForce(nodes, links, force, pos, areaH, w);
    separateNodes(nodes, force, pos, areaH, w);
    keepOutOfGroups(nodes, links, force, pos, areaH, w);
    idle.forEach((i, at) => {
        pos.set(nodes[i].key, { x: cellW * (at % cols) + cellW / 2,
                                y: areaH + GRAPH_CELL_H * Math.floor(at / cols) + GRAPH_CELL_DOT, idle: true });
    });
    return Math.max(areaH + gridH, minH);
}

// A group's box holds its members only: a linked node that is no member and sits inside a box, its cell overlapping it, is pushed out
// across the box's nearest side, a few passes, since a push can land it in another box. The groups are read from their links (groupLinks),
// each a star from its first member, and the box is the one drawn (groupBox).
function keepOutOfGroups(nodes, links, force, pos, areaH, w = GRAPH_W) {
    const groups = new Map();
    for (const [i, j, , , isGroup] of links) if (isGroup) (groups.get(i) || groups.set(i, new Set([i])).get(i)).add(j);
    for (let pass = 0; pass < 8 && groups.size; pass++) {
        let moved = false;
        for (const members of groups.values()) {
            const b = groupBox([...members].map(i => pos.get(nodes[i].key)));
            const x0 = b.x - GRAPH_CELL_W / 2, x1 = b.x + b.w + GRAPH_CELL_W / 2, y0 = b.y - GRAPH_CELL_DOT, y1 = b.y + b.h + GRAPH_CELL_DOT;
            for (const i of force) {
                const p = pos.get(nodes[i].key);
                if (members.has(i) || p.x <= x0 || p.x >= x1 || p.y <= y0 || p.y >= y1) continue;
                // The nearest side the picture's edge leaves room beyond, so a box against an edge sends the node out another way.
                const inside = (q) => q.x > x0 && q.x < x1 && q.y > y0 && q.y < y1;
                const tries = [[x0 - p.x, 0], [x1 - p.x, 0], [0, y0 - p.y], [0, y1 - p.y]].sort((a, c) => Math.abs(a[0] + a[1]) - Math.abs(c[0] + c[1]))
                    .map(([dx, dy]) => { const q = { x: p.x + dx, y: p.y + dy }; graphInside(q, areaH, w); return q; });
                const to = tries.find(q => !inside(q)) || tries[0];
                p.x = to.x; p.y = to.y;
                moved = true;
            }
        }
        if (separateGroups(nodes, [...groups.values()], pos, areaH, w)) moved = true;
        if (!moved) return;
    }
}

// Rectangle overlap removal for the boxes, as separateNodes does for the cells: two groups with no member in common end at least
// GRAPH_GROUP_GAP apart, the group that moves going whole along the axis the boxes overlap least, the one holding this device staying put,
// else the larger. Groups sharing a member keep their overlap, which is how an Euler diagram draws a shared member. True when one moved.
function separateGroups(nodes, groups, pos, areaH, w = GRAPH_W) {
    let moved = false;
    const holdsSelf = (g) => [...g].some(i => nodes[i].self);
    const box = (g) => groupBox([...g].map(i => pos.get(nodes[i].key)));
    for (let a = 0; a < groups.length; a++) {
        for (let c = a + 1; c < groups.length; c++) {
            if ([...groups[a]].some(i => groups[c].has(i))) continue;
            const ba = box(groups[a]), bc = box(groups[c]);
            const ox = Math.min(ba.x + ba.w, bc.x + bc.w) - Math.max(ba.x, bc.x) + GRAPH_GROUP_GAP;
            const oy = Math.min(ba.y + ba.h, bc.y + bc.h) - Math.max(ba.y, bc.y) + GRAPH_GROUP_GAP;
            if (ox <= 0 || oy <= 0) continue;
            const aMoves = holdsSelf(groups[c]) || (!holdsSelf(groups[a]) && groups[a].size <= groups[c].size);
            const [mover, bm, bo] = aMoves ? [groups[a], ba, bc] : [groups[c], bc, ba];
            const away = (m0, mLen, o0, oLen) => ((m0 + mLen / 2) >= (o0 + oLen / 2) ? 1 : -1);
            const dx = ox <= oy ? ox * away(bm.x, bm.w, bo.x, bo.w) : 0;
            const dy = ox <= oy ? 0 : oy * away(bm.y, bm.h, bo.y, bo.h);
            for (const i of mover) { const p = pos.get(nodes[i].key); p.x += dx; p.y += dy; graphInside(p, areaH, w); }
            moved = true;
        }
    }
    return moved;
}

// A linked node kept within the force area, its labels inside the picture.
function graphInside(p, areaH, w = GRAPH_W) {
    p.x = Math.min(w - GRAPH_MARGIN_X, Math.max(GRAPH_MARGIN_X, p.x));
    p.y = Math.min(areaH - GRAPH_MARGIN_BOTTOM, Math.max(GRAPH_MARGIN_TOP, p.y));
}

// Node overlap removal after the force layout: two nodes whose cells, a dot and its labels, overlap are pushed
// apart along the axis that overlaps least, this device staying put, until none overlaps or the passes run out.
function separateNodes(nodes, members, pos, areaH, w = GRAPH_W) {
    const cw = GRAPH_CELL_W, ch = GRAPH_CELL_H - 10;
    for (let pass = 0; pass < 60; pass++) {
        let moved = false;
        for (let a = 0; a < members.length; a++) {
            for (let b = a + 1; b < members.length; b++) {
                const na = nodes[members[a]], nb = nodes[members[b]];
                const pa = pos.get(na.key), pb = pos.get(nb.key);
                const dx = pb.x - pa.x, dy = pb.y - pa.y;
                const ox = cw - Math.abs(dx), oy = ch - Math.abs(dy);
                if (ox <= 0 || oy <= 0) continue;
                moved = true;
                const shareA = na.self ? 0 : (nb.self ? 1 : 0.5);
                if (ox < oy) {
                    const sx = dx < 0 ? -1 : 1;
                    pa.x -= sx * ox * shareA; pb.x += sx * ox * (1 - shareA);
                } else {
                    const sy = dy < 0 ? -1 : 1;
                    pa.y -= sy * oy * shareA; pb.y += sy * oy * (1 - shareA);
                }
                if (!na.self) graphInside(pa, areaH, w);
                if (!nb.self) graphInside(pb, areaH, w);
            }
        }
        if (!moved) return;
    }
}

function layoutForce(nodes, links, members, pos, areaH, w = GRAPH_W) {
    const n = members.length;
    const cx = w / 2, cy = areaH / 2;
    const inside = (p) => graphInside(p, areaH, w);
    let fresh = 0;
    // A group's links changed since the last round: two settled nodes now pulled together need the relaxation too.
    const groupSig = links.filter(l => l[4]).map(([i, j]) => `${nodes[i].key}>${nodes[j].key}`).sort().join("|");
    const regrouped = pos.groupSig !== undefined && pos.groupSig !== groupSig;
    pos.groupSig = groupSig;
    for (const i of members) {
        const key = nodes[i].key;
        const p = pos.get(key);
        if (p && !p.idle) { inside(p); continue; }
        const h = graphHash(key);
        pos.set(key, { x: cx + (h % 200) - 100, y: cy + ((h >>> 8) % 120) - 60 });
        fresh++;
    }
    if (n < 2) { for (const i of members) if (nodes[i].self) pos.set(nodes[i].key, { x: cx, y: cy }); return; }
    const k = 0.8 * Math.sqrt((w * areaH) / n);   // the ideal distance
    // A settled picture with a newcomer or two needs few rounds; a fresh one more, fewer as it grows (O(n²) a round).
    const rounds = fresh === 0 && !regrouped ? 0 : Math.max(20, Math.min(200, Math.floor(60000 / (n * n)) + 20));
    const at = new Map(members.map((i, m) => [i, m]));
    let heat = w / 8;
    for (let r = 0; r < rounds; r++) {
        const disp = members.map(() => ({ x: 0, y: 0 }));
        for (let a = 0; a < n; a++) {
            const pa = pos.get(nodes[members[a]].key);
            for (let b = a + 1; b < n; b++) {
                const pb = pos.get(nodes[members[b]].key);
                let dx = pa.x - pb.x, dy = pa.y - pb.y;
                const d = Math.max(0.5, Math.hypot(dx, dy));
                const f = (k * k) / d;
                dx /= d; dy /= d;
                disp[a].x += dx * f; disp[a].y += dy * f;
                disp[b].x -= dx * f; disp[b].y -= dy * f;
            }
        }
        for (const [i, j] of links) {
            const a = at.get(i), b = at.get(j);
            const pa = pos.get(nodes[i].key), pb = pos.get(nodes[j].key);
            let dx = pa.x - pb.x, dy = pa.y - pb.y;
            const d = Math.max(0.5, Math.hypot(dx, dy));
            const f = (d * d) / k;
            dx /= d; dy /= d;
            disp[a].x -= dx * f; disp[a].y -= dy * f;
            disp[b].x += dx * f; disp[b].y += dy * f;
        }
        for (let a = 0; a < n; a++) {
            const node = nodes[members[a]];
            const p = pos.get(node.key);
            if (node.self) { p.x = cx; p.y = cy; continue; }
            disp[a].x += (cx - p.x) * 0.05;   // a little gravity, so a group not linked to this device stays in view
            disp[a].y += (cy - p.y) * 0.05;
            const d = Math.max(0.5, Math.hypot(disp[a].x, disp[a].y));
            const step = Math.min(d, heat);
            p.x += (disp[a].x / d) * step;
            p.y += (disp[a].y / d) * step;
            inside(p);
        }
        heat *= 0.95;
    }
}

// The peers' own graphs, merged into this one: every node of the same kind as this device's own
// node serves the same list at its `url`, so the page reads it from there, a few at a time, while
// the map is on screen, again five seconds after the last round. A link one end reports and the
// other, a peer that answered, does not, is drawn broken; a peer that does not answer, unreachable.
const PEER_REFRESH_MS = 5000, PEER_TIMEOUT_MS = 3000, PEER_PARALLEL = 4;
const PEER_SUMMARY_MS = 60000;   // how often a peer too old to serve its list is asked what it runs: its whole state, several kilobytes

const graphKey = (row) => String(row.ip ?? row.name);

export async function fetchPeerGraph(url, opts) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), PEER_TIMEOUT_MS);
    try {
        const res = await fetch(`${url.replace(/\/$/, "")}/api/modules/${encodeURIComponent(opts.moduleName)}`, { signal: ctl.signal });
        if (!res.ok) return { state: "absent", rows: [] };   // it answers, but serves no such list: older firmware
        const mod = await res.json();
        const c = (mod.controls || []).find(x => x.name === opts.ctrlName);
        if (!c) return { state: "absent", rows: [] };
        const value = Array.isArray(c.value) ? c.value : [];
        const detail = Array.isArray(c.detail) ? c.detail : [];
        return { state: "ok", rows: value.map((r, i) => Object.assign({}, r, detail[i] || {})) };
    } catch (_) {
        return { state: "silent", rows: [] };
    } finally {
        clearTimeout(timer);
    }
}

// What a MoonLight node has behind, for its mark and its tooltip: its app behind its channel, a MoonBase or a WiFi coprocessor behind its app; "" when all are current.
function nodeBehind(row, releases, update) {
    const u = update(row, releases);
    return [u ? `${u.label} available` : "", row.moonbaseMismatch ? "MoonBase behind the app" : "",
            row.coprocessorMismatch ? "WiFi coprocessor behind the app" : ""].filter(Boolean).join(", ");
}

// The release list the map marks nodes behind their channel by, asked on every draw, which the browser's HTTP cache answers between the
// feed's checks; a list that changed draws the map again. Null until one has loaded.
function graphReleases(container, h) {
    if (!container._releasesAsking) {
        container._releasesAsking = true;
        Promise.resolve(h.releases()).then(list => {
            container._releasesAsking = false;
            if (list && list !== container._releases) { container._releases = list; redrawGraph(container); }
        });
    }
    return container._releases || null;
}

function redrawGraph(container) {
    if (container._graphPanning) return;   // the pan's end redraws
    const a = container._graphArgs;
    const old = container.querySelector("svg.list-graph");
    if (!a || !old) return;
    const focused = old.contains(document.activeElement) ? document.activeElement.closest("g.graph-link")?.dataset.key : null;
    const fresh = buildListGraph(container, a.rows, a.details, a.opts);
    old.replaceWith(fresh);
    // The node a keyboard user is on keeps the focus, though the redraw, about once a second, replaced it.
    if (focused != null) fresh.querySelector(`g.graph-link[data-key="${CSS.escape(focused)}"]`)?.focus({ preventScroll: true });
}

// Draw the map into `container`, or redraw the one drawn there in place, so an update every second keeps the box's height: the filters,
// a box whose height the page gives it, and a redraw whenever the box's size changes.
export function mountDeviceMap(container, rows, details, opts) {
    if (container.querySelector(":scope > .graph-box")) {
        container._graphArgs = { rows, details, opts };
        redrawGraph(container);
        return;
    }
    container.replaceChildren();
    container.appendChild(buildGraphFilters(container));
    const box = document.createElement("div");
    box.className = "graph-box";
    container.appendChild(box);
    box.appendChild(buildListGraph(container, rows, details, opts));
    new ResizeObserver(() => {
        const d = container._graphDrawn, h = parseFloat(box.style.height) || 0;
        if (d && d.w === box.clientWidth && d.h === h) return;
        requestAnimationFrame(() => redrawGraph(container));
    }).observe(box);
    hooks(opts).fit(box);   // a rebuilt box takes back its share of the page
}

function refreshPeerGraphs(container) {
    const a = container._graphArgs;
    if (!a || !a.opts) return;
    const own = a.rows.map((r, i) => Object.assign({}, r, a.details[i] || {}));
    const self = own.find(r => r.self);
    // A list with no device of its own, as MoonDeck's presence rows, has no peers: the graphs a list before it gathered go, and a round
    // still in flight fills the map dropped with them.
    if (!self) { container._peerGraphs = null; return; }
    if (container._peerFetching) return;
    if (Date.now() - (container._peerAt || 0) < PEER_REFRESH_MS) return;
    if (!container.isConnected || container.offsetParent === null) return;   // asked only while the map shows
    const queue = own.filter(r => !r.self && r.type === self.type && r.url);
    const peers = container._peerGraphs || (container._peerGraphs = new Map());
    // A peer gone from this device's list takes its graph, and what only it saw, off the map.
    const asked = new Set(queue.map(graphKey));
    for (const key of [...peers.keys()]) if (!asked.has(key)) peers.delete(key);
    container._peerFetching = true;
    // A peer too old to serve its list still says what it runs (peerFirmware, its summary or its state), once a minute, which its node takes as a
    // peer's own row would give it, so the map marks it behind as it marks any other.
    const runs = container._peerRuns || (container._peerRuns = new Map());
    const worker = async () => {
        for (let r = queue.shift(); r; r = queue.shift()) {
            const key = graphKey(r), pg = await fetchPeerGraph(r.url, a.opts);
            if (pg.state === "absent") {
                if (Date.now() - (runs.get(key)?.at || 0) > PEER_SUMMARY_MS) runs.set(key, { at: Date.now(), fw: await peerFirmware(r.url.replace(/\/$/, "")) });
                const fw = runs.get(key).fw;
                if (fw) pg.rows = [{ ip: r.ip, name: r.name, self: true, version: fw.version, ...(fw.firmware ? { firmware: fw.firmware } : {}) }];
            }
            peers.set(key, pg);
        }
    };
    Promise.all(Array.from({ length: Math.min(PEER_PARALLEL, queue.length) }, worker)).then(() => {
        container._peerFetching = false;
        container._peerAt = Date.now();
        redrawGraph(container);
        setTimeout(() => redrawGraph(container), PEER_REFRESH_MS);   // which asks again while the map still shows
    });
}

// The device groups the graphs name, by id: each its name, its members' node indices, and those missing. The first graph is this device's:
// a group it names is decided by its rows alone, so a member it removed stays removed while a cached peer report still lists it; a group
// only peers name is merged from their rows by its id.
function graphGroups(graphs, index) {
    const groups = new Map();
    const local = new Set();
    graphs.forEach((g, at) => {
        for (const row of g) {
            const i = index.get(graphKey(row));
            if (i === undefined) continue;
            for (const grp of row.groups || []) {
                if (!grp || !grp.id) continue;
                if (at === 0) local.add(grp.id);
                else if (local.has(grp.id)) continue;
                const entry = groups.get(grp.id) || groups.set(grp.id, { name: grp.name || "", members: new Set(), missing: new Set() }).get(grp.id);
                entry.members.add(i);
                if (row.missing) entry.missing.add(i);
            }
        }
    });
    return groups;
}

// The devices a search matches, by index: every space-separated term, ignoring case, appears in a node's name, address, type, version,
// firmware, board or one of its groups' names; null for an empty search, which matches the whole map. The map greys out the rest. Pure.
export function graphMatches(nodes, groups, query) {
    const terms = String(query).toLowerCase().split(/\s+/).filter(Boolean);
    if (!terms.length) return null;
    const groupNames = (i) => [...groups.values()].filter(grp => grp.members.has(i)).map(grp => grp.name);
    const text = (node, i) => [node.row.name, node.row.ip, node.row.type, node.row.version, node.row.firmware, node.row.board, ...groupNames(i)]
        .filter(Boolean).join(" ").toLowerCase();
    return new Set(nodes.map((node, i) => i).filter(i => terms.every(t => text(nodes[i], i).includes(t))));
}

// The invisible links that pull a group's members together in the force layout, each member to the first: a star, n - 1 links a group.
function groupLinks(groups) {
    const out = [];
    for (const { members } of groups.values()) {
        const [first, ...rest] = [...members];
        for (const m of rest) out.push([first, m, "", false, true]);   // the fifth: a group's link, which layoutForce relaxes when it changes
    }
    return out;
}

// The box drawn around a group's members: their dots' extent, wide enough for a label cell and tall enough for the label lines beneath.
function groupBox(points) {
    const xs = points.map(p => p.x), ys = points.map(p => p.y);
    return { x: Math.min(...xs) - GRAPH_CELL_W / 2 + 4, y: Math.min(...ys) - 22,
             w: Math.max(...xs) - Math.min(...xs) + GRAPH_CELL_W - 8, h: Math.max(...ys) - Math.min(...ys) + GRAPH_CELL_H };
}

function buildListGraph(container, rows, details, opts) {
    const NS = "http://www.w3.org/2000/svg";
    container._graphArgs = { rows, details, opts };   // what a peer round redraws from
    const nodes = rows.map((item, i) => {
        const row = Object.assign({}, item, details[i] || {});
        return { row, key: graphKey(row), self: !!row.self };
    });
    const index = new Map(nodes.map((node, i) => [node.key, i]));
    const peers = (nodes.some(n => n.self) && container._peerGraphs) || new Map();   // a list with no device of its own has no peers
    // A device a peer sees and this one does not joins the map, and a peer's own row adds what it runs and offers to its node.
    for (const [, pg] of peers) {
        for (const row of pg.rows) {
            const key = graphKey(row);
            if (index.has(key)) {
                if (row.self) {
                    const node = nodes[index.get(key)];
                    // Only what the peer reports: an older one sends fewer fields, and a key with an undefined value shows as an empty line.
                    const own = ["version", "firmware", "mode", "moonbaseMismatch", "coprocessorMismatch", "services", "outputs"].filter(k => row[k] !== undefined);
                    node.row = Object.assign({}, node.row, Object.fromEntries(own.map(k => [k, row[k]])));
                }
                continue;
            }
            index.set(key, nodes.length);
            nodes.push({ row: Object.assign({}, row, { self: false }), key, self: false });
        }
    }
    // Every graph's rows: this device's own, and each peer's, its own row among them.
    const graphs = [nodes.map(n => n.row)];
    for (const [, pg] of peers) if (pg.state === "ok") graphs.push(pg.rows);
    // In a graph its own row's edges are what it sends, another row's what it receives from that row.
    const owners = new Set(), received = new Set();
    for (const g of graphs) {
        const owner = g.find(r => r.self);
        if (owner) owners.add(graphKey(owner));
        for (const row of g) if (!row.self) for (const e of row.edges || []) received.add(`${graphKey(row)}|${e.to}|${String(e.label || "").split(" ")[0]}`);
    }
    const links = [];   // [from, to, label, broken]
    const seen = new Set();
    for (const senders of [true, false]) {   // a sender's report first, so its label wins
        for (const g of graphs) {
            for (const row of g) {
                if (!!row.self !== senders) continue;
                const i = index.get(graphKey(row));
                for (const e of row.edges || []) {
                    const j = index.get(String(e && e.to));
                    if (i === undefined || j === undefined || i === j) continue;
                    const id = `${graphKey(row)}|${e.to}|${String(e.label || "").split(" ")[0]}`;
                    if (seen.has(id)) continue;
                    seen.add(id);
                    links.push([i, j, e.label || "", senders && owners.has(String(e.to)) && !received.has(id)]);
                }
            }
        }
    }
    const groups = graphGroups(graphs, index);
    refreshPeerGraphs(container);
    const pos = container._graphPos || (container._graphPos = new Map());
    for (const key of [...pos.keys()]) if (!index.has(key)) pos.delete(key);   // a device gone takes its spot with it
    const match = graphMatches(nodes, groups, container._graphQuery || "");   // null: no search, every device shows as it is
    const dim = (i) => match !== null && !match.has(i);
    // Drawn in real pixels at its box's size: a wider or taller box gives the nodes more room, the text staying its size.
    const box = container.querySelector(".graph-box");
    const w = Math.max(2 * GRAPH_CELL_W, box && box.clientWidth ? box.clientWidth - 2 * GRAPH_PAD_X : GRAPH_W);
    const assigned = box ? parseFloat(box.style.height) || 0 : 0;
    const height = layoutGraph(nodes, links.concat(groupLinks(groups)), pos, w, assigned);
    const boxH = assigned || height;   // a box shorter than the picture shows it zoomed out to fit
    container._graphDrawn = { w: box ? box.clientWidth : 0, h: assigned };
    if (box && box.dataset.base !== String(pos.frame.natural)) {
        hooks(opts).fit(box, pos.frame.natural);   // its natural height moved, so the page gives it its room again
    }

    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("class", "list-graph");
    const full = { x: -GRAPH_PAD_X, y: 0, w: w + 2 * GRAPH_PAD_X, h: height };
    container._graphFull = full;
    showGraphView(container, svg);
    svg.setAttribute("preserveAspectRatio", "xMidYMid meet");
    svg.style.height = `${boxH}px`;
    wireGraphZoom(container, svg);
    svg.setAttribute("role", "group");   // its nodes are buttons, each reached on its own
    svg.setAttribute("aria-label", `${nodes.length} device${nodes.length === 1 ? "" : "s"}`);
    const show = container._graphShow || {};
    fillWledInfo(container, nodes);
    const boxes = nodes.map(node => graphNodeBox(pos.get(node.key), graphNodeLines(node.row, node.key, show)));
    const traffic = graphShown(show, "traffic");
    for (const [id, grp] of groups) {   // behind the edges and the nodes
        const b = groupBox([...grp.members].map(i => pos.get(nodes[i].key)));
        const rect = document.createElementNS(NS, "rect");
        rect.setAttribute("x", b.x); rect.setAttribute("y", b.y); rect.setAttribute("width", b.w); rect.setAttribute("height", b.h);
        rect.setAttribute("rx", 8);
        rect.setAttribute("class", "graph-group" + (grp.missing.size ? " graph-group-missing" : "") +
                                   ([...grp.members].every(dim) ? " graph-dim" : ""));
        rect.dataset.group = id;
        const open = () => hooks(opts).openGroup({ id, name: grp.name, rows: [...grp.members].map(i => nodes[i].row) }, opts);
        rect.classList.add("graph-link");
        rect.setAttribute("tabindex", "0");
        rect.setAttribute("role", "button");
        rect.setAttribute("aria-label", `${grp.name || "group"}: update`);
        rect.addEventListener("click", open);
        rect.addEventListener("keydown", (e) => {
            if (e.key !== "Enter" && e.key !== " ") return;
            e.preventDefault();
            open();
        });
        const tip = document.createElementNS(NS, "title");
        tip.textContent = `${grp.name}: ${grp.members.size} member${grp.members.size === 1 ? "" : "s"}${grp.missing.size ? `, ${grp.missing.size} missing` : ""}`;
        rect.appendChild(tip);
        svg.appendChild(rect);
        const label = document.createElementNS(NS, "text");
        label.setAttribute("x", b.x + 6); label.setAttribute("y", b.y + 12);
        label.setAttribute("class", "graph-group-label" + ([...grp.members].every(dim) ? " graph-dim" : ""));
        label.textContent = grp.name;
        svg.appendChild(label);
    }
    const labelAt = placeEdgeLabels(links.map(([i, j, label]) => [pos.get(nodes[i].key), pos.get(nodes[j].key), traffic ? graphTrafficLabel(label) : ""]), boxes);
    links.forEach(([i, j, label, broken], li) => {
        const a = pos.get(nodes[i].key), b = pos.get(nodes[j].key);
        const line = document.createElementNS(NS, "line");
        line.setAttribute("x1", a.x); line.setAttribute("y1", a.y);
        line.setAttribute("x2", b.x); line.setAttribute("y2", b.y);
        line.setAttribute("class", "graph-edge" + (broken ? " graph-edge-broken" : "") + (dim(i) || dim(j) ? " graph-dim" : ""));
        if (broken) {
            const why = document.createElementNS(NS, "title");
            why.textContent = "sent, but the receiver reports nothing arriving";
            line.appendChild(why);
        }
        svg.appendChild(line);
        if (labelAt[li]) {
            const t = document.createElementNS(NS, "text");
            t.setAttribute("x", labelAt[li].x); t.setAttribute("y", labelAt[li].y);
            t.setAttribute("class", "graph-edge-label" + (dim(i) || dim(j) ? " graph-dim" : ""));
            t.textContent = graphTrafficLabel(label);
            const whole = document.createElementNS(NS, "title");   // the protocol by name, on hover
            whole.textContent = label;
            t.appendChild(whole);
            svg.appendChild(t);
        }
    });
    const h = hooks(opts);
    const releases = graphReleases(container, h);
    for (const node of nodes) {
        const p = pos.get(node.key);
        const g = document.createElementNS(NS, "g");
        const age = h.rowAge(node.row);
        const behind = node.row.type === "MoonLight" ? nodeBehind(node.row, releases, h.update) : "";
        g.setAttribute("class", "graph-node" + (node.self ? " graph-self" : "") +
                                (age === "age-stale" ? " graph-faded" : "") +
                                (behind ? " graph-behind" : "") +
                                (node.row.missing ? " graph-missing" : "") +
                                (peers.get(node.key)?.state === "silent" ? " graph-unreachable" : "") +
                                (dim(index.get(node.key)) ? " graph-dim" : ""));
        const kind = graphKind(node.row);
        const dot = document.createElementNS(NS, "circle");
        dot.setAttribute("cx", p.x); dot.setAttribute("cy", p.y); dot.setAttribute("r", node.self ? 9 : 7);
        dot.setAttribute("fill", GRAPH_KIND_COLORS[kind] ?? GRAPH_KIND_COLORS.generic);
        g.appendChild(dot);
        if (behind) {
            const mark = document.createElementNS(NS, "text");   // ⬆ beside the dot: its channel offers a newer version
            mark.setAttribute("x", p.x + (node.self ? 9 : 7) + 2); mark.setAttribute("y", p.y - 4);
            mark.setAttribute("class", "graph-behind-mark");
            mark.textContent = "⬆";
            g.appendChild(mark);
        }
        let y = p.y + GRAPH_LABEL_DY;
        for (const [line, cls] of graphNodeLines(node.row, node.key, show)) {
            const text = document.createElementNS(NS, "text");
            text.setAttribute("x", p.x); text.setAttribute("y", y);
            text.setAttribute("class", cls);
            text.textContent = line;
            g.appendChild(text);
            y += GRAPH_LINE_H[cls];
        }
        const where = node.row.port ? `${node.row.ip}:${node.row.port}` : node.row.ip;
        const services = Array.isArray(node.row.services) ? node.row.services.join(", ") : "";
        const inGroups = [...groups.values()].filter(grp => grp.members.has(index.get(node.key))).map(grp => grp.name).filter(Boolean).join(", ");
        const membership = inGroups ? (node.row.missing ? `missing from ${inGroups}` : `in ${inGroups}`) : "";
        const summary = [node.row.name, where, kind, node.row.version, behind, membership, node.row.firmware, graphOutputs(node.row), services].filter(Boolean).join(" · ");
        const title = document.createElementNS(NS, "title");
        title.textContent = summary;
        g.appendChild(title);
        // A button, reached by Tab and pressed by Enter or Space as by a click.
        g.classList.add("graph-link");
        g.dataset.key = node.key;   // which node, so a redraw gives the focus back to it
        g.setAttribute("tabindex", "0");
        g.setAttribute("role", "button");
        g.setAttribute("aria-label", summary);
        g.addEventListener("click", () => h.openNode(node.row, opts));
        g.addEventListener("keydown", (e) => {
            if (e.key !== "Enter" && e.key !== " ") return;
            e.preventDefault();
            h.openNode(node.row, opts);
        });
        svg.appendChild(g);
    }
    return svg;
}

// The kind of light an output drives, as its emoji: moving heads, or its driver's kind.
function graphOutputEmoji(o) {
    return o.movingHeads ? "\u{1F3AF}" : (GRAPH_OUTPUT_EMOJI[o.driver] ?? "");
}

// What drives a device's lights, "🔲 Hub75 4096 @ 60/s, 💡 RmtLed 300 @ 60/s": each output's kind, its driver less its role noun, its lights and frames a second;
// `short` leaves the driver's name out where the kind has an emoji, as a node's label does.
export function graphOutputs(row, short = false) {
    return (Array.isArray(row.outputs) ? row.outputs : []).map(o => {
        const kind = graphOutputEmoji(o), name = typeDisplayName(String(o.driver));
        return [kind, short && kind ? "" : name, o.lights, o.fps ? `@ ${o.fps}/s` : ""].filter(x => x !== "").join(" ");
    }).join(short ? "  " : ", ");
}

// A node's label lines under its dot, each cut to its cell: its name, what drives its lights, where it
// answers and what it runs, and what it offers as emoji, which stays whole so each emoji keeps both halves.
function graphNodeLines(row, key, show = {}) {
    const on = (what) => graphShown(show, what);
    const where = row.port ? `${row.ip}:${row.port}` : row.ip;
    const offered = Array.isArray(row.services) ? row.services : [];
    return [[graphFit(String(row.name ?? key), 18), "graph-label"],
            [row.type === "MoonBase" ? "\u{1F6DF} MoonBase" : graphFit(graphOutputs(row, true), 28), "graph-sublabel"],   // a board MoonLight's installer holds
            [graphFit([on("ip") && where !== row.name ? where : "", on("version") ? row.version : ""].filter(Boolean).join(" · "), 22), "graph-sublabel"],
            [offered.map(graphServiceEmoji).join(" "), "graph-services"]].filter(([line]) => line);
}

// The part of the picture `full` the map shows: all of it at zoom 1, a 1/z part around (cx, cy) zoomed in, kept inside the picture.
function graphViewBox(full, view) {
    const z = Math.min(GRAPH_ZOOM_MAX, Math.max(1, view.z || 1)), vw = full.w / z, vh = full.h / z;
    const within = (c, lo, len, span) => Math.min(lo + len - span / 2, Math.max(lo + span / 2, c ?? lo + len / 2));
    const cx = within(view.cx, full.x, full.w, vw), cy = within(view.cy, full.y, full.h, vh);
    return [cx - vw / 2, cy - vh / 2, vw, vh];
}

// The view zoomed by `factor` around the point at fractions (fx, fy) of what the map shows, which stays where it is: zoom at the pointer.
function graphZoom(full, view, factor, fx = 0.5, fy = 0.5) {
    const [x, y, vw, vh] = graphViewBox(full, view);
    const z = Math.min(GRAPH_ZOOM_MAX, Math.max(1, (view.z || 1) * factor)), nw = full.w / z, nh = full.h / z;
    const nx = x + fx * vw - fx * nw, ny = y + fy * vh - fy * nh;
    return { z, cx: nx + nw / 2, cy: ny + nh / 2 };
}

// Show the part of the picture the view names. Zoomed in, a touch on the map is the map's to pan; at the whole picture it is the page's, so a
// swipe scrolls past the map and a pinch zooms the page.
function showGraphView(container, svg) {
    const view = container._graphView || {};
    svg.setAttribute("viewBox", graphViewBox(container._graphFull, view).join(" "));
    svg.style.touchAction = (view.z || 1) > 1 ? "none" : "";
}

// Zoom and pan as a map does, and as the preview does: the scroll wheel or a trackpad pinch zooms at the pointer,
// a drag on the background pans and a double-click on it fits the whole picture again.
function wireGraphZoom(container, svg) {
    const apply = () => showGraphView(container, svg);
    const fraction = (e) => {
        const ctm = svg.getScreenCTM();
        if (!ctm) return [0.5, 0.5];
        const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(ctm.inverse());
        const [x, y, vw, vh] = graphViewBox(container._graphFull, container._graphView || {});
        return [(p.x - x) / vw, (p.y - y) / vh];
    };
    svg.addEventListener("wheel", (e) => {
        // The map's, as the preview's wheel is, when it zooms; at the whole picture a wheel outward scrolls the page instead. A trackpad pinch arrives as a Ctrl-wheel.
        if (e.deltaY > 0 && !(((container._graphView || {}).z || 1) > 1)) return;
        e.preventDefault();
        const [fx, fy] = fraction(e);
        const rate = e.ctrlKey ? 0.01 : 0.0015;   // a wheel notch at the preview's rate; a pinch sends smaller steps
        container._graphView = graphZoom(container._graphFull, container._graphView || {}, Math.exp(-e.deltaY * rate), fx, fy);
        apply();
    }, { passive: false });
    svg.addEventListener("pointerdown", (e) => {
        if (e.target.closest(".graph-node") || !((container._graphView || {}).z > 1)) return;
        const ctm = svg.getScreenCTM();
        if (!ctm) return;
        const start = { x: e.clientX, y: e.clientY, view: graphViewBox(container._graphFull, container._graphView) };
        container._graphPanning = true;
        svg.setPointerCapture(e.pointerId);
        const move = (m) => {
            const [x, y, vw, vh] = start.view;
            container._graphView = { z: container._graphView.z, cx: x + vw / 2 - (m.clientX - start.x) / ctm.a, cy: y + vh / 2 - (m.clientY - start.y) / ctm.d };
            apply();
        };
        const end = () => {
            svg.removeEventListener("pointermove", move);
            container._graphPanning = false;
            redrawGraph(container);
        };
        svg.addEventListener("pointermove", move);
        svg.addEventListener("pointerup", end, { once: true });
        svg.addEventListener("pointercancel", end, { once: true });
    });
    svg.addEventListener("dblclick", (e) => {
        if (e.target.closest(".graph-node")) return;
        container._graphView = {};
        apply();
    });
}

// Whether the map shows `what`: the viewer's choice, else its default in GRAPH_SHOW.
function graphShown(show, what) {
    return what in show ? !!show[what] : !!(GRAPH_SHOW.find(([w]) => w === what) || [])[2];
}

// A WLED's version and flavor for its node, read from its /json/info, which WLED serves cross-origin; its presence packet carries only its address and name.
// WLED-MM answers product "MoonModules", WLED "FOSS". Asked again a minute after a WLED that did not answer and ten minutes after one that did, the
// last answer shown meanwhile; a WLED by another name at the address is another device, asked afresh.
function fillWledInfo(container, nodes, now = Date.now()) {
    const known = container._wledInfo || (container._wledInfo = new Map());
    for (const node of nodes) {
        if (node.row.type !== "WLED" || !node.row.ip) continue;
        const name = node.row.name;
        const k = known.get(node.key)?.name === name ? known.get(node.key) : null;
        if (k && k.ver) node.row = Object.assign({}, node.row, { version: k.ver, flavor: k.flavor });
        if (k && now - k.at < (k.ver ? GRAPH_WLED_INFO_MS : GRAPH_WLED_ASK_MS)) continue;
        known.set(node.key, { ...k, at: now, name });
        fetch(`http://${node.row.ip}/json/info`).then(r => r.json()).then(info => {
            if (!info || !info.ver) return;
            known.set(node.key, { at: now, name, ver: String(info.ver), flavor: info.product === "MoonModules" ? "WLED-MM" : "WLED" });
            redrawGraph(container);
        }).catch(() => { /* no answer: asked again in a minute */ });
    }
}

// What this viewer chose to show on the map; kept in this browser only.
function graphShowLoad() {
    try { return JSON.parse(localStorage.getItem(GRAPH_SHOW_KEY) || "{}") || {}; } catch (_) { return {}; }
}

// A search field above the map: typing greys out the devices that do not match (graphMatches), redrawn as each key lands; Escape clears it.
// The search lasts while the page is open, since a filter left on from an earlier visit would grey out devices with no reason in sight.
function buildGraphSearch(container) {
    const input = document.createElement("input");
    input.type = "search";
    input.className = "graph-search";
    input.placeholder = "Search devices";
    input.setAttribute("aria-label", "Search the map's devices by name, address, type, version, firmware or group; the rest grey out");
    input.value = container._graphQuery || "";
    const apply = () => { container._graphQuery = input.value; redrawGraph(container); };
    input.addEventListener("input", apply);
    input.addEventListener("keydown", (e) => { if (e.key === "Escape" && input.value) { input.value = ""; apply(); } });
    return input;
}

// A row of toggles above the map, one per thing a node or a line can show; a toggle redraws the map at once.
function buildGraphFilters(container) {
    const bar = document.createElement("div");
    bar.className = "graph-filters";
    bar.setAttribute("role", "group");
    bar.setAttribute("aria-label", "What the map shows");
    const show = graphShowLoad();
    bar.appendChild(buildGraphSearch(container));
    for (const [what, label] of GRAPH_SHOW) {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "graph-filter";
        btn.textContent = label;
        btn.setAttribute("aria-pressed", String(graphShown(show, what)));
        btn.addEventListener("click", () => {
            const now = graphShowLoad();
            now[what] = !graphShown(now, what);
            try { localStorage.setItem(GRAPH_SHOW_KEY, JSON.stringify(now)); } catch (_) { /* a private window keeps it for this draw only */ }
            container._graphShow = now;
            btn.setAttribute("aria-pressed", String(now[what]));
            redrawGraph(container);
        });
        bar.appendChild(btn);
    }
    // The whole map again, after the scroll wheel zoomed in.
    const fit = document.createElement("button");
    fit.type = "button";
    fit.className = "graph-filter graph-zoom";
    fit.textContent = "\u2316";   // ⌖, the preview's reset-view glyph
    fit.title = "Reset the view: the whole map";
    fit.setAttribute("aria-label", fit.title);
    fit.addEventListener("click", () => {
        const svg = container.querySelector("svg.list-graph");
        if (!svg || !container._graphFull) return;
        container._graphView = {};
        showGraphView(container, svg);
    });
    bar.appendChild(fit);
    container._graphShow = show;
    return bar;
}

// The box a node's dot and labels cover, from the lines' estimated widths, which an edge label keeps clear of.
function graphNodeBox(p, lines) {
    const w = Math.max(18, ...lines.map(([line, cls]) => [...line].length * GRAPH_CHAR_W[cls]));
    const h = GRAPH_LABEL_DY + lines.reduce((sum, [, cls]) => sum + GRAPH_LINE_H[cls], 0);
    return { x0: p.x - w / 2, x1: p.x + w / 2, y0: p.y - 10, y1: p.y + h - 8 };
}

// Where each edge's label goes, or null for an unlabeled edge: greedy label placement, each label
// at the first spot along its edge, from the middle outward, that covers no node and no label placed
// before it; with none free, the spot covering least, so a crowded map still shows every label.
function placeEdgeLabels(edges, nodeBoxes) {
    const placed = [];
    const overlap = (a, b) => Math.max(0, Math.min(a.x1, b.x1) - Math.max(a.x0, b.x0)) * Math.max(0, Math.min(a.y1, b.y1) - Math.max(a.y0, b.y0));
    return edges.map(([a, b, label]) => {
        if (!label) return null;
        const w = label.length * GRAPH_CHAR_W["graph-edge-label"], h = 11;
        let best = null;
        for (const t of [0.5, 0.4, 0.6, 0.3, 0.7, 0.2, 0.8]) {
            const x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t - 3;   // its baseline 3 px above the line
            const box = { x0: x - w / 2, x1: x + w / 2, y0: y - h + 2, y1: y + 2 };
            const cost = [...nodeBoxes, ...placed].reduce((sum, o) => sum + overlap(box, o), 0);
            if (!best || cost < best.cost) best = { x, y, box, cost };
            if (cost === 0) break;
        }
        placed.push(best.box);
        return { x: best.x, y: best.y };
    });
}

// A line's label, "audio sync 23 fps", with its protocol as its emoji: "🔊 23 fps"; a protocol lacking one keeps its name.
function graphTrafficLabel(label) {
    const protocol = Object.keys(GRAPH_PROTOCOL_EMOJI).filter(p => label.startsWith(p)).sort((a, b) => b.length - a.length)[0];
    return protocol ? GRAPH_PROTOCOL_EMOJI[protocol] + label.slice(protocol.length) : label;
}

// A reported service, "HTTP 80" or "ColorLight", as its emoji: the protocol alone, its port dropped, looked up.
function graphServiceEmoji(service) {
    const protocol = service.replace(/ \d+$/, "");
    return GRAPH_PROTOCOL_EMOJI[protocol] ?? protocol;
}

// A label cut to `max` characters with an ellipsis, so it stays within its cell; the tooltip has it whole.
function graphFit(text, max) {
    const chars = [...text];   // by code point, so no emoji is split into halves
    return chars.length > max ? `${chars.slice(0, max - 1).join("")}…` : text;
}

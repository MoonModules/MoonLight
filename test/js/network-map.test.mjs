// The Devices map (app.js, a list drawn as a graph): where the nodes go, which color each kind
// takes, and that a peer gone from the list leaves the map. Run without a browser, on the
// functions' own source.
// Run: `node --test test/js/network-map.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { src, fnSource } from "./app-source.mjs";

// The map's constants, every top-level `const GRAPH_...` and `const PEER_...` line.
const constants = src.split("\n").filter(l => /^const (GRAPH_|PEER_)/.test(l)).join("\n");
const graphKey = "const graphKey = (row) => String(row.ip ?? row.name);";

function load(names, extra = "") {
    const body = `${constants}\n${graphKey}\n${extra}\n${names.map(fnSource).join("\n")}\nreturn { ${[...names, "GRAPH_W", "GRAPH_CELL_H", "GRAPH_KIND_COLORS"].join(", ")} };`;
    return new Function(body)();
}

const node = (name, extra = {}) => ({ key: name, self: !!extra.self, row: { name, ...extra } });

test("devices without traffic sit in a grid beneath the linked ones, by name, every label inside", () => {
    const { layoutGraph, GRAPH_W, GRAPH_CELL_H } = load(["graphHash", "layoutGraph", "layoutForce"]);
    const nodes = [node("self", { self: true }), node("peer"), node("zeta"), node("alpha"), node("mid")];
    const links = [[0, 1, "DDP", false]];
    const pos = new Map();
    const height = layoutGraph(nodes, links, pos);
    const at = (k) => pos.get(k);
    assert.equal(at("self").x, GRAPH_W / 2);
    const linkedBottom = Math.max(at("self").y, at("peer").y);
    for (const k of ["alpha", "mid", "zeta"]) assert.ok(at(k).y > linkedBottom, `${k} sits below the linked nodes`);
    assert.ok(at("alpha").x < at("mid").x && at("mid").x < at("zeta").x, "the grid runs by name");
    // The lowest dot leaves room for its three label lines, about 45 px, inside the picture.
    const lowest = Math.max(...[...pos.values()].map(p => p.y));
    assert.ok(lowest + 45 <= height, `labels fit: dot at ${lowest}, picture ${height} high`);
    assert.ok(height <= 300 + GRAPH_CELL_H, "one grid row adds one cell");
});

test("a device that gains traffic leaves the grid and is placed by force", () => {
    const { layoutGraph } = load(["graphHash", "layoutGraph", "layoutForce"]);
    const nodes = [node("self", { self: true }), node("peer")];
    const pos = new Map();
    layoutGraph(nodes, [], pos);
    assert.equal(pos.get("peer").idle, true);
    layoutGraph(nodes, [[0, 1, "DDP", false]], pos);
    assert.ok(!pos.get("peer").idle, "re-seeded by the force layout");
});

test("every kind Discovery names has a color of its own", () => {
    const { GRAPH_KIND_COLORS } = load([]);
    const header = readFileSync(new URL("../../src/core/system/DeviceIdentify.h", import.meta.url), "utf8");
    const labels = [...new Set([...header.matchAll(/return "([^"]+)";/g)].map(m => m[1]))];
    assert.ok(labels.length >= 5, "devTypeStr's labels found");
    for (const label of labels) assert.ok(GRAPH_KIND_COLORS[label], `${label} has a color`);
    assert.equal(new Set(labels.map(l => GRAPH_KIND_COLORS[l])).size, labels.length, "no two kinds share one");
});

test("a peer gone from this device's list takes its merged graph off the map", () => {
    const fetched = [];
    const { refreshPeerGraphs } = load(["refreshPeerGraphs"],
        "async function fetchPeerGraph(url) { fetched.push(url); return { state: 'ok', rows: [] }; }\n" +
        "function redrawGraph() {}\nconst fetched = [];\nglobalThis.__fetched = fetched;");
    const rows = [{ name: "me", ip: "10.0.0.1", type: "MoonLight", self: true },
                  { name: "still", ip: "10.0.0.2", type: "MoonLight", url: "http://10.0.0.2/" }];
    const container = {
        _graphArgs: { rows, details: [], opts: { moduleName: "Discovery", ctrlName: "devices" } },
        _peerGraphs: new Map([["10.0.0.9", { state: "ok", rows: [{ name: "gone-peer's view", ip: "10.0.0.50" }] }],
                              ["10.0.0.2", { state: "ok", rows: [] }]]),
        isConnected: true,
        offsetParent: {},
    };
    refreshPeerGraphs(container);
    assert.ok(!container._peerGraphs.has("10.0.0.9"), "the departed peer's graph is dropped");
    assert.ok(container._peerGraphs.has("10.0.0.2"), "a listed peer's graph stays until refreshed");
});

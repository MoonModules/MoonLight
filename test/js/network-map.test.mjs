// The Devices map (app.js, a list drawn as a graph): where the nodes go, which color each kind
// takes, and that a peer gone from the list leaves the map. Run without a browser, on the
// functions' own source.
// Run: `node --test test/js/network-map.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { src, fnSource } from "./app-source.mjs";
import { typeDisplayName } from "../../src/ui/help.js";

// The map's constants, every top-level `const GRAPH_...` and `const PEER_...` line.
const constants = src.split("\n").filter(l => /^const (GRAPH_|PEER_)/.test(l)).join("\n");
const graphKey = "const graphKey = (row) => String(row.ip ?? row.name);";

function load(names, extra = "") {
    const body = `${constants}\n${graphKey}\n${extra}\n${names.map(fnSource).join("\n")}\nreturn { ${[...names, "GRAPH_W", "GRAPH_CELL_H", "GRAPH_KIND_COLORS"].join(", ")} };`;
    return new Function("typeDisplayName", body)(typeDisplayName);
}

const node = (name, extra = {}) => ({ key: name, self: !!extra.self, row: { name, ...extra } });

test("devices without traffic sit in a grid beneath the linked ones, by name, every label inside", () => {
    const { layoutGraph, GRAPH_W, GRAPH_CELL_H } = load(["graphHash", "layoutGraph", "layoutForce", "graphInside", "separateNodes"]);
    const nodes = [node("self", { self: true }), node("peer"), node("zeta"), node("alpha"), node("mid")];
    const links = [[0, 1, "DDP", false]];
    const pos = new Map();
    const height = layoutGraph(nodes, links, pos);
    const at = (k) => pos.get(k);
    assert.equal(at("self").x, GRAPH_W / 2);
    const linkedBottom = Math.max(at("self").y, at("peer").y);
    for (const k of ["alpha", "mid", "zeta"]) assert.ok(at(k).y > linkedBottom, `${k} sits below the linked nodes`);
    assert.ok(at("alpha").x < at("mid").x && at("mid").x < at("zeta").x, "the grid runs by name");
    // The lowest dot leaves room for its four label lines, about 57 px, inside the picture.
    const lowest = Math.max(...[...pos.values()].map(p => p.y));
    assert.ok(lowest + 57 <= height, `labels fit: dot at ${lowest}, picture ${height} high`);
    assert.ok(height <= 300 + GRAPH_CELL_H, "one grid row adds one cell");
});

test("a device that gains traffic leaves the grid and is placed by force", () => {
    const { layoutGraph } = load(["graphHash", "layoutGraph", "layoutForce", "graphInside", "separateNodes"]);
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

test("a WLED takes its flavor's color once its info says which, and the generic one until then", () => {
    const { graphKind } = load(["graphKind"]);
    assert.equal(graphKind({ type: "WLED", flavor: "WLED-MM" }), "WLED-MM");
    assert.equal(graphKind({ type: "WLED", flavor: "WLED" }), "WLED");
    assert.equal(graphKind({ type: "WLED" }), "generic", "a WLED-MM that does not answer never shows as a plain WLED");
    assert.equal(graphKind({ type: "Tasmota" }), "Tasmota");
    assert.equal(graphKind({}), "generic");
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

test("a node shows each service it offers as an emoji, and a service without one as its protocol", () => {
    const { graphServiceEmoji } = load(["graphServiceEmoji"]);
    assert.equal(graphServiceEmoji("HTTP 80"), "\u{1F310}", "the port is dropped, the emoji found");
    assert.equal(graphServiceEmoji("audio sync 11988"), "\u{1F50A}", "a protocol of two words");
    assert.equal(graphServiceEmoji("ColorLight"), "\u{1F9F1}", "a service with no port");
    assert.equal(graphServiceEmoji("Zigbee 8080"), "Zigbee", "an unknown service keeps its name");
});

const LINE_FNS = ["graphShown", "graphFit", "graphServiceEmoji", "graphTrafficLabel", "graphOutputEmoji", "graphOutputs", "graphNodeLines", "graphNodeBox", "placeEdgeLabels"];

test("a node reads its name, then what drives its lights, then where it answers, then what it offers", () => {
    const { graphNodeLines } = load(LINE_FNS);
    const lines = graphNodeLines({ name: "MM-P4-hdmi", ip: "192.168.1.132", version: "7.1.0", services: ["HTTP 80"],
                                   outputs: [{ driver: "HdmiDriver", lights: 4096 }, { driver: "ParallelLedDriver", lights: 300 }] }, "k", { ip: true, version: true });
    assert.deepEqual(lines.map(([t]) => t), ["MM-P4-hdmi", "\u{1F4FA} 4096  \u{1F4A1} 300", "192.168.1.132 · 7.1.0", "\u{1F310}"]);
    assert.deepEqual(graphNodeLines({ name: "WLED-Troy", ip: "192.168.1.223" }, "k", { ip: true }).map(([t]) => t), ["WLED-Troy", "192.168.1.223"],
                     "a line with nothing to say is left out");
});

test("two edge labels that would meet at their middles are placed apart, each clear of the nodes", () => {
    const { placeEdgeLabels, graphNodeLines, graphNodeBox } = load(LINE_FNS);
    // Two flows from one sender to two receivers close together, as .221's audio sync to two groups.
    const sender = { x: 300, y: 250 }, a = { x: 280, y: 50 }, b = { x: 320, y: 50 };
    const boxes = [sender, a, b].map((p, i) => graphNodeBox(p, graphNodeLines({ name: `node${i}`, ip: "10.0.0.1" }, "k")));
    const [la, lb] = placeEdgeLabels([[sender, a, "audio sync 23 fps"], [sender, b, "audio sync 22 fps"]], boxes);
    const w = "audio sync 23 fps".length * 5.4;
    assert.ok(Math.abs(la.x - lb.x) >= w || Math.abs(la.y - lb.y) >= 11, `apart: ${JSON.stringify([la, lb])}`);
    for (const l of [la, lb]) for (const box of boxes)
        assert.ok(l.y + 2 <= box.y0 || l.y - 9 >= box.y1 || l.x + w / 2 <= box.x0 || l.x - w / 2 >= box.x1, "clear of every node");
    assert.equal(placeEdgeLabels([[sender, a, ""]], boxes)[0], null, "an edge without a label gets no spot");
});

test("an output shows the kind of light it drives: moving heads whatever drives them, a driver without an emoji by name", () => {
    const { graphOutputs } = load(LINE_FNS);
    const row = { outputs: [{ driver: "NetworkSendDriver", lights: 12, movingHeads: true }, { driver: "Hub75Driver", lights: 4096, fps: 45 },
                            { driver: "FutureDriver", lights: 8 }] };
    assert.equal(graphOutputs(row, true), "\u{1F3AF} 12  \u{1F532} 4096 @ 45/s  Future 8", "a node's label: the kind, or the name, and its frames a second");
    assert.equal(graphOutputs(row), "\u{1F3AF} NetworkSend 12, \u{1F532} Hub75 4096 @ 45/s, Future 8", "the popup: kind and name");
});

test("a hub linked to many devices, as Home Assistant to every board, spreads them so no two cells overlap", () => {
    const { layoutGraph, GRAPH_W } = load(["graphHash", "layoutGraph", "layoutForce", "graphInside", "separateNodes"]);
    const names = ["self", "ha", ...Array.from({ length: 10 }, (_, i) => `board${i}`)];
    const nodes = names.map((n, i) => node(n, { self: i === 0 }));
    const links = names.slice(2).map((_, i) => [1, i + 2, "WLED API 24 req/min", false]).concat([[0, 1, "E1.31", false]]);
    const pos = new Map();
    layoutGraph(nodes, links, pos);
    const cells = names.map(n => pos.get(n));
    for (let a = 0; a < cells.length; a++) for (let b = a + 1; b < cells.length; b++) {
        const apart = Math.abs(cells[a].x - cells[b].x) >= 120 - 0.5 || Math.abs(cells[a].y - cells[b].y) >= 76 - 10 - 0.5;
        assert.ok(apart, `${names[a]} and ${names[b]} overlap: ${JSON.stringify([cells[a], cells[b]])}`);
    }
    assert.equal(pos.get("self").x, GRAPH_W / 2, "this device stays at the center");
});

test("a node's IP and version start hidden and show when the viewer turns them on; its name, lights and services always show", () => {
    const { graphNodeLines, graphShown } = load(LINE_FNS);
    const row = { name: "MM-P4-pc", ip: "192.168.1.221", version: "7.1.0", services: ["HTTP 80"], outputs: [{ driver: "HdmiDriver", lights: 64800 }] };
    assert.deepEqual(graphNodeLines(row, "k").map(([t]) => t), ["MM-P4-pc", "\u{1F4FA} 64800", "\u{1F310}"], "by default: no IP, no version");
    assert.deepEqual(graphNodeLines(row, "k", { version: true }).map(([t]) => t), ["MM-P4-pc", "\u{1F4FA} 64800", "7.1.0", "\u{1F310}"]);
    assert.equal(graphShown({}, "traffic"), true, "traffic starts shown");
    assert.equal(graphShown({ traffic: false }, "traffic"), false);
});

test("the map lays out at its box's size: a wider box holds more devices a row, a taller one gives the linked ones the room", () => {
    const { layoutGraph } = load(["graphHash", "layoutGraph", "layoutForce", "graphInside", "separateNodes"]);
    const make = () => [node("self", { self: true }), node("peer"), ...Array.from({ length: 8 }, (_, i) => node(`idle${i}`))];
    const narrow = new Map(), wide = new Map();
    const hNarrow = layoutGraph(make(), [[0, 1, "DDP", false]], narrow, 480);
    const hWide = layoutGraph(make(), [[0, 1, "DDP", false]], wide, 960);
    assert.ok(hWide < hNarrow, `eight idle devices take fewer rows at 960 px (${hWide}) than at 480 (${hNarrow})`);
    const tall = new Map();
    assert.equal(layoutGraph(make(), [[0, 1, "DDP", false]], tall, 480, hNarrow + 200), hNarrow + 200, "drawn to the height it is given");
    assert.equal(tall.frame.natural, hNarrow, "and still knows its natural height, which the panel shares from");
    assert.ok(Math.min(...[...tall.values()].filter(p => p.idle).map(p => p.y)) > Math.min(...[...narrow.values()].filter(p => p.idle).map(p => p.y)),
              "the extra height goes to the linked devices' area, above the grid");
});

test("the map zooms as a map does: the whole picture at zoom 1, the point under the pointer kept as it zooms in, never past the picture's edge", () => {
    const { graphViewBox, graphZoom } = load(["graphViewBox", "graphZoom"]);
    const full = { x: -12, y: 0, w: 624, h: 400 };
    assert.deepEqual(graphViewBox(full, {}), [-12, 0, 624, 400], "unzoomed, the whole picture");
    // Zoom in at the picture's left quarter: that point stays at the same fraction of what is shown.
    const view = graphZoom(full, {}, 2, 0.25, 0.5);
    const [x, , vw] = graphViewBox(full, view);
    assert.equal(vw, 312);
    assert.equal(x + 0.25 * vw, -12 + 0.25 * 624, "the point under the pointer stays under it");
    assert.deepEqual(graphViewBox(full, { z: 2, cx: -1000, cy: 0 }).slice(0, 2), [-12, 0], "panned past the edge, held at it");
    assert.equal(graphZoom(full, { z: 8 }, 2).z, 8, "zoom stops at eight times");
    assert.equal(graphZoom(full, {}, 0.5).z, 1, "and out at the whole picture");
});

test("a line shows its protocol as an emoji, the longest protocol name matched, one without an emoji by name", () => {
    const { graphTrafficLabel } = load(LINE_FNS);
    assert.equal(graphTrafficLabel("audio sync 23/s"), "\u{1F50A} 23/s");
    assert.equal(graphTrafficLabel("WLED API 24/m"), "\u{1F419} 24/m", "Home Assistant's polling, WLED's octopus");
    assert.equal(graphTrafficLabel("DDP"), "\u{1F4E6}", "a flow without a rate");
    assert.equal(graphTrafficLabel("Zigbee 3/s"), "Zigbee 3/s");
});

test("a board MoonBase holds shows a lifebuoy for its lights", () => {
    const { graphNodeLines } = load(LINE_FNS);
    assert.deepEqual(graphNodeLines({ name: "MM-Olimex", ip: "192.168.1.207", type: "MoonBase" }, "k").map(([t]) => t),
                     ["MM-Olimex", "\u{1F6DF} MoonBase"]);
});

test("a WLED offers Update to MoonLight for a chip MoonLight builds for, its migration image from the installer's mirror", () => {
    const installerUrl = src.split("\n").find(l => l.startsWith("const INSTALLER_URL"));
    const { wledChip, migrateImageUrl, wledFamily } = load(["wledChip", "migrateImageUrl", "wledFamily"], installerUrl);
    assert.equal(wledChip({ arch: "esp32-c3" }), "esp32c3");
    assert.equal(wledChip({ arch: "esp32" }), "esp32");
    assert.equal(wledChip({ arch: "esp32-p4" }), "esp32p4");
    assert.equal(wledChip({ arch: "esp8266" }), null, "a chip MoonLight does not build for");
    assert.equal(wledChip({}), null);
    assert.equal(migrateImageUrl("latest", "esp32"), "https://moonmodules.org/MoonLight/install/releases/latest/shared-migrate-esp32.bin");
    assert.equal(wledFamily({ e32model: "ESP32-P4 rev.301" }, "esp32p4"), "esp32p4rev3", "a P4 from revision 3 takes its own image");
    assert.equal(wledFamily({ e32model: "ESP32-P4 rev.103" }, "esp32p4"), "esp32p4", "a revision-1 P4 takes the chip's");
    assert.equal(wledFamily({}, "esp32p4"), "esp32p4", "no chip model: the chip's image, which a revision-3 P4 refuses unwritten");
    assert.equal(wledFamily({ e32model: "ESP32-S3 rev.2" }, "esp32s3"), "esp32s3");
});

test("a node's popup lists its flavor as its type, and as its url the .local address a MoonLight board answers on", () => {
    const { popupFields } = load(["popupFields"]);
    assert.deepEqual(popupFields({ name: "WLED-Troy", ip: "192.168.1.223", type: "WLED", url: "http://192.168.1.223/", flavor: "WLED-MM" }),
                     { name: "WLED-Troy", ip: "192.168.1.223", type: "WLED-MM" }, "the ip links to the device, so the same url goes");
    assert.equal(popupFields({ name: "ML-olimex", ip: "192.168.1.207", type: "MoonLight", url: "http://192.168.1.207/" }).url, "http://ML-olimex.local/");
    assert.equal(popupFields({ name: "m4-pc", ip: "192.168.1.147", type: "MoonLight", port: 8080, url: "http://192.168.1.147:8080/" }).url, undefined,
                 "a desktop serves on a port and answers no mDNS");
    assert.equal(popupFields({ name: "my lights", ip: "192.168.1.9", type: "MoonLight" }).url, undefined, "a name that is no hostname");
});

test("a WLED on Ethernet hears whether the move carries its wiring over or brings it back on WiFi", () => {
    const mapped = src.split("\n").find(l => l.startsWith("const WLED_ETHERNET_MAPPED"));
    const { wledEthernetNote } = load(["wledEthernetNote"], mapped);
    assert.equal(wledEthernetNote(0), "", "a WLED on WiFi");
    assert.equal(wledEthernetNote(undefined), "", "a WLED that did not answer /json/cfg");
    assert.match(wledEthernetNote(4), /comes back on the cable/, "the QuinLED wiring, the Olimex Gateway's");
    assert.match(wledEthernetNote(7), /saved WiFi/, "the Ethernet Kit's IP101");
    assert.match(wledEthernetNote(11), /saved WiFi/, "a board WLED and WLED-MM number differently");
});

test("the map's list of carried WLED Ethernet boards is the migration's table: the rows of kWledEthernet with a PHY", () => {
    const header = readFileSync(new URL("../../src/core/util/WledNetworks.h", import.meta.url), "utf8");
    const table = header.slice(header.indexOf("kWledEthernet[] = {"), header.indexOf("};", header.indexOf("kWledEthernet[] = {")));
    const carried = [...table.matchAll(/\{(\d+),[^}]*\},\s*\/\/ (\d+) /g)].filter(m => m[1] !== "0").map(m => Number(m[2]));
    const listed = JSON.parse(src.split("\n").find(l => l.startsWith("const WLED_ETHERNET_MAPPED")).replace(/^[^[]*/, "").replace(/;\s*$/, ""));
    assert.deepEqual(listed, carried);
});

test("the MD5 ESPHome asks for before its update matches RFC 1321's own vectors and Node's on a megabyte", async () => {
    const { md5Hex } = load(["md5Hex"]);
    const enc = (t) => new TextEncoder().encode(t);
    assert.equal(md5Hex(enc("")), "d41d8cd98f00b204e9800998ecf8427e");
    assert.equal(md5Hex(enc("abc")), "900150983cd24fb0d6963f7d28e17f72");
    assert.equal(md5Hex(enc("message digest")), "f96b697d7cb7938d525a2f31aaf161d0");
    assert.equal(md5Hex(enc("12345678901234567890123456789012345678901234567890123456789012345678901234567890")), "57edf4a22be3c955ac49da2e2107b67a");
    const { createHash } = await import("node:crypto");
    const big = new Uint8Array(1037584).map((_, i) => (i * 2654435761) >>> 24);   // a migration image's size, any bytes
    assert.equal(md5Hex(big), createHash("md5").update(big).digest("hex"));
});

